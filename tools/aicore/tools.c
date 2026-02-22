// SPDX-License-Identifier: GPL-2.0
/*
 * tools/aicore/tools.c - OpenClaw userspace tool implementations
 *
 * All tools run as root. This is intentional for the developer AI OS.
 * Tool execution is logged; the AI explains before acting.
 *
 * Tools implemented here:
 *   shell_exec     - Execute arbitrary shell command, capture output
 *   file_read      - Read file contents
 *   file_write     - Write to a file
 *   file_list      - List directory contents
 *   sysctl_get     - Read a sysctl value
 *   sysctl_set     - Write a sysctl value
 *   service_ctl    - systemctl start/stop/restart/status
 *   pkg_manage     - apt/dnf/pacman install/remove/update
 *   net_info       - Network interface information
 *   process_kill   - Send signal to process
 *   env_get        - Read environment variable
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <signal.h>

#include "tools.h"
#include "config.h"

/* Include kernel UAPI header for claw listing */
#include "../../include/uapi/linux/ai.h"

/* ===== Utility: run a shell command, capture stdout+stderr ===== */

static int run_command(const char *cmd, char *out, size_t out_len)
{
	FILE *fp;
	size_t nread = 0;
	char buf[4096];

	/* popen uses /bin/sh -c */
	fp = popen(cmd, "r");
	if (!fp) {
		snprintf(out, out_len,
			 "{\"error\":\"popen failed: %s\"}", strerror(errno));
		return -errno;
	}

	out[0] = '\0';
	while (nread < out_len - 1) {
		size_t n = fread(buf, 1, sizeof(buf), fp);
		if (n == 0)
			break;
		size_t remaining = out_len - 1 - nread;
		if (n > remaining)
			n = remaining;
		memcpy(out + nread, buf, n);
		nread += n;
	}
	out[nread] = '\0';
	pclose(fp);
	return 0;
}

/* Escape a string for JSON embedding (basic: escape " and \) */
static void json_escape(const char *in, char *out, size_t out_len)
{
	size_t j = 0;

	for (size_t i = 0; in[i] && j + 3 < out_len; i++) {
		unsigned char c = (unsigned char)in[i];
		if (c == '"' || c == '\\') {
			out[j++] = '\\';
			out[j++] = c;
		} else if (c == '\n') {
			out[j++] = '\\';
			out[j++] = 'n';
		} else if (c == '\r') {
			out[j++] = '\\';
			out[j++] = 'r';
		} else if (c == '\t') {
			out[j++] = '\\';
			out[j++] = 't';
		} else if (c < 0x20) {
			j += snprintf(out + j, out_len - j, "\\u%04x", c);
		} else {
			out[j++] = c;
		}
	}
	out[j] = '\0';
}

/* Extract a JSON string field value (simple, non-nested) */
static int extract_json_str(const char *json, const char *key,
			    char *out, size_t out_len)
{
	char needle[128];
	const char *p, *start, *end;
	size_t len;

	snprintf(needle, sizeof(needle), "\"%s\"", key);
	p = strstr(json, needle);
	if (!p)
		return -1;
	p += strlen(needle);
	while (*p == ' ' || *p == ':' || *p == '\t')
		p++;
	if (*p != '"')
		return -1;
	start = p + 1;
	end = start;
	while (*end && *end != '"') {
		if (*end == '\\')
			end++;
		end++;
	}
	len = (size_t)(end - start);
	if (len >= out_len)
		len = out_len - 1;
	memcpy(out, start, len);
	out[len] = '\0';
	return 0;
}

/* ===== Tool Implementations ===== */

