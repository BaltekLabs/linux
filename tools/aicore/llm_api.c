// SPDX-License-Identifier: GPL-2.0
/*
 * tools/aicore/llm_api.c - LLM HTTP API client
 *
 * Implements HTTP communication with Anthropic Claude API and
 * OpenAI-compatible APIs (Ollama, llama.cpp, LM Studio, etc.)
 *
 * Uses libcurl for HTTP, parses JSON responses manually (no external deps).
 * Supports streaming via Server-Sent Events (SSE).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <curl/curl.h>

#include "llm_api.h"
#include "config.h"

/* ===== curl write callback buffer ===== */

struct curl_buf {
	char  *data;
	size_t used;
	size_t alloc;
	/* Streaming support */
	llm_stream_cb_t stream_cb;
	void           *stream_userdata;
	/* Accumulated text for stream parsing */
	char            line_buf[8192];
	size_t          line_used;
};

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	struct curl_buf *buf = userdata;
	size_t n = size * nmemb;

	if (buf->used + n + 1 > buf->alloc) {
		size_t new_alloc = (buf->used + n + 1) * 2;
		char *new_data = realloc(buf->data, new_alloc);
		if (!new_data)
			return 0;
		buf->data  = new_data;
		buf->alloc = new_alloc;
	}

	memcpy(buf->data + buf->used, ptr, n);
	buf->used += n;
	buf->data[buf->used] = '\0';
	return n;
}

/* ===== Minimal JSON construction helpers ===== */

/* JSON-encode a string into dst, max dst_len */
static void json_str_encode(const char *src, char *dst, size_t dst_len)
{
	size_t j = 0;

	for (size_t i = 0; src[i] && j + 4 < dst_len; i++) {
		unsigned char c = (unsigned char)src[i];
		switch (c) {
		case '"':  dst[j++] = '\\'; dst[j++] = '"';  break;
		case '\\': dst[j++] = '\\'; dst[j++] = '\\'; break;
		case '\n': dst[j++] = '\\'; dst[j++] = 'n';  break;
		case '\r': dst[j++] = '\\'; dst[j++] = 'r';  break;
		case '\t': dst[j++] = '\\'; dst[j++] = 't';  break;
		default:
			if (c < 0x20)
				j += snprintf(dst + j, dst_len - j, "\\u%04x", c);
			else
				dst[j++] = c;
		}
	}
	dst[j] = '\0';
}

/* ===== JSON response parsing helpers ===== */

/* Find value for "key" in flat JSON, returns pointer into json or NULL */
static const char *json_find_key(const char *json, const char *key)
{
	char needle[128];
	const char *p;

	snprintf(needle, sizeof(needle), "\"%s\"", key);
	p = strstr(json, needle);
	if (!p)
		return NULL;
	p += strlen(needle);
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ':')
		p++;
	return p;
}

/* Extract string value at position p (p points past the opening ") */
static int json_extract_str_at(const char *p, char *out, size_t out_len)
{
	if (*p != '"')
		return -1;
	p++;
	size_t j = 0;
	while (*p && j + 1 < out_len) {
		if (*p == '"')
			break;
		if (*p == '\\') {
			p++;
			switch (*p) {
			case 'n': out[j++] = '\n'; break;
			case 'r': out[j++] = '\r'; break;
			case 't': out[j++] = '\t'; break;
			case '"': out[j++] = '"';  break;
			case '\\':out[j++] = '\\'; break;
			default:  out[j++] = *p;   break;
			}
		} else {
			out[j++] = *p;
		}
		p++;
	}
	out[j] = '\0';
	return 0;
}

/* ===== Anthropic API request builder ===== */

/*
 * Build the JSON body for an Anthropic messages API request.
 * This is the main serialization function for the conversation.
 */
