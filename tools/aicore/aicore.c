// SPDX-License-Identifier: GPL-2.0
/*
 * tools/aicore/aicore.c - Linux AI Core Daemon
 *
 * The aicore daemon is the central broker between:
 *   - /dev/ai (kernel AI subsystem)
 *   - Unix domain socket /run/aicore.sock (DTE and other clients)
 *   - LLM API (Anthropic Claude or local OpenAI-compatible)
 *   - OpenClaw tool executor (tools.c)
 *
 * Architecture:
 *
 *   1. Opens /dev/ai and registers as the AI daemon (AI_IOC_SET_DAEMON)
 *   2. Listens on /run/aicore.sock for DTE/client connections
 *   3. Main event loop (poll/select) handles:
 *        - Incoming queries from socket clients and /dev/ai
 *        - LLM API calls (with tool use loop)
 *        - Tool execution (OpenClaw dispatch)
 *        - Response routing back to originators
 *
 * Each client connection gets a dedicated conversation context, enabling
 * multi-turn AI interactions with full history.
 *
 * Usage:
 *   aicore [--config /etc/aicore/config.json] [--daemon] [--debug]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <time.h>

#include "config.h"
#include "tools.h"
#include "llm_api.h"
#include "../../include/uapi/linux/ai.h"

#define MAX_CLIENTS		64
#define SOCKET_BACKLOG		16
#define AI_MSG_BUF_SIZE		(AI_MAX_MSG_SIZE + sizeof(struct ai_message))

/* ===== Logging ===== */

static int g_log_level = 2;
static FILE *g_logfile;

#define log_error(fmt, ...) do { \
	if (g_log_level >= 0) { \
		fprintf(g_logfile ? g_logfile : stderr, \
			"[ERROR] " fmt "\n", ##__VA_ARGS__); \
		fflush(g_logfile ? g_logfile : stderr); \
	} \
} while (0)

#define log_warn(fmt, ...) do { \
	if (g_log_level >= 1) { \
		fprintf(g_logfile ? g_logfile : stderr, \
			"[WARN]  " fmt "\n", ##__VA_ARGS__); \
	} \
} while (0)

#define log_info(fmt, ...) do { \
	if (g_log_level >= 2) { \
		fprintf(g_logfile ? g_logfile : stderr, \
			"[INFO]  " fmt "\n", ##__VA_ARGS__); \
	} \
} while (0)

#define log_debug(fmt, ...) do { \
	if (g_log_level >= 3) { \
		fprintf(g_logfile ? g_logfile : stderr, \
			"[DEBUG] " fmt "\n", ##__VA_ARGS__); \
	} \
} while (0)

/* ===== Client state ===== */

struct client_conn {
	int			fd;
	int			is_kernel;	/* 1 = /dev/ai, 0 = Unix socket */
	struct llm_context	ctx;
	uint32_t		session;
	time_t			last_active;
	char			read_buf[AI_MSG_BUF_SIZE];
	size_t			read_used;
};

/* ===== Global state ===== */

static volatile sig_atomic_t g_running = 1;
static struct aicore_config  g_cfg;
static int		     g_ai_fd   = -1;
static int		     g_sock_fd = -1;
static struct client_conn    g_clients[MAX_CLIENTS];
static int		     g_num_clients;
static pthread_mutex_t	     g_client_lock = PTHREAD_MUTEX_INITIALIZER;

/* ===== Signal handling ===== */

static void sig_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

/* ===== System context enrichment ===== */

/*
 * Build a system context prefix for AI queries by reading /proc/ai/ files.
 * This gives the AI situational awareness without it needing to ask.
 */
static void build_system_context(char *out, size_t out_len)
{
	char sysinfo[4096]   = {0};
	char resources[2048] = {0};
	FILE *f;

	if (g_cfg.include_sysinfo) {
		f = fopen("/proc/ai/sysinfo", "r");
		if (f) {
			fread(sysinfo, 1, sizeof(sysinfo) - 1, f);
			fclose(f);
		}
	}

	if (g_cfg.include_resources) {
		f = fopen("/proc/ai/resources", "r");
		if (f) {
			fread(resources, 1, sizeof(resources) - 1, f);
			fclose(f);
		}
	}

	if (sysinfo[0] || resources[0]) {
		snprintf(out, out_len,
			 "[System Context]\nSysinfo: %s\nResources: %s\n\n",
			 sysinfo[0] ? sysinfo : "unavailable",
			 resources[0] ? resources : "unavailable");
	} else {
		out[0] = '\0';
	}
}

