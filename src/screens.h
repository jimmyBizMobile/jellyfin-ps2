/*
 * Every screen, and the functions screens share with each other.
 */

#ifndef SCREENS_H
#define SCREENS_H

#include "app.h"

/* One per feature file. */
extern const Screen scr_login;
extern const Screen scr_home;
extern const Screen scr_next;
extern const Screen scr_connect;
extern const Screen scr_users;
extern const Screen scr_browse;
extern const Screen scr_detail;
extern const Screen scr_tracks;
extern const Screen scr_player;
extern const Screen scr_video;
extern const Screen scr_usb;

/* Which track scr_tracks has highlighted, and how the player is opened. */

/* Put the list and its synopsis panel back to the top, for a new album or
 * season. */
void scr_tracks_reset(void);

/* Put the detail page's synopsis back to the first line, for a new item. */
void scr_detail_reset(void);

/*
 * Which item the video screen should stream.
 *
 * An empty id plays mass:/test.bin instead, which keeps the decoder
 * testable with the network out of the picture.
 */

/* The same, starting at start_ticks (100 ns) instead of the beginning. */
void scr_video_open_at(const char *item_id, int audio_index, int sub_index,
                       long long start_ticks);

/*
 * The demuxed audio from the film currently playing.
 *
 * Filled by the reader thread, independently of the picture. The timestamps
 * are in milliseconds on a clock both streams share, or -1 before the first
 * one arrives.
 */
int scr_video_audio_read(void *buf, int len);
int scr_video_audio_avail(void);

/*
 * The PTS (90 kHz) of the first audio byte in the ring, or -1 before any.
 * The audio clock counts from here.
 */
long long scr_video_audio_pts0(void);

/* The first picture is on screen: the sound may start. */
int scr_video_first_shown(void);

/* That first picture's PTS (90 kHz): the sound starts there too. */
long long scr_video_first_vpts(void);

/* The stream has ended: whatever is in the rings is all there will be. */
int scr_video_source_done(void);

/* 1 while the reader thread's stack has never overflowed. */
int scr_video_stack_ok(void);
void scr_player_open(int track);

/* Begin the track the player was opened on. */
void scr_player_start(void);
extern const Screen scr_search;

/*
 * Jobs are the blocking half of a screen. They live next to the screen
 * that starts them, and are declared here because app.c chains between
 * them from the result page.
 */
void job_connect(void);
void job_load_users(void);

/* The sign-in screen: name asked first when ask_name, note shown in red. */
void login_open(const char *name, int ask_name, const char *note);
void job_load_items(void);

/* HOME: load both rows and show them. g_from_home: the detail page was
 * opened from HOME, so Back returns there. */
void job_load_home(void);

/* TITLE during a film: stop as STOP does, then go to HOME (v60). */
void scr_video_stop_to_home(void);

/* Auto-play: start jf_next with the last episode's tracks (scr_detail.c). */
void job_play_next(void);

/* The freeze safety net (freeze.h): stop playback; show why on the page. */
void scr_video_abort(char code);
void scr_detail_set_notice(const char *s);
extern int g_from_home;

/* Shared with HOME (scr_browse.c): one poster into one slot; the tab row,
 * on = lit tab (-1 HOME), focused = the d-pad is on it. */
int  scr_load_poster(int slot, const char *id, int max_width);
void scr_draw_tabs(int on, int focused);

/* The lit-tab value for the USB tab, after the library tabs. */
#define SCR_TAB_USB JF_TAB_COUNT

#endif