static char *build_anthropic_request(const struct aicore_config *cfg,
				     const struct llm_context *ctx,
				     const char *tools_json)
{
	/* Estimate required size */
	size_t sz = 4096 + strlen(cfg->system_prompt) +
		    (tools_json ? strlen(tools_json) : 2);

	for (int i = 0; i < ctx->count; i++) {
		sz += 128;
		if (ctx->history[i].content)
			sz += strlen(ctx->history[i].content) * 2; /* escape overhead */
	}

	char *buf = malloc(sz);
	if (!buf)
		return NULL;

	char *esc_system = malloc(strlen(cfg->system_prompt) * 2 + 16);
	if (!esc_system) {
		free(buf);
		return NULL;
	}
	json_str_encode(cfg->system_prompt, esc_system,
			strlen(cfg->system_prompt) * 2 + 16);

	int used = snprintf(buf, sz,
		"{\"model\":\"%s\","
		"\"max_tokens\":%d,"
		"\"system\":\"%s\","
		"\"tools\":%s,"
		"\"messages\":[",
		cfg->model,
		cfg->max_tokens,
		esc_system,
		tools_json ? tools_json : "[]");
	free(esc_system);

	for (int i = 0; i < ctx->count; i++) {
		const struct llm_message *m = &ctx->history[i];
		const char *role_str = (m->role == MSG_ROLE_USER) ? "user" : "assistant";
		size_t content_len = m->content ? strlen(m->content) : 0;
		char *esc = malloc(content_len * 2 + 16);
		if (!esc)
			break;
		if (m->content)
			json_str_encode(m->content, esc, content_len * 2 + 16);
		else
			esc[0] = '\0';

		int r;
		if (m->role == MSG_ROLE_TOOL) {
			/* Tool result format for Anthropic */
			r = snprintf(buf + used, sz - used,
				     "%s{\"role\":\"user\",\"content\":["
				     "{\"type\":\"tool_result\","
				     "\"tool_use_id\":\"%s\","
				     "\"content\":\"%s\"}]}",
				     (i > 0) ? "," : "",
				     m->tool_use_id,
				     esc);
		} else {
			r = snprintf(buf + used, sz - used,
				     "%s{\"role\":\"%s\",\"content\":\"%s\"}",
				     (i > 0) ? "," : "",
				     role_str, esc);
		}
		free(esc);

		if (r < 0 || r >= (int)(sz - used))
			break;
		used += r;
	}

	snprintf(buf + used, sz - used, "]}");
	return buf;
}

/* ===== OpenAI-compatible request builder ===== */

static char *build_openai_request(const struct aicore_config *cfg,
				  const struct llm_context *ctx,
				  const char *tools_json)
{
	size_t sz = 4096 + strlen(cfg->system_prompt);
	for (int i = 0; i < ctx->count; i++)
		if (ctx->history[i].content)
			sz += strlen(ctx->history[i].content) * 2 + 256;

	char *buf = malloc(sz);
	if (!buf)
		return NULL;

	char *esc_sys = malloc(strlen(cfg->system_prompt) * 2 + 16);
	if (!esc_sys) { free(buf); return NULL; }
	json_str_encode(cfg->system_prompt, esc_sys,
			strlen(cfg->system_prompt) * 2 + 16);

	int used = snprintf(buf, sz,
		"{\"model\":\"%s\","
		"\"max_tokens\":%d,"
		"\"messages\":["
		"{\"role\":\"system\",\"content\":\"%s\"}",
		cfg->model, cfg->max_tokens, esc_sys);
	free(esc_sys);

	for (int i = 0; i < ctx->count; i++) {
		const struct llm_message *m = &ctx->history[i];
		const char *role = (m->role == MSG_ROLE_USER) ? "user" : "assistant";
		size_t clen = m->content ? strlen(m->content) : 0;
		char *esc = malloc(clen * 2 + 16);
		if (!esc) break;
		if (m->content)
			json_str_encode(m->content, esc, clen * 2 + 16);
		else esc[0] = '\0';

		int r = snprintf(buf + used, sz - used,
				 ",{\"role\":\"%s\",\"content\":\"%s\"}",
				 role, esc);
		free(esc);
		if (r < 0 || r >= (int)(sz - used)) break;
		used += r;
	}

	/* Add tools if provided (OpenAI function calling format) */
	if (tools_json && strlen(tools_json) > 2)
		snprintf(buf + used, sz - used, "],\"tools\":%s}", tools_json);
	else
		snprintf(buf + used, sz - used, "]}");

	return buf;
}

/* ===== Response parser ===== */

static struct llm_response *parse_anthropic_response(const char *json)
{
	struct llm_response *resp = calloc(1, sizeof(*resp));
	if (!resp)
		return NULL;

	/* Extract stop_reason */
	const char *stop = json_find_key(json, "stop_reason");
	if (stop && strncmp(stop, "\"tool_use\"", 10) == 0)
		resp->stop_reason_tool = true;

