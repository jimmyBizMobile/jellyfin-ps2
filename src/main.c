/*
 * Jellyfin PS2
 *
 *   settings.txt -> URL entry -> connect -> who is watching -> sign in -> library
 *
 * main.c does three things: bring up hardware, read input, run frames.
 *
 * Order matters here. Video comes up before anything that can hang, so a
 * failure shows a message instead of a black screen.
 */

#include <kernel.h>
#include <sifrpc.h>
#include <libpad.h>
#include <libkbd.h>
#include <iopcontrol.h>
#include <loadfile.h>
#include <stdio.h>

#include "app.h"
#include "config.h"
#include "settings.h"
#include "display.h"
#include "freeze.h"
#include "mods.h"
#include "net.h"
#include "ui.h"
#include "osk.h"
#include "jf.h"
#include "audio.h"


static char pad_buf[256] __attribute__((aligned(64)));

/* Open the first controller port with libpad. */
static void pad_setup(void)
{
	padInit(0);
	padPortOpen(0, 0, pad_buf);
}

/* Keep rising edges and held state separate so only UI scrolling repeats. */
static u32 pad_pressed(u32 *held)
{
	/* An unavailable controller must not leave UI repetition running. */
	if (held) *held = 0;
#if SKIP_PAD
	return 0;
#else
	static u32 prev = 0;
	struct padButtonStatus b;

	if (padGetState(0, 0) != PAD_STATE_STABLE) return 0;
	if (padRead(0, 0, &b) == 0) return 0;

	u32 now = 0xFFFF ^ b.btns;
	if (held) *held = now;
	u32 hit = now & ~prev;
	prev = now;
	return hit;
#endif
}

/*
 * The buttons held right now, waiting up to a second for the controller to
 * be ready after pad_setup. For R1-at-start.
 */
static u32 pad_held_at_start(void)
{
#if SKIP_PAD
	return 0;
#else
	struct padButtonStatus b;

	for (int i = 0; i < 60; i++) {
		if (padGetState(0, 0) == PAD_STATE_STABLE && padRead(0, 0, &b))
			return 0xFFFF ^ b.btns;
		net_snooze(16);
	}
	return 0;
#endif
}

/*
 * The display mode from settings.txt, chosen safely.
 *
 * The app always starts in 480i, which every TV shows. With video=480p or
 * video=240p it then switches and asks to keep the mode: X keeps it,
 * Circle or ten seconds without an answer go back to 480i - so a screen
 * that cannot show the mode (480p on a 15 kHz CRT, 240p on some flat TVs)
 * never leaves the app on a blank screen. The question comes back at every start: settings.txt is never
 * changed by it. Holding R1 while the app starts skips all this and stays
 * in 480i.
 */
static void choose_display(void)
{
	int id;

	switch (settings.video_mode) {
	case VIDEO_480P: id = DISPLAY_480P; break;
	case VIDEO_240P: id = DISPLAY_240P; break;
	case VIDEO_1080I: id = DISPLAY_1080I; break;
	default:         return;          /* 480i: nothing to do */
	}

	if (pad_held_at_start() & PAD_R1)
		return;                       /* the escape hatch */

	ui_set_display(id);

	const int frames = 10 * 60;       /* ten seconds of refreshes */
	char title[24];

	snprintf(title, sizeof(title), "Keep %s?", disp->name);

	for (int f = 0; f < frames; f++) {
		u32 p = pad_pressed(NULL);

		if (p & PAD_CROSS)
			return;                   /* keep it */

		if (p & PAD_CIRCLE)
			break;

		char line[64], keep[24];

		snprintf(line, sizeof(line), "Back to 480i in %d s",
		         (frames - f + 59) / 60);
		snprintf(keep, sizeof(keep), "Keep %s", disp->name);

		ui_begin();
		ui_waves();
		ui_text_c(SCR_W / 2, 150, 2.0f, C_WHITE, title);
		ui_text_c(SCR_W / 2, 220, 1.0f, C_DIM, line);
		ui_text_c(SCR_W / 2, 250, 1.0f, C_DIM,
		          "Hold R1 while starting to skip this.");
		{
			static const BtnGlyph g[] = { BTN_CROSS, BTN_CIRCLE };
			const char *l[] = { keep, "480i" };

			ui_hint_row(g, l, 2);
		}
		ui_end();
	}

	ui_set_display(DISPLAY_480I);
}

/* A USB keyboard types into whatever the OSK is currently bound to. */
void osk_append(char c);
void osk_backspace(void);

