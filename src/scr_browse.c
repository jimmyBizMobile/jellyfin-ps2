/*
 * The library screen: tabs (HOME, Movies, Series, Music, Search), a 3x2
 * poster grid per page, an A-Z rail to jump, and the jobs that load pages,
 * artwork, albums, seasons and film pages.
 */

#include "screens.h"
#include "ui.h"
#include "jf.h"
#include "poster.h"
#include "net.h"

#include <libpad.h>
#include <stdio.h>
#include <string.h>

/*
 * The library. Four tabs, an A-Z rail, and six posters per page.
 *
 * Music has two levels in this same grid: artists first, then that artist's
 * albums. Cross goes down, Circle comes back up, and the rail is hidden at
 * the album level where there is little to jump through.
 *
 * Navigation:
 *   grid    d-pad moves, up from the top row goes to the tabs,
 *           left from the first column goes to the rail
 *   tabs    left and right switch, down or Cross returns to the grid
 *   rail    up and down pick a letter, Cross jumps, right returns
 *   L1/R1   page, from anywhere
 */

static const char *TAB_NAMES[JF_TAB_COUNT] =
	{ "MOVIES", "SERIES", "MUSIC", "SEARCH" };

typedef enum { FOCUS_GRID, FOCUS_TABS, FOCUS_RAIL } Focus;

/* The grid is COLS x 2 posters, one page; COLS comes from the layout. */
#define COLS (lay->grid_cols)

/* TILE rather than POSTER: POSTER_H is poster.h's include guard. */
#define TILE_W 90.0f
#define TILE_H 135.0f
#define HGAP     26.0f
#define VGAP     14.0f

/*
 * The grid centred across the screen, nudged 19 right of centre to clear
 * the letter rail: 178 with 3 columns on 640 (as it always was), 169 with
 * 5 columns on the wide layout's 853.
 */
#define GRID_W ((float)COLS * (TILE_W + HGAP) - HGAP)
#define GRID_X ((SCR_W - GRID_W) * 0.5f + 19.0f)
#define GRID_Y 92.0f

#define RAIL_X     30.0f
#define RAIL_TOP   100.0f
#define RAIL_PITCH 22.0f
#define RAIL_VIS   12          /* 26 letters at 22px will not fit 448 lines */

/* Why each slot is empty, shown in the placeholder. Cheap to keep and it is
 * the difference between "no images" and knowing which stage failed. */
__attribute__((unused)) static int g_art_bytes[POSTER_SLOTS_MAX];
__attribute__((unused)) static int g_art_rc[POSTER_SLOTS_MAX];

static Focus g_focus = FOCUS_GRID;
static int   g_sel;            /* index within the page */
static int   g_letter;         /* rail cursor, 0 to 25 */

/* ------------------------------------------------------------------ */
/* Jobs                                                               */
/* ------------------------------------------------------------------ */

/*
 * Artwork is fetched with the page rather than on selection.
 *
 * Everything here blocks, so loading on cursor movement would freeze the
 * console on every d-pad press. Doing it once per page turn costs a wait the
 * user already expects, because they just changed pages.
 *
 * Each poster is several Range slices, so this is the slowest thing the app
 * does. The progress line matters.
 */
/*
 * Load one poster into a slot, working down the size ladder.
 *
 * The acceptance test is the decode itself. Fetching cannot tell you whether
 * an image is sound: the segment bug removes bytes from the middle, which
 * leaves the FF D8 and FF D9 markers intact, so a damaged JPEG looks perfect
 * until TurboJPEG refuses it. So each attempt fetches and decodes, and only
 * a successful decode counts.
 *
 * Returns the byte count that worked, 0 if the item has no artwork, or
 * negative if every attempt failed.
 */
static int load_one_poster(int slot, const char *id, int max_width);

/* Shared with HOME: one poster into one slot. */
int scr_load_poster(int slot, const char *id, int max_width)
{
	return load_one_poster(slot, id, max_width);
}

