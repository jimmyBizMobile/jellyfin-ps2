/*
 * The PS2 DVD remote, read from the console's built-in IR receiver.
 *
 * The receiver is part of the disc drive's controller (MechaCon), which
 * decodes the remote's signal itself. Sony's RMMAN2 driver reads it with
 * one "S-command", ReadSIRc (0x1E); this module sends the same command
 * through libcdvd, so no extra IOP module is needed. The reply, as
 * documented in ps2sdk's remote notes ("0014 DAD1 6000" for a key):
 *
 *     byte 1      0x14 while a key is held
 *     bytes 2-5   the key's code, little-endian: DA D1 60 00 = 0x0060D1DA,
 *                 the same values as librm.h's RM_ constants
 *
 * A key is accepted only if its code also looks like one: its low byte
 * names a Sony device family the PS2 answers to (0x49 the DVD player
 * keys, 0xDA the PS2 keys, 0x93 the PSX DESR remote) and the next byte
 * is in the D0-D7 range every code uses. Anything else reads as "no key".
 *
 * Consoles without a built-in receiver (the first fat models, which used
 * a dongle in a controller port) answer the command with nothing usable;
 * the first poll finds that and the module stays off.
 */

#include "remote.h"
#include "settings.h"
#include "config.h"

#include <libcdvd.h>
#include <libpad.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define SCMD_READ_SIRC   0x1E

/* librm.h's codes, by the label on the SCPH-10150 remote (the later
 * SCPH-10420 says TOP MENU for TITLE and MENU for DVD MENU: same codes). */
#define RM_DVD_ENTER     0x00B0D049
#define RM_DVD_RETURN    0x00E0D049
#define RM_DVD_CLEAR     0x00F0D049
#define RM_DVD_TITLE     0x00A0D149
#define RM_DVD_MENU      0x00B0D149
#define RM_DVD_TIME      0x0080D249
#define RM_DVD_PREV      0x0000D349
#define RM_DVD_NEXT      0x0010D349
#define RM_DVD_PLAY      0x0020D349
#define RM_DVD_SCAN_BACK 0x0030D349
#define RM_DVD_SCAN_FORW 0x0040D349
#define RM_DVD_STOP      0x0080D349
#define RM_DVD_PAUSE     0x0090D349
#define RM_DVD_DISPLAY   0x0040D549
#define RM_DVD_SUBTITLE  0x0030D649

/*
 * The PSX DESR remote (RMT-P001, mode switch on 1), from librm.h. NOT
 * TESTED on real hardware. Only its keys without a controller twin are
 * mapped: if the DESR, like the SCPH-50000, passes its remote's
 * controller keys to controller 1, mapping those too would make them act
 * twice.
 */
#define RM_DESR_TOP_MENU 0x00C0D293
#define RM_DESR_MENU     0x0090D293
#define RM_DESR_RETURN   0x0030D493
#define RM_DESR_HOME     0x0020D493
#define RM_DESR_ENTER    0x0000D693
#define RM_DESR_CLEAR    0x00F0D093
#define RM_DESR_DISPLAY  0x0050D293
#define RM_DESR_PLAY     0x00A0D193
#define RM_DESR_PAUSE    0x0090D193
#define RM_DESR_STOP     0x0080D193

typedef struct {
	u32 code;
	u32 bits;
	int repeat;     /* repeats while held (SCAN, PREV, NEXT) */
} KeyMap;

static const KeyMap g_map[] = {
	{ RM_DVD_ENTER,     PAD_CROSS,    0 },
	{ RM_DVD_RETURN,    PAD_CIRCLE,   0 },
	{ RM_DVD_MENU,      PAD_CIRCLE,   0 },
	{ RM_DVD_CLEAR,     PAD_SQUARE,   0 },
	{ RM_DVD_SUBTITLE,  PAD_R3,       0 },
	{ RM_DVD_TITLE,     RMK_HOME,     0 },
	{ RM_DVD_PLAY,      RMK_PLAY,     0 },
	{ RM_DVD_PAUSE,     RMK_PAUSE,    0 },
	{ RM_DVD_STOP,      RMK_STOP,     0 },
	{ RM_DVD_PREV,      RMK_PREV,     1 },
	{ RM_DVD_NEXT,      RMK_NEXT,     1 },
	{ RM_DVD_SCAN_BACK, RMK_SCAN_B,   1 },
	{ RM_DVD_SCAN_FORW, RMK_SCAN_F,   1 },
	{ RM_DVD_DISPLAY,   RMK_DISPLAY,  0 },
	{ RM_DVD_TIME,      RMK_DISPLAY,  0 },

	/* PSX DESR remote: untested. */
	{ RM_DESR_ENTER,    PAD_CROSS,    0 },
	{ RM_DESR_RETURN,   PAD_CIRCLE,   0 },
	{ RM_DESR_MENU,     PAD_CIRCLE,   0 },
	{ RM_DESR_CLEAR,    PAD_SQUARE,   0 },
	{ RM_DESR_TOP_MENU, RMK_HOME,     0 },
	{ RM_DESR_HOME,     RMK_HOME,     0 },
	{ RM_DESR_PLAY,     RMK_PLAY,     0 },
	{ RM_DESR_PAUSE,    RMK_PAUSE,    0 },
	{ RM_DESR_STOP,     RMK_STOP,     0 },
	{ RM_DESR_DISPLAY,  RMK_DISPLAY,  0 },
};

