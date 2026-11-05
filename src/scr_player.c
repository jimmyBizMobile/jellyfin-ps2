/*
 * The music player: an album's track with cover, time bar and transport
 * buttons, moving on to the next track by itself.
 */

#include "screens.h"
#include "config.h"
#include "ui.h"
#include "jf.h"
#include "poster.h"
#include "audio.h"
#include "net.h"

#include <libpad.h>
#include "remote.h"
#include <stdio.h>
#include <string.h>

/*
 * The player.
 *
 * The transport is fully interactive but nothing is decoded yet: the buttons
 * change state, the track changes, and the clock stays at zero. That makes
 * the screen honest about what it does while leaving the audio pipeline as a
 * separate job.
 *
 * When playback exists, only the four handlers below need to do real work.
 */

typedef enum { BTN_PREV, BTN_PLAY, BTN_STOP, BTN_NEXT, BTN_N } Btn;

#define BW 52.0f
#define BH 42.0f
#define BGAP 14.0f
#define BTOP 322.0f   /* clear of the status line at 284 */

static int g_btn = BTN_PLAY;
static int g_track;          /* index into jf_tracks */

/*
 * Playback state lives in audio.c, on the worker thread. Reading it rather
 * than keeping a copy means the screen can never disagree with reality: a
 * track that ends on its own, or a connection that fails, is reflected the
 * next frame without anything having to notice and tell us.
 */

static int is_playing(void)
{
	AudioState st = audio_state();
	return st == AUDIO_PLAYING || st == AUDIO_BUFFERING;
}

/*
 * Begin the current track. Public so the track list can play immediately on
 * Cross rather than opening the player and waiting for a second press.
 */
void scr_player_start(void)
{
	if (g_track < 0 || g_track >= jf_track_count)
		return;

	audio_play(jf_host(), jf_port(),
	           jf_track_stream_path(jf_tracks[g_track].id), jf_key());
}

/*
 * Open the player on a track of the loaded album, stopping whatever played
 * before.
 */
void scr_player_open(int track)
{
	g_track = track;
	g_btn   = BTN_PLAY;

	audio_stop();          /* whatever was playing before is not this */
}

/* Whether there is a track before this one. */
static int has_prev(void) { return g_track > 0; }

/* Whether there is a track after this one. */
static int has_next(void) { return g_track + 1 < jf_track_count; }

/* Keep the track index inside the album. */
static void enter(void)
{
	if (g_track >= jf_track_count)
		g_track = jf_track_count ? jf_track_count - 1 : 0;
}

/* Act on the focused transport button: previous, play/pause, stop, next. */
static void activate(void)
{
	switch (g_btn) {
	case BTN_PREV:
		if (has_prev()) {
			int was = is_playing();
			g_track--;
			audio_stop();
			if (was) scr_player_start();   /* keep going, on the new track */
		}
		break;

	case BTN_PLAY:
		switch (audio_state()) {
		case AUDIO_PLAYING:
		case AUDIO_BUFFERING:
			audio_pause();
			break;

		case AUDIO_PAUSED:
			audio_resume();
			break;

		default:
			scr_player_start();
			break;
		}
		break;

	case BTN_STOP:
		audio_stop();
		break;

	case BTN_NEXT:
		if (has_next()) {
			int was = is_playing();
			g_track++;
			audio_stop();
			if (was) scr_player_start();
		}
		break;

	default:
		break;
	}
}

/*
 * Moves to the next track when one ends, then Left/Right between buttons,
 * X to act, Circle back to the track list.
 */
