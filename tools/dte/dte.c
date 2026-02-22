// SPDX-License-Identifier: GPL-2.0
/*
 * tools/dte/dte.c - Developer Terminal Environment
 *
 * A tiled, terminal-based AI chat interface for the Linux AI subsystem.
 * DTE is the primary user interface for interacting with the natively
 * integrated AI. It replaces the traditional "desktop" concept with an
 * AI-first interaction model.
 *
 * Key bindings:
 *   Enter        - Send query to AI
 *   Ctrl+C/Q     - Quit
 *   Tab          - Cycle focused pane
 *   Ctrl+S       - Toggle side pane (output/sysinfo)
 *   PgUp/PgDn    - Scroll focused pane
 *   End          - Scroll to bottom
 *   Ctrl+L       - Clear chat history (new conversation)
 *   Ctrl+P       - Ping daemon (check connection)
 *   Ctrl+R       - Reload system info in side pane
 *   F1           - Show help
 *   F2           - Toggle sysinfo side pane
 *   F3           - Toggle logs side pane
 *   Up/Down      - Navigate input history
 *
 * Usage:
 *   dte [--socket PATH] [--model MODEL] [--help]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <ncurses.h>
#include <pthread.h>

#include "window.h"
#include "chat.h"

#define DTE_VERSION		"0.1.0"
#define INPUT_HISTORY_MAX	256
#define PROMPT_STR		"> "
#define PROMPT_LEN		2

/* ===== DTE state ===== */

struct dte_state {
	struct wm_state		wm;		/* Window manager */
	struct chat_state	chat;		/* AI chat connection */

	/* Input line */
	char		input_buf[MAX_INPUT_LEN];
	size_t		input_len;
	size_t		input_cursor;

	/* Input history */
	char		history[INPUT_HISTORY_MAX][MAX_INPUT_LEN];
	int		history_count;
	int		history_pos;	/* -1 = current input */

	/* Status */
	bool		waiting_for_ai;
	char		status_left[256];
	char		status_right[256];

	/* Socket path */
	char		socket_path[256];

	/* Signals */
	volatile bool	resize_pending;
	volatile bool	quit;
};

static struct dte_state g_dte;

/* ===== Signal handlers ===== */

static void sig_winch(int s)
{
	(void)s;
	g_dte.resize_pending = true;
}

static void sig_term(int s)
{
	(void)s;
	g_dte.quit = true;
}

/* ===== Chat event callback (called from reader thread) ===== */

