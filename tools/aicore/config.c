// SPDX-License-Identifier: GPL-2.0
/*
 * tools/aicore/config.c - aicore daemon configuration
 *
 * Minimal JSON config parser (no external JSON library dependency).
 * Config format: see /etc/aicore/config.json.example
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "config.h"

/* ===== Minimal JSON key-value extractor ===== */

/**
 * json_get_string() - Extract a string value from a flat JSON object
 *
 * Very simple: finds "key":"value" or "key": "value" patterns.
 * Not a full parser - only handles flat JSON with string/int/bool values.
 */
static int json_get_string(const char *json, const char *key,
			   char *out, size_t out_len)
{
	char needle[128];
	const char *p;
	const char *start, *end;
	size_t len;

	snprintf(needle, sizeof(needle), "\"%s\"", key);
	p = strstr(json, needle);
	if (!p)
		return -1;

	p += strlen(needle);
	while (*p && isspace((unsigned char)*p))
		p++;
	if (*p != ':')
		return -1;
	p++;
	while (*p && isspace((unsigned char)*p))
		p++;

	if (*p == '"') {
		start = p + 1;
		end = strchr(start, '"');
		if (!end)
			return -1;
		len = (size_t)(end - start);
		if (len >= out_len)
			len = out_len - 1;
		memcpy(out, start, len);
		out[len] = '\0';
		return 0;
	}

	/* Non-string value: copy until , } whitespace */
	start = p;
	end = start;
	while (*end && *end != ',' && *end != '}' && !isspace((unsigned char)*end))
		end++;
	len = (size_t)(end - start);
	if (len >= out_len)
		len = out_len - 1;
	memcpy(out, start, len);
	out[len] = '\0';
	return 0;
}

static int json_get_int(const char *json, const char *key, int def)
{
	char buf[32];

	if (json_get_string(json, key, buf, sizeof(buf)) < 0)
		return def;
	return atoi(buf);
}

static bool json_get_bool(const char *json, const char *key, bool def)
{
	char buf[16];

	if (json_get_string(json, key, buf, sizeof(buf)) < 0)
		return def;
	return (strcmp(buf, "true") == 0 || strcmp(buf, "1") == 0);
}

/* ===== Backend name mapping ===== */

static llm_backend_t parse_backend(const char *name)
{
	if (strcmp(name, "anthropic") == 0) return LLM_BACKEND_ANTHROPIC;
	if (strcmp(name, "openai")    == 0) return LLM_BACKEND_OPENAI;
	if (strcmp(name, "ollama")    == 0) return LLM_BACKEND_OLLAMA;
	if (strcmp(name, "llamacpp")  == 0) return LLM_BACKEND_LLAMACPP;
	return LLM_BACKEND_ANTHROPIC;
}

static tool_mode_t parse_tool_mode(const char *name)
{
	if (strcmp(name, "auto")     == 0) return TOOL_MODE_AUTO;
	if (strcmp(name, "confirm")  == 0) return TOOL_MODE_CONFIRM;
	if (strcmp(name, "readonly") == 0) return TOOL_MODE_READONLY;
	return TOOL_MODE_AUTO;
}

/* ===== Public API ===== */

void config_defaults(struct aicore_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));

	cfg->backend      = LLM_BACKEND_ANTHROPIC;
	cfg->max_tokens   = 4096;
	cfg->temperature  = 0.7f;
	cfg->timeout_secs = 120;
	cfg->tool_mode    = TOOL_MODE_AUTO;

	strncpy(cfg->model, "claude-opus-4-6", sizeof(cfg->model) - 1);
	strncpy(cfg->api_url,
		"https://api.anthropic.com/v1/messages",
		sizeof(cfg->api_url) - 1);
	strncpy(cfg->socket_path, AICORE_SOCKET_PATH, sizeof(cfg->socket_path) - 1);
	strncpy(cfg->dev_ai_path, AICORE_DEV_AI, sizeof(cfg->dev_ai_path) - 1);
	strncpy(cfg->log_path, AICORE_LOG_PATH, sizeof(cfg->log_path) - 1);

	cfg->log_level         = 2; /* info */
	cfg->enable_shell      = true;
	cfg->enable_file_write = true;
	cfg->enable_sysctl_write = true;
	cfg->enable_service_ctl  = true;
	cfg->include_sysinfo   = true;
	cfg->include_resources = true;
	cfg->history_depth     = 20;
	cfg->daemonize         = false;

	strncpy(cfg->system_prompt,
		"You are the Linux OS AI assistant with complete system access. "
		"You can read and modify any file, run any command, manage processes, "
		"configure the kernel, and interact with all hardware. "
		"You have sudo privileges. Be concise and technical. "
		"When you need to take actions, use the available tools (claws). "
		"Always explain what you are doing before executing commands.",
		sizeof(cfg->system_prompt) - 1);
}