	/* Extract text content */
	const char *content = strstr(json, "\"type\":\"text\"");
	if (content) {
		const char *text_key = strstr(content, "\"text\"");
		if (text_key) {
			text_key = json_find_key(text_key, "text");
			if (text_key) {
				char *text_buf = malloc(LLM_MAX_MSG_LEN);
				if (text_buf) {
					json_extract_str_at(text_key, text_buf,
							    LLM_MAX_MSG_LEN);
					resp->text = text_buf;
				}
			}
		}
	}

	/* Extract tool_use blocks */
	const char *p = json;
	int num_tools = 0;

	/* Count tool_use blocks */
	while ((p = strstr(p, "\"type\":\"tool_use\"")) != NULL) {
		num_tools++;
		p++;
	}

	if (num_tools > 0 && resp->stop_reason_tool) {
		resp->tool_calls = calloc(num_tools, sizeof(struct llm_tool_call));
		if (!resp->tool_calls) {
			llm_response_free(resp);
			return NULL;
		}
		resp->num_tool_calls = num_tools;

		p = json;
		for (int i = 0; i < num_tools; i++) {
			p = strstr(p, "\"type\":\"tool_use\"");
			if (!p) break;

			struct llm_tool_call *tc = &resp->tool_calls[i];

			/* Extract id */
			const char *id_val = json_find_key(p, "id");
			if (id_val)
				json_extract_str_at(id_val, tc->tool_use_id,
						    sizeof(tc->tool_use_id));

			/* Extract name */
			const char *name_val = json_find_key(p, "name");
			if (name_val)
				json_extract_str_at(name_val, tc->name,
						    sizeof(tc->name));

			/* Extract input object (raw JSON) */
			const char *input_val = json_find_key(p, "input");
			if (input_val && *input_val == '{') {
				/* Copy the raw JSON object */
				int depth = 0;
				size_t j = 0;
				const char *q = input_val;
				while (*q && j + 1 < sizeof(tc->input)) {
					tc->input[j++] = *q;
					if (*q == '{') depth++;
					else if (*q == '}') {
						depth--;
						if (depth == 0) { q++; break; }
					}
					q++;
				}
				tc->input[j] = '\0';
			}

			p++;
		}
	}

	/* Extract token usage */
	const char *usage = json_find_key(json, "usage");
	if (usage) {
		const char *in_tok = json_find_key(usage, "input_tokens");
		const char *out_tok = json_find_key(usage, "output_tokens");
		if (in_tok)  resp->input_tokens  = atoi(in_tok);
		if (out_tok) resp->output_tokens = atoi(out_tok);
	}

	return resp;
}

static struct llm_response *parse_openai_response(const char *json)
{
	struct llm_response *resp = calloc(1, sizeof(*resp));
	if (!resp)
		return NULL;

	/* OpenAI format: choices[0].message.content */
	const char *content = json_find_key(json, "content");
	if (content) {
		char *text_buf = malloc(LLM_MAX_MSG_LEN);
		if (text_buf) {
			json_extract_str_at(content, text_buf, LLM_MAX_MSG_LEN);
			resp->text = text_buf;
		}
	}

	return resp;
}

/* ===== curl HTTP request ===== */

static int do_http_post(const struct aicore_config *cfg,
			const char *body,
			struct curl_buf *out_buf)
{
	CURL *curl;
	CURLcode res;
	struct curl_slist *headers = NULL;
	char auth_header[600];
	int ret = 0;

	curl = curl_easy_init();
	if (!curl)
		return -ENOMEM;

	curl_easy_setopt(curl, CURLOPT_URL, cfg->api_url);
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, out_buf);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)cfg->timeout_secs);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

	headers = curl_slist_append(headers, "Content-Type: application/json");

	switch (cfg->backend) {
	case LLM_BACKEND_ANTHROPIC:
		snprintf(auth_header, sizeof(auth_header),
			 "x-api-key: %s", cfg->api_key);
		headers = curl_slist_append(headers, auth_header);
		headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
		break;
	case LLM_BACKEND_OPENAI:
	case LLM_BACKEND_OLLAMA:
	case LLM_BACKEND_LLAMACPP:
		if (cfg->api_key[0]) {
			snprintf(auth_header, sizeof(auth_header),
				 "Authorization: Bearer %s", cfg->api_key);
			headers = curl_slist_append(headers, auth_header);
		}
		break;
	}

	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		fprintf(stderr, "aicore: HTTP error: %s\n", curl_easy_strerror(res));
		ret = -EIO;
	}

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return ret;
}