static void input(u32 p)
{
	/*
	 * Move to the next track when this one finishes.
	 *
	 * input runs every frame, not only on a button press, so this is the
	 * place for it. audio_ended distinguishes a track that ran out from one
	 * that was stopped, which matters: pressing Stop should stop, not skip.
	 *
	 * On the last track it simply stops, rather than wrapping round to the
	 * beginning of the album.
	 */
	if (audio_ended() && audio_state() == AUDIO_STOPPED) {
		if (has_next()) {
			g_track++;
			scr_player_start();
		}
	}

	if (p & PAD_LEFT)  { if (g_btn > 0)         g_btn--; }
	if (p & PAD_RIGHT) { if (g_btn < BTN_N - 1) g_btn++; }

	if (p & PAD_CROSS)
		activate();

	/*
	 * The remote's own keys (v60). PLAY only plays and PAUSE only pauses;
	 * STOP, PREV and NEXT do what their buttons do, without moving the
	 * focus.
	 */
	if (p & RMK_PLAY) {
		if (audio_state() == AUDIO_PAUSED)
			audio_resume();
		else if (!is_playing() && jf_track_count > 0)
			scr_player_start();
	}

	if ((p & RMK_PAUSE) && is_playing())
		audio_pause();

	if (p & (RMK_STOP | RMK_PREV | RMK_NEXT)) {
		int keep = g_btn;

		g_btn = (p & RMK_STOP) ? BTN_STOP : (p & RMK_PREV) ? BTN_PREV : BTN_NEXT;
		activate();
		g_btn = keep;
	}

	if (p & PAD_CIRCLE) {
		/*
		 * Back stops playback and waits for the worker to close its
		 * socket, so nothing is left running behind the track list.
		 */
		audio_stop();


		app_goto(SCR_TRACKS);
	}
}

/* ------------------------------------------------------------------ */

static void draw_button(float x, float y, Btn kind, int enabled, int focused)
{
	if (focused)
		ui_round(x - 3, y - 3, BW + 6, BH + 6, 7, C_PURPLE);

	ui_round(x, y, BW, BH, 5,
	         enabled ? C_ROWSEL
	                 : GS_SETREG_RGBAQ(0x1C, 0x1E, 0x3C, 0x80, 0x00));

	u64 col = enabled ? C_WHITE : C_RULE;

	float cx = x + BW * 0.5f;
	float cy = y + BH * 0.5f;

	switch (kind) {
	case BTN_PLAY:
		if (is_playing()) {
			/* Pause: two bars. */
			ui_box(cx - 8, cy - 10, 6, 20, col);
			ui_box(cx + 2, cy - 10, 6, 20, col);
		} else {
			/* Play: a triangle, built from rows since the GS has no
			 * polygon primitive in this toolkit. */
			for (int i = 0; i < 20; i++) {
				float half = (i < 10) ? i : 19 - i;
				ui_box(cx - 7, cy - 10 + i, 4 + half * 1.6f, 1, col);
			}
		}
		break;

	case BTN_STOP:
		ui_box(cx - 9, cy - 9, 18, 18, col);
		break;

	case BTN_PREV:
		ui_box(cx - 11, cy - 9, 3, 18, col);
		for (int i = 0; i < 18; i++) {
			float half = (i < 9) ? i : 17 - i;
			ui_box(cx + 2 - half * 1.1f, cy - 9 + i, half * 1.1f + 1, 1, col);
		}
		break;

	case BTN_NEXT:
		ui_box(cx + 8, cy - 9, 3, 18, col);
		for (int i = 0; i < 18; i++) {
			float half = (i < 9) ? i : 17 - i;
			ui_box(cx - 2, cy - 9 + i, half * 1.1f + 1, 1, col);
		}
		break;

	default:
		break;
	}
}

/*
 * The music player: cover, title, artist and album, the time bar, status,
 * the transport buttons.
 */
