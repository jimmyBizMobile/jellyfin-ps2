/*
 * The screen manager. Holds the table of screens (each an enter/input/draw
 * triple), switches between them, runs 'jobs' - blocking work such as
 * network requests - after a frame with a busy message on screen, and
 * shows the result/error screen.
 */

#include "app.h"
#include "config.h"
#include "audio.h"
#include "freeze.h"
#include "screens.h"
#include "ui.h"
#include "jf.h"
#include "remote.h"

#include <libpad.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static ScreenId    g_screen = SCR_CONNECT;
static JobFn       g_job    = NULL;
static char g_msg[80] = "";

static char g_res_title[64] = "";
static char g_res_body[320] = "";
static int  g_res_ok        = 0;

/*
 * Where a job was started from, and the job itself, so a failure that
 * left that screen untouched (app_fail_here) can go back to it and Retry
 * can run the job again. Every other failure goes back to the connect
 * screen, as before.
 */
static ScreenId g_job_from  = SCR_CONNECT;
static JobFn    g_last_job  = NULL;
static char     g_last_msg[80] = "";
static ScreenId g_fail_from = SCR_CONNECT;
static int      g_fail_here = 0;

static u32 g_scroll_direction;
static unsigned long g_scroll_last, g_scroll_wait;

/* Only vertical UI navigation repeats. Other controls keep rising edges;
 * screens still own selection bounds, so list ends never wrap. */
static u32 scroll_repeat(u32 held)
{
	u32 direction = held & (PAD_UP | PAD_DOWN);
	if ((g_screen != SCR_HOME && g_screen != SCR_BROWSE &&
	     g_screen != SCR_DETAIL && g_screen != SCR_TRACKS) ||
	    !direction || direction == (PAD_UP | PAD_DOWN)) {
		g_scroll_direction = 0;
		return 0;
	}

	unsigned long now = (unsigned long)clock();
	if (direction != g_scroll_direction) {
		g_scroll_direction = direction;
		g_scroll_last = now;
		g_scroll_wait = (unsigned long)(CLOCKS_PER_SEC * 350LL / 1000);
		return 0; /* The physical press already supplied the first move. */
	}

	/* Unsigned differences tolerate clock rollover. A stalled/slow frame
	 * emits at most one move, rather than catching up in a burst. */
	if (now - g_scroll_last < g_scroll_wait)
		return 0;
	g_scroll_last = now;
	g_scroll_wait = (unsigned long)(CLOCKS_PER_SEC * 90LL / 1000);
	return direction;
}

/* ------------------------------------------------------------------ */
/* Busy and result are owned by the framework, not by a feature file.  */
/* ------------------------------------------------------------------ */

/*
 * Deliberately the same furniture as the connect page, minus the field and
 * the keyboard, so a page transition does not look like a different app.
 */
static void busy_draw(void)
{
	ui_waves();
	ui_brand();
	ui_text_c(SCR_W / 2, 176, 2.0f, C_WHITE, g_msg);
}

/*
 * Result screen input. X goes back: to the browsing screen a failed job
 * started from (a season that would not load returns to the seasons,
 * signed in as before), otherwise to the connect screen. START retries:
 * the same job for a browsing failure; otherwise, on success it continues
 * to the user page, on failure it connects again.
 */
static void result_input(u32 p)
{
	int back = !g_res_ok && g_fail_here && g_last_job;

	if (p & PAD_CROSS) {
		if (back)
			g_screen = g_fail_from;   /* as it was: no enter(), no reload */
		else
			app_goto(SCR_CONNECT);
	}

	if (p & PAD_START) {
		if (back)
			app_run_job(g_last_job, g_last_msg);
		else if (g_res_ok)
			app_run_job(job_load_users, "Loading users...");
		else
			app_run_job(job_connect,    "Connecting...");
	}
}

/*
 * Result screen: a green or red title, the explanation wrapped below it,
 * and the button hints for Back / Retry or Browse library.
 */
static void result_draw(void)
{
	ui_waves();
	ui_brand();

	ui_text_c(SCR_W / 2, 140, 2.0f, g_res_ok ? C_OK : C_ERR, g_res_title);
	/* max_w is in pixels, not characters. It was 62, which wrapped after
	 * about four letters. */
	ui_wrap(50, 196, 24, 1.0f, C_WHITE, g_res_body, SCR_W - 100, 8);

	if (g_res_ok) {
		static const BtnGlyph b[] = { BTN_START, BTN_CROSS };
		static const char *l[] = { "Browse library", "Back" };
		ui_hint_row(b, l, 2);
	} else {
		static const BtnGlyph b[] = { BTN_CROSS, BTN_START };
		static const char *l[] = { "Back", "Retry" };
		ui_hint_row(b, l, 2);
	}
}