int config_load(struct aicore_config *cfg, const char *path)
{
	FILE *f;
	char *buf;
	long fsize;
	char tmp[512];

	config_defaults(cfg);

	f = fopen(path, "r");
	if (!f) {
		if (errno != ENOENT)
			fprintf(stderr, "aicore: config open %s: %s\n",
				path, strerror(errno));
		return (errno == ENOENT) ? 0 : -errno;
	}

	fseek(f, 0, SEEK_END);
	fsize = ftell(f);
	rewind(f);

	if (fsize <= 0 || fsize > 1024 * 64) {
		fclose(f);
		return -EINVAL;
	}

	buf = malloc(fsize + 1);
	if (!buf) {
		fclose(f);
		return -ENOMEM;
	}

	if (fread(buf, 1, fsize, f) != (size_t)fsize) {
		free(buf);
		fclose(f);
		return -EIO;
	}
	buf[fsize] = '\0';
	fclose(f);

	/* Parse fields */
	if (json_get_string(buf, "backend", tmp, sizeof(tmp)) == 0)
		cfg->backend = parse_backend(tmp);
	if (json_get_string(buf, "api_key", cfg->api_key, sizeof(cfg->api_key)) < 0)
		cfg->api_key[0] = '\0';
	if (json_get_string(buf, "api_url", cfg->api_url, sizeof(cfg->api_url)) < 0)
		; /* keep default */
	if (json_get_string(buf, "model", cfg->model, sizeof(cfg->model)) < 0)
		; /* keep default */
	if (json_get_string(buf, "system_prompt",
			    cfg->system_prompt, sizeof(cfg->system_prompt)) < 0)
		; /* keep default */

	cfg->max_tokens   = json_get_int(buf,  "max_tokens",   cfg->max_tokens);
	cfg->timeout_secs = json_get_int(buf,  "timeout_secs", cfg->timeout_secs);
	cfg->log_level    = json_get_int(buf,  "log_level",    cfg->log_level);
	cfg->history_depth = json_get_int(buf, "history_depth", cfg->history_depth);

	if (json_get_string(buf, "tool_mode", tmp, sizeof(tmp)) == 0)
		cfg->tool_mode = parse_tool_mode(tmp);

	cfg->enable_shell       = json_get_bool(buf, "enable_shell",       cfg->enable_shell);
	cfg->enable_file_write  = json_get_bool(buf, "enable_file_write",  cfg->enable_file_write);
	cfg->enable_sysctl_write = json_get_bool(buf, "enable_sysctl_write", cfg->enable_sysctl_write);
	cfg->enable_service_ctl = json_get_bool(buf, "enable_service_ctl", cfg->enable_service_ctl);
	cfg->include_sysinfo    = json_get_bool(buf, "include_sysinfo",    cfg->include_sysinfo);
	cfg->include_resources  = json_get_bool(buf, "include_resources",  cfg->include_resources);
	cfg->daemonize          = json_get_bool(buf, "daemonize",          cfg->daemonize);

	if (json_get_string(buf, "socket_path",
			    cfg->socket_path, sizeof(cfg->socket_path)) < 0)
		; /* keep default */

	free(buf);
	return 0;
}

void config_apply_env(struct aicore_config *cfg)
{
	const char *e;

	if ((e = getenv("AICORE_API_KEY")) && *e)
		strncpy(cfg->api_key, e, sizeof(cfg->api_key) - 1);
	if ((e = getenv("AICORE_MODEL")) && *e)
		strncpy(cfg->model, e, sizeof(cfg->model) - 1);
	if ((e = getenv("AICORE_BACKEND")) && *e)
		cfg->backend = parse_backend(e);
	if ((e = getenv("AICORE_API_URL")) && *e)
		strncpy(cfg->api_url, e, sizeof(cfg->api_url) - 1);
	if ((e = getenv("ANTHROPIC_API_KEY")) && *e && !cfg->api_key[0])
		strncpy(cfg->api_key, e, sizeof(cfg->api_key) - 1);
	if ((e = getenv("OPENAI_API_KEY")) && *e && !cfg->api_key[0])
		strncpy(cfg->api_key, e, sizeof(cfg->api_key) - 1);
}

void config_dump(const struct aicore_config *cfg)
{
	static const char *backend_names[] = {
		"anthropic", "openai", "ollama", "llamacpp"
	};
	static const char *tool_mode_names[] = {
		"confirm", "auto", "readonly"
	};

	fprintf(stderr,
		"aicore config:\n"
		"  backend:    %s\n"
		"  model:      %s\n"
		"  api_url:    %s\n"
		"  api_key:    %s\n"
		"  tool_mode:  %s\n"
		"  shell:      %s\n"
		"  log_level:  %d\n",
		backend_names[cfg->backend],
		cfg->model,
		cfg->api_url,
		cfg->api_key[0] ? "***set***" : "(not set)",
		tool_mode_names[cfg->tool_mode],
		cfg->enable_shell ? "yes" : "no",
		cfg->log_level);
}
