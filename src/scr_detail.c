/*
 * A film's (or episode's) page: poster, synopsis, Play and Resume, audio
 * and subtitle choices. Starts playback, and starts the next episode for
 * auto-play.
 */

#include "screens.h"
#include "ui.h"
#include "jf.h"
#include "subs.h"
#include "poster.h"

#include <libpad.h>
#include <stdio.h>
#include <string.h>

/*
 * One item, fetched on demand by jf_load_detail rather than prefetched with
 * the page. Overviews run to hundreds of bytes and six of them would push
 * the listing past what the receive mailbox can hold.
 */

/*
 * How much of a long synopsis is visible, and where it starts.
 *
 * OV_VIS lines fit between the metadata and the hint row. L2 and R2 move the
 * window, the same way they do on the episode list, so the two screens
 * behave alike.
 */
#define OV_VIS 6

/*
 * Where the selector rows start.
 *
 * Below six lines of synopsis and above the hint row, in the text column.
 * The Play button used to be here; moving it under the poster freed the
 * space, which is what these rows wanted.
 */
#define ROW_Y 336.0f

static int g_ov_top;

/*
 * Which track is chosen, and which row has the focus.
 *
 * Chosen before playback rather than during it: changing either mid-film
 * would mean a fresh transcode from a new position, which is a different
 * feature. Rows appear only when there is something to choose - a film with
 * one audio track and no subtitles shows nothing but Play.
 */
static int g_audio_sel;      /* into jf_audio */
static int g_sub_sel;        /* -1 for none, else into jf_subs */
static int g_row;            /* 0..n, the last row being Play */

/*
 * Which rows exist, numbered top to bottom as they appear.
 *
 * Play is 0 because it sits under the poster, above the selectors in the
 * text column. Numbering it last put the indices in the opposite order to
 * the screen, so up moved down.
 */
/*
 * Resume sits under Play, under the poster, and only when this user has a
 * place to resume from. The selectors in the text column follow it.
 */
static int has_resume(void) { return jf_detail.resume_ticks > 0; }

/* Row index of the Play button (always 0). */
static int play_row(void)   { return 0; }

/* Row index of Resume, or -1 when there is no resume point. */
static int resume_row(void) { return has_resume() ? 1 : -1; }

/* First row after the buttons. */
static int first_sel(void)  { return has_resume() ? 2 : 1; }

/* Row of the audio selector, or -1 with only one audio track. */
static int audio_row(void)  { return jf_audio_count > 1 ? first_sel() : -1; }

/* Row of the subtitle selector, or -1 with no subtitles. */
static int sub_row(void)
{
	if (jf_sub_count < 1)
		return -1;

	return (audio_row() >= 0) ? audio_row() + 1 : first_sel();
}

/* The largest valid row index for this item. */
static int last_row(void)
{
	if (sub_row()    >= 0) return sub_row();
	if (audio_row()  >= 0) return audio_row();
	if (resume_row() >= 0) return resume_row();

	return 0;
}

/* Lines the synopsis needs, at the width the detail page draws it. */
/*
 * The poster box and the text column beside it.
 *
 * Shared so that measuring and drawing cannot disagree. They did: the line
 * count was taken at a width 28 pixels wider than the text is actually drawn,
 * so it came out short and scrolling stopped before the end of the synopsis.
 */
#define PX 40.0f
#define PY 66.0f
#define PW 130.0f
#define PH 195.0f

#define TX (PX + PW + 28.0f)
#define TW (SCR_W - TX - 40.0f)

/* Lines the synopsis needs, to know how far L2/R2 can scroll it. */
static int overview_lines(void)
{
	if (!jf_detail.overview[0])
		return 0;

	return ui_wrap_lines(1.0f, jf_detail.overview, TW);
}

/* A message from the last playback, shown until something else happens. */
static char g_notice[80];

/*
 * A red line for the top of the page, such as the freeze safety net's
 * message.
 */
void scr_detail_set_notice(const char *s)
{
	snprintf(g_notice, sizeof(g_notice), "%s", s ? s : "");
}

/*
 * Fresh state for a newly opened item: focus on Play, default tracks, no
 * notice.
 */
void scr_detail_reset(void)
{
	g_notice[0] = '\0';
	g_ov_top = 0;
	g_row    = play_row();      /* start on Play: the common case is to press it */

	/*
	 * Default audio is the first track, which is the one Jellyfin lists
	 * first and usually flags as default. Default subtitles are none:
	 * burning them costs the server real time, and anyone who wants them
	 * can say so.
	 */
	g_audio_sel = 0;
	g_sub_sel   = -1;
}

