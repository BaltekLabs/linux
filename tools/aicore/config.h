/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tools/aicore/config.h - aicore daemon configuration
 *
 * Supports both remote LLM APIs (Anthropic Claude, OpenAI-compatible)
 * and local LLMs via Ollama or llama.cpp HTTP servers.
 */
#ifndef AICORE_CONFIG_H
#define AICORE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/* Default config path */
#define AICORE_CONFIG_DEFAULT	"/etc/aicore/config.json"
#define AICORE_SOCKET_PATH	"/run/aicore.sock"
#define AICORE_DEV_AI		"/dev/ai"
#define AICORE_LOG_PATH		"/var/log/aicore.log"

/* LLM backend types */
typedef enum {
	LLM_BACKEND_ANTHROPIC = 0,	/* Anthropic Claude API */
	LLM_BACKEND_OPENAI,		/* OpenAI or compatible (local/remote) */
	LLM_BACKEND_OLLAMA,		/* Ollama local inference */
	LLM_BACKEND_LLAMACPP,		/* llama.cpp HTTP server */
} llm_backend_t;

/* Tool execution modes */
typedef enum {
	TOOL_MODE_CONFIRM = 0,	/* Ask for confirmation (interactive) */
	TOOL_MODE_AUTO,		/* Execute automatically (developer mode) */
	TOOL_MODE_READONLY,	/* Only allow read-only tools */
} tool_mode_t;

/**
 * struct aicore_config - Runtime configuration for the aicore daemon
 */
struct aicore_config {
	/* LLM backend */
	llm_backend_t	backend;
	char		api_key[512];		/* API key (Anthropic/OpenAI) */
	char		api_url[512];		/* Base URL for API */
	char		model[128];		/* Model name/ID */
	int		max_tokens;		/* Max tokens per response */
	float		temperature;		/* Sampling temperature */
	int		timeout_secs;		/* HTTP request timeout */

	/* System prompt / persona */
	char		system_prompt[4096];

	/* Tool / claw settings */
	tool_mode_t	tool_mode;
	bool		enable_shell;		/* Allow shell_exec claw */
	bool		enable_file_write;	/* Allow file write claw */
	bool		enable_sysctl_write;	/* Allow sysctl_set claw */
	bool		enable_service_ctl;	/* Allow service control */

	/* Daemon settings */
	char		socket_path[256];
	char		dev_ai_path[64];
	char		log_path[256];
	int		log_level;		/* 0=error 1=warn 2=info 3=debug */
	bool		daemonize;

	/* Context enrichment */
	bool		include_sysinfo;	/* Prepend /proc/ai/sysinfo to queries */
	bool		include_resources;	/* Prepend /proc/ai/resources */
	int		history_depth;		/* Conversation turns to keep */
};

/* Load config from JSON file. Falls back to defaults if file missing. */
int config_load(struct aicore_config *cfg, const char *path);

/* Apply environment variable overrides (AICORE_API_KEY, AICORE_MODEL, etc.) */
void config_apply_env(struct aicore_config *cfg);

/* Print config summary to log */
void config_dump(const struct aicore_config *cfg);

/* Fill in defaults for any unset fields */
void config_defaults(struct aicore_config *cfg);

#endif /* AICORE_CONFIG_H */