static int tool_shell_exec(const char *params, char *result, size_t result_len,
			   const struct aicore_config *cfg)
{
	char command[4096] = {0};
	char raw_out[TOOL_RESULT_MAX] = {0};
	char escaped[TOOL_RESULT_MAX * 2] = {0};
	int exit_code;
	FILE *fp;
	char cmd_with_exit[4096 + 64];

	if (!cfg->enable_shell) {
		snprintf(result, result_len, "{\"error\":\"shell_exec disabled\"}");
		return -EPERM;
	}

	if (extract_json_str(params, "command", command, sizeof(command)) < 0) {
		snprintf(result, result_len,
			 "{\"error\":\"missing 'command' parameter\"}");
		return -EINVAL;
	}

	/* Run command and capture exit code */
	snprintf(cmd_with_exit, sizeof(cmd_with_exit),
		 "(%s) 2>&1; echo \"__EXIT__:$?\"", command);

	fp = popen(cmd_with_exit, "r");
	if (!fp) {
		snprintf(result, result_len,
			 "{\"error\":\"popen: %s\"}", strerror(errno));
		return -errno;
	}

	size_t nread = 0;
	char buf[4096];
	while (nread < sizeof(raw_out) - 1) {
		size_t n = fread(buf, 1, sizeof(buf), fp);
		if (!n)
			break;
		size_t rem = sizeof(raw_out) - 1 - nread;
		memcpy(raw_out + nread, buf, n < rem ? n : rem);
		nread += n < rem ? n : rem;
	}
	raw_out[nread] = '\0';
	pclose(fp);

	/* Extract exit code from marker */
	exit_code = -1;
	char *marker = strstr(raw_out, "\n__EXIT__:");
	if (!marker)
		marker = strstr(raw_out, "__EXIT__:");
	if (marker) {
		exit_code = atoi(marker + strlen("\n__EXIT__:") -
				 (marker[0] == '\n' ? 0 : 1));
		*marker = '\0';
	}

	json_escape(raw_out, escaped, sizeof(escaped));
	snprintf(result, result_len,
		 "{\"exit_code\":%d,\"output\":\"%s\"}",
		 exit_code, escaped);
	return 0;
}

static int tool_file_read(const char *params, char *result, size_t result_len,
			  const struct aicore_config *cfg)
{
	char path[4096] = {0};
	char raw[TOOL_RESULT_MAX] = {0};
	char escaped[TOOL_RESULT_MAX + 512] = {0};
	FILE *f;
	size_t nread;

	if (extract_json_str(params, "path", path, sizeof(path)) < 0) {
		snprintf(result, result_len, "{\"error\":\"missing 'path'\"}");
		return -EINVAL;
	}

	f = fopen(path, "r");
	if (!f) {
		snprintf(result, result_len,
			 "{\"error\":\"open %s: %s\"}", path, strerror(errno));
		return -errno;
	}

	nread = fread(raw, 1, sizeof(raw) - 1, f);
	raw[nread] = '\0';
	fclose(f);

	json_escape(raw, escaped, sizeof(escaped));
	snprintf(result, result_len,
		 "{\"path\":\"%s\",\"size\":%zu,\"content\":\"%s\"}",
		 path, nread, escaped);
	return 0;
}

static int tool_file_write(const char *params, char *result, size_t result_len,
			   const struct aicore_config *cfg)
{
	char path[4096] = {0};
	char content[TOOL_RESULT_MAX] = {0};
	FILE *f;
	size_t written;

	if (!cfg->enable_file_write) {
		snprintf(result, result_len, "{\"error\":\"file_write disabled\"}");
		return -EPERM;
	}

	if (extract_json_str(params, "path", path, sizeof(path)) < 0 ||
	    extract_json_str(params, "content", content, sizeof(content)) < 0) {
		snprintf(result, result_len,
			 "{\"error\":\"missing 'path' or 'content'\"}");
		return -EINVAL;
	}

	f = fopen(path, "w");
	if (!f) {
		snprintf(result, result_len,
			 "{\"error\":\"open %s: %s\"}", path, strerror(errno));
		return -errno;
	}

	written = fwrite(content, 1, strlen(content), f);
	fclose(f);

	snprintf(result, result_len,
		 "{\"path\":\"%s\",\"bytes_written\":%zu}", path, written);
	return 0;
}

static int tool_file_list(const char *params, char *result, size_t result_len,
			  const struct aicore_config *cfg)
{
	char path[4096] = {0};
	char cmd[4096 + 64];

	if (extract_json_str(params, "path", path, sizeof(path)) < 0)
		strncpy(path, ".", sizeof(path) - 1);

	snprintf(cmd, sizeof(cmd), "ls -la --color=never %s 2>&1", path);
	char raw[TOOL_RESULT_MAX] = {0};
	char escaped[TOOL_RESULT_MAX + 64] = {0};
	run_command(cmd, raw, sizeof(raw));
	json_escape(raw, escaped, sizeof(escaped));
	snprintf(result, result_len,
		 "{\"path\":\"%s\",\"listing\":\"%s\"}", path, escaped);
	return 0;
}