/*
 * The tracks last chosen, by name, so the next episode can pick the same
 * ones: the same label if it has it, else the same language.
 */
static char g_last_audio[48];
static char g_last_sub[48];     /* "" for none */
static int  g_last_burn;        /* ...and whether it was the burned-in one */

/* Play jf_detail with the chosen tracks, from the start or resumed. */
static void start_play(int resume)
{
	g_notice[0] = '\0';
	int a = (jf_audio_count > 0) ? jf_audio[g_audio_sel].index : -1;
	int b = (g_sub_sel >= 0)     ? jf_subs[g_sub_sel].index    : -1;

	snprintf(g_last_audio, sizeof(g_last_audio), "%s",
	         jf_audio_count ? jf_audio[g_audio_sel].label : "");
	snprintf(g_last_sub, sizeof(g_last_sub), "%s",
	         g_sub_sel >= 0 ? jf_subs[g_sub_sel].label : "");
	g_last_burn = g_sub_sel >= 0 && jf_subs[g_sub_sel].burn;

	/*
	 * Text subtitles are fetched now and drawn by the PS2, so the
	 * stream itself is the plain one: as quick to start, resume
	 * and seek as with no subtitles at all. Only picture-based
	 * tracks (PGS, VobSub) are still burned in - and a text track
	 * whose download fails falls back to that too.
	 */
	if (b >= 0 && jf_subs[g_sub_sel].is_text) {
		const char *c   = jf_subs[g_sub_sel].codec;
		int         ass = !strcmp(c, "ass") || !strcmp(c, "ssa");

		app_progress("Loading subtitles...");

		if (subs_load(jf_detail.id, b, ass) > 0)
			b = -1;              /* drawn here, not burned in */
	} else {
		subs_clear();
	}

	scr_video_open_at(jf_detail.id, a, b,
	                  resume ? jf_detail.resume_ticks : 0);
	app_goto(SCR_VIDEO);
}

/* The language part of a label: "French (Forced)" -> "French". */
static int same_language(const char *a, const char *b)
{
	int la = strcspn(a, "("), lb = strcspn(b, "(");

	while (la > 0 && a[la - 1] == ' ') la--;
	while (lb > 0 && b[lb - 1] == ' ') lb--;

	return la > 0 && la == lb && strncmp(a, b, la) == 0;
}

/*
 * The index of the track matching want: the same label, else the same
 * language. -1 if none.
 */
static int pick(const JfStream *s, int n, const char *want)
{
	for (int i = 0; i < n; i++)
		if (strcmp(s[i].label, want) == 0)
			return i;

	for (int i = 0; i < n; i++)
		if (same_language(s[i].label, want))
			return i;

	return -1;
}

/*
 * Auto-play: the next episode, with the tracks the last one was played
 * with. jf_next has been found; this loads its page data and starts it.
 */
void job_play_next(void)
{
	if (jf_load_detail(jf_next.id) < 0) {
		app_fail(jf_err_title(), jf_err_body());
		return;
	}

	poster_clear(DETAIL_SLOT);
	scr_load_poster(DETAIL_SLOT, jf_next.image_id, DETAIL_MAX_W - 8);

	scr_detail_reset();

	if (g_last_audio[0]) {
		int i = pick(jf_audio, jf_audio_count, g_last_audio);
		if (i >= 0)
			g_audio_sel = i;
	}

	g_sub_sel = g_last_sub[0] ? pick(jf_subs, jf_sub_count, g_last_sub) : -1;

	/* The burned-in copy of an ASS track follows the drawn one. */
	if (g_sub_sel >= 0 && g_last_burn && g_sub_sel + 1 < jf_sub_count &&
	    jf_subs[g_sub_sel + 1].burn &&
	    jf_subs[g_sub_sel + 1].index == jf_subs[g_sub_sel].index)
		g_sub_sel++;

	start_play(0);
}

/*
 * Up/Down move between rows, Left/Right change a selector, X plays or
 * resumes, L2/R2 scroll the synopsis, Circle goes back (to HOME, the
 * episode list or the grid).
 */
