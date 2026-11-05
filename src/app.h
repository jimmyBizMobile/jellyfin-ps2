/*
 * Screens and jobs: the screen ids, the Screen structure, and switching,
 * progress, jobs and errors.
 */

#ifndef APP_H
#define APP_H

#include <tamtypes.h>

typedef enum {
	SCR_CONNECT,
	SCR_BUSY,        /* framework-owned */
	SCR_RESULT,      /* framework-owned */
	SCR_USERS,
	SCR_LOGIN,       /* typing a name or password to sign in */
	SCR_HOME,        /* Continue Watching and Next Up */
	SCR_NEXT,        /* "Next episode in 5" after an episode ends */
	SCR_BROWSE,
	SCR_DETAIL,
	SCR_TRACKS,
	SCR_PLAYER,
	SCR_SEARCH,
	SCR_VIDEO,
	SCR_USB,         /* the USB tab (work in progress) */
	SCR_COUNT
} ScreenId;

/*
 * A screen is three optional callbacks. Adding a page means writing one
 * file and adding one line to the table in app.c.
 */
typedef struct {
	void (*enter)(void);          /* on transition in */
	void (*input)(u32 pressed);   /* rising edges, plus UI Up/Down repeat */
	void (*draw) (void);          /* per frame */
} Screen;

typedef void (*JobFn)(void);

void app_init(void);
/* Press events (including keyboard START) and current controller levels. */
void app_frame(u32 pressed, u32 held);

void app_goto(ScreenId id);

/*
 * Draws one frame showing `msg`, then runs `fn` with the screen already
 * visible. Networking blocks, so this is how the console shows something
 * other than a frozen previous screen.
 *
 * A job may call app_run_job again to chain, which repaints in between.
 */
void app_run_job(JobFn fn, const char *msg);

/*
 * Update the busy screen from inside a job and repaint immediately.
 *
 * Jobs block, so without this a long job shows one stale message and looks
 * like a hang. Use it for things the user cares about, such as counting
 * posters, not for internal retries.
 */
void app_progress(const char *msg);

/* Jump to the result screen. */
void app_fail(const char *title, const char *body);

/* The freeze safety net set a thread aside: stop what used it (freeze.h). */
void app_freeze(char code);


#endif