static const Screen scr_busy   = { NULL, NULL,         busy_draw   };
static const Screen scr_result = { NULL, result_input, result_draw };

/* ------------------------------------------------------------------ */

static const Screen *g_screens[SCR_COUNT] = {
	[SCR_CONNECT] = &scr_connect,
	[SCR_BUSY]    = &scr_busy,
	[SCR_RESULT]  = &scr_result,
	[SCR_USERS]   = &scr_users,
	[SCR_LOGIN]   = &scr_login,
	[SCR_HOME]    = &scr_home,
	[SCR_NEXT]    = &scr_next,
	[SCR_BROWSE]  = &scr_browse,
	[SCR_DETAIL]  = &scr_detail,
	[SCR_TRACKS]  = &scr_tracks,
	[SCR_PLAYER]  = &scr_player,
	[SCR_SEARCH]  = &scr_search,
	[SCR_VIDEO]   = &scr_video,
	[SCR_USB]     = &scr_usb,
};

/*
 * Switch to another screen and run its enter() hook. Every screen change
 * disarms the freeze safety net (freeze.h); only the video screen arms it
 * again, and only while playing.
 */
void app_goto(ScreenId id)
{
	freeze_arm(0);                 /* the video screen re-arms itself */
	g_scroll_direction = 0;
	g_screen = id;
	if (g_screens[id]->enter)
		g_screens[id]->enter();
}

/*
 * Draw one 'busy' frame at once with msg - the brand, the waves and a line
 * of text - for jobs that run several steps on the screen thread and want
 * to show which step they are on.
 */
void app_progress(const char *msg)
{
	snprintf(g_msg, sizeof(g_msg), "%s", msg);

	ui_begin();
	busy_draw();
	ui_end();
}

/*
 * Queue fn to run on the screen thread after the current frame, with msg
 * on the busy screen meanwhile. Jobs block the screen thread on purpose
 * (network requests, artwork), so the freeze safety net is disarmed first.
 */
void app_run_job(JobFn fn, const char *msg)
{
	freeze_arm(0);                 /* jobs block the screen on purpose */
	g_scroll_direction = 0;

	/* A job queued by a job keeps the first one's origin. */
	if (g_screen != SCR_BUSY && g_screen != SCR_RESULT)
		g_job_from = g_screen;

	g_last_job = fn;
	snprintf(g_last_msg, sizeof(g_last_msg), "%s", msg);

	g_job    = fn;
	snprintf(g_msg, sizeof(g_msg), "%s", msg);
	g_screen = SCR_BUSY;      /* no enter hook, so no recursion risk */
}

/*
 * Fill the result screen (ok selects green or red) and switch to it. Used
 * by app_fail.
 */
static void set_result(int ok, const char *title, const char *body)
{
	/* Where Back returns to: the job's origin, or the screen that failed. */
	g_fail_from = (g_screen == SCR_BUSY) ? g_job_from : g_screen;
	g_fail_here = 0;

	g_res_ok = ok;
	g_scroll_direction = 0;
	snprintf(g_res_title, sizeof(g_res_title), "%s", title);
	snprintf(g_res_body,  sizeof(g_res_body),  "%s", body);
	g_screen = SCR_RESULT;
}

/*
 * Show the error screen with a title and an explanation. The way every job
 * reports a failure.
 */
void app_fail(const char *t, const char *b) { set_result(0, t, b); }

/*
 * The same, for a job that loads into its own data (a season's episodes,
 * an album's tracks, a film's page) and so leaves the screen it started
 * from as it was: Back returns there, Retry runs the job again, and the
 * sign-in is untouched (v58: a season too large to load used to land on
 * the connect screen, which looked like being signed out).
 */
void app_fail_here(const char *t, const char *b)
{
	set_result(0, t, b);
	g_fail_here = 1;
}


/*
 * Always the connect screen first: nothing is usable until the server is
 * reached and someone has signed in on the user page.
 */
void app_init(void)
{
	app_goto(SCR_CONNECT);
}

/*
 * TITLE on the remote (TOP MENU on later models): HOME from anywhere once
 * signed in. A film or music stops first, through the same careful path
 * as STOP or Circle - every thread finished, the position reported, the
 * transcode ended - and only then is HOME loaded. Before sign-in there is
 * no HOME, so the key does nothing.
 */
