// SPDX-License-Identifier: GPL-2.0
/*
 * kernel/ai/ai_core.c - Linux AI Subsystem Core
 *
 * Implements the /dev/ai character device and OpenClaw registry.
 *
 * Architecture overview:
 *
 *   Multiple userspace clients (processes, DTE) open /dev/ai and write
 *   ai_message structs (queries). The aicore daemon also opens /dev/ai
 *   and registers itself via AI_IOC_SET_DAEMON.
 *
 *   Message flow:
 *     Client write(query)  -> kernel rx_queue -> daemon read()
 *     Daemon write(resp)   -> kernel routes by msg.id -> client read()
 *
 *   Each open fd is tracked as an ai_client. The daemon fd is special:
 *   it reads ALL pending queries and writes responses with correlation IDs.
 *   Regular client fds read only their own responses.
 *
 *   OpenClaw registry:
 *     Kernel subsystems call ai_claw_register() to publish tools.
 *     The daemon queries via AI_IOC_LIST_CLAWS and includes tool schemas
 *     in LLM API requests, enabling the AI to call back into the kernel.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/random.h>
#include <linux/ai.h>

#define AI_DEVICE_NAME		"ai"
#define AI_CLASS_NAME		"ai"
#define AI_MINOR_BASE		0
#define AI_MINOR_COUNT		1

/* ===== Message Queue Entry ===== */

/**
 * struct ai_queued_msg - A message waiting in the kernel queue
 * @list:     Linked list node
 * @src_pid:  PID of the process that sent this message
 * @src_uid:  UID of the process that sent this message
 * @msg:      The actual message (header + data, variable length)
 */
struct ai_queued_msg {
	struct list_head list;
	pid_t		 src_pid;
	uid_t		 src_uid;
	struct ai_message msg; /* Must be last - flexible array follows */
};

/* ===== Per-fd Client State ===== */

/**
 * struct ai_client - State for one open /dev/ai file descriptor
 * @list:      Global client list node
 * @is_daemon: True if this fd registered via AI_IOC_SET_DAEMON
 * @session:   Session ID (for multi-turn conversations)
 * @rx_list:   Messages waiting to be read by this client
 * @rx_lock:   Protects rx_list
 * @rx_wait:   Wait queue for blocking read()
 * @pid:       PID of process holding this fd
 * @uid:       UID of process holding this fd
 */
struct ai_client {
	struct list_head  list;
	bool		  is_daemon;
	u32		  session;
	struct list_head  rx_list;
	spinlock_t	  rx_lock;
	wait_queue_head_t rx_wait;
	pid_t		  pid;
	uid_t		  uid;
};

/* ===== Global Subsystem State ===== */

static dev_t		   ai_devno;
static struct cdev	   ai_cdev;
static struct class	  *ai_class;
static struct device	  *ai_device;

/* All open client fds */
static LIST_HEAD(ai_client_list);
static DEFINE_SPINLOCK(ai_client_lock);

/* The one registered daemon client (if any) */
static struct ai_client *ai_daemon_client;
static DEFINE_MUTEX(ai_daemon_mutex);

/* OpenClaw registry */
static LIST_HEAD(ai_claw_list);
static DEFINE_RWLOCK(ai_claw_lock);
static atomic_t ai_claw_count = ATOMIC_INIT(0);

/* Global stats (exported via include/linux/ai.h) */
struct ai_stats ai_global_stats;
EXPORT_SYMBOL_GPL(ai_global_stats);

/* Next message ID (monotonically increasing) */
static atomic_t ai_next_msg_id = ATOMIC_INIT(1);

/* Subsystem ready flag */
static bool ai_ready;

/* ===== Helpers ===== */