static int tool_sysctl_get(const char *params, char *result, size_t result_len,
			   const struct aicore_config *cfg)
{
	char key[256] = {0};
	char cmd[512];
	char raw[4096] = {0};
	char escaped[8192] = {0};

	if (extract_json_str(params, "key", key, sizeof(key)) < 0) {
		snprintf(result, result_len, "{\"error\":\"missing 'key'\"}");
		return -EINVAL;
	}

	snprintf(cmd, sizeof(cmd), "sysctl -n %s 2>&1", key);
	run_command(cmd, raw, sizeof(raw));

	/* Strip trailing newline */
	size_t len = strlen(raw);
	while (len > 0 && (raw[len - 1] == '\n' || raw[len - 1] == '\r'))
		raw[--len] = '\0';

	json_escape(raw, escaped, sizeof(escaped));
	snprintf(result, result_len,
		 "{\"key\":\"%s\",\"value\":\"%s\"}", key, escaped);
	return 0;
}

static int tool_sysctl_set(const char *params, char *result, size_t result_len,
			   const struct aicore_config *cfg)
{
	char key[256] = {0};
	char value[256] = {0};
	char cmd[1024];
	char raw[4096] = {0};

	if (!cfg->enable_sysctl_write) {
		snprintf(result, result_len, "{\"error\":\"sysctl_set disabled\"}");
		return -EPERM;
	}

	if (extract_json_str(params, "key",   key,   sizeof(key))   < 0 ||
	    extract_json_str(params, "value", value, sizeof(value)) < 0) {
		snprintf(result, result_len,
			 "{\"error\":\"missing 'key' or 'value'\"}");
		return -EINVAL;
	}

	snprintf(cmd, sizeof(cmd), "sysctl -w '%s=%s' 2>&1", key, value);
	run_command(cmd, raw, sizeof(raw));

	snprintf(result, result_len,
		 "{\"key\":\"%s\",\"value\":\"%s\",\"result\":\"ok\"}", key, value);
	return 0;
}

static int tool_service_ctl(const char *params, char *result, size_t result_len,
			    const struct aicore_config *cfg)
{
	char service[256] = {0};
	char action[64]   = {0};
	char cmd[1024];
	char raw[TOOL_RESULT_MAX] = {0};
	char escaped[TOOL_RESULT_MAX + 64] = {0};

	static const char *allowed_actions[] = {
		"start", "stop", "restart", "status", "enable", "disable",
		"reload", "is-active", "is-enabled", NULL
	};

	if (!cfg->enable_service_ctl) {
		snprintf(result, result_len, "{\"error\":\"service_ctl disabled\"}");
		return -EPERM;
	}

	if (extract_json_str(params, "service", service, sizeof(service)) < 0 ||
	    extract_json_str(params, "action",  action,  sizeof(action))  < 0) {
		snprintf(result, result_len,
			 "{\"error\":\"missing 'service' or 'action'\"}");
		return -EINVAL;
	}

	/* Validate action against allowlist */
	int valid = 0;
	for (int i = 0; allowed_actions[i]; i++)
		if (strcmp(action, allowed_actions[i]) == 0) { valid = 1; break; }
	if (!valid) {
		snprintf(result, result_len,
			 "{\"error\":\"invalid action '%s'\"}", action);
		return -EINVAL;
	}

	snprintf(cmd, sizeof(cmd), "systemctl %s %s 2>&1", action, service);
	run_command(cmd, raw, sizeof(raw));
	json_escape(raw, escaped, sizeof(escaped));
	snprintf(result, result_len,
		 "{\"service\":\"%s\",\"action\":\"%s\",\"output\":\"%s\"}",
		 service, action, escaped);
	return 0;
}