/* ===== Streaming callback for DTE ===== */

struct stream_ctx {
	int	 client_fd;
	uint32_t msg_id;
	uint32_t session;
	int	 is_kernel;
};

static void stream_cb(const char *chunk, size_t len, void *userdata)
{
	struct stream_ctx *sctx = userdata;

	if (!chunk || len == 0 || sctx->client_fd < 0)
		return;

	/* Build a streaming AI_MSG_STREAM message */
	size_t total = sizeof(struct ai_message) + len + 1;
	struct ai_message *msg = malloc(total);
	if (!msg)
		return;

	msg->magic    = AI_MAGIC;
	msg->type     = AI_MSG_STREAM;
	msg->id       = sctx->msg_id;
	msg->flags    = AI_FLAG_DAEMON;
	msg->uid      = 0;
	msg->pid      = 0;
	msg->session  = sctx->session;
	msg->data_len = len;
	memset(msg->reserved, 0, sizeof(msg->reserved));
	memcpy(msg->data, chunk, len);
	msg->data[len] = '\0';

	write(sctx->client_fd, msg, total);
	free(msg);
}

/* ===== Send response back to client ===== */

static void send_response(int fd, uint32_t msg_id, uint32_t session,
			  const char *text, bool is_final)
{
	if (!text)
		text = "";

	size_t text_len = strlen(text);
	size_t total    = sizeof(struct ai_message) + text_len + 1;
	struct ai_message *msg = malloc(total);
	if (!msg)
		return;

	msg->magic    = AI_MAGIC;
	msg->type     = is_final ? AI_MSG_RESPONSE : AI_MSG_STREAM;
	msg->id       = msg_id;
	msg->flags    = AI_FLAG_DAEMON | AI_FLAG_COMPLETE;
	msg->uid      = 0;
	msg->pid      = 0;
	msg->session  = session;
	msg->data_len = text_len;
	memset(msg->reserved, 0, sizeof(msg->reserved));
	memcpy(msg->data, text, text_len);
	msg->data[text_len] = '\0';

	if (write(fd, msg, total) < 0)
		log_warn("send_response: write fd=%d: %s", fd, strerror(errno));
	free(msg);
}

static void send_error(int fd, uint32_t msg_id, uint32_t session,
		       const char *errmsg)
{
	char json[1024];
	snprintf(json, sizeof(json), "{\"error\":\"%s\"}", errmsg);
	send_response(fd, msg_id, session, json, true);
}

/* ===== Core AI query processing ===== */

/**
 * process_query() - Run a full AI query with tool use loop
 *
 * This implements the agentic loop:
 *   1. Send query to LLM with tool schema
 *   2. If LLM returns tool calls, execute them
 *   3. Feed tool results back to LLM
 *   4. Repeat until LLM returns text with no tool calls
 *   5. Send final response to client
 */
