// SPDX-License-Identifier: GPL-2.0
/*
 * tools/dte/chat.c - DTE AI chat protocol handler
 *
 * Manages the Unix socket connection to the aicore daemon, serializes
 * ai_message structs, and parses streaming/complete responses.
 * A background reader thread handles incoming messages asynchronously,
 * calling the event callback for each received event.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>

#include "chat.h"

static atomic_int g_busy = 0;

/* ===== Reader thread ===== */

static void *reader_thread(void *arg)
{
	struct chat_state *cs = arg;
	char *buf = malloc(CHAT_RECV_BUF + sizeof(struct ai_message) + 1);
	size_t used = 0;
	struct chat_event evt;

	if (!buf) {
		cs->reader_running = false;
		return NULL;
	}

	while (cs->reader_running) {
		ssize_t n = read(cs->fd, buf + used,
				 CHAT_RECV_BUF + sizeof(struct ai_message) - used);

		if (n <= 0) {
			if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
				/* Disconnected */
				memset(&evt, 0, sizeof(evt));
				evt.type = CHAT_EVENT_DISCONNECTED;
				if (cs->on_event)
					cs->on_event(&evt, cs->on_event_userdata);
				break;
			}
			/* EAGAIN: nothing to read, brief sleep */
			struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 };
			nanosleep(&ts, NULL);
			continue;
		}

		used += n;

		/* Parse complete ai_message packets */
		while (used >= sizeof(struct ai_message)) {
			struct ai_message *msg = (struct ai_message *)buf;
			size_t total;

			if (msg->magic != AI_MAGIC) {
				used = 0; /* Desync - flush */
				break;
			}

			total = sizeof(struct ai_message) + msg->data_len;
			if (used < total)
				break; /* Incomplete packet */

			/* Null-terminate the data */
			msg->data[msg->data_len] = '\0';

			memset(&evt, 0, sizeof(evt));
			evt.msg_id  = msg->id;
			evt.session = msg->session;

			switch (msg->type) {
			case AI_MSG_STREAM:
				evt.type = CHAT_EVENT_STREAM_TOKEN;
				/* Accumulate into response buffer */
				if (cs->response_len + msg->data_len <
				    sizeof(cs->response_buf) - 1) {
					memcpy(cs->response_buf + cs->response_len,
					       msg->data, msg->data_len);
					cs->response_len += msg->data_len;
					cs->response_buf[cs->response_len] = '\0';
				}
				strncpy(evt.text, msg->data,
					sizeof(evt.text) - 1);
				if (cs->on_event)
					cs->on_event(&evt, cs->on_event_userdata);
				break;

			case AI_MSG_RESPONSE:
				/* Check for final stream marker */
				if (cs->response_len > 0) {
					/* Already accumulated via streaming */
					strncpy(evt.text, cs->response_buf,
						sizeof(evt.text) - 1);
				} else {
					strncpy(evt.text, msg->data,
						sizeof(evt.text) - 1);
				}
				evt.type = CHAT_EVENT_RESPONSE_DONE;
				cs->response_len    = 0;
				cs->response_buf[0] = '\0';
				atomic_store(&g_busy, 0);
				if (cs->on_event)
					cs->on_event(&evt, cs->on_event_userdata);
				break;

			case AI_MSG_ERROR:
				evt.type = CHAT_EVENT_ERROR;
				strncpy(evt.text, msg->data, sizeof(evt.text) - 1);
				atomic_store(&g_busy, 0);
				if (cs->on_event)
					cs->on_event(&evt, cs->on_event_userdata);
				break;

			case AI_MSG_SYSTEM_EVENT:
				evt.type = CHAT_EVENT_SYSTEM;
				strncpy(evt.text, msg->data, sizeof(evt.text) - 1);
				if (cs->on_event)
					cs->on_event(&evt, cs->on_event_userdata);
				break;

			case AI_MSG_PONG:
				/* Ping response - ignore */
				break;

			default:
				break;
			}

			/* Shift buffer */
			used -= total;
			if (used > 0)
				memmove(buf, buf + total, used);
		}
	}

	free(buf);
	cs->reader_running = false;
	return NULL;
}