static int tool_net_info(const char *params, char *result, size_t result_len,
			 const struct aicore_config *cfg)
{
	char raw[TOOL_RESULT_MAX] = {0};
	char escaped[TOOL_RESULT_MAX + 64] = {0};

	run_command("ip -j addr 2>/dev/null || ip addr 2>&1", raw, sizeof(raw));
	json_escape(raw, escaped, sizeof(escaped));
	snprintf(result, result_len, "{\"net_info\":\"%s\"}", escaped);
	return 0;
}

static int tool_process_kill(const char *params, char *result, size_t result_len,
			     const struct aicore_config *cfg)
{
	char pid_str[32] = {0};
	char sig_str[32] = {0};
	int pid, sig;

	if (extract_json_str(params, "pid", pid_str, sizeof(pid_str)) < 0) {
		snprintf(result, result_len, "{\"error\":\"missing 'pid'\"}");
		return -EINVAL;
	}

	pid = atoi(pid_str);
	if (pid <= 1) {
		snprintf(result, result_len,
			 "{\"error\":\"invalid pid %d\"}", pid);
		return -EINVAL;
	}

	sig = SIGTERM;
	if (extract_json_str(params, "signal", sig_str, sizeof(sig_str)) == 0) {
		if (strcmp(sig_str, "SIGKILL") == 0 || strcmp(sig_str, "9") == 0)
			sig = SIGKILL;
		else if (strcmp(sig_str, "SIGHUP")  == 0 || strcmp(sig_str, "1") == 0)
			sig = SIGHUP;
	}

	if (kill(pid, sig) < 0) {
		snprintf(result, result_len,
			 "{\"error\":\"kill(%d): %s\"}", pid, strerror(errno));
		return -errno;
	}

	snprintf(result, result_len,
		 "{\"pid\":%d,\"signal\":%d,\"result\":\"sent\"}", pid, sig);
	return 0;
}

/* ===== Tool Registry ===== */

static struct ai_tool userspace_tools[] = {
	{
		.name        = "shell_exec",
		.description = "Execute any shell command as root and return stdout+stderr",
		.schema      = "{\"type\":\"object\","
			       "\"properties\":{\"command\":{\"type\":\"string\","
			       "\"description\":\"Shell command to execute\"}},"
			       "\"required\":[\"command\"]}",
		.execute     = tool_shell_exec,
		.requires_sudo = 1,
	},
	{
		.name        = "file_read",
		.description = "Read the contents of any file",
		.schema      = "{\"type\":\"object\","
			       "\"properties\":{\"path\":{\"type\":\"string\"}},"
			       "\"required\":[\"path\"]}",
		.execute     = tool_file_read,
	},
	{
		.name        = "file_write",
		.description = "Write content to a file (creates or overwrites)",
		.schema      = "{\"type\":\"object\","
			       "\"properties\":{"
			       "\"path\":{\"type\":\"string\"},"
			       "\"content\":{\"type\":\"string\"}},"
			       "\"required\":[\"path\",\"content\"]}",
		.execute     = tool_file_write,
		.requires_sudo = 1,
	},
	{
		.name        = "file_list",
		.description = "List directory contents (ls -la)",
		.schema      = "{\"type\":\"object\","
			       "\"properties\":{\"path\":{\"type\":\"string\","
			       "\"description\":\"Directory path (default: .)\"}}}",
		.execute     = tool_file_list,
	},
	{
		.name        = "sysctl_get",
		.description = "Read a kernel sysctl parameter value",
		.schema      = "{\"type\":\"object\","
			       "\"properties\":{\"key\":{\"type\":\"string\","
			       "\"description\":\"sysctl key, e.g. vm.swappiness\"}},"
			       "\"required\":[\"key\"]}",
		.execute     = tool_sysctl_get,
	},
	{
		.name        = "sysctl_set",
		.description = "Set a kernel sysctl parameter value",
		.schema      = "{\"type\":\"object\","
			       "\"properties\":{"
			       "\"key\":{\"type\":\"string\"},"
			       "\"value\":{\"type\":\"string\"}},"
			       "\"required\":[\"key\",\"value\"]}",
		.execute     = tool_sysctl_set,
		.requires_sudo = 1,
	},
	{
		.name        = "service_ctl",
		.description = "Control systemd services: start/stop/restart/status/enable/disable",
		.schema      = "{\"type\":\"object\","
			       "\"properties\":{"
			       "\"service\":{\"type\":\"string\"},"
			       "\"action\":{\"type\":\"string\","
			       "\"enum\":[\"start\",\"stop\",\"restart\",\"status\","
			       "\"enable\",\"disable\",\"reload\",\"is-active\"]}},"
			       "\"required\":[\"service\",\"action\"]}",
		.execute     = tool_service_ctl,
		.requires_sudo = 1,
	},
	{
		.name        = "net_info",
		.description = "Get network interface addresses and configuration",
		.schema      = "{\"type\":\"object\",\"properties\":{}}",
		.execute     = tool_net_info,
	},
	{
		.name        = "process_kill",
		.description = "Send a signal to a process by PID",
		.schema      = "{\"type\":\"object\","
			       "\"properties\":{"
			       "\"pid\":{\"type\":\"integer\"},"
			       "\"signal\":{\"type\":\"string\","
			       "\"enum\":[\"SIGTERM\",\"SIGKILL\",\"SIGHUP\"],"
			       "\"default\":\"SIGTERM\"}},"
			       "\"required\":[\"pid\"]}",
		.execute     = tool_process_kill,
		.requires_sudo = 1,
	},
};