static void process_query(struct client_conn *conn, int reply_fd,
			  uint32_t msg_id, const char *query)
{
	char tools_json[32 * 1024] = {0};
	char sys_ctx[4096]          = {0};
	char full_query[LLM_MAX_MSG_LEN] = {0};
	struct llm_response *resp   = NULL;
	int  ret;
	int  tool_iterations         = 0;
	const int max_tool_iterations = 10;

	struct stream_ctx sctx = {
		.client_fd = reply_fd,
		.msg_id    = msg_id,
		.session   = conn->session,
		.is_kernel = conn->is_kernel,
	};

	/* Build tool schema */
	tools_build_schema(tools_json, sizeof(tools_json), &g_cfg);

	/* Prepend system context if enabled */
	build_system_context(sys_ctx, sizeof(sys_ctx));
	if (sys_ctx[0])
		snprintf(full_query, sizeof(full_query), "%s%s", sys_ctx, query);
	else
		strncpy(full_query, query, sizeof(full_query) - 1);

	log_info("query [session=%u id=%u]: %.80s%s",
		 conn->session, msg_id, query,
		 strlen(query) > 80 ? "..." : "");

	/* Initial LLM call */
	ret = llm_chat(&g_cfg, &conn->ctx, full_query, tools_json,
		       &resp, stream_cb, &sctx);

	if (ret < 0) {
		log_error("llm_chat failed: %s", strerror(-ret));
		send_error(reply_fd, msg_id, conn->session,
			   "LLM API request failed");
		return;
	}

	/* Agentic tool-use loop */
	while (resp && resp->stop_reason_tool &&
	       resp->num_tool_calls > 0 &&
	       tool_iterations++ < max_tool_iterations) {

		log_info("tool use: %d tool call(s) requested",
			 resp->num_tool_calls);

		const char **results = calloc(resp->num_tool_calls, sizeof(char *));
		if (!results) {
			llm_response_free(resp);
			send_error(reply_fd, msg_id, conn->session, "OOM");
			return;
		}

		/* Execute each tool call */
		for (int i = 0; i < resp->num_tool_calls; i++) {
			struct llm_tool_call *tc = &resp->tool_calls[i];
			char *result_buf = malloc(TOOL_RESULT_MAX);
			if (!result_buf) {
				results[i] = strdup("{\"error\":\"OOM\"}");
				continue;
			}

			log_info("  claw: %s(%s)", tc->name, tc->input);

			ret = tools_execute(tc->name, tc->input,
					    result_buf, TOOL_RESULT_MAX, &g_cfg);
			if (ret < 0)
				log_warn("  claw %s failed: %d", tc->name, ret);
			else
				log_debug("  claw result: %.120s", result_buf);

			results[i] = result_buf;
		}

		/* Feed results back to LLM */
		struct llm_response *next_resp = NULL;
		ret = llm_tool_result(&g_cfg, &conn->ctx, tools_json,
				      resp->tool_calls, resp->num_tool_calls,
				      results, &next_resp, NULL, NULL);

		for (int i = 0; i < resp->num_tool_calls; i++)
			free((char *)results[i]);
		free(results);

		llm_response_free(resp);
		resp = next_resp;

		if (ret < 0) {
			send_error(reply_fd, msg_id, conn->session,
				   "tool result submission failed");
			return;
		}
	}

	/* Send final response */
	if (resp && resp->text) {
		log_info("response [%d tok]: %.80s%s",
			 resp->output_tokens, resp->text,
			 strlen(resp->text) > 80 ? "..." : "");
		send_response(reply_fd, msg_id, conn->session, resp->text, true);
	} else {
		send_error(reply_fd, msg_id, conn->session,
			   "no response from LLM");
	}

	llm_response_free(resp);
}

/* ===== Per-query thread ===== */

struct query_args {
	struct client_conn *conn;
	int		    reply_fd;
	uint32_t	    msg_id;
	char		    query[LLM_MAX_MSG_LEN];
};

static void *query_thread(void *arg)
{
	struct query_args *qa = arg;

	process_query(qa->conn, qa->reply_fd, qa->msg_id, qa->query);
	free(qa);
	return NULL;
}

static void dispatch_query_async(struct client_conn *conn, int reply_fd,
				 uint32_t msg_id, const char *query)
{
	pthread_t tid;
	struct query_args *qa = malloc(sizeof(*qa));
	if (!qa)
		return;

	qa->conn     = conn;
	qa->reply_fd = reply_fd;
	qa->msg_id   = msg_id;
	strncpy(qa->query, query, sizeof(qa->query) - 1);
	qa->query[sizeof(qa->query) - 1] = '\0';

	pthread_create(&tid, NULL, query_thread, qa);
	pthread_detach(tid);
}

/* ===== Message handling ===== */

static void handle_message(struct client_conn *conn, struct ai_message *msg,
			   int reply_fd)
{
	if (msg->magic != AI_MAGIC) {
		log_warn("bad magic: 0x%08x from fd=%d", msg->magic, conn->fd);
		return;
	}

	switch (msg->type) {
	case AI_MSG_QUERY:
		if (msg->data_len > 0) {
			msg->data[msg->data_len] = '\0';
			dispatch_query_async(conn, reply_fd, msg->id, msg->data);
		}
		break;

	case AI_MSG_PING:
		send_response(reply_fd, msg->id, conn->session,
			      "{\"pong\":true}", true);
		break;

	case AI_MSG_SHUTDOWN:
		log_info("shutdown requested by client fd=%d", conn->fd);
		g_running = 0;
		break;

	default:
		log_debug("unhandled msg type=0x%04x from fd=%d",
			  msg->type, conn->fd);
	}
}

/* ===== Client I/O ===== */

