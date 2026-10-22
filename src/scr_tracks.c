/*
 * An album's tracks or a season's episodes, as a list beside the cover and
 * description.
 */

#include "screens.h"
#include "ui.h"
#include "app.h"
#include "jf.h"
#include "poster.h"

#include <libpad.h>
#include <stdio.h>
#include <string.h>

/*
 * An album's tracks, or a season's episodes.
 *
 * The same shape serves both: a numbered list of titles with a runtime, an
 * artwork panel, and a header. Episodes open a detail page rather than
 * playing, and the header carries the season synopsis in place of the
 * artist and year.
 *
 * Reached from the Music tab instead of the film detail page, because a list
 * of songs is what an album actually is, and from the Series tab once a
 * season is open.
 */

#define ROW_H     26.0f
#define LIST_TOP  168.0f
#define ROW_VIS   8

static int g_sel;
static int g_top;          /* first visible row, for albums over 8 tracks */

/*
 * The synopsis panel, when a season's overview runs past the header.
 *
 * DESC_VIS lines are visible, and L2 and R2 scroll them. The d-pad always
 * belongs to the episode list, so there is no focus to move and no mode to be
 * in: the two are simply independent.
 *
 * Triggers rather than the right stick because they are ordinary buttons.
 * The stick needs the pad put into analogue mode, which can fail quietly on
 * some controllers and leaves the feature apparently dead.
 */
#define DESC_VIS 3

static int g_desc_top;     /* first visible line of the synopsis */

/* Lines the synopsis needs, or 0 when there is none. */
static int desc_lines(void)
{
	if (!jf_album_desc[0])
		return 0;

	return ui_wrap_lines(1.0f, jf_album_desc, SCR_W - 144.0f - 40.0f);
}


/* Fresh state for a newly opened album or season. */
void scr_tracks_reset(void)
{
	g_sel      = 0;
	g_top      = 0;
	g_desc_top = 0;
}


/* Keep the selection inside the list. */
static void enter(void)
{
	if (g_sel >= jf_track_count)
		g_sel = jf_track_count ? jf_track_count - 1 : 0;

	if (g_top > g_sel) g_top = g_sel;
	if (g_sel >= g_top + ROW_VIS) g_top = g_sel - ROW_VIS + 1;
}

/*
 * An episode's detail page, reusing the film one: a title, a synopsis and a
 * Play button. jf_load_detail works on any item id.
 */
static void job_episode_detail(void)
{
	if (jf_load_detail(jf_tracks[g_sel].id) < 0) {
		app_fail(jf_err_title(), jf_err_body());
		return;
	}

	scr_detail_reset();
	app_goto(SCR_DETAIL);
}

/*
 * Up/Down choose, L1/R1 page, X plays (music) or opens the episode page,
 * L2/R2 scroll the description, Circle goes back.
 */
static void input(u32 p)
{
	/*
	 * L2 and R2 scroll the synopsis, independently of the list. Nothing
	 * here touches g_sel, so moving through episodes and reading the
	 * season blurb do not interfere with each other.
	 */
	{
		int max = desc_lines() - DESC_VIS;

		if (max > 0) {
			if ((p & PAD_L2) && g_desc_top > 0)   g_desc_top--;
			if ((p & PAD_R2) && g_desc_top < max) g_desc_top++;
		}
	}

	if (p & PAD_UP) {
		if (g_sel > 0) g_sel--;
	}

	if (p & PAD_DOWN) {
		if (g_sel + 1 < jf_track_count) g_sel++;
	}

	/* Keep the cursor inside the window. */
	if (g_sel < g_top)            g_top = g_sel;
	if (g_sel >= g_top + ROW_VIS) g_top = g_sel - ROW_VIS + 1;

	if ((p & PAD_CROSS) && jf_track_count > 0) {
		if (jf_tab == JF_TAB_SERIES) {
			/*
			 * Episodes open their detail page, like a film. There is
			 * nothing to play yet, and the synopsis is what the screen
			 * is for.
			 */
			app_run_job(job_episode_detail, "Loading...");
		} else {
			/* The footer says Play, so play rather than merely opening
			 * the player and waiting for a second press. */
			scr_player_open(g_sel);
			scr_player_start();
			app_goto(SCR_PLAYER);
		}
	}

	if (p & PAD_CIRCLE)
		app_goto(SCR_BROWSE);
}

/*
 * An album's tracks or a season's episodes: cover, title, description and
 * the list.
 */
