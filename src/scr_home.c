/*
 * HOME: Continue Watching and Next Up, two scrolling rows of posters with
 * a description panel. The first screen after signing in.
 */

#include "screens.h"
#include "ui.h"
#include "jf.h"
#include "poster.h"

#include <libpad.h>
#include <stdio.h>
#include <string.h>

/*
 * HOME: Continue Watching and Next Up, the first thing after signing in.
 *
 * Two rows of VIS posters (3, or 5 in the wide layout). Each row owns VIS
 * of the poster slots, as a ring: a poster lives in slot
 * (row * VIS + index % VIS), so scrolling a
 * row by one brings exactly one new poster into view, and only that one is
 * fetched, into the slot of the poster that scrolled out.
 */

#define VIS      (lay->home_visible)   /* posters per row, from the layout */
#define PW       72.0f
#define PH       108.0f
#define PGAP     16.0f
#define ROW_X    48.0f
#define ROW1_Y   100.0f              /* label; posters 28 below */
#define ROW2_Y   (ROW1_Y + 28 + PH + 22)

enum { ROW_CW = 0, ROW_NU = 1 };

static int  g_row;                   /* the row the cursor is in */
static int  g_sel[2];                /* selected item in each row */
static int  g_first[2];              /* first visible item in each row */
static int  g_tabs;                  /* the d-pad is on the tab row */
static char g_slot_id[POSTER_SLOTS_MAX][40];   /* which poster each slot holds */

/*
 * A poster scrolled into view, fetched on the next frame rather than at
 * once: the frame in between shows its placeholder, so the cursor moves
 * straight away and the picture fills in, instead of the screen freezing
 * on the old position while it downloads.
 */
static int  g_pend_row = -1, g_pend_i;

int g_from_home;                     /* the detail page goes back here */

static int count(int row)
{
	return row == ROW_CW ? jf_home_cw_count : jf_home_nu_count;
}

/* Item i of a row. */
static JfHomeItem *item(int row, int i)
{
	return row == ROW_CW ? &jf_home_cw[i] : &jf_home_nu[i];
}

/*
 * The poster slot of item i in a row: each row cycles through its own
 * VIS slots.
 */
static int slot_of(int row, int i) { return row * VIS + i % VIS; }

/* The poster for item i of row, if its slot does not hold it already. */
static void ensure_poster(int row, int i)
{
	int s = slot_of(row, i);
	JfHomeItem *h = item(row, i);

	if (strcmp(g_slot_id[s], h->image_id) == 0)
		return;

	poster_clear(s);
	scr_load_poster(s, h->image_id, POSTER_REQ_W);
	snprintf(g_slot_id[s], sizeof(g_slot_id[s]), "%s", h->image_id);
}

/*
 * Job: wait for pending reports, load both rows, fetch the visible
 * posters, and show HOME.
 */
void job_load_home(void)
{
	/*
	 * Coming back from a film: its stop report and "played" mark are sent
	 * in the background. Let them reach the server first, or Continue
	 * Watching and Next Up are asked for before they know.
	 */
	jf_reports_wait(3000);

	if (jf_load_home() < 0) {
		app_fail(jf_err_title(), jf_err_body());
		return;
	}

	poster_reset();
	memset(g_slot_id, 0, sizeof(g_slot_id));
	g_pend_row = -1;

	for (int r = 0; r < 2; r++) {
		g_sel[r]   = 0;
		g_first[r] = 0;
	}

	int total = 0;

	for (int r = 0; r < 2; r++)
		for (int i = 0; i < VIS && i < count(r); i++)
			total++;

	int done = 0;

	for (int r = 0; r < 2; r++) {
		for (int i = 0; i < VIS && i < count(r); i++) {
			char msg[48];

			snprintf(msg, sizeof(msg), "Loading artwork %d/%d", ++done, total);
			app_progress(msg);
			ensure_poster(r, i);
		}
	}

	/* Start in the first row with something in it. */
	g_row  = jf_home_cw_count ? ROW_CW : ROW_NU;
	g_tabs = !jf_home_cw_count && !jf_home_nu_count;

	app_goto(SCR_HOME);
}