static void go_home(void)
{
	switch (g_screen) {
	case SCR_VIDEO:
		scr_video_stop_to_home();
		break;

	case SCR_PLAYER:
		audio_stop();             /* waits for the worker, as Circle does */
		app_run_job(job_load_home, "Loading...");
		break;

	case SCR_HOME:
	case SCR_BROWSE:
	case SCR_DETAIL:
	case SCR_TRACKS:
	case SCR_SEARCH:
	case SCR_USB:
	case SCR_NEXT:
		app_run_job(job_load_home, "Loading...");
		break;

	default:
		break;
	}
}

/*
 * The remote's own keys (remote.h: RMK_ bits), turned into what each
 * screen understands. Video and music handle them themselves; the others
 * get the controller button that does the same thing, or nothing.
 */
static u32 route_remote(u32 pressed)
{
	u32 r = pressed & RMK_ALL;
	u32 p = pressed & ~RMK_ALL;

	if (!r)
		return pressed;

	if (r & RMK_HOME) {
		go_home();
		return 0;                 /* nothing else this frame */
	}

	switch (g_screen) {
	case SCR_VIDEO:
	case SCR_PLAYER:
		return pressed;

	case SCR_BROWSE:
		if (r & RMK_PREV) p |= PAD_L1;
		if (r & RMK_NEXT) p |= PAD_R1;
		break;

	case SCR_DETAIL:
	case SCR_TRACKS:
		if (r & RMK_PLAY) p |= PAD_CROSS;
		break;

	case SCR_NEXT:
		if (r & RMK_PLAY) p |= PAD_CROSS;
		if (r & RMK_STOP) p |= PAD_CIRCLE;
		break;

	default:
		break;
	}

	return p;
}

/*
 * One frame of the application, called by main's loop: the current
 * screen's input, then its draw, then any job queued with app_run_job
 * (after the frame is on screen, so the busy message is visible while the
 * job blocks).
 */
void app_frame(u32 pressed, u32 held)
{
	const Screen *s = g_screens[g_screen];

#if DEBUG_UI
	/*
	 * The geometry test pattern (ui.h), development builds only: R2 shows
	 * and hides it on any screen but video and the busy screen; while it
	 * shows, only R2 does anything.
	 */
	static int geometry;

	if ((pressed & PAD_R2) && g_screen != SCR_VIDEO && g_screen != SCR_BUSY)
		geometry = !geometry;

	if (geometry && g_screen != SCR_VIDEO) {
		g_scroll_direction = 0;
		ui_begin();
		ui_geometry_pattern();
		ui_end();
		return;
	}
#endif

	pressed |= scroll_repeat(held);
	pressed  = route_remote(pressed);

	if (g_screen != SCR_VIDEO && g_screen != SCR_PLAYER)
		pressed &= ~RMK_ALL;          /* never reaches a screen that ignores it */

	s = g_screens[g_screen];          /* TITLE may have queued a job */

	if (s->input)
		s->input(pressed);

	ui_begin();
	g_screens[g_screen]->draw();     /* re-read: input may have moved us */
#if DEBUG_UI
	/* The remote's last raw reply, for mapping a key that does nothing. */
	if (g_screen != SCR_VIDEO)
		ui_text(16, SCR_H - 16, 0.6f, C_DIM, remote_debug());
#endif
	ui_end();

	/*
	 * Blocking work runs only after the frame is on screen, so the user
	 * sees "Connecting..." rather than a frozen previous page.
	 */
	if (g_screen == SCR_BUSY && g_job) {
		JobFn fn = g_job;
		g_job = NULL;
		fn();
	}
}
/*
 * A thread the freeze safety net set aside (freeze.h). During video, stop
 * it and go back to the film's page with the code; during music, stop the
 * song. Elsewhere it was a background thread, and there is nothing to
 * stop: the app carries on.
 */
void app_freeze(char code)
{
	char msg[64];

	snprintf(msg, sizeof(msg), "Playback stopped unexpectedly (%c)", code);

	if (g_screen == SCR_VIDEO || g_screen == SCR_NEXT) {
		scr_video_abort(code);
		scr_detail_set_notice(msg);
		app_goto(SCR_DETAIL);
		return;
	}

	if (g_screen == SCR_PLAYER && (code == 'M' || code == 'P')) {
		audio_stop();
		app_fail("Music stopped unexpectedly", msg);
	}
}