/* ===== Public API ===== */

int llm_init(const struct aicore_config *cfg)
{
	(void)cfg;
	curl_global_init(CURL_GLOBAL_DEFAULT);
	return 0;
}

void llm_cleanup(void)
{
	curl_global_cleanup();
}

void llm_context_init(struct llm_context *ctx, int max_depth)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->max_depth = max_depth > 0 ? max_depth : 20;
}

void llm_context_free(struct llm_context *ctx)
{
	for (int i = 0; i < ctx->count; i++)
		free(ctx->history[i].content);
	memset(ctx, 0, sizeof(*ctx));
}

void llm_context_add(struct llm_context *ctx, msg_role_t role, const char *content)
{
	/* Evict oldest if at capacity */
	if (ctx->count >= LLM_MAX_HISTORY) {
		free(ctx->history[0].content);
		memmove(&ctx->history[0], &ctx->history[1],
			(LLM_MAX_HISTORY - 1) * sizeof(struct llm_message));
		ctx->count--;
	}

	struct llm_message *m = &ctx->history[ctx->count++];
	m->role    = role;
	m->content = content ? strdup(content) : NULL;
}

int llm_chat(const struct aicore_config *cfg,
	     struct llm_context *ctx,
	     const char *query,
	     const char *tools_json,
	     struct llm_response **resp_out,
	     llm_stream_cb_t stream_cb,
	     void *userdata)
{
	struct curl_buf buf = {0};
	char *body = NULL;
	int ret = 0;

	/* Add user message to context */
	llm_context_add(ctx, MSG_ROLE_USER, query);

	/* Build request body */
	switch (cfg->backend) {
	case LLM_BACKEND_ANTHROPIC:
		body = build_anthropic_request(cfg, ctx, tools_json);
		break;
	default:
		body = build_openai_request(cfg, ctx, tools_json);
		break;
	}

	if (!body)
		return -ENOMEM;

	buf.alloc = 64 * 1024;
	buf.data  = malloc(buf.alloc);
	if (!buf.data) {
		free(body);
		return -ENOMEM;
	}
	buf.stream_cb       = stream_cb;
	buf.stream_userdata = userdata;

	ret = do_http_post(cfg, body, &buf);
	free(body);

	if (ret < 0) {
		free(buf.data);
		return ret;
	}

	/* Parse response */
	struct llm_response *resp = NULL;
	switch (cfg->backend) {
	case LLM_BACKEND_ANTHROPIC:
		resp = parse_anthropic_response(buf.data);
		break;
	default:
		resp = parse_openai_response(buf.data);
		break;
	}

	free(buf.data);

	if (!resp)
		return -ENOMEM;

	/* Add assistant response to context */
	if (resp->text)
		llm_context_add(ctx, MSG_ROLE_ASSISTANT, resp->text);

	/* If streaming callback provided, emit the full text */
	if (stream_cb && resp->text)
		stream_cb(resp->text, strlen(resp->text), userdata);

	*resp_out = resp;
	return 0;
}

int llm_tool_result(const struct aicore_config *cfg,
		    struct llm_context *ctx,
		    const char *tools_json,
		    struct llm_tool_call *calls,
		    int num_calls,
		    const char **results,
		    struct llm_response **resp_out,
		    llm_stream_cb_t stream_cb,
		    void *userdata)
{
	/* Add tool results to context then re-query */
	for (int i = 0; i < num_calls; i++) {
		struct llm_message *m = &ctx->history[ctx->count++];

		m->role    = MSG_ROLE_TOOL;
		m->content = results[i] ? strdup(results[i]) : strdup("{}");
		strncpy(m->tool_use_id, calls[i].tool_use_id,
			sizeof(m->tool_use_id) - 1);
		strncpy(m->tool_name, calls[i].name, sizeof(m->tool_name) - 1);
	}

	/* Now re-query with "continue" (empty user message triggers continuation) */
	return llm_chat(cfg, ctx, "", tools_json, resp_out, stream_cb, userdata);
}

void llm_response_free(struct llm_response *resp)
{
	if (!resp)
		return;
	free(resp->text);
	free(resp->tool_calls);
	free(resp);
}