/* Job: open the selected item's page (from HOME, so Back returns here). */
static void job_open(void)
{
	JfHomeItem *h = item(g_row, g_sel[g_row]);

	if (jf_load_detail(h->id) < 0) {
		app_fail_here(jf_err_title(), jf_err_body());
		return;
	}

	poster_clear(DETAIL_SLOT);
	app_progress("Loading artwork");
	scr_load_poster(DETAIL_SLOT, h->image_id, DETAIL_MAX_W - 8);

	g_from_home = 1;
	scr_detail_reset();
	app_goto(SCR_DETAIL);
}

/* Nothing to prepare: job_load_home does it. */
static void enter(void) { }

/*
 * Move within a row; if the selection scrolls out of view, schedule the
 * new poster for the next frame.
 */
static void move(int row, int delta)
{
	int n = count(row);
	int s = g_sel[row] + delta;

	if (s < 0 || s >= n)
		return;

	g_sel[row] = s;

	if (s < g_first[row]) {
		g_first[row] = s;
	} else if (s >= g_first[row] + VIS) {
		g_first[row] = s - VIS + 1;
	} else {
		return;                    /* already on screen */
	}

	g_pend_row = row;              /* fetched next frame, see above */
	g_pend_i   = s;
}

/*
 * Fetch a poster scrolled into view last frame, then: Left/Right within a
 * row, Up/Down between rows and the tabs, X opens, Circle back to the
 * users.
 */
static void input(u32 p)
{
	/* The poster that scrolled into view last frame. */
	if (g_pend_row >= 0) {
		ensure_poster(g_pend_row, g_pend_i);
		g_pend_row = -1;
	}

	if (p & PAD_CIRCLE) {
		app_run_job(job_load_users, "Loading users...");
		return;
	}

	if (g_tabs) {
		if (p & PAD_RIGHT) {
			jf_tab = JF_TAB_MOVIES;
			jf_start_index = 0;
			app_run_job(job_load_items, "Loading...");
			return;
		}
		if (p & PAD_LEFT) {
			app_goto(SCR_USB);         /* the last tab, wrapping round */
			return;
		}
		if ((p & (PAD_DOWN | PAD_CROSS)) && (jf_home_cw_count || jf_home_nu_count)) {
			g_tabs = 0;
			g_row  = jf_home_cw_count ? ROW_CW : ROW_NU;
		}
		return;
	}

	if (p & PAD_LEFT)  move(g_row, -1);
	if (p & PAD_RIGHT) move(g_row, +1);

	if (p & PAD_UP) {
		if (g_row == ROW_NU && jf_home_cw_count)
			g_row = ROW_CW;
		else
			g_tabs = 1;
	}

	if ((p & PAD_DOWN) && g_row == ROW_CW && jf_home_nu_count)
		g_row = ROW_NU;

	if ((p & PAD_CROSS) && count(g_row))
		app_run_job(job_open, "Loading...");
}