static int read_and_handle(struct client_conn *conn)
{
	ssize_t n;

	n = read(conn->fd, conn->read_buf + conn->read_used,
		 sizeof(conn->read_buf) - conn->read_used - 1);
	if (n <= 0) {
		if (n == 0 || errno == ECONNRESET || errno == EPIPE)
			return -1; /* Client disconnected */
		if (errno == EAGAIN || errno == EINTR)
			return 0;
		return -1;
	}

	conn->read_used += n;
	conn->last_active = time(NULL);

	/* Process complete messages */
	while (conn->read_used >= sizeof(struct ai_message)) {
		struct ai_message *msg = (struct ai_message *)conn->read_buf;
		size_t total = sizeof(struct ai_message) + msg->data_len;

		if (msg->magic != AI_MAGIC) {
			conn->read_used = 0; /* Desync: flush */
			break;
		}

		if (msg->data_len > AI_MAX_MSG_SIZE) {
			conn->read_used = 0;
			break;
		}

		if (conn->read_used < total)
			break; /* Need more data */

		/* We have a complete message */
		handle_message(conn, msg, conn->fd);

		/* Shift remaining data */
		conn->read_used -= total;
		if (conn->read_used > 0)
			memmove(conn->read_buf, conn->read_buf + total,
				conn->read_used);
	}

	return 0;
}

/* ===== Socket server ===== */

static int setup_socket(const char *path)
{
	struct sockaddr_un addr;
	int fd, flags;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		log_error("socket: %s", strerror(errno));
		return -1;
	}

	flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	unlink(path); /* Remove stale socket */
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		log_error("bind %s: %s", path, strerror(errno));
		close(fd);
		return -1;
	}

	chmod(path, 0666); /* Allow all users to connect */

	if (listen(fd, SOCKET_BACKLOG) < 0) {
		log_error("listen: %s", strerror(errno));
		close(fd);
		return -1;
	}

	log_info("listening on %s", path);
	return fd;
}

static int add_client(int fd, int is_kernel)
{
	if (g_num_clients >= MAX_CLIENTS) {
		log_warn("max clients reached");
		return -1;
	}

	struct client_conn *c = &g_clients[g_num_clients++];
	memset(c, 0, sizeof(*c));
	c->fd          = fd;
	c->is_kernel   = is_kernel;
	c->last_active = time(NULL);

	/* Random session ID */
	c->session = (uint32_t)rand();

	llm_context_init(&c->ctx, g_cfg.history_depth);

	int flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	log_debug("new client fd=%d (kernel=%d)", fd, is_kernel);
	return 0;
}

static void remove_client(int idx)
{
	struct client_conn *c = &g_clients[idx];

	log_debug("client disconnected fd=%d", c->fd);
	llm_context_free(&c->ctx);
	close(c->fd);

	/* Compact the array */
	g_num_clients--;
	if (idx < g_num_clients)
		memmove(&g_clients[idx], &g_clients[idx + 1],
			(g_num_clients - idx) * sizeof(struct client_conn));
}

/* ===== Main event loop ===== */

static void run_event_loop(void)
{
	struct pollfd fds[MAX_CLIENTS + 2];
	int nfds;

	while (g_running) {
		nfds = 0;

		/* Socket listener */
		fds[nfds].fd     = g_sock_fd;
		fds[nfds].events = POLLIN;
		nfds++;

		/* Kernel /dev/ai fd */
		if (g_ai_fd >= 0) {
			fds[nfds].fd     = g_ai_fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}

		/* Active clients */
		for (int i = 0; i < g_num_clients; i++) {
			fds[nfds].fd     = g_clients[i].fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}

		int ret = poll(fds, nfds, 1000); /* 1s timeout */
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			log_error("poll: %s", strerror(errno));
			break;
		}

		/* New socket connection */
		if (fds[0].revents & POLLIN) {
			int cfd = accept(g_sock_fd, NULL, NULL);
			if (cfd >= 0)
				add_client(cfd, 0);
		}

		/* Kernel /dev/ai readable */
		int fds_offset = (g_ai_fd >= 0) ? 2 : 1;
		if (g_ai_fd >= 0 && (fds[1].revents & POLLIN)) {
			/* Find the kernel client */
			for (int i = 0; i < g_num_clients; i++) {
				if (g_clients[i].is_kernel) {
					if (read_and_handle(&g_clients[i]) < 0)
						remove_client(i);
					break;
				}
			}
		}

		/* Socket clients */
		for (int i = 0; i < g_num_clients && !g_clients[i].is_kernel; ) {
			int fds_idx = fds_offset + i;
			if (fds_idx < nfds && (fds[fds_idx].revents & (POLLIN | POLLHUP))) {
				if (read_and_handle(&g_clients[i]) < 0) {
					remove_client(i);
					/* Don't increment i - array compacted */
					continue;
				}
			}
			i++;
		}
	}
}