static void on_chat_event(const struct chat_event *evt, void *userdata)
{
	struct dte_state *dte = userdata;
	struct pane *chat_pane = wm_chat_pane(&dte->wm);

	if (!chat_pane)
		return;

	switch (evt->type) {
	case CHAT_EVENT_CONNECTED:
		pane_add_line(chat_pane,
			      "-- Connected to aicore daemon --",
			      COLOR_PAIR_SYSTEM, false);
		snprintf(dte->status_left, sizeof(dte->status_left),
			 "DTE v" DTE_VERSION " | connected");
		break;

	case CHAT_EVENT_DISCONNECTED:
		pane_add_line(chat_pane,
			      "-- Disconnected from aicore daemon --",
			      COLOR_PAIR_ERROR, true);
		dte->waiting_for_ai = false;
		snprintf(dte->status_left, sizeof(dte->status_left),
			 "DTE v" DTE_VERSION " | disconnected");
		break;

	case CHAT_EVENT_STREAM_TOKEN:
		/*
		 * For streaming: we accumulate tokens and update the last
		 * line in the chat pane to show progressive output.
		 * Simple implementation: add token as a new partial line.
		 */
		if (chat_pane->num_lines > 0) {
			struct pane_line *last =
				&chat_pane->lines[chat_pane->num_lines - 1];
			/* Append to last line if it's an AI line */
			size_t cur_len = strlen(last->text);
			size_t tok_len = strlen(evt->text);
			if (cur_len + tok_len < MAX_LINE_LEN - 1) {
				strncat(last->text, evt->text, MAX_LINE_LEN - cur_len - 1);
				chat_pane->dirty = true;
			} else {
				/* Line full: start new line */
				pane_add_line(chat_pane, evt->text,
					      COLOR_PAIR_AI, false);
			}
		} else {
			pane_add_line(chat_pane, evt->text, COLOR_PAIR_AI, false);
		}
		break;

	case CHAT_EVENT_RESPONSE_DONE: {
		/* Final response: display it cleanly if not already streamed */
		if (strlen(evt->text) > 0) {
			/* Split response into lines */
			char *text = strdup(evt->text);
			if (text) {
				char *line = strtok(text, "\n");
				while (line) {
					pane_add_line(chat_pane, line,
						      COLOR_PAIR_AI, false);
					line = strtok(NULL, "\n");
				}
				free(text);
			}
		}

		pane_add_line(chat_pane, "", COLOR_PAIR_DEFAULT, false);
		dte->waiting_for_ai = false;

		time_t now = time(NULL);
		struct tm *tm = localtime(&now);
		snprintf(dte->status_right, sizeof(dte->status_right),
			 "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
		break;
	}

	case CHAT_EVENT_ERROR: {
		char errmsg[256];
		snprintf(errmsg, sizeof(errmsg), "[Error] %s", evt->text);
		pane_add_line(chat_pane, errmsg, COLOR_PAIR_ERROR, true);
		dte->waiting_for_ai = false;
		break;
	}

	case CHAT_EVENT_SYSTEM: {
		char sysmsg[512];
		snprintf(sysmsg, sizeof(sysmsg), "[System] %s", evt->text);
		pane_add_line(chat_pane, sysmsg, COLOR_PAIR_SYSTEM, false);
		break;
	}

	default:
		break;
	}

	/* Trigger a redraw */
	wm_scroll_bottom(&dte->wm);
}

/* ===== Input rendering ===== */

static void render_input(struct dte_state *dte)
{
	WINDOW *win = dte->wm.input_win;
	if (!win)
		return;

	werase(win);
	wbkgd(win, COLOR_PAIR(COLOR_PAIR_INPUT));
	wattron(win, COLOR_PAIR(COLOR_PAIR_INPUT) | A_BOLD);

	if (dte->waiting_for_ai) {
		/* Show spinner/indicator while waiting */
		static const char spinners[] = "|/-\\";
		static int spin_idx = 0;
		mvwprintw(win, 0, 0, "[%c] AI thinking... ", spinners[spin_idx++ % 4]);
	} else {
		mvwprintw(win, 0, 0, PROMPT_STR);
		wattron(win, COLOR_PAIR(COLOR_PAIR_DEFAULT));
		/* Display input text */
		int avail = dte->wm.term_cols - PROMPT_LEN - 1;
		if (avail > 0) {
			/* Show end of input if cursor is near end */
			int start = 0;
			if ((int)dte->input_cursor > avail - 1)
				start = (int)dte->input_cursor - avail + 1;
			mvwaddnstr(win, 0, PROMPT_LEN,
				   dte->input_buf + start,
				   avail);
			wmove(win, 0, PROMPT_LEN + (int)dte->input_cursor - start);
		}
	}

	wattroff(win, A_BOLD | COLOR_PAIR(COLOR_PAIR_INPUT));
	wnoutrefresh(win);
}

/* ===== Input history ===== */

static void history_push(struct dte_state *dte, const char *line)
{
	if (!line || !*line)
		return;

	/* Don't add duplicate of last entry */
	if (dte->history_count > 0 &&
	    strcmp(dte->history[dte->history_count - 1], line) == 0)
		return;

	int idx = dte->history_count % INPUT_HISTORY_MAX;
	strncpy(dte->history[idx], line, MAX_INPUT_LEN - 1);
	dte->history_count++;
	dte->history_pos = -1;
}

static void history_prev(struct dte_state *dte)
{
	if (dte->history_count == 0)
		return;

	if (dte->history_pos < 0)
		dte->history_pos = dte->history_count - 1;
	else if (dte->history_pos > 0)
		dte->history_pos--;
	else
		return;

	int idx = dte->history_pos % INPUT_HISTORY_MAX;
	strncpy(dte->input_buf, dte->history[idx], MAX_INPUT_LEN - 1);
	dte->input_len    = strlen(dte->input_buf);
	dte->input_cursor = dte->input_len;
}

static void history_next(struct dte_state *dte)
{
	if (dte->history_pos < 0)
		return;

	dte->history_pos++;
	if (dte->history_pos >= dte->history_count) {
		dte->history_pos  = -1;
		dte->input_buf[0] = '\0';
		dte->input_len    = 0;
		dte->input_cursor = 0;
	} else {
		int idx = dte->history_pos % INPUT_HISTORY_MAX;
		strncpy(dte->input_buf, dte->history[idx], MAX_INPUT_LEN - 1);
		dte->input_len    = strlen(dte->input_buf);
		dte->input_cursor = dte->input_len;
	}
}

/* ===== Send query ===== */

static void send_query(struct dte_state *dte)
{
	if (!dte->input_buf[0] || dte->waiting_for_ai)
		return;

	struct pane *chat_pane = wm_chat_pane(&dte->wm);
	if (!chat_pane)
		return;

	/* Display user message */
	char user_line[MAX_INPUT_LEN + 8];
	snprintf(user_line, sizeof(user_line), "You: %s", dte->input_buf);
	pane_add_line(chat_pane, user_line, COLOR_PAIR_USER, true);
	pane_add_line(chat_pane, "", COLOR_PAIR_DEFAULT, false);

	/* Add placeholder for streaming response */
	pane_add_line(chat_pane, "AI: ", COLOR_PAIR_AI, false);

	history_push(dte, dte->input_buf);

	int ret = chat_send_query(&dte->chat, dte->input_buf, true);
	if (ret < 0) {
		char errmsg[128];
		snprintf(errmsg, sizeof(errmsg), "[Error] send failed: %s",
			 strerror(-ret));
		pane_add_line(chat_pane, errmsg, COLOR_PAIR_ERROR, true);
	} else {
		dte->waiting_for_ai = true;
		snprintf(dte->status_left, sizeof(dte->status_left),
			 "DTE | waiting for AI...");
	}

	/* Clear input */
	dte->input_buf[0] = '\0';
	dte->input_len    = 0;
	dte->input_cursor = 0;

	wm_scroll_bottom(&dte->wm);
}

/* ===== Help display ===== */

static void show_help(struct dte_state *dte)
{
	struct pane *p = wm_chat_pane(&dte->wm);
	if (!p)
		return;

	pane_add_line(p, "=== DTE Key Bindings ===", COLOR_PAIR_SYSTEM, true);
	pane_add_line(p, "  Enter      Send query to AI",     COLOR_PAIR_SYSTEM, false);
	pane_add_line(p, "  Ctrl+Q/C   Quit DTE",             COLOR_PAIR_SYSTEM, false);
	pane_add_line(p, "  Tab        Cycle pane focus",      COLOR_PAIR_SYSTEM, false);
	pane_add_line(p, "  Ctrl+S     Toggle side pane",      COLOR_PAIR_SYSTEM, false);
	pane_add_line(p, "  PgUp/PgDn  Scroll focused pane",   COLOR_PAIR_SYSTEM, false);
	pane_add_line(p, "  End        Scroll to bottom",      COLOR_PAIR_SYSTEM, false);
	pane_add_line(p, "  Ctrl+L     Clear conversation",    COLOR_PAIR_SYSTEM, false);
	pane_add_line(p, "  Up/Down    Input history",         COLOR_PAIR_SYSTEM, false);
	pane_add_line(p, "  F1         This help",             COLOR_PAIR_SYSTEM, false);
	pane_add_line(p, "  F2         Sysinfo side pane",     COLOR_PAIR_SYSTEM, false);
	pane_add_line(p, "  F3         Logs side pane",        COLOR_PAIR_SYSTEM, false);
	pane_add_line(p, "", COLOR_PAIR_DEFAULT, false);
	p->dirty = true;
}

/* ===== Sysinfo pane refresh ===== */

static void refresh_sysinfo(struct dte_state *dte)
{
	struct pane *side = wm_get_side_pane(&dte->wm, PANE_SYSINFO);
	if (!side)
		return;

	side->num_lines  = 0;
	side->scroll_pos = 0;

	FILE *f = fopen("/proc/ai/resources", "r");
	if (f) {
		char line[256];
		while (fgets(line, sizeof(line), f)) {
			size_t len = strlen(line);
			while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
				line[--len] = '\0';
			pane_add_line(side, line, COLOR_PAIR_SYSTEM, false);
		}
		fclose(f);
	} else {
		pane_add_line(side, "(/proc/ai not available)", COLOR_PAIR_ERROR, false);
	}

	/* Also show uptime from /proc/ai/resources */
	side->dirty = true;
}

/* ===== Main input loop ===== */

static void handle_key(struct dte_state *dte, int ch)
{
	switch (ch) {

	case KEY_ENTER:
	case '\n':
	case '\r':
		send_query(dte);
		break;

	case 'q' & 0x1f:	/* Ctrl+Q */
	case 3:			/* Ctrl+C */
		dte->quit = true;
		break;

	case '\t':		/* Tab - cycle panes */
		wm_focus_next(&dte->wm);
		break;

	case 19:		/* Ctrl+S - toggle side pane */
		wm_toggle_side(&dte->wm);
		break;

	case 12:		/* Ctrl+L - clear chat */
		if (wm_chat_pane(&dte->wm)) {
			struct pane *p = wm_chat_pane(&dte->wm);
			p->num_lines  = 0;
			p->scroll_pos = 0;
			p->dirty      = true;
			pane_add_line(p, "-- Conversation cleared --",
				      COLOR_PAIR_SYSTEM, false);
		}
		break;

	case 16:		/* Ctrl+P - ping */
		chat_ping(&dte->chat);
		break;

	case 18:		/* Ctrl+R - refresh sysinfo */
		refresh_sysinfo(dte);
		break;

	case KEY_F(1):
		show_help(dte);
		break;

	case KEY_F(2):
		wm_get_side_pane(&dte->wm, PANE_SYSINFO);
		refresh_sysinfo(dte);
		break;

	case KEY_F(3):
		wm_get_side_pane(&dte->wm, PANE_LOGS);
		break;

	case KEY_PPAGE:		/* PgUp */
		wm_scroll_up(&dte->wm, (dte->wm.panes[0].rows - 4) / 2);
		break;

	case KEY_NPAGE:		/* PgDn */
		wm_scroll_down(&dte->wm, (dte->wm.panes[0].rows - 4) / 2);
		break;

	case KEY_END:
		wm_scroll_bottom(&dte->wm);
		break;

	case KEY_UP:
		history_prev(dte);
		break;

	case KEY_DOWN:
		history_next(dte);
		break;

	case KEY_LEFT:
		if (dte->input_cursor > 0)
			dte->input_cursor--;
		break;

	case KEY_RIGHT:
		if (dte->input_cursor < dte->input_len)
			dte->input_cursor++;
		break;

	case KEY_HOME:
		dte->input_cursor = 0;
		break;

	case KEY_BACKSPACE:
	case 127:
	case 8:
		if (dte->input_cursor > 0 && !dte->waiting_for_ai) {
			memmove(dte->input_buf + dte->input_cursor - 1,
				dte->input_buf + dte->input_cursor,
				dte->input_len - dte->input_cursor + 1);
			dte->input_len--;
			dte->input_cursor--;
		}
		break;

	case KEY_DC:		/* Delete */
		if (dte->input_cursor < dte->input_len && !dte->waiting_for_ai) {
			memmove(dte->input_buf + dte->input_cursor,
				dte->input_buf + dte->input_cursor + 1,
				dte->input_len - dte->input_cursor);
			dte->input_len--;
		}
		break;

	default:
		/* Printable character - insert at cursor */
		if (ch >= 32 && ch < 0x100 && !dte->waiting_for_ai &&
		    dte->input_len < MAX_INPUT_LEN - 1) {
			memmove(dte->input_buf + dte->input_cursor + 1,
				dte->input_buf + dte->input_cursor,
				dte->input_len - dte->input_cursor + 1);
			dte->input_buf[dte->input_cursor] = (char)ch;
			dte->input_len++;
			dte->input_cursor++;
		}
		break;
	}
}

/* ===== Entry point ===== */

static void print_usage(const char *prog)
{
	printf("Usage: %s [OPTIONS]\n"
	       "\n"
	       "DTE - Developer Terminal Environment for Linux AI subsystem\n"
	       "\n"
	       "Options:\n"
	       "  --socket PATH   aicore socket (default: /run/aicore.sock)\n"
	       "  --help          Show this help\n"
	       "\n"
	       "DTE connects to the aicore daemon and provides a tiled terminal\n"
	       "chat interface for interacting with the natively integrated AI.\n"
	       "The AI has full system access via OpenClaw tools.\n",
	       prog);
}

int main(int argc, char *argv[])
{
	const char *socket_path = CHAT_SOCKET_PATH;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
			socket_path = argv[++i];
		else if (strcmp(argv[i], "--help") == 0) {
			print_usage(argv[0]);
			return 0;
		}
	}

	memset(&g_dte, 0, sizeof(g_dte));
	strncpy(g_dte.socket_path, socket_path, sizeof(g_dte.socket_path) - 1);
	g_dte.history_pos = -1;

	/* Signal setup */
	signal(SIGWINCH, sig_winch);
	signal(SIGTERM,  sig_term);
	signal(SIGINT,   sig_term);
	signal(SIGPIPE,  SIG_IGN);

	/* Initialize window manager */
	if (wm_init(&g_dte.wm) < 0) {
		fprintf(stderr, "dte: failed to initialize TUI\n");
		return 1;
	}

	/* Welcome message */
	struct pane *chat = wm_chat_pane(&g_dte.wm);
	if (chat) {
		pane_add_line(chat,
			      "DTE v" DTE_VERSION " - Linux AI Developer Terminal",
			      COLOR_PAIR_SYSTEM, true);
		pane_add_line(chat,
			      "Press F1 for help. Type a message and press Enter.",
			      COLOR_PAIR_SYSTEM, false);
		pane_add_line(chat, "", COLOR_PAIR_DEFAULT, false);
		pane_add_line(chat,
			      "Connecting to aicore daemon...",
			      COLOR_PAIR_SYSTEM, false);
		chat->dirty = true;
	}

	wm_set_status(&g_dte.wm,
		      "DTE v" DTE_VERSION " | connecting...",
		      "F1=Help Ctrl+Q=Quit");
	wm_refresh(&g_dte.wm);

	/* Connect to aicore */
	int ret = chat_connect(&g_dte.chat, socket_path, on_chat_event, &g_dte);
	if (ret < 0) {
		if (chat)
			pane_add_line(chat,
				      "[Warning] Cannot connect to aicore daemon. "
				      "Start it with: sudo aicore",
				      COLOR_PAIR_ERROR, true);
	}

	/* Set input window for getch */
	wtimeout(g_dte.wm.input_win, 50); /* 50ms timeout for animations */

	/* Main loop */
	while (!g_dte.quit) {
		/* Handle resize */
		if (g_dte.resize_pending) {
			g_dte.resize_pending = false;
			wm_resize(&g_dte.wm);
		}

		/* Update status bar */
		wm_set_status(&g_dte.wm, g_dte.status_left,
			      g_dte.status_right[0] ? g_dte.status_right
						    : "F1=Help ^Q=Quit ^S=Split");

		/* Render */
		wm_refresh(&g_dte.wm);
		render_input(&g_dte);
		doupdate();

		/* Get input */
		int ch = wgetch(g_dte.wm.input_win);
		if (ch == ERR)
			continue; /* Timeout - loop for animation */

		handle_key(&g_dte, ch);
	}

	/* Cleanup */
	chat_disconnect(&g_dte.chat);
	wm_cleanup(&g_dte.wm);

	printf("DTE: goodbye.\n");
	return 0;
}