/*
 * Fetch one poster into a slot, walking POSTER_LADDER (sizes and JPEG
 * qualities) with POSTER_TRIES attempts each, until one decodes. Returns
 * the bytes fetched, 0 if every rung failed.
 */
static int load_one_poster(int slot, const char *id, int max_width)
{
	static const int ladder[][2] = POSTER_LADDER;
	static const int tries[]     = POSTER_TRIES;
	const int steps = (int)(sizeof(tries) / sizeof(tries[0]));

	for (int i = 0; i < steps; i++) {
		int w = ladder[i][0];
		int q = ladder[i][1];

		if (w > max_width)
			w = max_width;

		for (int t = 0; t < tries[i]; t++) {
			const unsigned char *data;
			int n = jf_fetch_image(id, w, q, &data);

			if (n == 0)
				return 0;              /* genuinely has no artwork */

			/* Over the size where contents stop being trustworthy.
			 * Retrying will not shrink it, so go straight down a rung. */
			if (n == -3)
				break;

			if (n > 0 && poster_upload(ui_gs(), slot, data, n) == 0)
				return n;              /* fetched and decoded */

			net_snooze(100);
		}
	}

	return -1;
}

/*
 * Artwork is fetched with the page rather than on selection.
 *
 * Everything here blocks, so loading on cursor movement would freeze the
 * console on every d-pad press. Doing it once per page turn costs a wait the
 * user already expects, because they just changed pages.
 */
static void load_artwork(void)
{
	poster_reset();

#if !POSTER_ENABLE
	return;
#endif

	for (int i = 0; i < jf_item_count; i++) {
		char msg[48];
		snprintf(msg, sizeof(msg), "Loading artwork %d/%d",
		         i + 1, jf_item_count);
		app_progress(msg);

		g_art_bytes[i] = load_one_poster(i, jf_items[i].id, POSTER_REQ_W);
		g_art_rc[i]    = poster_ready(i) ? 0 : -1;

		/*
		 * Breathe between posters. Each one is several requests on several
		 * fresh sockets, and dozens back to back leave the stack short of
		 * PCBs and buffers for whatever comes next.
		 */
		net_snooze(POSTER_GAP_MS);
	}

	/* And once more before the caller makes its next JSON request. */
	net_snooze(250);
}

/*
 * Job: load the current tab's page of items (jf_load_items), then its
 * artwork, then show the grid.
 */
void job_load_items(void)
{
	if (jf_load_items() < 0) {
		app_fail(jf_err_title(), jf_err_body());
		return;
	}

	if (g_sel >= jf_item_count)
		g_sel = jf_item_count ? jf_item_count - 1 : 0;

	g_letter = jf_rail_letter();

	load_artwork();

	app_goto(SCR_BROWSE);
}

static char g_jump_letter;

/*
 * Job: jump to the page starting with the chosen letter, then reload
 * artwork.
 */
static void job_jump(void)
{
	if (jf_jump_to_letter(g_jump_letter) < 0) {
		app_fail(jf_err_title(), jf_err_body());
		return;
	}

	g_sel    = 0;
	g_letter = jf_rail_letter();

	/* The page changed, so the slots hold the wrong posters. Without this a
	 * letter jump showed the previous page's artwork against the new
	 * titles. */
	load_artwork();

	app_goto(SCR_BROWSE);
}

/* An album opens its track list; anything else opens the detail page. */
static void job_episodes(void)
{
	if (jf_load_episodes(jf_show_id, jf_items[g_sel].id,
	                     jf_items[g_sel].name) < 0) {
		app_fail_here(jf_err_title(), jf_err_body());
		return;
	}

	scr_tracks_reset();

	/* The season poster is drawn on the episode list, so load it into the
	 * same large slot the album cover uses. */
	poster_clear(DETAIL_SLOT);
	app_progress("Loading artwork");
	load_one_poster(DETAIL_SLOT, jf_items[g_sel].id, DETAIL_MAX_W - 8);

	app_goto(SCR_TRACKS);
}

/*
 * Job: open an album or a season: load its tracks or episodes and the
 * cover, then the list screen.
 */