static struct ai_queued_msg *alloc_queued_msg(const struct ai_message __user *umsg,
					      size_t total_size)
{
	struct ai_queued_msg *qmsg;

	/* total_size includes header + data */
	qmsg = kmalloc(sizeof(*qmsg) - sizeof(qmsg->msg) + total_size,
		       GFP_KERNEL);
	if (!qmsg)
		return NULL;

	INIT_LIST_HEAD(&qmsg->list);
	qmsg->src_pid = current->pid;
	qmsg->src_uid = from_kuid_munged(current_user_ns(), current_uid());

	if (copy_from_user(&qmsg->msg, umsg, total_size)) {
		kfree(qmsg);
		return NULL;
	}

	/* Kernel stamps uid/pid; clients cannot spoof these */
	qmsg->msg.uid = qmsg->src_uid;
	qmsg->msg.pid = qmsg->src_pid;

	return qmsg;
}

static void deliver_to_client(struct ai_client *client, struct ai_queued_msg *qmsg)
{
	struct ai_queued_msg *copy;
	size_t msg_size = AI_MSG_HEADER_SIZE + qmsg->msg.data_len;

	copy = kmalloc(sizeof(*copy) - sizeof(copy->msg) + msg_size, GFP_ATOMIC);
	if (!copy)
		return;

	memcpy(copy, qmsg, sizeof(*qmsg) - sizeof(qmsg->msg) + msg_size);
	INIT_LIST_HEAD(&copy->list);

	spin_lock(&client->rx_lock);
	list_add_tail(&copy->list, &client->rx_list);
	spin_unlock(&client->rx_lock);

	wake_up_interruptible(&client->rx_wait);
}

/**
 * route_response_to_client() - Route a daemon response back to its originating client
 *
 * The daemon writes responses with the correlation ID of the original query.
 * We find the client that sent that query by matching session IDs, or broadcast
 * to all non-daemon clients if session routing is unavailable.
 */
static void route_response_to_client(struct ai_queued_msg *qmsg)
{
	struct ai_client *client;
	bool routed = false;

	spin_lock(&ai_client_lock);
	list_for_each_entry(client, &ai_client_list, list) {
		if (client->is_daemon)
			continue;
		if (qmsg->msg.session && client->session != qmsg->msg.session)
			continue;
		deliver_to_client(client, qmsg);
		routed = true;
		if (qmsg->msg.session)
			break; /* Session-targeted: one client only */
	}
	spin_unlock(&ai_client_lock);

	if (!routed)
		pr_debug("ai: response id=%u not routed (no matching client)\n",
			 qmsg->msg.id);
}

/* ===== OpenClaw Registry ===== */

/**
 * ai_claw_register() - Register a kernel AI tool
 */
int ai_claw_register(struct ai_claw *claw)
{
	struct ai_claw *existing;

	if (!claw || !claw->name || !claw->execute)
		return -EINVAL;
	if (strlen(claw->name) >= AI_MAX_TOOL_NAME)
		return -EINVAL;

	write_lock(&ai_claw_lock);
	list_for_each_entry(existing, &ai_claw_list, list) {
		if (strcmp(existing->name, claw->name) == 0) {
			write_unlock(&ai_claw_lock);
			return -EEXIST;
		}
	}
	INIT_LIST_HEAD(&claw->list);
	list_add_tail(&claw->list, &ai_claw_list);
	write_unlock(&ai_claw_lock);

	atomic_inc(&ai_claw_count);
	pr_info("ai: registered OpenClaw '%s'\n", claw->name);
	return 0;
}
EXPORT_SYMBOL_GPL(ai_claw_register);

/**
 * ai_claw_unregister() - Remove a kernel AI tool
 */
void ai_claw_unregister(struct ai_claw *claw)
{
	if (!claw)
		return;

	write_lock(&ai_claw_lock);
	list_del(&claw->list);
	write_unlock(&ai_claw_lock);

	atomic_dec(&ai_claw_count);
	pr_info("ai: unregistered OpenClaw '%s'\n", claw->name);
}
EXPORT_SYMBOL_GPL(ai_claw_unregister);

/**
 * ai_claw_invoke() - Execute a named claw from kernel or daemon context
 */
