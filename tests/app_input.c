/* Exercise real app dispatch with deterministic time and held controller state. */
#include "app.h"
#include "screens.h"
#include "ui.h"
#include <libpad.h>
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <time.h>

static unsigned long now;
static u32 previous, received;
static const Layout layout = {.w = 640};
const Layout *lay = &layout;

clock_t clock(void) { return (clock_t)now; }
static void input(u32 p) { received = p; }
static void draw(void) {}
#define SCREEN(name) const Screen name = {NULL, input, draw}
SCREEN(scr_connect); SCREEN(scr_users); SCREEN(scr_login); SCREEN(scr_home);
SCREEN(scr_next); SCREEN(scr_browse); SCREEN(scr_detail); SCREEN(scr_tracks);
SCREEN(scr_player); SCREEN(scr_search); SCREEN(scr_video); SCREEN(scr_usb);

void freeze_arm(int on) { (void)on; }
void ui_begin(void) {}
void ui_end(void) {}
void ui_waves(void) {}
void ui_brand(void) {}
void ui_text_c(float x, float y, float scale, u64 c, const char *s)
{ (void)x; (void)y; (void)scale; (void)c; (void)s; }
int ui_wrap(float x, float y, float lh, float scale, u64 c, const char *s, float w, int n)
{ (void)x; (void)y; (void)lh; (void)scale; (void)c; (void)s; (void)w; (void)n; return 0; }
void ui_hint_row(const BtnGlyph *b, const char **s, int n)
{ (void)b; (void)s; (void)n; }
void job_load_users(void) {}
void job_connect(void) {}

static unsigned long ticks(unsigned long ms)
{
	return (unsigned long)(CLOCKS_PER_SEC * (unsigned long long)ms / 1000);
}

static u32 frame(unsigned long ms, u32 held)
{
	now = ticks(ms);
	received = 0;
	app_frame(held & ~previous, held);
	previous = held;
	return received;
}

static void begin(ScreenId screen)
{
	previous = 0;
	app_goto(screen);
}

static void slow_job(void)
{
	now += ticks(10000);
	app_goto(SCR_TRACKS);
}

int main(void)
{
	const ScreenId scrolling[] = {SCR_HOME, SCR_BROWSE, SCR_DETAIL, SCR_TRACKS};
	app_init();
	for (unsigned i = 0; i < sizeof(scrolling) / sizeof(scrolling[0]); i++) {
		begin(scrolling[i]);
		assert(frame(0, PAD_DOWN) == PAD_DOWN);
		assert(frame(100, PAD_DOWN) == 0);
		assert(frame(349, PAD_DOWN) == 0);
		assert(frame(350, PAD_DOWN) == PAD_DOWN);
		assert(frame(439, PAD_DOWN) == 0);
		assert(frame(440, PAD_DOWN) == PAD_DOWN);
		assert(frame(2000, PAD_DOWN) == PAD_DOWN); /* One move after a stall. */
		assert(frame(2000, PAD_DOWN) == 0);
		assert(frame(2010, 0) == 0);
		assert(frame(2015, PAD_DOWN) == PAD_DOWN); /* Quick second tap. */
		assert(frame(2020, 0) == 0);
		assert(frame(3000, 0) == 0);
		assert(frame(3001, PAD_UP) == PAD_UP);
		assert(frame(3350, PAD_UP) == 0);
		assert(frame(3351, PAD_UP) == PAD_UP);
		assert(frame(3352, PAD_DOWN) == PAD_DOWN); /* Direction change resets delay. */
		assert(frame(3701, PAD_DOWN) == 0);
		assert(frame(3702, PAD_DOWN) == PAD_DOWN);
	}

	const u32 edge_only[] = {PAD_LEFT, PAD_RIGHT, PAD_CROSS, PAD_CIRCLE,
	                        PAD_START, PAD_SELECT, PAD_L2, PAD_R2};
	for (unsigned i = 0; i < sizeof(edge_only) / sizeof(edge_only[0]); i++) {
		begin(SCR_TRACKS);
		assert(frame(0, edge_only[i] | PAD_DOWN) == (edge_only[i] | PAD_DOWN));
		assert(frame(350, edge_only[i] | PAD_DOWN) == PAD_DOWN);
		assert(frame(440, edge_only[i] | PAD_DOWN) == PAD_DOWN);
	}

	const ScreenId no_repeat[] = {SCR_CONNECT, SCR_USERS, SCR_LOGIN, SCR_NEXT,
	                             SCR_PLAYER, SCR_SEARCH, SCR_VIDEO, SCR_USB};
	for (unsigned i = 0; i < sizeof(no_repeat) / sizeof(no_repeat[0]); i++) {
		begin(no_repeat[i]);
		assert(frame(0, PAD_DOWN) == PAD_DOWN);
		assert(frame(350, PAD_DOWN) == 0);
		assert(frame(2000, PAD_DOWN) == 0);
	}

	begin(SCR_TRACKS);
	assert(frame(0, PAD_DOWN) == PAD_DOWN);
	assert(frame(350, PAD_DOWN) == PAD_DOWN);
	app_goto(SCR_DETAIL);
	assert(frame(351, PAD_DOWN) == 0);
	assert(frame(700, PAD_DOWN) == 0);
	assert(frame(701, PAD_DOWN) == PAD_DOWN);
	app_fail("Error", "Test");
	assert(frame(702, PAD_DOWN) == 0);
	app_goto(SCR_TRACKS);
	assert(frame(703, PAD_DOWN) == 0);
	assert(frame(1052, PAD_DOWN) == 0);
	assert(frame(1053, PAD_DOWN) == PAD_DOWN);
	app_run_job(slow_job, "Loading...");
	assert(frame(1054, PAD_DOWN) == 0);
	assert(frame(11055, PAD_DOWN) == 0);
	assert(frame(11404, PAD_DOWN) == 0);
	assert(frame(11405, PAD_DOWN) == PAD_DOWN);

	/* Opposing directions cancel repetition until a single direction remains. */
	assert(frame(12000, PAD_UP | PAD_DOWN) == PAD_UP);
	assert(frame(13000, PAD_UP | PAD_DOWN) == 0);
	assert(frame(13001, PAD_UP) == 0);
	assert(frame(13350, PAD_UP) == 0);
	assert(frame(13351, PAD_UP) == PAD_UP);

	/* Time subtraction remains correct across unsigned clock rollover. */
	begin(SCR_TRACKS);
	now = ULONG_MAX - ticks(100);
	app_frame(PAD_DOWN, PAD_DOWN);
	now += ticks(349);
	received = 0; app_frame(0, PAD_DOWN); assert(received == 0);
	now += ticks(1);
	received = 0; app_frame(0, PAD_DOWN); assert(received == PAD_DOWN);
	puts("UI input tests passed (delay, cadence, release, direction change, scope, transitions, clock rollover)");
	return 0;
}