#define REPEAT_FIRST_MS  450     /* a held key repeats after this... */
#define REPEAT_EVERY_MS  200     /* ...then this often */

static int  g_on;
static u32  g_held;              /* code of the key held, 0 for none */
static u32  g_last_poll_ms;
static u32  g_next_repeat_ms;
static char g_dbg[96] = "remote: off";

/* Milliseconds, for differences only. */
static u32 now_ms(void)
{
	return (u32)((unsigned long long)clock() * 1000ULL / CLOCKS_PER_SEC);
}

/* Send ReadSIRc; the key held as a code, 0 for none, -1 if the command
 * itself failed. The raw reply goes to the DEBUG_UI line. */
static long long read_sirc(void)
{
	static u8 out[16] __attribute__((aligned(64)));

	memset(out, 0, sizeof(out));

	if (!sceCdApplySCmd(SCMD_READ_SIRC, NULL, 0, out))
		return -1;

	u32 code = (u32)out[2] | ((u32)out[3] << 8) |
	           ((u32)out[4] << 16) | ((u32)out[5] << 24);

	u8 fam  = code & 0xFF;
	u8 mid  = (code >> 8) & 0xFF;
	int ok  = out[1] == 0x14 &&
	          (fam == 0x49 || fam == 0xDA || fam == 0x93) &&
	          mid >= 0xD0 && mid <= 0xD7;

	snprintf(g_dbg, sizeof(g_dbg),
	         "remote %02X %02X %02X %02X %02X %02X %02X %02X  key %08X%s",
	         out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7],
	         ok ? (unsigned)code : 0u, ok ? "" : " (none)");

	return ok ? (long long)code : 0;
}

void remote_init(void)
{
	g_on = 0;

	if (settings.remote == REMOTE_OFF) {
		snprintf(g_dbg, sizeof(g_dbg), "remote: off (settings.txt)");
		return;
	}

	/*
	 * libcdvd's RPC to the IOP's resident CD/DVD service. INoD: do not
	 * wait for a disc; the drive may well be empty.
	 */
	if (!sceCdInit(SCECdINoD)) {
		snprintf(g_dbg, sizeof(g_dbg), "remote: no CD/DVD service");
		return;
	}

	/* One read decides: a console without a receiver gives no answer. */
	if (read_sirc() < 0) {
		snprintf(g_dbg, sizeof(g_dbg), "remote: no receiver");
		return;
	}

	g_on = 1;
}

int remote_active(void) { return g_on; }

const char *remote_debug(void) { return g_dbg; }

u32 remote_pressed(void)
{
	if (!g_on)
		return 0;

	/*
	 * Every other frame is plenty for a remote (its signal repeats about
	 * every 45 ms) and halves the requests to the IOP.
	 */
	u32 now = now_ms();

	if (now - g_last_poll_ms < 30)
		return 0;
	g_last_poll_ms = now;

	long long r = read_sirc();

	if (r < 0)
		return 0;                    /* a failed read: try again later */

	u32 code = (u32)r;

	if (code == 0) {                 /* released */
		g_held = 0;
		return 0;
	}

	const KeyMap *k = NULL;

	for (unsigned i = 0; i < sizeof(g_map) / sizeof(g_map[0]); i++)
		if (g_map[i].code == code) {
			k = &g_map[i];
			break;
		}

	/*
	 * A new key: one press. The arrows send their DVD code, then their
	 * PS2 code, while held; neither is in the map (the controller path
	 * has them), so they only clear g_held, which is right.
	 */
	if (code != g_held) {
		g_held = code;
		g_next_repeat_ms = now + REPEAT_FIRST_MS;
		return k ? k->bits : 0;
	}

	/* The same key, still held: repeat it if it is one that repeats. */
	if (k && k->repeat && (int)(now - g_next_repeat_ms) >= 0) {
		g_next_repeat_ms = now + REPEAT_EVERY_MS;
		return k->bits;
	}

	return 0;
}
