// SPDX-License-Identifier: GPL-2.0
/*
 * tools/dte/window.c - DTE tiled window manager implementation
 *
 * Layout management for the ncurses-based AI terminal interface.
 * Handles pane creation, resizing, scrolling, and border rendering.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncurses.h>
#include <signal.h>
#include <unistd.h>
#include "window.h"

/* ===== Color scheme initialization ===== */

static void init_colors(void)
{
	if (!has_colors())
		return;

	start_color();
	use_default_colors();

	/* DEFAULT: white on black */
	init_pair(COLOR_PAIR_DEFAULT,   COLOR_WHITE,   -1);
	/* USER: bright cyan on black */
	init_pair(COLOR_PAIR_USER,      COLOR_CYAN,    -1);
	/* AI: bright green on black */
	init_pair(COLOR_PAIR_AI,        COLOR_GREEN,   -1);
	/* TOOL: yellow (tool calls look like terminal commands) */
	init_pair(COLOR_PAIR_TOOL,      COLOR_YELLOW,  -1);
	/* SYSTEM: blue */
	init_pair(COLOR_PAIR_SYSTEM,    COLOR_BLUE,    -1);
	/* STATUS bar: black on cyan */
	init_pair(COLOR_PAIR_STATUS,    COLOR_BLACK,   COLOR_CYAN);
	/* BORDER: dim white */
	init_pair(COLOR_PAIR_BORDER,    COLOR_WHITE,   -1);
	/* ERROR: red */
	init_pair(COLOR_PAIR_ERROR,     COLOR_RED,     -1);
	/* INPUT: white on dark blue */
	init_pair(COLOR_PAIR_INPUT,     COLOR_WHITE,   COLOR_BLUE);
	/* HIGHLIGHT: black on white */
	init_pair(COLOR_PAIR_HIGHLIGHT, COLOR_BLACK,   COLOR_WHITE);
}

/* ===== Layout calculation ===== */

static void calc_layout(struct wm_state *wm)
{
	getmaxyx(stdscr, wm->term_rows, wm->term_cols);

	/* Reserve 2 rows at bottom: 1 for status, 1 for input */
	int content_rows = wm->term_rows - 2;
	int content_cols = wm->term_cols;

	if (content_rows < 4)
		content_rows = 4;

	if (wm->show_side_pane && wm->num_panes > 1) {
		/* Split: main chat on left, side pane on right */
		int side_w = wm->side_pane_width;
		if (side_w < 20)
			side_w = 20;
		if (side_w > content_cols - 20)
			side_w = content_cols - 20;

		int main_w = content_cols - side_w;

		/* Chat pane */
		wm->panes[0].y    = 0;
		wm->panes[0].x    = 0;
		wm->panes[0].rows = content_rows;
		wm->panes[0].cols = main_w;

		/* Side pane */
		if (wm->num_panes > 1) {
			wm->panes[1].y    = 0;
			wm->panes[1].x    = main_w;
			wm->panes[1].rows = content_rows;
			wm->panes[1].cols = side_w;
		}
	} else {
		/* Full width chat pane */
		wm->panes[0].y    = 0;
		wm->panes[0].x    = 0;
		wm->panes[0].rows = content_rows;
		wm->panes[0].cols = content_cols;
	}

	/* Status bar */
	wresize(wm->status_win, 1, wm->term_cols);
	mvwin(wm->status_win, wm->term_rows - 2, 0);

	/* Input bar */
	wresize(wm->input_win, 1, wm->term_cols);
	mvwin(wm->input_win, wm->term_rows - 1, 0);
}

static void resize_pane_windows(struct wm_state *wm)
{
	for (int i = 0; i < wm->num_panes; i++) {
		struct pane *p = &wm->panes[i];

		if (!p->visible)
			continue;

		if (p->win) {
			wresize(p->win, p->rows, p->cols);
			mvwin(p->win, p->y, p->x);
		} else {
			p->win = newwin(p->rows, p->cols, p->y, p->x);
			if (!p->win)
				continue;
			scrollok(p->win, FALSE);
			keypad(p->win, TRUE);
		}
		p->dirty = true;
	}
}

/* ===== Pane rendering ===== */