/* ===== /dev/ai setup ===== */

static int setup_kernel_ai(void)
{
	int fd;

	fd = open(g_cfg.dev_ai_path, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		log_warn("open %s: %s (kernel AI subsystem not available)",
			 g_cfg.dev_ai_path, strerror(errno));
		return -1;
	}

	/* Register as the AI daemon */
	if (ioctl(fd, AI_IOC_SET_DAEMON) < 0) {
		log_error("AI_IOC_SET_DAEMON: %s", strerror(errno));
		close(fd);
		return -1;
	}

	/* Load kernel-registered claws into tool registry */
	tools_load_kernel_claws(fd);

	log_info("registered as AI daemon on %s", g_cfg.dev_ai_path);
	return fd;
}

/* ===== Entry point ===== */

int main(int argc, char *argv[])
{
	const char *config_path = AICORE_CONFIG_DEFAULT;
	bool debug = false;
	int ret;

	/* Parse args */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
			config_path = argv[++i];
		else if (strcmp(argv[i], "--debug") == 0)
			debug = true;
		else if (strcmp(argv[i], "--daemon") == 0)
			; /* TODO: daemonize */
		else if (strcmp(argv[i], "--help") == 0) {
			printf("Usage: aicore [--config PATH] [--debug] [--daemon]\n"
			       "\n"
			       "Linux AI Core Daemon - bridges /dev/ai to LLM APIs\n"
			       "\n"
			       "Options:\n"
			       "  --config PATH   Config file (default: %s)\n"
			       "  --debug         Enable debug logging\n"
			       "  --daemon        Run as background daemon\n"
			       "\nEnvironment:\n"
			       "  AICORE_API_KEY  API key (or ANTHROPIC_API_KEY)\n"
			       "  AICORE_MODEL    Model name override\n"
			       "  AICORE_BACKEND  Backend: anthropic|openai|ollama|llamacpp\n",
			       AICORE_CONFIG_DEFAULT);
			return 0;
		}
	}

	/* Load config */
	if (config_load(&g_cfg, config_path) < 0) {
		fprintf(stderr, "aicore: failed to load config\n");
		return 1;
	}
	config_apply_env(&g_cfg);

	if (debug)
		g_cfg.log_level = 3;

	g_log_level = g_cfg.log_level;

	/* Open log file if configured */
	if (g_cfg.log_path[0] && strcmp(g_cfg.log_path, "-") != 0) {
		g_logfile = fopen(g_cfg.log_path, "a");
		if (!g_logfile)
			fprintf(stderr, "aicore: cannot open log %s\n", g_cfg.log_path);
	}
	if (!g_logfile)
		g_logfile = stderr;

	log_info("aicore starting (model=%s backend=%d)", g_cfg.model, g_cfg.backend);
	config_dump(&g_cfg);

	/* Signal handlers */
	signal(SIGINT,  sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGPIPE, SIG_IGN);

	/* Initialize subsystems */
	ret = llm_init(&g_cfg);
	if (ret < 0) {
		log_error("llm_init: %d", ret);
		return 1;
	}

	ret = tools_init(&g_cfg);
	if (ret < 0) {
		log_error("tools_init: %d", ret);
		return 1;
	}

	log_info("%d OpenClaw tools loaded", tools_count());

	/* Setup Unix socket */
	g_sock_fd = setup_socket(g_cfg.socket_path);
	if (g_sock_fd < 0) {
		log_error("failed to setup socket");
		return 1;
	}

	/* Setup kernel /dev/ai (optional - works without kernel module) */
	g_ai_fd = setup_kernel_ai();
	if (g_ai_fd >= 0)
		add_client(g_ai_fd, 1 /* is_kernel */);

	log_info("aicore ready%s",
		 g_ai_fd >= 0 ? " (kernel AI subsystem connected)" : " (userspace only)");

	/* Main event loop */
	run_event_loop();

	/* Cleanup */
	log_info("shutting down");
	llm_cleanup();

	if (g_sock_fd >= 0) {
		close(g_sock_fd);
		unlink(g_cfg.socket_path);
	}
	if (g_ai_fd >= 0)
		close(g_ai_fd);

	if (g_logfile && g_logfile != stderr)
		fclose(g_logfile);

	return 0;
}