static void job_tracks(void)
{
	if (jf_load_tracks(jf_items[g_sel].id, jf_items[g_sel].name,
	                   jf_items[g_sel].year) < 0) {
		app_fail_here(jf_err_title(), jf_err_body());
		return;
	}

	scr_tracks_reset();

	/* The cover is drawn on both the track list and the player, so load it
	 * into the large slot the detail page uses. */
	poster_clear(DETAIL_SLOT);
	app_progress("Loading cover");
	load_one_poster(DETAIL_SLOT, jf_items[g_sel].id, DETAIL_MAX_W - 8);

	app_goto(SCR_TRACKS);
}

/* Job: open a film's page: load its detail and its larger poster. */
static void job_detail(void)
{
	if (jf_load_detail(jf_items[g_sel].id) < 0) {
		app_fail_here(jf_err_title(), jf_err_body());
		return;
	}

	/*
	 * Its own fetch, so a failed load shows a placeholder rather than the
	 * last film you opened. The ladder caps the width, so DETAIL_MAX_W is
	 * an upper bound rather than a request: until the segment limit is
	 * lifted the detail image is the same size as a grid poster, just drawn
	 * larger.
	 */
	poster_clear(DETAIL_SLOT);

	app_progress("Loading artwork");

	load_one_poster(DETAIL_SLOT, jf_items[g_sel].id, DETAIL_MAX_W - 8);

	scr_detail_reset();
	app_goto(SCR_DETAIL);
}

/* ------------------------------------------------------------------ */
/* Input                                                              */
/* ------------------------------------------------------------------ */

static void change_tab(int delta)
{
	int t = (int)jf_tab + delta;

	/* HOME sits before MOVIES; USB after SEARCH (then HOME again). */
	if (t < 0) {
		g_focus = FOCUS_TABS;
		app_run_job(job_load_home, "Loading...");
		return;
	}
	if (t >= JF_TAB_COUNT) {
		g_focus = FOCUS_TABS;
		app_goto(SCR_USB);
		return;
	}

	jf_tab         = (JfTab)t;
	jf_start_index = 0;
	g_sel          = 0;

	/* Always start at the top level, whichever was last open. */
	if (jf_tab == JF_TAB_MUSIC)  jf_music_up();
	if (jf_tab == JF_TAB_SERIES) jf_tv_up();

	if (jf_tab == JF_TAB_SEARCH && !jf_query[0]) {
		app_goto(SCR_SEARCH);       /* nothing to show until something is typed */
		return;
	}

	app_run_job(job_load_items, "Loading...");
}

/* Move a page forward or back (L1/R1) and reload. */
static void page(int delta)
{
	int next = jf_start_index + delta * JF_PAGE;

	if (next < 0 || next >= jf_total_count)
		return;

	jf_start_index = next;
	g_sel          = 0;
	app_run_job(job_load_items, "Loading...");
}

/*
 * The poster grid: d-pad moves, Up from the top row goes to the tabs, Left
 * from the first column to the letter rail, X opens, Circle goes up a
 * level (or back to the users at the top).
 */
