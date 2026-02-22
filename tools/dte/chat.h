/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tools/dte/chat.h - DTE chat protocol handler
 *
 * Manages the connection to aicore daemon via /run/aicore.sock,
 * handles streaming responses, and drives the AI message protocol.
 */
#ifndef DTE_CHAT_H
#define DTE_CHAT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "../../include/uapi/linux/ai.h"

#define CHAT_SOCKET_PATH	"/run/aicore.sock"
#define CHAT_RECV_BUF		(512 * 1024)

/* Chat event types (from daemon to DTE) */
typedef enum {
	CHAT_EVENT_STREAM_TOKEN,	/* Streaming AI response chunk */
	CHAT_EVENT_RESPONSE_DONE,	/* Final complete response */
	CHAT_EVENT_TOOL_CALL,		/* AI invoked a tool (display info) */
	CHAT_EVENT_TOOL_RESULT,		/* Tool returned a result */
	CHAT_EVENT_ERROR,		/* Error from daemon */
	CHAT_EVENT_CONNECTED,		/* Connected to daemon */
	CHAT_EVENT_DISCONNECTED,	/* Lost connection to daemon */
	CHAT_EVENT_SYSTEM,		/* System event from kernel */
} chat_event_type_t;

struct chat_event {
	chat_event_type_t type;
	uint32_t          msg_id;
	uint32_t          session;
	char              text[256 * 1024];	/* Response/token text */
};

/* Callback called from reader thread with each event */
typedef void (*chat_event_cb_t)(const struct chat_event *evt, void *userdata);

/**
 * struct chat_state - DTE chat connection state
 */
struct chat_state {
	int		fd;		/* Socket fd to aicore daemon */
	uint32_t	session;	/* Our session ID */
	uint32_t	next_msg_id;	/* Monotonic message counter */
	bool		connected;

	/* Reader thread */
	pthread_t	reader_thread;
	volatile bool	reader_running;

	/* Event callback */
	chat_event_cb_t	on_event;
	void		*on_event_userdata;

	/* Current response accumulation */
	char		response_buf[256 * 1024];
	size_t		response_len;
	uint32_t	pending_msg_id;

	/* Mutex for fd writes */
	pthread_mutex_t	write_lock;
};

/* Connect to aicore daemon. Retries if not yet started. */
int  chat_connect(struct chat_state *cs, const char *socket_path,
		  chat_event_cb_t cb, void *userdata);
void chat_disconnect(struct chat_state *cs);

/* Send a query to the AI */
int  chat_send_query(struct chat_state *cs, const char *text, bool stream);

/* Send a ping to check connection */
int  chat_ping(struct chat_state *cs);

/* Check if currently waiting for a response */
bool chat_is_busy(struct chat_state *cs);

#endif /* DTE_CHAT_H */
