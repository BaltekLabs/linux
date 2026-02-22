/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Linux AI Subsystem - Userspace API
 *
 * Native AI integration for Linux kernel: message protocol, ioctl interface,
 * and OpenClaw tool schema definitions for kernel<->userspace AI communication.
 *
 * Architecture:
 *   [userspace processes / DTE] <--/dev/ai--> [aicore daemon] <--HTTP--> [LLM API]
 *                                      |
 *                               [kernel/ai subsystem]
 *                                      |
 *                              [OpenClaw registry]
 *                           (proc, net, fs, sched claws)
 */
#ifndef _UAPI_LINUX_AI_H
#define _UAPI_LINUX_AI_H

#include <linux/types.h>
#include <linux/ioctl.h>

/* ===== Message Types ===== */
#define AI_MSG_QUERY		0x0001	/* User -> AI: natural language query */
#define AI_MSG_RESPONSE		0x0002	/* AI -> User: final response */
#define AI_MSG_STREAM		0x0003	/* AI -> User: streaming token chunk */
#define AI_MSG_TOOL_CALL	0x0004	/* AI -> Kernel: invoke a claw (tool) */
#define AI_MSG_TOOL_RESULT	0x0005	/* Kernel -> AI: claw execution result */
#define AI_MSG_SYSTEM_EVENT	0x0006	/* Kernel -> AI: async system event */
#define AI_MSG_ERROR		0x0007	/* Any direction: error report */
#define AI_MSG_PING		0x0008	/* Keepalive */
#define AI_MSG_PONG		0x0009	/* Keepalive reply */
#define AI_MSG_REGISTER_DAEMON	0x000A	/* Daemon registers itself with kernel */
#define AI_MSG_SHUTDOWN		0x000B	/* Graceful shutdown */

/* ===== Message Flags ===== */
#define AI_FLAG_STREAM		0x0001	/* Request streaming response */
#define AI_FLAG_SUDO		0x0002	/* Elevated privilege context */
#define AI_FLAG_ASYNC		0x0004	/* Do not block for response */
#define AI_FLAG_COMPLETE	0x0008	/* Last message in a stream */
#define AI_FLAG_SYSTEM_CTX	0x0010	/* Include full system context */
#define AI_FLAG_NO_TOOLS	0x0020	/* Disable tool/claw usage */
#define AI_FLAG_DAEMON		0x0040	/* Sender is the aicore daemon */

/* ===== Size Limits ===== */
#define AI_MAX_MSG_SIZE		(512 * 1024)	/* 512 KB per message */
#define AI_MAX_QUERY_LEN	(64 * 1024)	/* 64 KB query text */
#define AI_MAX_TOOL_NAME	64		/* Max claw/tool name length */
#define AI_MAX_QUEUE_DEPTH	256		/* Max pending messages */
#define AI_MAGIC		0x41490000	/* 'AI\0\0' - message magic */

/**
 * struct ai_message - Core message structure for AI<->kernel communication
 *
 * All communication through /dev/ai uses this framing. Payload is always
 * JSON-encoded data, enabling extensibility without ABI changes.
 *
 * @magic:    Must be AI_MAGIC (0x41490000), validates message integrity
 * @type:     AI_MSG_* constant
 * @id:       32-bit correlation ID; responses carry the same ID as queries
 * @flags:    AI_FLAG_* bitmask
 * @uid:      Effective UID of sending process (set by kernel on write)
 * @pid:      PID of sending process (set by kernel on write)
 * @session:  Session ID for multi-turn conversation continuity
 * @data_len: Length of the @data payload in bytes
 * @reserved: Must be zero
 * @data:     JSON payload (query text, tool args, response, etc.)
 */
struct ai_message {
	__u32 magic;
	__u32 type;
	__u32 id;
	__u32 flags;
	__u32 uid;
	__u32 pid;
	__u32 session;
	__u32 data_len;
	__u32 reserved[4];
	char  data[];
};

#define AI_MSG_HEADER_SIZE	(sizeof(struct ai_message))

/**
 * struct ai_claw_info - Describes a registered OpenClaw tool to userspace
 *
 * The AI daemon queries this via AI_IOC_LIST_CLAWS to populate the LLM's
 * tool schema, enabling the AI to call kernel-registered capabilities.
 *
 * @name:        Tool name (e.g. "proc_list", "sysctl_set")
 * @description: Human-readable description for LLM function-calling schema
 * @schema:      JSON Schema string describing input parameters
 * @flags:       Claw capability flags
 */
struct ai_claw_info {
	char name[AI_MAX_TOOL_NAME];
	char description[256];
	char schema[1024];
	__u32 flags;
#define AI_CLAW_FLAG_SUDO_REQ	0x01	/* Requires elevated privileges */
#define AI_CLAW_FLAG_READONLY	0x02	/* Read-only, no side effects */
#define AI_CLAW_FLAG_KERNEL	0x04	/* Implemented in kernel space */
	__u32 reserved;
};

/* ===== ioctl Interface ===== */
#define AI_IOC_MAGIC		'A'

/* Set daemon configuration */
#define AI_IOC_SET_CONFIG	_IOW(AI_IOC_MAGIC, 1, struct ai_ioctl_config)
/* Get subsystem status */
#define AI_IOC_GET_STATUS	_IOR(AI_IOC_MAGIC, 2, struct ai_ioctl_status)
/* Flush message queue */
#define AI_IOC_FLUSH		_IO(AI_IOC_MAGIC,  3)
/* Mark this fd as the daemon fd (privileged) */
#define AI_IOC_SET_DAEMON	_IO(AI_IOC_MAGIC,  4)
/* List registered claws; arg = (struct ai_claw_info *) array */
#define AI_IOC_LIST_CLAWS	_IOR(AI_IOC_MAGIC, 5, struct ai_ioctl_claw_list)
/* Get/set session ID for this fd */
#define AI_IOC_SET_SESSION	_IOW(AI_IOC_MAGIC, 6, __u32)

struct ai_ioctl_config {
	__u32 flags;
	__u32 max_queue_depth;
	__u32 timeout_ms;
	__u32 max_msg_size;
};

struct ai_ioctl_status {
	__u32 state;
#define AI_STATE_OFFLINE	0	/* No daemon connected */
#define AI_STATE_ONLINE		1	/* Daemon connected and ready */
#define AI_STATE_BUSY		2	/* Processing a request */
#define AI_STATE_ERROR		3	/* Error state */
	__u32 queue_depth;
	__u32 num_claws;
	__u32 reserved;
	__u64 total_queries;
	__u64 total_responses;
	__u64 total_tool_calls;
	__u64 uptime_ms;
};

struct ai_ioctl_claw_list {
	__u32 count;		/* In: max claws to return. Out: actual count */
	__u32 reserved;
	/* Followed by count * struct ai_claw_info */
};

#endif /* _UAPI_LINUX_AI_H */
