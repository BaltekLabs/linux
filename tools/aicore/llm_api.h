/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tools/aicore/llm_api.h - LLM API client interface
 *
 * Supports:
 *   - Anthropic Claude API (messages endpoint)
 *   - OpenAI-compatible API (local: Ollama, llama.cpp, LM Studio, etc.)
 *
 * All requests include the tool schema so the LLM can call OpenClaw tools.
 * Streaming responses are supported via a callback.
 */
#ifndef AICORE_LLM_API_H
#define AICORE_LLM_API_H

#include <stddef.h>
#include <stdbool.h>
#include "config.h"

/* Max conversation history entries */
#define LLM_MAX_HISTORY		64
#define LLM_MAX_MSG_LEN		(256 * 1024)

/* Message roles */
typedef enum {
	MSG_ROLE_USER      = 0,
	MSG_ROLE_ASSISTANT = 1,
	MSG_ROLE_TOOL      = 2,		/* Tool result (Anthropic format) */
} msg_role_t;

/**
 * struct llm_message - One turn in a conversation
 */
struct llm_message {
	msg_role_t  role;
	char       *content;		/* Allocated string, caller frees */
	char        tool_use_id[64];	/* For tool results: correlates to tool_use */
	char        tool_name[64];	/* For tool results: which tool was called */
};

/**
 * struct llm_context - Full conversation context
 */
struct llm_context {
	struct llm_message history[LLM_MAX_HISTORY];
	int    count;
	int    max_depth;		/* From config.history_depth */
};

/**
 * struct llm_tool_call - Describes a tool invocation requested by the AI
 */
struct llm_tool_call {
	char tool_use_id[64];		/* Unique ID for correlation */
	char name[64];			/* Tool/claw name */
	char input[64 * 1024];		/* JSON input parameters */
};

/**
 * struct llm_response - Parsed response from the LLM
 */
struct llm_response {
	char        *text;		/* Text response (allocated, may be NULL) */
	struct llm_tool_call *tool_calls; /* Array of tool calls (may be NULL) */
	int          num_tool_calls;
	bool         stop_reason_tool;	/* Stopped because of tool use */
	int          input_tokens;
	int          output_tokens;
};

/* Stream token callback: called for each streamed text chunk */
typedef void (*llm_stream_cb_t)(const char *chunk, size_t len, void *userdata);

/* Initialize LLM client (validates config, sets up curl) */
int llm_init(const struct aicore_config *cfg);
void llm_cleanup(void);

/**
 * llm_chat() - Send a conversation turn and get a response
 *
 * @cfg:      Configuration (API key, model, etc.)
 * @ctx:      Conversation history (updated in place with this turn)
 * @query:    User query to append
 * @tools_json: JSON array of available tools (from tools_build_schema())
 * @resp:     Output: allocated response (caller must call llm_response_free())
 * @stream_cb: Optional streaming callback (NULL for blocking mode)
 * @userdata: Passed to stream_cb
 *
 * Returns 0 on success, negative errno on failure.
 */
int llm_chat(const struct aicore_config *cfg,
	     struct llm_context *ctx,
	     const char *query,
	     const char *tools_json,
	     struct llm_response **resp,
	     llm_stream_cb_t stream_cb,
	     void *userdata);

/**
 * llm_tool_result() - Submit tool results and continue the conversation
 *
 * After receiving tool_calls in a response, execute them and call this
 * to feed results back to the LLM and get its final answer.
 */
int llm_tool_result(const struct aicore_config *cfg,
		    struct llm_context *ctx,
		    const char *tools_json,
		    struct llm_tool_call *calls,
		    int num_calls,
		    const char **results,	/* One JSON result per call */
		    struct llm_response **resp,
		    llm_stream_cb_t stream_cb,
		    void *userdata);

/* Free an llm_response struct */
void llm_response_free(struct llm_response *resp);

/* Context management */
void llm_context_init(struct llm_context *ctx, int max_depth);
void llm_context_free(struct llm_context *ctx);
void llm_context_add(struct llm_context *ctx, msg_role_t role,
		     const char *content);

#endif /* AICORE_LLM_API_H */