static void grid_input(u32 p)
{
	int row = g_sel / COLS, col = g_sel % COLS;

	if (p & PAD_UP) {
		if (row > 0) g_sel -= COLS;
		else         g_focus = FOCUS_TABS;
		return;
	}

	if (p & PAD_DOWN) {
		if (g_sel + COLS < jf_item_count) g_sel += COLS;
		return;
	}

	if (p & PAD_LEFT) {
		if (col > 0) g_sel--;
		else if (jf_tab != JF_TAB_SEARCH &&
		         !(jf_tab == JF_TAB_MUSIC &&
		           jf_music_level == JF_MUSIC_ALBUMS) &&
		         !(jf_tab == JF_TAB_SERIES &&
		           jf_tv_level == JF_TV_SEASONS))
			g_focus = FOCUS_RAIL;   /* none inside an artist or show */
		return;
	}

	if (p & PAD_RIGHT) {
		if (col < COLS - 1 && g_sel + 1 < jf_item_count) g_sel++;
		return;
	}

	if ((p & PAD_CROSS) && jf_item_count > 0) {
		if (jf_tab == JF_TAB_MUSIC &&
		    jf_music_level == JF_MUSIC_ARTISTS) {
			/* Down a level: the same grid, now showing this artist's
			 * albums. */
			jf_music_enter_artist(jf_items[g_sel].id,
			                      jf_items[g_sel].name);
			g_sel    = 0;
			g_letter = 'A';
			app_run_job(job_load_items, "Loading albums...");
		} else if (jf_tab == JF_TAB_MUSIC) {
			app_run_job(job_tracks, "Loading tracks...");
		} else if (jf_tab == JF_TAB_SERIES &&
		           jf_tv_level == JF_TV_SHOWS) {
			/* Down a level: this show's seasons, same grid. */
			jf_tv_enter_show(jf_items[g_sel].id, jf_items[g_sel].name);
			g_sel    = 0;
			g_letter = 'A';
			app_run_job(job_load_items, "Loading seasons...");
		} else if (jf_tab == JF_TAB_SERIES) {
			app_run_job(job_episodes, "Loading episodes...");
		} else {
			app_run_job(job_detail, "Loading...");
		}
	}

	if (p & PAD_TRIANGLE) {
		if (jf_tab == JF_TAB_SEARCH)
			app_goto(SCR_SEARCH);
	}

	if (p & PAD_CIRCLE) {
		if (jf_tab == JF_TAB_MUSIC && jf_music_level == JF_MUSIC_ALBUMS) {
			/* Back to the artist list rather than out of the library. */
			jf_music_up();
			g_sel    = 0;
			g_letter = 'A';
			app_run_job(job_load_items, "Loading artists...");
		} else if (jf_tab == JF_TAB_SERIES &&
		           jf_tv_level == JF_TV_SEASONS) {
			jf_tv_up();
			g_sel    = 0;
			g_letter = 'A';
			app_run_job(job_load_items, "Loading series...");
		} else {
			/* Out of the library: back to "Who is watching?". */
			app_run_job(job_load_users, "Loading users...");
		}
	}
}

/*
 * The tab row: Left/Right change tab (HOME before MOVIES), Down or X back
 * to the grid.
 */
static void tabs_input(u32 p)
{
	if (p & PAD_LEFT)  { change_tab(-1); return; }
	if (p & PAD_RIGHT) { change_tab(+1); return; }

	if (p & (PAD_DOWN | PAD_CROSS)) g_focus = FOCUS_GRID;
	if (p & PAD_CIRCLE)             g_focus = FOCUS_GRID;
}

/*
 * The A-Z rail: Up/Down choose a letter, X jumps to it, Right or Circle
 * back to the grid.
 */
static void rail_input(u32 p)
{
	if (p & PAD_UP)    { if (g_letter > 0)  g_letter--; return; }
	if (p & PAD_DOWN)  { if (g_letter < 25) g_letter++; return; }

	if (p & (PAD_RIGHT | PAD_CIRCLE)) { g_focus = FOCUS_GRID; return; }

	if (p & PAD_CROSS) {
		g_jump_letter = (char)('A' + g_letter);
		g_focus       = FOCUS_GRID;
		app_run_job(job_jump, "Jumping...");
	}
}

/*
 * Per-frame input: L1/R1 page from anywhere, then whichever part has the
 * focus.
 */
static void input(u32 p)
{
	/* Paging works from any focus. */
	if (p & PAD_R1) { page(+1); return; }
	if (p & PAD_L1) { page(-1); return; }

	switch (g_focus) {
	case FOCUS_GRID: grid_input(p); break;
	case FOCUS_TABS: tabs_input(p); break;
	case FOCUS_RAIL: rail_input(p); break;
	}
}

/* ------------------------------------------------------------------ */
/* Drawing                                                            */
/* ------------------------------------------------------------------ */

