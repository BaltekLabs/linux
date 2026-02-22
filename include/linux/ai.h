/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Linux AI Subsystem - Kernel-internal API
 *
 * Provides the OpenClaw registration interface for kernel subsystems to
 * expose capabilities to the AI daemon, and the kernel-side messaging API.
 */
#ifndef _LINUX_AI_H
#define _LINUX_AI_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <uapi/linux/ai.h>

/* ===== OpenClaw Kernel Registration API ===== */

/**
 * struct ai_claw - Kernel-registered AI tool (OpenClaw entry)
 *
 * Kernel subsystems register claws to expose their functionality to the AI.
 * The AI daemon queries registered claws to build its tool schema, then calls
 * them by name when the LLM emits a tool_use block.
 *
 * Example registration (from a hypothetical process-management claw):
 *   static struct ai_claw proc_claw = {
 *       .name        = "proc_list",
 *       .description = "List running processes with CPU/memory usage",
 *       .schema      = "{\"type\":\"object\",\"properties\":{\"sort\":{\"type\":\"string\"}}}",
 *       .execute     = proc_claw_execute,
 *       .flags       = AI_CLAW_FLAG_READONLY,
 *       .owner       = THIS_MODULE,
 *   };
 *   ai_claw_register(&proc_claw);
 *
 * @name:        Unique tool name (matches AI function-calling schema name)
 * @description: One-line description for LLM context
 * @schema:      JSON Schema for input parameters (passed verbatim to LLM)
 * @execute:     Called when AI invokes this tool. @params is JSON, result
 *               written to @result (NUL-terminated, max @result_len bytes).
 *               Returns 0 on success, negative errno on error.
 * @flags:       AI_CLAW_FLAG_* bitmask
 * @owner:       Module owning this claw (for refcounting)
 * @list:        Linked list node (internal use)
 */
struct ai_claw {
	const char	*name;
	const char	*description;
	const char	*schema;
	int		(*execute)(const char *params, char *result, size_t result_len);
	u32		 flags;
	struct module	*owner;

	/* Internal use - do not set */
	struct list_head list;
};

/**
 * ai_claw_register() - Register an AI tool with the kernel claw registry
 * @claw: Fully populated ai_claw structure. Must remain valid until unregister.
 *
 * Returns 0 on success, -EEXIST if name already registered, -EINVAL if bad.
 */
int ai_claw_register(struct ai_claw *claw);

/**
 * ai_claw_unregister() - Remove a previously registered claw
 * @claw: Same pointer passed to ai_claw_register()
 */
void ai_claw_unregister(struct ai_claw *claw);

/**
 * ai_claw_invoke() - Invoke a named claw from kernel context
 * @name:       Claw name to call
 * @params:     JSON parameter string
 * @result:     Output buffer
 * @result_len: Size of @result buffer
 *
 * Returns 0 on success, negative errno on failure.
 */
int ai_claw_invoke(const char *name, const char *params,
		   char *result, size_t result_len);

/* ===== Kernel -> Daemon Messaging API ===== */

/**
 * ai_send_event() - Send a system event notification to the AI daemon
 * @event_type: Short event type string (e.g. "oom_kill", "panic_imminent")
 * @data:       JSON data payload
 * @len:        Length of @data
 *
 * Non-blocking; drops message if queue is full. Safe to call from any context.
 * Returns 0 on success, -ENOMEM if queue full, -ENODEV if no daemon attached.
 */
int ai_send_event(const char *event_type, const char *data, size_t len);

/**
 * ai_subsystem_ready() - Check if AI subsystem is online with daemon attached
 */
bool ai_subsystem_ready(void);

/* ===== AI Subsystem State (exported for /proc and sysfs) ===== */

/**
 * struct ai_stats - Runtime statistics for the AI subsystem
 */
struct ai_stats {
	atomic64_t total_queries;
	atomic64_t total_responses;
	atomic64_t total_tool_calls;
	atomic64_t total_errors;
	atomic_t   queue_depth;
	ktime_t    daemon_connect_time;
};

extern struct ai_stats ai_global_stats;

#endif /* _LINUX_AI_H */