static void draw(void)
{
	ui_waves();

	ui_text(24, 16, 1.0f, C_WHITE, "Jellyfin");
	ui_text(24 + ui_text_w(1.0f, "Jellyfin") + 6, 16, 1.0f, C_PURPLE, "PS2");

	{
		char pos[32];
		snprintf(pos, sizeof(pos), "%d of %d", g_track + 1, jf_track_count);
		ui_text(SCR_W - 24 - ui_text_w(1.0f, pos), 16, 1.0f, C_DIM, pos);
	}

	ui_box(24, 44, SCR_W - 48, 1, C_RULE);

	/* Cover */
	const float cs = 120.0f;
	const float cx = (SCR_W - cs) * 0.5f;

	if (poster_ready(DETAIL_SLOT)) {
		poster_draw(ui_gs(), DETAIL_SLOT, cx, 58, cs, cs);
	} else {
		ui_round(cx, 58, cs, cs, 4,
		         GS_SETREG_RGBAQ(0x1C, 0x1E, 0x3C, 0x80, 0x00));
		ui_box(cx, 58, cs, 3, C_PURPLE);
	}

	if (jf_track_count > 0)
		ui_text_c(SCR_W / 2, 190, 1.0f, C_WHITE, jf_tracks[g_track].name);

	{
		char sub[200] = "";

		if (jf_album_artist[0])
			snprintf(sub, sizeof(sub), "%s", jf_album_artist);

		if (jf_album_name[0]) {
			char a[140];
			snprintf(a, sizeof(a), "%s%s", sub[0] ? "   .   " : "",
			         jf_album_name);
			strncat(sub, a, sizeof(sub) - strlen(sub) - 1);
		}

		if (sub[0])
			ui_text_c(SCR_W / 2, 214, 1.0f, C_DIM, sub);
	}

	/*
	 * Progress, measured by how much PCM has reached the SPU2 rather than
	 * by a timer. That makes it honest: if the stream stalls, the bar
	 * stalls with it instead of drifting ahead of what you can hear.
	 */
	{
		const float bx = 110.0f, bw = SCR_W - 220.0f;

		int total = jf_track_count ? jf_tracks[g_track].seconds : 0;
		int pos   = audio_position();

		if (total > 0 && pos > total)
			pos = total;

		ui_box(bx, 250, bw, 5, C_RULE);

		if (total > 0 && pos > 0) {
			float frac = (float)pos / (float)total;
			ui_box(bx, 250, bw * frac, 5, C_PURPLE);
		}

		char left[16], right[16];
		snprintf(left,  sizeof(left),  "%d:%02d", pos / 60, pos % 60);
		snprintf(right, sizeof(right), "%d:%02d", total / 60, total % 60);

		ui_text(bx, 264, 1.0f, C_DIM, left);
		ui_text(bx + bw - ui_text_w(1.0f, right), 264, 1.0f, C_DIM, right);
	}

	/* Anything the worker wants to report: a failed connect, mostly. */
	if (audio_state() == AUDIO_ERROR && audio_error()[0])
		ui_text_c(SCR_W / 2, 284, 1.0f, C_ERR, audio_error());
	else if (audio_state() == AUDIO_BUFFERING) {
		ui_text_c(SCR_W / 2, 284, 1.0f, C_DIM, "Buffering...");

#if DEBUG_UI
		/* Where the music worker is: see audio_debug. */
		ui_text_c(SCR_W / 2, 372, 1.0f, C_DIM, audio_debug());
#endif
	}

	/* Transport */
	{
		float total = BTN_N * BW + (BTN_N - 1) * BGAP;
		float x     = (SCR_W - total) * 0.5f;

		int enabled[BTN_N];
		enabled[BTN_PREV] = has_prev();
		enabled[BTN_PLAY] = jf_track_count > 0;
		enabled[BTN_STOP] = is_playing() || audio_state() == AUDIO_PAUSED;
		enabled[BTN_NEXT] = has_next();

		for (int i = 0; i < BTN_N; i++) {
			draw_button(x, BTOP, (Btn)i, enabled[i], i == g_btn);
			x += BW + BGAP;
		}
	}

	{
		static const BtnGlyph b[] = { BTN_CROSS, BTN_DPAD,
		                              BTN_CIRCLE, BTN_SELECT };
		static const char *l[] = { "Select", "Move", "Back", "Quit" };
		ui_hint_row(b, l, 4);
	}
}

const Screen scr_player = { enter, input, draw };