/*
 * The tab row, shared with HOME and USB: HOME first, then the library
 * tabs, then USB. on is the lit tab (-1 for HOME, SCR_TAB_USB for USB),
 * focused whether the d-pad is on the row.
 */
void scr_draw_tabs(int on, int focused)
{
	const float gap = 26.0f;
	enum { N = JF_TAB_COUNT + 2 };     /* HOME, the library tabs, USB */
	const char *names[N];

	names[0] = "HOME";
	for (int i = 0; i < JF_TAB_COUNT; i++)
		names[i + 1] = TAB_NAMES[i];
	names[N - 1] = "USB";

	float total = 0.0f;
	for (int i = 0; i < N; i++)
		total += ui_text_w(1.0f, names[i]) + (i < N - 1 ? gap : 0);

	float x = (SCR_W - total) * 0.5f;

	for (int i = 0; i < N; i++) {
		float w   = ui_text_w(1.0f, names[i]);
		int   lit = (i == on + 1);

		ui_text(x, 52, 1.0f, lit ? C_WHITE : C_DIM, names[i]);

		if (lit) {
			ui_box(x, 76, w, 3, C_PURPLE);

			/* A dim underline marks which row the d-pad is driving. */
			if (focused)
				ui_box(x - 6, 48, w + 12, 1, C_LILAC);
		}

		x += w + gap;
	}

	ui_box(60, 88, SCR_W - 120, 1, C_RULE);
}

/* The tab row, with the current tab lit. */
static void draw_tabs(void)
{
	scr_draw_tabs((int)jf_tab, g_focus == FOCUS_TABS);
}

/* The A-Z letter rail on the left, where a jump makes sense. */
static void draw_rail(void)
{
	/* Not inside one artist: too few albums for it to earn the space, and
	 * the jump would need the same artist filter as the listing. */
	if ((jf_tab == JF_TAB_MUSIC  && jf_music_level == JF_MUSIC_ALBUMS) ||
	    (jf_tab == JF_TAB_SERIES && jf_tv_level    == JF_TV_SEASONS))
		return;

	int first = g_letter - RAIL_VIS / 2;
	if (first < 0) first = 0;
	if (first > 26 - RAIL_VIS) first = 26 - RAIL_VIS;

	for (int r = 0; r < RAIL_VIS; r++) {
		int   i = first + r;
		float y = RAIL_TOP + r * RAIL_PITCH;
		char  ch[2] = { (char)('A' + i), '\0' };

		if (i == g_letter) {
			ui_round(RAIL_X - 8, y - 1, 26, 22, 4,
			         g_focus == FOCUS_RAIL ? C_PURPLE : C_ROWSEL);
			ui_text_c(RAIL_X + 5, y, 1.0f, C_WHITE, ch);
		} else {
			ui_text_c(RAIL_X + 5, y, 1.0f,
			          g_focus == FOCUS_RAIL ? C_DIM : C_RULE, ch);
		}
	}

	if (first > 0)
		ui_text_c(RAIL_X + 5, RAIL_TOP - 20, 1.0f, C_RULE, "^");
	if (first + RAIL_VIS < 26)
		ui_text_c(RAIL_X + 5, RAIL_TOP + RAIL_VIS * RAIL_PITCH, 1.0f,
		          C_RULE, "v");
}

/*
 * A poster slot. Artwork is not loaded yet, so the placeholder carries the
 * title. Once images arrive the title moves to the detail page and this
 * becomes the fallback for items with no primary image.
 */