#define NUM_TOOLS (sizeof(userspace_tools) / sizeof(userspace_tools[0]))

int tools_init(const struct aicore_config *cfg)
{
	/* Apply config-driven enable flags */
	for (size_t i = 0; i < NUM_TOOLS; i++) {
		struct ai_tool *t = &userspace_tools[i];

		t->enabled = 1; /* Default: all enabled */

		if (strcmp(t->name, "shell_exec")  == 0 && !cfg->enable_shell)
			t->enabled = 0;
		if (strcmp(t->name, "file_write")  == 0 && !cfg->enable_file_write)
			t->enabled = 0;
		if (strcmp(t->name, "sysctl_set")  == 0 && !cfg->enable_sysctl_write)
			t->enabled = 0;
		if (strcmp(t->name, "service_ctl") == 0 && !cfg->enable_service_ctl)
			t->enabled = 0;
	}
	return 0;
}

int tools_execute(const char *name, const char *params,
		  char *result, size_t result_len,
		  const struct aicore_config *cfg)
{
	for (size_t i = 0; i < NUM_TOOLS; i++) {
		if (strcmp(userspace_tools[i].name, name) == 0) {
			if (!userspace_tools[i].enabled) {
				snprintf(result, result_len,
					 "{\"error\":\"tool '%s' is disabled\"}", name);
				return -EPERM;
			}
			return userspace_tools[i].execute(params, result, result_len, cfg);
		}
	}
	snprintf(result, result_len, "{\"error\":\"unknown tool '%s'\"}", name);
	return -ENOENT;
}

int tools_build_schema(char *out, size_t out_len, const struct aicore_config *cfg)
{
	size_t used = 0;
	int ret;
	int first = 1;

	ret = snprintf(out + used, out_len - used, "[");
	if (ret < 0 || ret >= (int)(out_len - used)) return -ENOSPC;
	used += ret;

	for (size_t i = 0; i < NUM_TOOLS; i++) {
		struct ai_tool *t = &userspace_tools[i];

		if (!t->enabled)
			continue;

		ret = snprintf(out + used, out_len - used,
			       "%s{\"name\":\"%s\","
			       "\"description\":\"%s\","
			       "\"input_schema\":%s}",
			       first ? "" : ",",
			       t->name, t->description, t->schema);
		if (ret < 0 || ret >= (int)(out_len - used))
			return -ENOSPC;
		used += ret;
		first = 0;
	}

	ret = snprintf(out + used, out_len - used, "]");
	if (ret < 0 || ret >= (int)(out_len - used)) return -ENOSPC;
	return 0;
}

int tools_count(void)
{
	int count = 0;

	for (size_t i = 0; i < NUM_TOOLS; i++)
		if (userspace_tools[i].enabled)
			count++;
	return count;
}

int tools_load_kernel_claws(int ai_fd)
{
	/* Query kernel for registered claws via ioctl */
	/* TODO: dynamically load kernel claws into tool registry */
	(void)ai_fd;
	return 0;
}