/*
 * Feed a USB keyboard, if one was found, into the on-screen keyboard's
 * buffer: printable keys, backspace, and Enter (returned as 1 so main
 * treats it as START).
 */
static int keyboard_poll(void)
{
	if (!mods_have_keyboard()) return 0;

	char c;
	int enter = 0;

	while (PS2KbdRead(&c) > 0) {
		if (c == 0x08 || c == 0x7F)      osk_backspace();
		else if (c == '\n' || c == '\r') enter = 1;
		else if (c >= 0x20 && c < 0x7F)  osk_append(c);
	}
	return enter;
}

/*
 * Hand the console back.
 *
 * The IOP is reset first so whatever runs next does not inherit our modules,
 * then rom0:OSDSYS starts the system browser. On a modded console that lands
 * in the boot menu, which is where you came from.
 */
static void quit_to_browser(void)
{
	/*
	 * The splash's layout with "See you later" under the logo. It stays
	 * up through the shutdown below, until the browser takes the screen.
	 */
	ui_goodbye();

	audio_stop();
	jf_reports_wait(2000);       /* a last stop report or resume point */
	net_stop();

	SifIopReset("", 0);
	while (!SifIopSync()) {}
	SifInitRpc(0);

	LoadExecPS2("rom0:OSDSYS", 0, NULL);

	/* Only reached if that failed. */
	SleepThread();
}

/*
 * Start-up, then the main loop. In order: SIF RPC, the vertical-blank
 * sleep timer, the screen and font, settings.txt, the IOP reset and module
 * loading (USB, network, sound), the controller, the screen thread's
 * priority and the freeze safety net, then app_frame for ever. SELECT
 * quits from anywhere.
 */
int main(int argc, char **argv)
{

	SifInitRpc(0);

	/* Before any thread sleeps: timed sleeps run on the vertical blank. */
	net_snooze_init();

	/* Video before anything that can block, so a failure has somewhere
	 * to report itself. */
	ui_init_video();
	ui_init_font();

	/* Logo and wordmark, always. With the boot log on, the step messages
	 * replace it a moment later. */
	ui_splash();

#if DEBUG_BOOT_LOG
	/*
	 * Every step names itself before it runs, so a freeze tells you which
	 * call blocked. Ugly, and exactly what you want when it breaks.
	 */
	settings_load(argc, argv);
	jf_init();
	ui_boot("read settings.txt");

	mods_load(ui_boot);

#if !SKIP_PAD
	ui_boot("padInit");
	pad_setup();
#endif

#if DEBUG_NET_LOG
	net_set_progress(ui_boot);
#endif
#else
	/*
	 * settings.txt is read before mods_load because the IOP reset in
	 * there takes uLaunchELF's USB modules with it. Ours load straight
	 * after, so mass: is writable again by the time anything is saved.
	 */
	settings_load(argc, argv);
	jf_init();

	mods_load(NULL);

#if !SKIP_PAD
	pad_setup();
#endif
#endif

	/* Sound. Needs libsd and audsrv, which mods_load has just installed. */
#if DEBUG_BOOT_LOG
	ui_boot("audio");
#endif
	audio_init();

	/*
	 * Drop the UI below playback and SDK networking.
	 *
	 * On the EE a lower number wins, and the main thread starts at 0. Since
	 * gsKit_sync_flip busy-waits on the GS rather than sleeping, the UI
	 * never yields, and without this the decoder is starved and playback
	 * crawls. Measured both ways: with it, playback runs; without it, it
	 * does not.
	 *
	 * Rendering is vsync-bound and loses nothing by giving way.
	 */
#if UI_THREAD_PRIO
	ChangeThreadPriority(GetThreadId(), UI_THREAD_PRIO);
	freeze_register(GetThreadId(), FZ_UI);
	freeze_init();
#endif

	/* 480p, if settings.txt asks for it and the TV shows it. */
	choose_display();

	osk_init();
	app_init();

	for (;;) {
		u32 held;
		u32 pressed = pad_pressed(&held);

		/* Enter on a real keyboard behaves exactly like START. */
		if (keyboard_poll())
			pressed |= PAD_START;

		/* SELECT quits from anywhere. Handled here rather than per screen
		 * so there is never a state you cannot get out of. */
		if (pressed & PAD_SELECT)
			quit_to_browser();

		/* A thread the freeze safety net had to set aside. */
		{
			char code = freeze_take();

			if (code)
				app_freeze(code);
		}

		app_frame(pressed, held);
	}

	return 0;
}