static void draw_slot(int slot, float x, float y, const char *title,
                      int selected)
{
	if (selected) {
		float rh = (jf_tab == JF_TAB_MUSIC) ? TILE_W : TILE_H;
		float ry = y + (TILE_H - rh) * 0.5f;

		ui_round(x - 4, ry - 4, TILE_W + 8, rh + 8, 5, C_PURPLE);
	}

	if (poster_ready(slot)) {
		/*
		 * Album covers are square, film posters are 2:3. Give artwork the
		 * same box the placeholder uses, so a cover and the placeholder it
		 * replaces sit in exactly the same place. poster_draw fits inside
		 * without distorting, so this only decides the space it may use.
		 */
		float aw = TILE_W;
		float ah = (jf_tab == JF_TAB_MUSIC) ? TILE_W : TILE_H;
		float ay = y + (TILE_H - ah) * 0.5f;

		poster_draw(ui_gs(), slot, x, ay, aw, ah);

		/* A thin bar keeps the selected item obvious over artwork. */
		if (selected)
			ui_box(x, ay, aw, 3, C_PURPLE);

#if DEBUG_POSTER_INFO
		if (slot >= 0 && slot < POSTER_SLOTS) {
			int iw, ih;
			poster_dims(slot, &iw, &ih);

			char d[32];
			snprintf(d, sizeof(d), "%dx%d", iw, ih);
			ui_text_c(x + TILE_W * 0.5f, y + TILE_H - 22.0f, 1.0f,
			          C_WHITE, d);

			snprintf(d, sizeof(d), "b%d", g_art_bytes[slot]);
			ui_text_c(x + TILE_W * 0.5f, y + TILE_H - 44.0f, 1.0f,
			          C_WHITE, d);
		}
#endif
		return;
	}

	/*
	 * Album covers are square, film posters are 2:3. Shape the placeholder
	 * to match, so an empty music slot is not a tall rectangle that a real
	 * cover will never fill.
	 */
	float pw = TILE_W;
	float ph = TILE_H;
	float px = x;
	float py = y;

	if (jf_tab == JF_TAB_MUSIC) {
		ph = pw;
		py = y + (TILE_H - ph) * 0.5f;   /* centred, as artwork is */
	}

	ui_round(px, py, pw, ph, 3,
	         GS_SETREG_RGBAQ(0x1C, 0x1E, 0x3C, 0x80, 0x00));
	ui_box(px, py, pw, 3, selected ? C_PURPLE : C_RULE);

	/*
	 * Why this slot is empty: bytes fetched, and the upload result. Behind
	 * DEBUG_POSTER_INFO with the rest of the artwork diagnostics, since an
	 * empty slot now means a real failure rather than the segment bug, and
	 * "b0 r0" under every placeholder is noise the user does not need.
	 */
#if DEBUG_POSTER_INFO
	if (slot >= 0 && slot < POSTER_SLOTS) {
		char d[32];
		snprintf(d, sizeof(d), "b%d r%d", g_art_bytes[slot], g_art_rc[slot]);
		ui_text_c(px + pw * 0.5f, py + ph - 20.0f, 1.0f, C_RULE, d);
	}
#endif

	/* Wrap the title inside the slot, a word at a time. */
	char line[80] = "";
	float ly = py + 12.0f;

	const char *p = title;
	while (*p && ly < py + ph - 22.0f) {
		char word[32];
		int n = 0;
		while (p[n] && p[n] != ' ' && n < (int)sizeof(word) - 1) n++;

		memcpy(word, p, n);
		word[n] = '\0';

		char probe[80];
		snprintf(probe, sizeof(probe), "%s%s%s",
		         line, line[0] ? " " : "", word);

		if (line[0] && ui_text_w(1.0f, probe) > pw - 12.0f) {
			ui_text_c(px + pw * 0.5f, ly, 1.0f,
			          selected ? C_WHITE : C_DIM, line);
			ly += 20.0f;
			snprintf(line, sizeof(line), "%s", word);
		} else {
			snprintf(line, sizeof(line), "%s", probe);
		}

		p += n;
		while (*p == ' ') p++;
	}

	if (line[0] && ly < py + ph - 6.0f)
		ui_text_c(px + pw * 0.5f, ly, 1.0f,
		          selected ? C_WHITE : C_DIM, line);
}

/*
 * The library screen: brand, breadcrumb, tabs, rail, the 3x2 poster grid
 * with titles, the page count and the button hints.
 */