static void input(u32 p)
{
	/* Scroll the synopsis. Nothing else on this screen uses the triggers. */
	{
		int max = overview_lines() - OV_VIS;

		if (max > 0) {
			if ((p & PAD_L2) && g_ov_top > 0)   g_ov_top--;
			if ((p & PAD_R2) && g_ov_top < max) g_ov_top++;
		}
	}

	/* Move between the selectors and Play. */
	if (p & PAD_UP)   { if (g_row > 0)            g_row--; }
	if (p & PAD_DOWN) { if (g_row < last_row())   g_row++; }

	/* Change the value on the focused row. */
	if (p & (PAD_LEFT | PAD_RIGHT)) {
		int step = (p & PAD_RIGHT) ? 1 : -1;

		if (g_row == audio_row() && jf_audio_count > 0) {
			g_audio_sel += step;

			if (g_audio_sel < 0)                g_audio_sel = jf_audio_count - 1;
			if (g_audio_sel >= jf_audio_count)  g_audio_sel = 0;
		} else if (g_row == sub_row() && jf_sub_count > 0) {
			/*
			 * None sits at -1, before the first track, so the list
			 * runs from -1 to count-1 and wraps at both ends.
			 */
			g_sub_sel += step;

			if (g_sub_sel < -1)              g_sub_sel = jf_sub_count - 1;
			if (g_sub_sel >= jf_sub_count)   g_sub_sel = -1;
		}
	}

	/*
	 * Play this film, with whatever was chosen: from the start, or from
	 * where this user left off when the Resume button is focused. Only
	 * the button resumes - there is no shortcut for it.
	 */
	if (p & PAD_CROSS) {
		start_play(g_row == resume_row());
		return;
	}

	/* Circle is the way back, so there is no state you cannot leave. */
	if (p & PAD_CIRCLE) {
		/*
		 * Back where it was opened from: HOME (reloaded, since positions
		 * may have changed), the episode list for an episode, the grid
		 * for a film. Going to the grid either way would drop the viewer
		 * out of the season they were in.
		 */
		if (g_from_home) {
			g_from_home = 0;
			app_run_job(job_load_home, "Loading...");
		} else {
			app_goto(jf_tab == JF_TAB_SERIES ? SCR_TRACKS : SCR_BROWSE);
		}
	}
}

/*
 * The film page: poster, title, year and runtime, synopsis, Play/Resume,
 * the audio and subtitle selectors with the TEXT/IMAGE badge, any notice.
 */
