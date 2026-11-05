/*
 * The PS2 DVD remote (v60): the keys a controller does not have.
 *
 * On consoles with a built-in IR receiver (SCPH-50000 series and the
 * slims) the console itself turns the remote's controller keys - X O
 * Triangle Square, the arrows, L1-R3, SELECT, START - into controller 1
 * input, so they already work everywhere. This module reads the rest:
 * PLAY, PAUSE, STOP, TITLE, DVD MENU, RETURN, ENTER, CLEAR, SCAN, PREV,
 * NEXT, DISPLAY, TIME, SUBTITLE.
 *
 * Keys that stand for a controller button come back as that button's
 * PAD_ bit (ENTER is X, RETURN and DVD MENU are Circle, CLEAR is Square,
 * SUBTITLE is R3); the others as RMK_ bits above the controller's 16, which
 * app.c turns into each screen's own action.
 */

#ifndef JFPS2_REMOTE_H
#define JFPS2_REMOTE_H

#include <tamtypes.h>

#define RMK_PLAY     (1u << 16)
#define RMK_PAUSE    (1u << 17)
#define RMK_STOP     (1u << 18)
#define RMK_HOME     (1u << 19)    /* TITLE (TOP MENU on later remotes) */
#define RMK_PREV     (1u << 20)
#define RMK_NEXT     (1u << 21)
#define RMK_SCAN_B   (1u << 22)
#define RMK_SCAN_F   (1u << 23)
#define RMK_DISPLAY  (1u << 24)    /* DISPLAY and TIME */

#define RMK_ALL      (0x1FFu << 16)

/* Start reading the receiver, unless settings.txt says remote=off. Call
 * once, after the IOP modules are loaded. */
void remote_init(void);

/* Whether the receiver answered at start-up. */
int  remote_active(void);

/* Keys pressed since the last call (rising edges, plus repeats for keys
 * held down that should repeat), as PAD_ and RMK_ bits. Once a frame. */
u32  remote_pressed(void);

/* DEBUG_UI: the last raw reply from the receiver and the key it gave. */
const char *remote_debug(void);

#endif