int ai_claw_invoke(const char *name, const char *params,
		   char *result, size_t result_len)
{
	struct ai_claw *claw;
	int ret = -ENOENT;

	read_lock(&ai_claw_lock);
	list_for_each_entry(claw, &ai_claw_list, list) {
		if (strcmp(claw->name, name) == 0) {
			if (!try_module_get(claw->owner)) {
				ret = -ENODEV;
				break;
			}
			read_unlock(&ai_claw_lock);
			ret = claw->execute(params, result, result_len);
			module_put(claw->owner);
			atomic64_inc(&ai_global_stats.total_tool_calls);
			return ret;
		}
	}
	read_unlock(&ai_claw_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(ai_claw_invoke);

/* ===== Kernel Event Broadcast ===== */

/**
 * ai_send_event() - Broadcast a system event to the AI daemon
 */
int ai_send_event(const char *event_type, const char *data, size_t len)
{
	struct ai_client *daemon;
	struct ai_queued_msg *qmsg;
	size_t json_len;
	char *json_buf;

	mutex_lock(&ai_daemon_mutex);
	daemon = ai_daemon_client;
	mutex_unlock(&ai_daemon_mutex);

	if (!daemon)
		return -ENODEV;

	/* Build JSON: {"event":"<type>","data":<data>} */
	json_len = 32 + strlen(event_type) + len;
	json_buf = kmalloc(json_len, GFP_KERNEL);
	if (!json_buf)
		return -ENOMEM;

	snprintf(json_buf, json_len, "{\"event\":\"%s\",\"data\":%s}",
		 event_type, data ? data : "null");

	qmsg = kmalloc(sizeof(*qmsg) + strlen(json_buf) + 1, GFP_KERNEL);
	if (!qmsg) {
		kfree(json_buf);
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&qmsg->list);
	qmsg->msg.magic    = AI_MAGIC;
	qmsg->msg.type     = AI_MSG_SYSTEM_EVENT;
	qmsg->msg.id       = atomic_inc_return(&ai_next_msg_id);
	qmsg->msg.flags    = AI_FLAG_DAEMON;
	qmsg->msg.uid      = 0;
	qmsg->msg.pid      = 0;
	qmsg->msg.session  = 0;
	qmsg->msg.data_len = strlen(json_buf);
	memset(qmsg->msg.reserved, 0, sizeof(qmsg->msg.reserved));
	memcpy(qmsg->msg.data, json_buf, qmsg->msg.data_len);
	kfree(json_buf);

	deliver_to_client(daemon, qmsg);
	kfree(qmsg);
	return 0;
}
EXPORT_SYMBOL_GPL(ai_send_event);

/**
 * ai_subsystem_ready() - Is the AI subsystem online with a daemon attached?
 */
bool ai_subsystem_ready(void)
{
	return ai_ready && ai_daemon_client != NULL;
}
EXPORT_SYMBOL_GPL(ai_subsystem_ready);

/* ===== Character Device File Operations ===== */

static int ai_open(struct inode *inode, struct file *filp)
{
	struct ai_client *client;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;

	INIT_LIST_HEAD(&client->list);
	INIT_LIST_HEAD(&client->rx_list);
	spin_lock_init(&client->rx_lock);
	init_waitqueue_head(&client->rx_wait);
	client->pid = current->pid;
	client->uid = from_kuid_munged(current_user_ns(), current_uid());

	/* Assign a random session ID */
	get_random_bytes(&client->session, sizeof(client->session));

	filp->private_data = client;

	spin_lock(&ai_client_lock);
	list_add_tail(&client->list, &ai_client_list);
	spin_unlock(&ai_client_lock);

	pr_debug("ai: client opened (pid=%d uid=%d session=%u)\n",
		 client->pid, client->uid, client->session);
	return 0;
}

static int ai_release(struct inode *inode, struct file *filp)
{
	struct ai_client *client = filp->private_data;
	struct ai_queued_msg *qmsg, *tmp;

	if (!client)
		return 0;

	/* If this was the daemon, clear the daemon reference */
	if (client->is_daemon) {
		mutex_lock(&ai_daemon_mutex);
		if (ai_daemon_client == client)
			ai_daemon_client = NULL;
		mutex_unlock(&ai_daemon_mutex);
		pr_info("ai: daemon disconnected\n");
	}

	spin_lock(&ai_client_lock);
	list_del(&client->list);
	spin_unlock(&ai_client_lock);

	/* Drain and free pending messages */
	spin_lock(&client->rx_lock);
	list_for_each_entry_safe(qmsg, tmp, &client->rx_list, list) {
		list_del(&qmsg->list);
		kfree(qmsg);
	}
	spin_unlock(&client->rx_lock);

	kfree(client);
	filp->private_data = NULL;
	return 0;
}

static ssize_t ai_read(struct file *filp, char __user *ubuf,
		       size_t count, loff_t *ppos)
{
	struct ai_client *client = filp->private_data;
	struct ai_queued_msg *qmsg;
	size_t msg_size;
	int ret;

	if (!client)
		return -EBADF;

	/* Block until a message is available */
	ret = wait_event_interruptible(client->rx_wait,
				       !list_empty(&client->rx_list));
	if (ret)
		return ret;

	spin_lock(&client->rx_lock);
	if (list_empty(&client->rx_list)) {
		spin_unlock(&client->rx_lock);
		return -EAGAIN;
	}
	qmsg = list_first_entry(&client->rx_list, struct ai_queued_msg, list);
	list_del(&qmsg->list);
	spin_unlock(&client->rx_lock);

	msg_size = AI_MSG_HEADER_SIZE + qmsg->msg.data_len;
	if (count < msg_size) {
		/* Re-queue: message doesn't fit in provided buffer */
		spin_lock(&client->rx_lock);
		list_add(&qmsg->list, &client->rx_list);
		spin_unlock(&client->rx_lock);
		return -EMSGSIZE;
	}

	if (copy_to_user(ubuf, &qmsg->msg, msg_size)) {
		kfree(qmsg);
		return -EFAULT;
	}

	kfree(qmsg);
	atomic_dec(&ai_global_stats.queue_depth);
	return msg_size;
}

static ssize_t ai_write(struct file *filp, const char __user *ubuf,
			size_t count, loff_t *ppos)
{
	struct ai_client *client = filp->private_data;
	struct ai_queued_msg *qmsg;
	struct ai_message hdr;
	size_t total_size;

	if (!client)
		return -EBADF;

	/* Read just the header first to validate */
	if (count < AI_MSG_HEADER_SIZE)
		return -EINVAL;

	if (copy_from_user(&hdr, ubuf, AI_MSG_HEADER_SIZE))
		return -EFAULT;

	if (hdr.magic != AI_MAGIC)
		return -EINVAL;

	if (hdr.data_len > AI_MAX_MSG_SIZE - AI_MSG_HEADER_SIZE)
		return -EMSGSIZE;

	total_size = AI_MSG_HEADER_SIZE + hdr.data_len;
	if (count < total_size)
		return -EINVAL;

	qmsg = alloc_queued_msg((const struct ai_message __user *)ubuf, total_size);
	if (!qmsg)
		return -ENOMEM;

	/* Assign a monotonic message ID if client didn't set one */
	if (qmsg->msg.id == 0)
		qmsg->msg.id = atomic_inc_return(&ai_next_msg_id);

	/* Tag with sender's session */
	qmsg->msg.session = client->session;

	if (qmsg->msg.type == AI_MSG_RESPONSE ||
	    qmsg->msg.type == AI_MSG_STREAM ||
	    qmsg->msg.type == AI_MSG_TOOL_RESULT) {
		/* Daemon sending back to clients */
		if (!client->is_daemon) {
			kfree(qmsg);
			return -EPERM;
		}
		route_response_to_client(qmsg);
		atomic64_inc(&ai_global_stats.total_responses);
	} else {
		/* Client sending query to daemon */
		struct ai_client *daemon;

		mutex_lock(&ai_daemon_mutex);
		daemon = ai_daemon_client;
		mutex_unlock(&ai_daemon_mutex);

		if (!daemon) {
			kfree(qmsg);
			return -ENODEV; /* No daemon attached */
		}

		deliver_to_client(daemon, qmsg);
		atomic64_inc(&ai_global_stats.total_queries);
		atomic_inc(&ai_global_stats.queue_depth);
	}

	kfree(qmsg);
	return total_size;
}

static __poll_t ai_poll(struct file *filp, poll_table *wait)
{
	struct ai_client *client = filp->private_data;
	__poll_t mask = 0;

	if (!client)
		return EPOLLERR;

	poll_wait(filp, &client->rx_wait, wait);

	spin_lock(&client->rx_lock);
	if (!list_empty(&client->rx_list))
		mask |= EPOLLIN | EPOLLRDNORM;
	spin_unlock(&client->rx_lock);

	/* Always writable (writes go to daemon queue, bounded separately) */
	mask |= EPOLLOUT | EPOLLWRNORM;

	return mask;
}

static long ai_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct ai_client *client = filp->private_data;
	int ret = 0;

	if (!client)
		return -EBADF;

	switch (cmd) {
	case AI_IOC_SET_DAEMON: {
		/* Only root can register as the AI daemon */
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;

		mutex_lock(&ai_daemon_mutex);
		if (ai_daemon_client && ai_daemon_client != client) {
			mutex_unlock(&ai_daemon_mutex);
			return -EBUSY; /* Another daemon is already registered */
		}
		client->is_daemon = true;
		client->flags |= AI_FLAG_DAEMON;
		ai_daemon_client = client;
		ai_global_stats.daemon_connect_time = ktime_get();
		mutex_unlock(&ai_daemon_mutex);

		pr_info("ai: daemon registered (pid=%d)\n", current->pid);
		break;
	}

	case AI_IOC_GET_STATUS: {
		struct ai_ioctl_status status = {};

		mutex_lock(&ai_daemon_mutex);
		status.state = ai_daemon_client ? AI_STATE_ONLINE : AI_STATE_OFFLINE;
		mutex_unlock(&ai_daemon_mutex);

		status.queue_depth     = atomic_read(&ai_global_stats.queue_depth);
		status.num_claws       = atomic_read(&ai_claw_count);
		status.total_queries   = atomic64_read(&ai_global_stats.total_queries);
		status.total_responses = atomic64_read(&ai_global_stats.total_responses);
		status.total_tool_calls = atomic64_read(&ai_global_stats.total_tool_calls);
		status.uptime_ms       = ktime_to_ms(
			ktime_sub(ktime_get(), ai_global_stats.daemon_connect_time));

		if (copy_to_user((void __user *)arg, &status, sizeof(status)))
			return -EFAULT;
		break;
	}

	case AI_IOC_SET_SESSION: {
		u32 session_id;

		if (get_user(session_id, (__u32 __user *)arg))
			return -EFAULT;
		client->session = session_id;
		break;
	}

	case AI_IOC_FLUSH: {
		struct ai_queued_msg *qmsg, *tmp;

		spin_lock(&client->rx_lock);
		list_for_each_entry_safe(qmsg, tmp, &client->rx_list, list) {
			list_del(&qmsg->list);
			kfree(qmsg);
		}
		spin_unlock(&client->rx_lock);
		break;
	}

	case AI_IOC_LIST_CLAWS: {
		struct ai_ioctl_claw_list req;
		struct ai_claw_info __user *uinfo;
		struct ai_claw *claw;
		u32 count = 0, max_count;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;

		max_count = req.count;
		uinfo = (struct ai_claw_info __user *)(arg + sizeof(req));

		read_lock(&ai_claw_lock);
		list_for_each_entry(claw, &ai_claw_list, list) {
			if (count < max_count) {
				struct ai_claw_info info = {};

				strscpy(info.name, claw->name, sizeof(info.name));
				if (claw->description)
					strscpy(info.description, claw->description,
						sizeof(info.description));
				if (claw->schema)
					strscpy(info.schema, claw->schema,
						sizeof(info.schema));
				info.flags = claw->flags | AI_CLAW_FLAG_KERNEL;

				if (copy_to_user(&uinfo[count], &info, sizeof(info))) {
					read_unlock(&ai_claw_lock);
					return -EFAULT;
				}
			}
			count++;
		}
		read_unlock(&ai_claw_lock);

		req.count = count;
		if (copy_to_user((void __user *)arg, &req, sizeof(req)))
			return -EFAULT;
		break;
	}

	default:
		ret = -ENOTTY;
	}

	return ret;
}

static const struct file_operations ai_fops = {
	.owner          = THIS_MODULE,
	.open           = ai_open,
	.release        = ai_release,
	.read           = ai_read,
	.write          = ai_write,
	.poll           = ai_poll,
	.unlocked_ioctl = ai_ioctl,
	.llseek         = no_llseek,
};

/* ===== Module Init / Exit ===== */

static int __init ai_core_init(void)
{
	int ret;

	pr_info("ai: Linux AI subsystem initializing\n");

	/* Allocate device number */
	ret = alloc_chrdev_region(&ai_devno, AI_MINOR_BASE, AI_MINOR_COUNT,
				  AI_DEVICE_NAME);
	if (ret) {
		pr_err("ai: failed to allocate chrdev region: %d\n", ret);
		return ret;
	}

	/* Initialize cdev */
	cdev_init(&ai_cdev, &ai_fops);
	ai_cdev.owner = THIS_MODULE;

	ret = cdev_add(&ai_cdev, ai_devno, AI_MINOR_COUNT);
	if (ret) {
		pr_err("ai: failed to add cdev: %d\n", ret);
		goto err_unreg_chrdev;
	}

	/* Create device class */
	ai_class = class_create(AI_CLASS_NAME);
	if (IS_ERR(ai_class)) {
		ret = PTR_ERR(ai_class);
		pr_err("ai: failed to create class: %d\n", ret);
		goto err_del_cdev;
	}

	/* Create /dev/ai */
	ai_device = device_create(ai_class, NULL, ai_devno, NULL, AI_DEVICE_NAME);
	if (IS_ERR(ai_device)) {
		ret = PTR_ERR(ai_device);
		pr_err("ai: failed to create device: %d\n", ret);
		goto err_destroy_class;
	}

	/* Initialize global stats */
	memset(&ai_global_stats, 0, sizeof(ai_global_stats));

	ai_ready = true;
	pr_info("ai: /dev/ai created (major=%d minor=%d)\n",
		MAJOR(ai_devno), MINOR(ai_devno));
	pr_info("ai: OpenClaw registry ready\n");
	pr_info("ai: AI subsystem online - awaiting aicore daemon\n");
	return 0;

err_destroy_class:
	class_destroy(ai_class);
err_del_cdev:
	cdev_del(&ai_cdev);
err_unreg_chrdev:
	unregister_chrdev_region(ai_devno, AI_MINOR_COUNT);
	return ret;
}

static void __exit ai_core_exit(void)
{
	ai_ready = false;

	device_destroy(ai_class, ai_devno);
	class_destroy(ai_class);
	cdev_del(&ai_cdev);
	unregister_chrdev_region(ai_devno, AI_MINOR_COUNT);

	pr_info("ai: subsystem unloaded\n");
}

subsys_initcall(ai_core_init);
module_exit(ai_core_exit);

MODULE_AUTHOR("Linux AI Project");
MODULE_DESCRIPTION("Linux AI Subsystem Core - OpenClaw native AI integration");
MODULE_LICENSE("GPL v2");