static void draw(void)
{
	ui_waves();

	/* Brand, small, top left. */
	ui_text(24, 16, 1.0f, C_WHITE, "Jellyfin");
	ui_text(24 + ui_text_w(1.0f, "Jellyfin") + 6, 16, 1.0f, C_PURPLE, "PS2");

	/*
	 * Whose albums these are, as a breadcrumb after the brand.
	 *
	 * It sat under the tab row before, which put it against the MUSIC
	 * underline and made it read as part of the tab rather than as the
	 * page you are on. Up here there is empty space and it says "where
	 * you are", which is what it is for.
	 */
	{
		const char *crumb = NULL;

		if (jf_tab == JF_TAB_MUSIC && jf_music_level == JF_MUSIC_ALBUMS)
			crumb = jf_artist_name;
		else if (jf_tab == JF_TAB_SERIES && jf_tv_level == JF_TV_SEASONS)
			crumb = jf_show_name;

		if (crumb && crumb[0]) {
			float bx = 24 + ui_text_w(1.0f, "Jellyfin") + 6
			              + ui_text_w(1.0f, "PS2");

			ui_text(bx + 12, 16, 1.0f, C_RULE, "/");
			ui_text(bx + 26, 16, 1.0f, C_DIM,  crumb);
		}
	}

	/* Page counter, top right. */
	char cnt[48];
	if (jf_total_count > 0)
		snprintf(cnt, sizeof(cnt), "%d-%d of %d",
		         jf_start_index + 1, jf_start_index + jf_item_count,
		         jf_total_count);
	else
		snprintf(cnt, sizeof(cnt), "nothing here");

	ui_text(SCR_W - 24 - ui_text_w(1.0f, cnt), 16, 1.0f, C_DIM, cnt);

	draw_tabs();


	if (jf_tab != JF_TAB_SEARCH)
		draw_rail();

	if (jf_tab == JF_TAB_SEARCH && !jf_query[0]) {
		ui_text_c(SCR_W / 2, 200, 1.0f, C_DIM,
		          "Press Triangle to type a search");
	} else if (jf_item_count == 0) {
		ui_text_c(SCR_W / 2, 200, 1.0f, C_DIM, "No results");
	} else {
		for (int i = 0; i < jf_item_count; i++) {
			float x = GRID_X + (i % COLS) * (TILE_W + HGAP);
			float y = GRID_Y + (i / COLS) * (TILE_H + VGAP);

			draw_slot(i, x, y, jf_items[i].name,
			          g_focus == FOCUS_GRID && i == g_sel);
		}
	}

	if (jf_tab == JF_TAB_SEARCH) {
		if (jf_tab == JF_TAB_MUSIC && jf_music_level == JF_MUSIC_ALBUMS) {
			static const BtnGlyph b[] = { BTN_CROSS, BTN_CIRCLE,
			                              BTN_L1R1, BTN_SELECT };
			static const char *l[] = { "Open", "Artists", "Page", "Quit" };
			ui_hint_row(b, l, 4);
		} else if (jf_tab == JF_TAB_SERIES &&
		           jf_tv_level == JF_TV_SEASONS) {
			static const BtnGlyph b[] = { BTN_CROSS, BTN_CIRCLE,
			                              BTN_L1R1, BTN_SELECT };
			static const char *l[] = { "Open", "Series", "Page", "Quit" };
			ui_hint_row(b, l, 4);
		} else {
			static const BtnGlyph b[] = { BTN_CROSS, BTN_TRIANGLE,
			                              BTN_L1R1, BTN_SELECT };
			static const char *l[] = { "Open", "Search", "Page", "Quit" };
			ui_hint_row(b, l, 4);
		}
	} else {
		static const BtnGlyph b[] = { BTN_CROSS, BTN_L1R1,
		                              BTN_CIRCLE, BTN_SELECT };
		static const char *l[] = { "Open", "Page", "Back", "Quit" };
		ui_hint_row(b, l, 4);
	}
}

/* Keep the selection inside the current page. */
static void enter(void)
{
	if (g_sel >= jf_item_count)
		g_sel = jf_item_count ? jf_item_count - 1 : 0;
}

const Screen scr_browse = { enter, input, draw };