/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tools/aicore/tools.h - OpenClaw userspace tool definitions
 *
 * The AI daemon implements these tools in userspace and executes them
 * when the LLM emits tool_use blocks. Each tool has full root access.
 *
 * Tool registration follows the same naming convention as kernel claws,
 * allowing the daemon to seamlessly merge kernel and userspace tools
 * into a unified tool schema for the LLM.
 */
#ifndef AICORE_TOOLS_H
#define AICORE_TOOLS_H

#include <stddef.h>
#include "config.h"

/* Max result buffer size for tool output */
#define TOOL_RESULT_MAX		(128 * 1024)	/* 128 KB */
#define TOOL_NAME_MAX		64
#define TOOL_DESC_MAX		256
#define TOOL_SCHEMA_MAX		2048

/**
 * struct ai_tool - Userspace OpenClaw tool descriptor
 * @name:        Tool name (must be unique, matches LLM function schema)
 * @description: One-line description for LLM context window
 * @schema:      JSON Schema for input parameters
 * @execute:     Function pointer; @params is JSON, result written to @result
 * @requires_sudo: Tool requires root (always true for aicore, noted for schema)
 * @enabled:     Runtime enable/disable (from config)
 */
struct ai_tool {
	const char *name;
	const char *description;
	const char *schema;
	int (*execute)(const char *params, char *result, size_t result_len,
		       const struct aicore_config *cfg);
	int requires_sudo;
	int enabled;
};

/* Initialize userspace tool registry with config */
int tools_init(const struct aicore_config *cfg);

/* Execute a named tool, returns JSON result string */
int tools_execute(const char *name, const char *params,
		  char *result, size_t result_len,
		  const struct aicore_config *cfg);

/* Build combined JSON tool schema array for LLM API requests */
int tools_build_schema(char *out, size_t out_len,
		       const struct aicore_config *cfg);

/* Get count of available (enabled) tools */
int tools_count(void);

/* Append kernel-registered claws (from /dev/ai via AI_IOC_LIST_CLAWS) */
int tools_load_kernel_claws(int ai_fd);

#endif /* AICORE_TOOLS_H */