static void draw(void)
{
	ui_waves();

	ui_text(24, 16, 1.0f, C_WHITE, "Jellyfin");
	ui_text(24 + ui_text_w(1.0f, "Jellyfin") + 6, 16, 1.0f, C_PURPLE, "PS2");

	{
		char cnt[32];
		const char *word = (jf_tab == JF_TAB_SERIES) ? "episode" : "track";

		snprintf(cnt, sizeof(cnt), "%d %s%s", jf_track_count, word,
		         jf_track_count == 1 ? "" : "s");
		ui_text(SCR_W - 24 - ui_text_w(1.0f, cnt), 16, 1.0f, C_DIM, cnt);
	}

	ui_box(24, 44, SCR_W - 48, 1, C_RULE);

	/* Cover, sized so it clears the rule below the header block. */
	const float cx = 40.0f, cy = 58.0f, cs = 88.0f;

	if (poster_ready(DETAIL_SLOT)) {
		poster_draw(ui_gs(), DETAIL_SLOT, cx, cy, cs, cs);
	} else {
		ui_round(cx, cy, cs, cs, 4,
		         GS_SETREG_RGBAQ(0x1C, 0x1E, 0x3C, 0x80, 0x00));
		ui_box(cx, cy, cs, 3, C_PURPLE);
	}

	const float tx = 144.0f;
	const float tw = SCR_W - tx - 40.0f;

	if (jf_album_desc[0]) {
		/*
		 * A season: the synopsis is worth more than a large title, so the
		 * name goes at body size and the text fills the rest of the
		 * header. Three lines is what fits above the rule; ui_wrap cuts
		 * the remainder.
		 */
		ui_wrap(tx, 58, 22, 1.0f, C_WHITE, jf_album_name, tw, 1);

		int total = desc_lines();

		ui_wrap_from(tx, 84, 20, 1.0f, C_DIM, jf_album_desc, tw,
		             DESC_VIS, g_desc_top);

		/* A bar in the margin, which vanishes when the text fits. */
		ui_scrollbar(SCR_W - 30.0f, 84, DESC_VIS * 20.0f,
		             total, DESC_VIS, g_desc_top);
	} else {
		ui_wrap(tx, 56, 44, 2.0f, C_WHITE, jf_album_name, tw, 1);

		char meta[128] = "";

		if (jf_album_artist[0])
			snprintf(meta, sizeof(meta), "%s", jf_album_artist);

		if (jf_album_year) {
			char y[24];
			snprintf(y, sizeof(y), "%s%d", meta[0] ? "   .   " : "",
			         jf_album_year);
			strncat(meta, y, sizeof(meta) - strlen(meta) - 1);
		}

		if (meta[0])
			ui_text(tx, 104, 1.0f, C_DIM, meta);
	}

	ui_box(40, 156, SCR_W - 80, 1, C_RULE);

	if (jf_track_count == 0) {
		ui_text_c(SCR_W / 2, 220, 1.0f, C_DIM,
		          jf_tab == JF_TAB_SERIES ? "No episodes" : "No tracks");
	}

	for (int i = g_top; i < jf_track_count && i < g_top + ROW_VIS; i++) {
		float y  = LIST_TOP + (i - g_top) * ROW_H;
		int   on = (i == g_sel);

		if (on) {
			ui_round(40, y - 3, SCR_W - 80, ROW_H, 4, C_ROWSEL);
			ui_box(40, y - 3, 3, ROW_H, C_PURPLE);
		}

		char num[16];   /* %02d of an int can run to 11 digits */
		snprintf(num, sizeof(num), "%02d", i + 1);
		ui_text(56, y, 1.0f, on ? C_PURPLE : C_RULE, num);

		char dur[16];
		snprintf(dur, sizeof(dur), "%d:%02d",
		         jf_tracks[i].seconds / 60, jf_tracks[i].seconds % 60);
		float dur_x = SCR_W - 56 - ui_text_w(1.0f, dur);

		/* Keep long titles on one row, clear of the right-aligned runtime. */
		ui_wrap(92, y, ROW_H, 1.0f, on ? C_WHITE : C_DIM,
		        jf_tracks[i].name, dur_x - 92 - 12, 1);
		ui_text(dur_x, y, 1.0f, C_DIM, dur);
	}

	{
		static const BtnGlyph b[] = { BTN_CROSS, BTN_CIRCLE, BTN_SELECT };
		static const char *lm[] = { "Play", "Back", "Quit" };
		static const char *lt[] = { "Open", "Back", "Quit" };

		/* Episodes open a detail page; tracks play straight away. */
		ui_hint_row(b, (jf_tab == JF_TAB_SERIES) ? lt : lm, 3);
	}
}

const Screen scr_tracks = { enter, input, draw };