static void render_pane(struct pane *p)
{
	if (!p || !p->win || !p->visible)
		return;

	/* Draw border */
	wattron(p->win, COLOR_PAIR(COLOR_PAIR_BORDER));
	box(p->win, 0, 0);
	wattroff(p->win, COLOR_PAIR(COLOR_PAIR_BORDER));

	/* Draw title */
	if (p->title[0]) {
		int title_x = 2;
		wattron(p->win, COLOR_PAIR(COLOR_PAIR_BORDER) | A_BOLD);
		mvwprintw(p->win, 0, title_x, " %s ", p->title);
		wattroff(p->win, COLOR_PAIR(COLOR_PAIR_BORDER) | A_BOLD);
	}

	/* Draw focus indicator */
	if (p->focused) {
		wattron(p->win, COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
		mvwaddch(p->win, 0, p->cols - 2, '*');
		wattroff(p->win, COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
	}

	/* Inner content area */
	int inner_rows = p->rows - 2;
	int inner_cols = p->cols - 2;

	if (inner_rows <= 0 || inner_cols <= 0)
		return;

	/* Calculate visible range */
	int first_line = p->scroll_pos;
	int last_line  = first_line + inner_rows;

	if (last_line > p->num_lines)
		last_line = p->num_lines;

	/* Clear content area */
	wattron(p->win, COLOR_PAIR(COLOR_PAIR_DEFAULT));
	for (int row = 0; row < inner_rows; row++)
		mvwhline(p->win, row + 1, 1, ' ', inner_cols);

	/* Render lines */
	for (int i = first_line; i < last_line; i++) {
		struct pane_line *line = &p->lines[i];
		int row = (i - first_line) + 1;

		if (row > inner_rows)
			break;

		int attrs = COLOR_PAIR(line->color_pair ? line->color_pair : COLOR_PAIR_DEFAULT);
		if (line->bold)
			attrs |= A_BOLD;
		if (line->dim)
			attrs |= A_DIM;

		wattron(p->win, attrs);

		/* Word-wrap long lines */
		size_t len = strlen(line->text);
		if ((int)len <= inner_cols) {
			mvwaddnstr(p->win, row, 1, line->text, inner_cols);
		} else {
			/* Truncate for now; proper word-wrap is complex */
			mvwaddnstr(p->win, row, 1, line->text, inner_cols - 1);
			mvwaddch(p->win, row, inner_cols, '>');
		}

		wattroff(p->win, attrs);
	}

	/* Scroll indicator */
	if (p->num_lines > inner_rows) {
		int sb_pos = (int)((float)p->scroll_pos /
				   (p->num_lines - inner_rows) * (inner_rows - 1));
		wattron(p->win, COLOR_PAIR(COLOR_PAIR_BORDER));
		mvwaddch(p->win, sb_pos + 1, p->cols - 1, ACS_BLOCK);
		wattroff(p->win, COLOR_PAIR(COLOR_PAIR_BORDER));
	}

	wnoutrefresh(p->win);
	p->dirty = false;
}

/* ===== Public API ===== */

int wm_init(struct wm_state *wm)
{
	memset(wm, 0, sizeof(*wm));

	initscr();
	cbreak();
	noecho();
	keypad(stdscr, TRUE);
	set_escdelay(25);
	curs_set(1);

	init_colors();

	getmaxyx(stdscr, wm->term_rows, wm->term_cols);

	/* Create status bar */
	wm->status_win = newwin(1, wm->term_cols, wm->term_rows - 2, 0);
	if (!wm->status_win)
		return -1;
	wbkgd(wm->status_win, COLOR_PAIR(COLOR_PAIR_STATUS));

	/* Create input bar */
	wm->input_win = newwin(1, wm->term_cols, wm->term_rows - 1, 0);
	if (!wm->input_win)
		return -1;
	wbkgd(wm->input_win, COLOR_PAIR(COLOR_PAIR_INPUT));
	keypad(wm->input_win, TRUE);

	/* Create main chat pane */
	wm->show_side_pane  = false;
	wm->side_pane_width = wm->term_cols / 3;
	wm->num_panes       = 1;
	wm->focused_pane    = 0;

	struct pane *chat = &wm->panes[0];
	chat->type    = PANE_CHAT;
	chat->visible = true;
	chat->focused = true;
	strncpy(chat->title, "AI Chat", sizeof(chat->title) - 1);

	calc_layout(wm);
	resize_pane_windows(wm);

	refresh();
	return 0;
}

void wm_cleanup(struct wm_state *wm)
{
	for (int i = 0; i < wm->num_panes; i++) {
		if (wm->panes[i].win)
			delwin(wm->panes[i].win);
	}
	if (wm->status_win)
		delwin(wm->status_win);
	if (wm->input_win)
		delwin(wm->input_win);
	endwin();
}

void wm_resize(struct wm_state *wm)
{
	endwin();
	refresh();
	clear();

	calc_layout(wm);
	resize_pane_windows(wm);

	for (int i = 0; i < wm->num_panes; i++)
		wm->panes[i].dirty = true;

	wm_refresh(wm);
}

void wm_refresh(struct wm_state *wm)
{
	for (int i = 0; i < wm->num_panes; i++) {
		if (wm->panes[i].visible && wm->panes[i].dirty)
			render_pane(&wm->panes[i]);
	}
	wnoutrefresh(wm->status_win);
	doupdate();
}

void wm_focus_next(struct wm_state *wm)
{
	wm->panes[wm->focused_pane].focused = false;
	wm->panes[wm->focused_pane].dirty   = true;

	wm->focused_pane = (wm->focused_pane + 1) % wm->num_panes;

	wm->panes[wm->focused_pane].focused = true;
	wm->panes[wm->focused_pane].dirty   = true;
}

void wm_toggle_side(struct wm_state *wm)
{
	wm->show_side_pane = !wm->show_side_pane;

	if (wm->show_side_pane && wm->num_panes < 2) {
		struct pane *side = &wm->panes[1];
		memset(side, 0, sizeof(*side));
		side->type    = PANE_OUTPUT;
		side->visible = true;
		strncpy(side->title, "Output", sizeof(side->title) - 1);
		wm->num_panes = 2;
	}

	calc_layout(wm);
	resize_pane_windows(wm);

	for (int i = 0; i < wm->num_panes; i++)
		wm->panes[i].dirty = true;
}

void wm_scroll_up(struct wm_state *wm, int lines)
{
	struct pane *p = &wm->panes[wm->focused_pane];
	int inner_rows = p->rows - 2;

	p->scroll_pos -= lines;
	if (p->scroll_pos < 0)
		p->scroll_pos = 0;
	p->dirty = true;
	(void)inner_rows;
}

void wm_scroll_down(struct wm_state *wm, int lines)
{
	struct pane *p = &wm->panes[wm->focused_pane];
	int inner_rows = p->rows - 2;
	int max_scroll = p->num_lines - inner_rows;

	if (max_scroll < 0)
		max_scroll = 0;

	p->scroll_pos += lines;
	if (p->scroll_pos > max_scroll)
		p->scroll_pos = max_scroll;
	p->dirty = true;
}

void wm_scroll_bottom(struct wm_state *wm)
{
	struct pane *p = &wm->panes[wm->focused_pane];
	int inner_rows = p->rows - 2;
	int max_scroll = p->num_lines - inner_rows;

	if (max_scroll < 0)
		max_scroll = 0;
	p->scroll_pos = max_scroll;
	p->dirty = true;
}

void pane_add_line(struct pane *p, const char *text,
		   int color_pair, bool bold)
{
	if (!p)
		return;

	int idx;
	if (p->num_lines < MAX_PANE_LINES) {
		idx = p->num_lines++;
	} else {
		/* Ring buffer: evict oldest line */
		memmove(&p->lines[0], &p->lines[1],
			(MAX_PANE_LINES - 1) * sizeof(struct pane_line));
		idx = MAX_PANE_LINES - 1;
	}

	strncpy(p->lines[idx].text, text, MAX_LINE_LEN - 1);
	p->lines[idx].text[MAX_LINE_LEN - 1] = '\0';
	p->lines[idx].color_pair = color_pair;
	p->lines[idx].bold       = bold;

	/* Auto-scroll to bottom if already at bottom */
	int inner_rows = p->rows - 2;
	int max_scroll = p->num_lines - inner_rows;
	if (max_scroll < 0) max_scroll = 0;

	if (p->scroll_pos >= max_scroll - 1)
		p->scroll_pos = max_scroll;

	p->dirty = true;
}

void wm_set_status(struct wm_state *wm, const char *left, const char *right)
{
	if (!wm->status_win)
		return;

	werase(wm->status_win);
	wbkgd(wm->status_win, COLOR_PAIR(COLOR_PAIR_STATUS));
	wattron(wm->status_win, COLOR_PAIR(COLOR_PAIR_STATUS) | A_BOLD);

	/* Left side */
	mvwprintw(wm->status_win, 0, 0, " %s", left ? left : "");

	/* Right side */
	if (right) {
		int rlen = (int)strlen(right);
		int rpos = wm->term_cols - rlen - 2;
		if (rpos > 0)
			mvwprintw(wm->status_win, 0, rpos, "%s ", right);
	}

	wattroff(wm->status_win, COLOR_PAIR(COLOR_PAIR_STATUS) | A_BOLD);
	wnoutrefresh(wm->status_win);
}

struct pane *wm_chat_pane(struct wm_state *wm)
{
	for (int i = 0; i < wm->num_panes; i++)
		if (wm->panes[i].type == PANE_CHAT)
			return &wm->panes[i];
	return NULL;
}

struct pane *wm_get_side_pane(struct wm_state *wm, pane_type_t type)
{
	for (int i = 0; i < wm->num_panes; i++)
		if (wm->panes[i].type == type)
			return &wm->panes[i];

	if (!wm->show_side_pane)
		wm_toggle_side(wm);

	if (wm->num_panes > 1) {
		wm->panes[1].type = type;
		switch (type) {
		case PANE_OUTPUT:  strncpy(wm->panes[1].title, "Output",  63); break;
		case PANE_SYSINFO: strncpy(wm->panes[1].title, "SysInfo", 63); break;
		case PANE_LOGS:    strncpy(wm->panes[1].title, "Logs",    63); break;
		default:           strncpy(wm->panes[1].title, "Pane",    63); break;
		}
		return &wm->panes[1];
	}
	return NULL;
}
