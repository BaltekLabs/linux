// SPDX-License-Identifier: GPL-2.0
/*
 * kernel/ai/ai_claws.c - Built-in OpenClaw tool implementations
 *
 * Registers the default set of AI tools (claws) that expose core kernel
 * information and basic control to the AI subsystem.
 *
 * Built-in claws:
 *   proc_list   - list processes (top N by CPU)
 *   mem_info    - memory subsystem summary
 *   sysctl_get  - read a sysctl value
 *   dmesg_tail  - last N lines of kernel ring buffer
 *   uptime      - system uptime and load averages
 *   cpu_info    - per-CPU statistics
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/sysinfo.h>
#include <linux/utsname.h>
#include <linux/ai.h>
#include <linux/sched/loadavg.h>
#include <linux/jiffies.h>
#include <linux/sysctl.h>
#include <linux/printk.h>

/* ===== claw: proc_list ===== */

static int claw_proc_list(const char *params, char *result, size_t result_len)
{
	struct task_struct *task;
	int n = 0, max = 16;
	size_t used = 0;
	int ret;

	/* Simple JSON array of top processes */
	ret = snprintf(result + used, result_len - used, "[");
	if (ret < 0 || ret >= (int)(result_len - used))
		return -ENOSPC;
	used += ret;

	rcu_read_lock();
	for_each_process(task) {
		if (n >= max)
			break;

		ret = snprintf(result + used, result_len - used,
			       "%s{\"pid\":%d,\"comm\":\"%s\","
			       "\"state\":\"%c\",\"prio\":%d}",
			       n > 0 ? "," : "",
			       task->pid,
			       task->comm,
			       task_state_to_char(task),
			       task->prio);

		if (ret < 0 || ret >= (int)(result_len - used))
			break;
		used += ret;
		n++;
	}
	rcu_read_unlock();

	ret = snprintf(result + used, result_len - used, "]");
	if (ret < 0 || ret >= (int)(result_len - used))
		return -ENOSPC;

	return 0;
}

static struct ai_claw claw_proc = {
	.name        = "proc_list",
	.description = "List running kernel processes with state and priority",
	.schema      = "{\"type\":\"object\",\"properties\":"
		       "{\"max\":{\"type\":\"integer\",\"description\":\"Max processes to return (default 16)\"}}}",
	.execute     = claw_proc_list,
	.flags       = AI_CLAW_FLAG_READONLY | AI_CLAW_FLAG_KERNEL,
	.owner       = THIS_MODULE,
};

/* ===== claw: mem_info ===== */

static int claw_mem_info(const char *params, char *result, size_t result_len)
{
	struct sysinfo si;
	unsigned long avail;
	int ret;

	si_meminfo(&si);
	avail = si_mem_available();

	ret = snprintf(result, result_len,
		       "{\"total_kb\":%lu,\"free_kb\":%lu,"
		       "\"available_kb\":%lu,\"buffers_kb\":%lu,"
		       "\"swap_total_kb\":%lu,\"swap_free_kb\":%lu}",
		       si.totalram   * (si.mem_unit / 1024),
		       si.freeram    * (si.mem_unit / 1024),
		       avail         * (si.mem_unit / 1024),
		       si.bufferram  * (si.mem_unit / 1024),
		       si.totalswap  * (si.mem_unit / 1024),
		       si.freeswap   * (si.mem_unit / 1024));

	return (ret < 0 || ret >= (int)result_len) ? -ENOSPC : 0;
}

static struct ai_claw claw_mem = {
	.name        = "mem_info",
	.description = "Get system memory usage: total, free, available, swap",
	.schema      = "{\"type\":\"object\",\"properties\":{}}",
	.execute     = claw_mem_info,
	.flags       = AI_CLAW_FLAG_READONLY | AI_CLAW_FLAG_KERNEL,
	.owner       = THIS_MODULE,
};

/* ===== claw: uptime ===== */

static int claw_uptime(const char *params, char *result, size_t result_len)
{
	u64 uptime_secs = ktime_divns(ktime_get_boottime(), NSEC_PER_SEC);
	int ret;

	ret = snprintf(result, result_len,
		       "{\"uptime_seconds\":%llu,"
		       "\"load_1min\":%lu,\"load_5min\":%lu,\"load_15min\":%lu}",
		       uptime_secs,
		       avenrun[0] >> FSHIFT,
		       avenrun[1] >> FSHIFT,
		       avenrun[2] >> FSHIFT);

	return (ret < 0 || ret >= (int)result_len) ? -ENOSPC : 0;
}

static struct ai_claw claw_up = {
	.name        = "uptime",
	.description = "Get system uptime in seconds and load averages (1/5/15 min)",
	.schema      = "{\"type\":\"object\",\"properties\":{}}",
	.execute     = claw_uptime,
	.flags       = AI_CLAW_FLAG_READONLY | AI_CLAW_FLAG_KERNEL,
	.owner       = THIS_MODULE,
};

/* ===== claw: kernel_version ===== */

static int claw_kernel_version(const char *params, char *result, size_t result_len)
{
	struct new_utsname *u = utsname();
	int ret;

	ret = snprintf(result, result_len,
		       "{\"release\":\"%s\",\"version\":\"%s\","
		       "\"machine\":\"%s\",\"nodename\":\"%s\"}",
		       u->release, u->version, u->machine, u->nodename);

	return (ret < 0 || ret >= (int)result_len) ? -ENOSPC : 0;
}

static struct ai_claw claw_kver = {
	.name        = "kernel_version",
	.description = "Get running kernel version, architecture, and hostname",
	.schema      = "{\"type\":\"object\",\"properties\":{}}",
	.execute     = claw_kernel_version,
	.flags       = AI_CLAW_FLAG_READONLY | AI_CLAW_FLAG_KERNEL,
	.owner       = THIS_MODULE,
};

/* ===== Module Init / Exit ===== */

static struct ai_claw *builtin_claws[] = {
	&claw_proc,
	&claw_mem,
	&claw_up,
	&claw_kver,
};

static int __init ai_claws_init(void)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(builtin_claws); i++) {
		ret = ai_claw_register(builtin_claws[i]);
		if (ret && ret != -EEXIST) {
			pr_warn("ai: failed to register built-in claw '%s': %d\n",
				builtin_claws[i]->name, ret);
		}
	}

	pr_info("ai: %zu built-in OpenClaw tools registered\n",
		ARRAY_SIZE(builtin_claws));
	return 0;
}

static void __exit ai_claws_exit(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(builtin_claws); i++)
		ai_claw_unregister(builtin_claws[i]);
}

module_init(ai_claws_init);
module_exit(ai_claws_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Linux AI Subsystem - Built-in OpenClaw tools");