/* ===== Public API ===== */

int chat_connect(struct chat_state *cs, const char *socket_path,
		 chat_event_cb_t cb, void *userdata)
{
	struct sockaddr_un addr;
	int fd, flags;
	struct chat_event evt;

	memset(cs, 0, sizeof(*cs));
	cs->fd              = -1;
	cs->on_event        = cb;
	cs->on_event_userdata = userdata;
	cs->next_msg_id     = 1;

	pthread_mutex_init(&cs->write_lock, NULL);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -errno;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

	/* Retry connect for a few seconds (daemon may be starting) */
	int retries = 10;
	while (retries-- > 0) {
		if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
			goto connected;
		if (errno != ENOENT && errno != ECONNREFUSED) {
			close(fd);
			return -errno;
		}
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 500000000 };
		nanosleep(&ts, NULL);
	}
	close(fd);
	return -ECONNREFUSED;

connected:
	/* Set non-blocking for reader */
	flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	cs->fd        = fd;
	cs->connected = true;

	/* Generate session ID */
	cs->session = (uint32_t)((uintptr_t)cs ^ (uint32_t)time(NULL));

	/* Start reader thread */
	cs->reader_running = true;
	if (pthread_create(&cs->reader_thread, NULL, reader_thread, cs) != 0) {
		close(fd);
		cs->fd = -1;
		return -errno;
	}

	/* Notify connected */
	memset(&evt, 0, sizeof(evt));
	evt.type = CHAT_EVENT_CONNECTED;
	if (cb)
		cb(&evt, userdata);

	return 0;
}

void chat_disconnect(struct chat_state *cs)
{
	if (!cs->connected)
		return;

	cs->reader_running = false;
	cs->connected      = false;

	if (cs->fd >= 0) {
		shutdown(cs->fd, SHUT_RDWR);
		close(cs->fd);
		cs->fd = -1;
	}

	pthread_join(cs->reader_thread, NULL);
	pthread_mutex_destroy(&cs->write_lock);
}

int chat_send_query(struct chat_state *cs, const char *text, bool stream)
{
	if (!cs->connected || cs->fd < 0)
		return -ENOTCONN;

	size_t text_len = strlen(text);
	if (text_len == 0)
		return -EINVAL;

	size_t total = sizeof(struct ai_message) + text_len;
	struct ai_message *msg = malloc(total + 1);
	if (!msg)
		return -ENOMEM;

	msg->magic    = AI_MAGIC;
	msg->type     = AI_MSG_QUERY;
	msg->id       = cs->next_msg_id++;
	msg->flags    = stream ? AI_FLAG_STREAM : 0;
	msg->uid      = (uint32_t)getuid();
	msg->pid      = (uint32_t)getpid();
	msg->session  = cs->session;
	msg->data_len = (uint32_t)text_len;
	memset(msg->reserved, 0, sizeof(msg->reserved));
	memcpy(msg->data, text, text_len);

	cs->pending_msg_id = msg->id;
	atomic_store(&g_busy, 1);

	pthread_mutex_lock(&cs->write_lock);
	ssize_t n = write(cs->fd, msg, total);
	pthread_mutex_unlock(&cs->write_lock);

	free(msg);

	if (n < 0) {
		atomic_store(&g_busy, 0);
		return -errno;
	}

	return 0;
}

int chat_ping(struct chat_state *cs)
{
	if (!cs->connected || cs->fd < 0)
		return -ENOTCONN;

	struct ai_message msg = {0};
	msg.magic    = AI_MAGIC;
	msg.type     = AI_MSG_PING;
	msg.id       = cs->next_msg_id++;
	msg.session  = cs->session;
	msg.data_len = 0;

	pthread_mutex_lock(&cs->write_lock);
	ssize_t n = write(cs->fd, &msg, sizeof(msg));
	pthread_mutex_unlock(&cs->write_lock);

	return (n < 0) ? -errno : 0;
}

bool chat_is_busy(struct chat_state *cs)
{
	(void)cs;
	return atomic_load(&g_busy) != 0;
}
