/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tools/dte/window.h - DTE tiled window manager
 *
 * The DTE (Developer Terminal Environment) uses a tiled window layout:
 *
 *   ┌─────────────────────────────────┬──────────────────┐
 *   │  CHAT window (main)             │  SIDE pane       │
 *   │  AI conversation history        │  (output/info)   │
 *   │  scrollable, wraps at terminal  │                  │
 *   │  width                          │                  │
 *   │                                 │                  │
 *   ├─────────────────────────────────┴──────────────────┤
 *   │  STATUS bar                                        │
 *   ├────────────────────────────────────────────────────┤
 *   │> INPUT line (prompt)                               │
 *   └────────────────────────────────────────────────────┘
 *
 * Windows can be split horizontally or vertically. Focus cycles with Tab.
 * The chat window is always primary; side panes show command output,
 * system info, or additional context.
 */
#ifndef DTE_WINDOW_H
#define DTE_WINDOW_H

#include <ncurses.h>
#include <stdbool.h>
#include <stddef.h>

#define MAX_PANES		8
#define MAX_PANE_LINES		4096
#define MAX_LINE_LEN		4096
#define MAX_INPUT_LEN		65536

/* Pane types */
typedef enum {
	PANE_CHAT    = 0,	/* Main AI chat window */
	PANE_OUTPUT  = 1,	/* Command output / tool results */
	PANE_SYSINFO = 2,	/* Live system info (/proc/ai/resources) */
	PANE_LOGS    = 3,	/* Kernel dmesg / aicore log tail */
	PANE_EDITOR  = 4,	/* Simple text editor for file viewing */
} pane_type_t;

/* A line in a pane's scrollback buffer */
struct pane_line {
	char    text[MAX_LINE_LEN];
	int     color_pair;	/* ncurses color pair index */
	bool    bold;
	bool    dim;
};

/* A single pane (window tile) */
struct pane {
	WINDOW      *win;	/* ncurses window */
	pane_type_t  type;
	char         title[64];

	/* Position and size (in terminal cells) */
	int y, x, rows, cols;

	/* Scrollback buffer */
	struct pane_line lines[MAX_PANE_LINES];
	int  num_lines;
	int  scroll_pos;	/* First visible line index */

	bool visible;
	bool focused;
	bool dirty;		/* Needs redraw */
};

/* Color pair IDs */
#define COLOR_PAIR_DEFAULT	1
#define COLOR_PAIR_USER		2	/* User messages */
#define COLOR_PAIR_AI		3	/* AI responses */
#define COLOR_PAIR_TOOL		4	/* Tool calls */
#define COLOR_PAIR_SYSTEM	5	/* System messages */
#define COLOR_PAIR_STATUS	6	/* Status bar */
#define COLOR_PAIR_BORDER	7	/* Window borders */
#define COLOR_PAIR_ERROR	8	/* Error messages */
#define COLOR_PAIR_INPUT	9	/* Input line */
#define COLOR_PAIR_HIGHLIGHT	10	/* Highlighted/selected text */

/* Global window layout state */
struct wm_state {
	int  term_rows;
	int  term_cols;

	struct pane panes[MAX_PANES];
	int  num_panes;
	int  focused_pane;

	/* Input bar (always at bottom) */
	WINDOW *input_win;
	WINDOW *status_win;

	bool  show_side_pane;
	int   side_pane_width;	/* Fraction of term_cols */
};

/* Initialize window manager and ncurses */
int  wm_init(struct wm_state *wm);
void wm_cleanup(struct wm_state *wm);

/* Resize handling (call on SIGWINCH) */
void wm_resize(struct wm_state *wm);

/* Redraw all dirty panes */
void wm_refresh(struct wm_state *wm);

/* Cycle focus to next/prev pane */
void wm_focus_next(struct wm_state *wm);
void wm_focus_prev(struct wm_state *wm);

/* Split current pane horizontally or vertically */
int  wm_split_horiz(struct wm_state *wm);
int  wm_split_vert(struct wm_state *wm);
void wm_close_pane(struct wm_state *wm, int pane_idx);

/* Toggle side pane visibility */
void wm_toggle_side(struct wm_state *wm);

/* Scroll focused pane */
void wm_scroll_up(struct wm_state *wm, int lines);
void wm_scroll_down(struct wm_state *wm, int lines);
void wm_scroll_bottom(struct wm_state *wm);

/* Add a line to a pane's scrollback, auto-scroll if at bottom */
void pane_add_line(struct pane *p, const char *text,
		   int color_pair, bool bold);

/* Update status bar */
void wm_set_status(struct wm_state *wm, const char *left, const char *right);

/* Get the main chat pane */
struct pane *wm_chat_pane(struct wm_state *wm);

/* Get or create a side pane of given type */
struct pane *wm_get_side_pane(struct wm_state *wm, pane_type_t type);

#endif /* DTE_WINDOW_H */