/* "8:19" or "1:16:30", from 100 ns ticks. */
static void clock_text(char *out, int max, long long ticks)
{
	int s = (int)(ticks / 10000000LL);

	if (s >= 3600)
		snprintf(out, max, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
	else
		snprintf(out, max, "%d:%02d", s / 60, s % 60);
}

/*
 * One row: its label, up to VIS posters with progress bars, the scroll
 * arrows, or the empty message.
 */
static void draw_row(int row, float y, const char *label, const char *empty)
{
	ui_text(ROW_X, y, 1.0f, C_WHITE, label);

	if (!count(row)) {
		ui_text(ROW_X, y + 28 + PH / 2 - 10, 1.0f, C_DIM, empty);
		return;
	}

	for (int k = 0; k < VIS; k++) {
		int i = g_first[row] + k;

		if (i >= count(row))
			break;

		JfHomeItem *h = item(row, i);
		float x  = ROW_X + k * (PW + PGAP);
		float py = y + 28;
		int   on = !g_tabs && row == g_row && i == g_sel[row];

		if (on)
			ui_round(x - 4, py - 4, PW + 8, PH + 8, 5, C_PURPLE);

		int s = slot_of(row, i);

		if (poster_ready(s) && strcmp(g_slot_id[s], h->image_id) == 0) {
			poster_draw(ui_gs(), s, x, py, PW, PH);
		} else {
			char initial[2] = { h->name[0] ? h->name[0] : '?', '\0' };

			ui_round(x, py, PW, PH, 3, C_ROWSEL);
			ui_text_c(x + PW / 2, py + PH / 2 - 10, 1.0f, C_DIM, initial);
		}

		/* How far in, under each Continue Watching poster. */
		if (row == ROW_CW && h->runtime_ticks > 0) {
			float f = (float)h->pos_ticks / (float)h->runtime_ticks;

			if (f > 1.0f) f = 1.0f;
			ui_box(x, py + PH + 5, PW, 3, C_RULE);
			ui_box(x, py + PH + 5, PW * f, 3, C_PURPLE);
		}
	}

	/* More to either side. */
	if (g_first[row] > 0)
		ui_text(ROW_X - 22, y + 28 + PH / 2 - 10, 1.0f, C_DIM, "<");
	if (g_first[row] + VIS < count(row))
		ui_text(ROW_X + VIS * (PW + PGAP) - 8, y + 28 + PH / 2 - 10, 1.0f,
		        C_DIM, ">");
}

/* The selected item, described on the right. */
static void draw_info(void)
{
	if (g_tabs || !count(g_row))
		return;

	JfHomeItem *h = item(g_row, g_sel[g_row]);
	float x = ROW_X + VIS * (PW + PGAP) + 20;
	float w = SCR_W - 24 - x;
	float y = ROW1_Y + 28;

	int lines = ui_wrap(x, y, 22, 1.0f, C_WHITE, h->name, w, 2);
	y += (lines > 2 ? 2 : lines) * 22 + 4;

	char sub[96];

	if (h->series[0])
		snprintf(sub, sizeof(sub), "%s  S%02dE%02d", h->series, h->season,
		         h->episode);
	else if (h->year > 0)
		snprintf(sub, sizeof(sub), "%d", h->year);
	else
		sub[0] = '\0';

	if (sub[0]) {
		ui_wrap(x, y, 22, 1.0f, C_DIM, sub, w, 2);
		y += 32;
	}

	if (g_row == ROW_CW && h->runtime_ticks > 0) {
		char a[16], b[16], line[48];

		clock_text(a, sizeof(a), h->pos_ticks);
		clock_text(b, sizeof(b), h->runtime_ticks);
		snprintf(line, sizeof(line), "%s of %s", a, b);
		ui_text(x, y, 1.0f, C_LILAC, line);
		y += 26;

		float f = (float)h->pos_ticks / (float)h->runtime_ticks;

		if (f > 1.0f) f = 1.0f;
		ui_box(x, y, w, 3, C_RULE);
		ui_box(x, y, w * f, 3, C_PURPLE);
		y += 16;

		snprintf(line, sizeof(line), "Resumes at %s", a);
		ui_text(x, y, 1.0f, C_DIM, line);
	} else {
		ui_text(x, y, 1.0f, C_DIM, "Starts from the beginning");
	}
}

/*
 * HOME: brand, user name, tabs, both rows and the panel describing the
 * selected item.
 */
static void draw(void)
{
	ui_waves();

	ui_text(24, 16, 1.0f, C_WHITE, "Jellyfin");
	ui_text(24 + ui_text_w(1.0f, "Jellyfin") + 6, 16, 1.0f, C_PURPLE, "PS2");
	ui_text(SCR_W - 24 - ui_text_w(1.0f, jf_user_name), 16, 1.0f, C_DIM,
	        jf_user_name);

	scr_draw_tabs(-1, g_tabs);

	if (!jf_home_cw_count && !jf_home_nu_count) {
		ui_text_c(SCR_W / 2, 190, 1.0f, C_DIM, "Nothing in progress yet.");
		ui_text_c(SCR_W / 2, 216, 1.0f, C_DIM,
		          "Start something from Movies or Series.");
	} else {
		draw_row(ROW_CW, ROW1_Y, "Continue Watching", "Nothing in progress");
		draw_row(ROW_NU, ROW2_Y, "Next Up", "No new episodes");
		draw_info();
	}

	static const BtnGlyph b[] = { BTN_CROSS, BTN_DPAD, BTN_CIRCLE };
	static const char *l[] = { "Open", "Move", "Users" };
	ui_hint_row(b, l, 3);
}

const Screen scr_home = { enter, input, draw };