static void draw(void)
{
	ui_waves();

	ui_text(24, 16, 1.0f, C_WHITE, "Jellyfin");
	ui_text(24 + ui_text_w(1.0f, "Jellyfin") + 6, 16, 1.0f, C_PURPLE, "PS2");

	if (g_notice[0])
		ui_text(SCR_W - 24 - ui_text_w(1.0f, g_notice), 16, 1.0f, C_ERR,
		        g_notice);

	ui_box(24, 44, SCR_W - 48, 1, C_RULE);

	const float px = PX, py = PY, pw = PW, ph = PH;

	if (poster_ready(DETAIL_SLOT)) {
		poster_draw(ui_gs(), DETAIL_SLOT, px, py, pw, ph);
	} else {
		ui_round(px, py, pw, ph, 4,
		         GS_SETREG_RGBAQ(0x1C, 0x1E, 0x3C, 0x80, 0x00));
		ui_box(px, py, pw, 3, C_PURPLE);
	}

	const float tx = TX;
	const float tw = TW;

	/*
	 * Title, two lines at double size if it needs them.
	 *
	 * ui_wrap reports the lines the title NEEDS, which can exceed the two
	 * it was allowed to draw, so clamp before using it to place what
	 * follows or a long title pushes the metadata off the screen.
	 */
	int lines = ui_wrap(tx, 66, 46, 2.0f, C_WHITE, jf_detail.name, tw, 2);
	if (lines > 2) lines = 2;

	float y = 66.0f + lines * 46.0f + 8.0f;

	/* Year, runtime, type on one line, skipping whatever is missing. */
	{
		char meta[96] = "";

		if (jf_detail.year)
			snprintf(meta, sizeof(meta), "%d", jf_detail.year);

		if (jf_detail.runtime_min > 0) {
			char t[32];
			snprintf(t, sizeof(t), "%s%dh %02dm", meta[0] ? "   .   " : "",
			         jf_detail.runtime_min / 60, jf_detail.runtime_min % 60);
			strncat(meta, t, sizeof(meta) - strlen(meta) - 1);
		}

		if (jf_detail.type[0]) {
			char t[32];
			snprintf(t, sizeof(t), "%s%s", meta[0] ? "   .   " : "",
			         jf_detail.type);
			strncat(meta, t, sizeof(meta) - strlen(meta) - 1);
		}

		if (meta[0]) {
			ui_text(tx, y, 1.0f, C_DIM, meta);
			y += 32.0f;
		}
	}

	if (jf_detail.overview[0]) {
		int total = ui_wrap_from(tx, y, 22, 1.0f, C_DIM,
		                         jf_detail.overview, tw, OV_VIS,
		                         g_ov_top);

		/*
		 * A bar in the margin rather than a line of button names: it
		 * shows how much is left as well as that there is more, and it
		 * disappears by itself when everything fits.
		 */
		ui_scrollbar(SCR_W - 30.0f, y, OV_VIS * 22.0f,
		             total, OV_VIS, g_ov_top);
	} else {
		ui_text(tx, y, 1.0f, C_RULE, "No description.");
	}

	/*
	 * The track selectors, under the synopsis.
	 *
	 * Only what there is a choice about: one audio track and no subtitles
	 * means neither row appears and the page looks as it always did.
	 *
	 * The value carries the language rather than reading On or Off,
	 * because a file can hold two French subtitle tracks and "On" says
	 * nothing about which one is about to be burned in.
	 */
	{
		for (int r = first_sel(); r <= last_row(); r++) {
			const char *label;
			const char *value;

			if (r == audio_row()) {
				label = "Audio";
				value = jf_audio_count ? jf_audio[g_audio_sel].label : "-";
			} else {
				label = "Subtitles";
				value = (g_sub_sel >= 0) ? jf_subs[g_sub_sel].label : "None";
			}

			float ry = ROW_Y + (r - first_sel()) * 32.0f;
			int   on = (g_row == r);

			/*
			 * The row spans the text column. Its parts, left to right:
			 * the label, "<", the value, ">", and for subtitles the
			 * TEXT / IMAGE / BURNED tag inside the row's right end. The
			 * value is shortened with "..." to fit between the arrows,
			 * so a long track name never runs under the tag (v58).
			 */
			float rw = TW;
			const char *tag = NULL;
			float bw = 0;

			if (r != audio_row() && g_sub_sel >= 0) {
				tag = jf_subs[g_sub_sel].is_text ? "TEXT"
				    : jf_subs[g_sub_sel].burn    ? "BURNED"
				                                 : "IMAGE";
				bw  = ui_text_w(1.0f, tag) + 12;
			}

			float right = tx + rw - (tag ? bw + 10 : 0);   /* end of the value area */
			float gt_x  = right - 16;                       /* ">" */
			float vx    = tx + 140;                         /* the value */

			ui_round(tx, ry, rw, 28, 4,
			         on ? GS_SETREG_RGBAQ(0x2A, 0x2C, 0x50, 0x80, 0x00)
			            : GS_SETREG_RGBAQ(0x1C, 0x1E, 0x3C, 0x80, 0x00));

			/* A brighter left edge marks the focused row. */
			ui_box(tx, ry, 3, 28, on ? C_LILAC : C_PURPLE);

			ui_text(tx + 14, ry + 6, 1.0f, on ? C_WHITE : C_DIM, label);

			/* Arrows only on the focused row, so the eye is not
			 * drawn to rows that are not being changed. */
			if (on) {
				ui_text(tx + 120, ry + 6, 1.0f, C_LILAC, "<");
				ui_text(gt_x,     ry + 6, 1.0f, C_LILAC, ">");
			}

			{
				char  fit[100];         /* the name, or its shortened form... */
				float maxw = gt_x - 8 - vx;

				snprintf(fit, sizeof(fit), "%s", value);

				if (ui_text_w(1.0f, fit) > maxw) {
					int n = (int)strlen(fit);

					while (n > 0) {
						fit[--n] = '\0';
						while (n > 0 && fit[n - 1] == ' ')
							fit[--n] = '\0';
						char tmp[sizeof(fit)];  /* ...and the same size for the trial */
						snprintf(tmp, sizeof(tmp), "%s...", fit);
						if (ui_text_w(1.0f, tmp) <= maxw) {
							snprintf(fit, sizeof(fit), "%s", tmp);
							break;
						}
					}
				}

				ui_text(vx, ry + 6, 1.0f, on ? C_WHITE : C_DIM, fit);
			}

			/*
			 * What kind of subtitles: TEXT is drawn by the PS2, quick
			 * to start and seek; IMAGE is burned in by the server,
			 * slower to start; BURNED is an ASS track burned in with
			 * all its signs and effects. Nothing for None.
			 */
			if (tag) {
				int   t  = jf_subs[g_sub_sel].is_text;
				float bx = tx + rw - bw - 4;

				ui_round(bx, ry + 3, bw, 22, 4,
				         t ? C_OK : GS_SETREG_RGBAQ(0xE8, 0xB0, 0x4A, 0x80, 0x00));
				ui_text(bx + 6, ry + 4, 1.0f,
				        GS_SETREG_RGBAQ(0x14, 0x16, 0x2C, 0x80, 0x00), tag);
			}
		}
	}

	{
		const char *label = "Play";

		/*
		 * Under the poster, where there was nothing but background.
		 * It used to sit in the text column and crowd the synopsis.
		 */
		float bw = PW;
		float bh = 34.0f;
		float bx = PX;
		float by = PY + PH + 18.0f;

		ui_round(bx, by, bw, bh, 6,
		         (g_row == play_row()) ? C_PURPLE
		                               : GS_SETREG_RGBAQ(0x5C, 0x35, 0x6E, 0x80, 0x00));

		/* A lighter bar along the top edge, so it reads as a raised
		 * control rather than a flat block. */
		ui_round(bx + 3, by + 2, bw - 6, 3, 1, C_LILAC);

		/* Cross glyph, then the word. */
		{
			float cx = bx + 20.0f, cy = by + bh * 0.5f;

			ui_box(cx - 6, cy - 1, 13, 2, C_WHITE);
			ui_box(cx - 1, cy - 6, 2, 13, C_WHITE);
		}

		ui_text(bx + 36.0f, by + 7.0f, 1.0f, C_WHITE, label);
	}

	/*
	 * Resume, under Play: the same button, quieter, with how far this
	 * user got - a thin bar in the music player's colours and the time.
	 */
	if (has_resume()) {
		float bw = PW;
		float bh = 34.0f;
		float bx = PX;
		float by = PY + PH + 18.0f + 34.0f + 8.0f;
		int   on = (g_row == resume_row());

		ui_round(bx, by, bw, bh, 6,
		         on ? C_PURPLE : GS_SETREG_RGBAQ(0x3A, 0x30, 0x5C, 0x80, 0x00));
		ui_round(bx + 3, by + 2, bw - 6, 3, 1, on ? C_LILAC : C_FIELD);

		/* A play arrow, as on the music player's Play button. */
		{
			float cx = bx + 20.0f, cy = by + bh * 0.5f;

			for (int i = 0; i < 14; i++) {
				float half = (i < 7) ? i : 13 - i;
				ui_box(cx - 5, cy - 7 + i, 2 + half * 1.4f, 1, C_WHITE);
			}
		}

		ui_text(bx + 36.0f, by + 7.0f, 1.0f, C_WHITE, "Resume");

		/* How far: the bar, then the time under it. */
		{
			long long len = jf_detail.runtime_ticks;
			long long pos = jf_detail.resume_ticks;
			float     ty  = by + bh + 8.0f;
			char      t[24];
			int       sec = (int)(pos / 10000000LL);

			ui_box(bx, ty, bw, 3, C_RULE);

			if (len > 0) {
				float f = (float)((double)pos / (double)len);

				if (f > 1.0f) f = 1.0f;
				ui_box(bx, ty, bw * f, 3, C_PURPLE);
			}

			if (sec >= 3600)
				snprintf(t, sizeof(t), "%d:%02d:%02d", sec / 3600,
				         (sec / 60) % 60, sec % 60);
			else
				snprintf(t, sizeof(t), "%d:%02d", sec / 60, sec % 60);

			ui_text_c(bx + bw * 0.5f, ty + 8.0f, 1.0f, C_DIM, t);
		}
	}

	/*
	 * If saving the last stop's position failed, say so quietly: the
	 * Resume button is then only good until the page is reloaded.
	 */
	{
		int st = jf_resume_status();

		if (st != 0 && (st < 200 || st > 299)) {
			char m[48];

			snprintf(m, sizeof(m), "position not saved (%d)", st);

			/* Top right of the header, where the music screen shows
			 * its track count: clear of the buttons and selectors. */
			ui_text(SCR_W - 24 - ui_text_w(1.0f, m), 16, 1.0f, C_ERR, m);
		}
	}

	{
		static const BtnGlyph b[] = { BTN_CROSS, BTN_CIRCLE, BTN_SELECT };
		static const char *l[] = { "Play", "Back", "Quit" };

		ui_hint_row(b, l, 3);
	}
}

const Screen scr_detail = { NULL, input, draw };