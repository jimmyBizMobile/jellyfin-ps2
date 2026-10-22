/*
 * The MPEG-2 video decoder: a thread running libmpeg on the IPU, fed 2 KB
 * at a time from the stream, one picture decoded ahead; the screen thread
 * scatters each picture into a texture and draws it. Also the end-of-
 * stream watchdog.
 */

#include "video.h"
#include "freeze.h"
#include "poster.h"
#include "config.h"
#include "net.h"        /* net_snooze, for the bounded wait when stopping */

#include <libmpeg.h>
#include <dma.h>
#include <dma_tags.h>
#include <gif_tags.h>
#include <gs_psm.h>
#include <gs_gp.h>
#include <packet.h>

#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <kernel.h>
#include <time.h>

/* VIDEO_MAX_W and VIDEO_MAX_H are in config.h: jf.c needs them too. */

/*
 * The IPU and its two DMA channels, addressed directly.
 *
 * libmpeg drives these by register and does not expose them, so stopping
 * what it started means writing them ourselves. Standard EE addresses:
 * channel 3 is fromIPU, channel 4 is toIPU.
 */
#define R_IPU_CMD    ((volatile u32 *)0x10002000)
#define R_IPU_CTRL   ((volatile u32 *)0x10002010)

/*
 * Corrected. These were 0x1000A000 and 0x1000B000 - channel 2 (the GIF,
 * gsKit's own) and channel 3 - so every reset stopped the GIF instead of
 * toIPU, and a 2 KB block still in flight on toIPU went on feeding the IPU
 * after it was reset. That is the "seven quadwords no reset clears", and
 * the intermittent 0x0 sequence and stall after stopping mid-film. ORBIT
 * uses the same addresses as below.
 *
 *   D3 fromIPU  0x1000B000      D4 toIPU  0x1000B400
 */
#define R_D3_CHCR    ((volatile u32 *)0x1000B000)
#define R_D3_QWC     ((volatile u32 *)0x1000B020)
#define R_D4_CHCR    ((volatile u32 *)0x1000B400)
#define R_D4_QWC     ((volatile u32 *)0x1000B420)

#define R_D_ENABLER  ((volatile u32 *)0x1000F520)
#define R_D_ENABLEW  ((volatile u32 *)0x1000F590)
#define R_IPU_BP     ((volatile u32 *)0x10002020)

#define IPU_CTRL_BUSY (1 << 31)
#define IPU_CTRL_RST  (1 << 30)

/*
 * Put the IPU and its channels back to a known state.
 *
 * This is libmpeg's own recovery sequence, from the error path in
 * _MPEG_WaitBDEC, and the order is the whole point:
 *
 *   clear the START BIT on channel 4, not the whole CHCR
 *   wait for the output FIFO to drain, which is bits 4-7 of IPU_CTRL
 *   clear the start bit on channel 3
 *   suspend the DMAC, and reset the IPU WHILE IT IS SUSPENDED
 *   only then BCLR
 *
 * An earlier version zeroed both CHCRs and reset with the controller
 * running. That silently did nothing: the BUSY bit was never even observed
 * to set, and the input FIFO still reported seven quadwords held afterwards.
 * The decoder then read a sequence header out of those leftovers and
 * reported a picture of 0x0, which asks the IPU to convert nothing and never
 * returns.
 *
 * Verified in isolation: four rounds of decode-part-way-then-restart, with
 * IPU_CTRL and IPU_BP both reading zero before each one.
 */
/* How many resets the last clear needed, and whether it succeeded. */
static int g_reset_tries;

/*
 * Reset until the input FIFO actually reads empty.
 *
 * One pass is enough in isolation, but not in the app: with a decoder thread
 * and gsKit both alive, IPU_CTRL has been seen still reporting seven
 * quadwords held afterwards - and a decoder started in that state reads its
 * sequence header out of the leftovers and reports 0x0.
 *
 * Bits 0-3 of IPU_CTRL are that count, so the condition is checkable. Eight
 * attempts, and the count is recorded so a stubborn one is visible rather
 * than silent.
 */
static void ipu_reset_once(void);

/*
 * Reset the IPU (the MPEG decoder hardware) until it reports idle, up to 8
 * tries.
 */
static void ipu_reset(void)
{
	for (g_reset_tries = 1; g_reset_tries <= 8; g_reset_tries++) {
		ipu_reset_once();

		if ((*R_IPU_CTRL & 0x0F) == 0)
			return;
	}
}

/*
 * One reset: stop the DMA channel feeding the IPU, let it drain, stop the
 * one reading from it, then reset the IPU with every DMA channel briefly
 * held so nothing moves meanwhile.
 */
static void ipu_reset_once(void)
{
	int spins;

	*R_D4_CHCR &= ~0x100;

	for (spins = 0; spins < 100000 && (*R_IPU_CTRL & 0xF0); spins++)
		;

	*R_D3_CHCR &= ~0x100;

	{
		int oldintr = DIntr();

		*R_D_ENABLEW = *R_D_ENABLER | 0x10000;
		*R_IPU_CTRL  = IPU_CTRL_RST;

		for (spins = 0; spins < 100000 &&
		     (*R_IPU_CTRL & IPU_CTRL_BUSY); spins++)
			;

		*R_D_ENABLEW = *R_D_ENABLER & ~0x10000;

		if (oldintr)
			EIntr();
	}

	*R_D3_QWC = 0;
	*R_D4_QWC = 0;

	*R_IPU_CMD = 0;                    /* BCLR, on a quiet IPU */

	for (spins = 0; spins < 100000 && (*R_IPU_CTRL & IPU_CTRL_BUSY); spins++)
		;
}

/*
 * How the picture reaches the screen, and the state it needs.
 *
 * libmpeg writes RGBA32 into main memory, but not as a linear image: the
 * output is a grid of 16x16 macroblocks, each 1024 bytes, in raster order.
 * Rearranging that into a linear texture on the EE would be a 1.15 MB copy
 * every frame, so the GS does it instead - one DMA chain with a tag per
 * macroblock, each setting TRXPOS to that block's place in the texture.
 */

static GSTEXTURE g_tex;
static void     *g_pic;          /* decoded macroblocks, main memory */
static int       g_pic_size;
static packet_t *g_xfer;         /* the per-macroblock DMA chain */
static int       g_ready;        /* a frame has been decoded */
static int       g_active;
static int       g_ended;        /* the stream finished by itself */
static int       g_ms_frame;
static int       g_img_w, g_img_h;   /* the picture, not the allocation */
static int       g_crop_w, g_crop_h; /* the real picture, if smaller */
static char      g_err[128];
static char      g_info[64];
static s64       g_cur_pts;

static int (*g_read)(void *buf, int len);
static long long (*g_pts_cb)(void);

/*
 * The held picture's presentation time, and the last one handed out.
 *
 * Written by the worker before it raises g_have, read by the UI after it
 * sees g_have, so the flag orders them.
 */
static volatile long long g_pic_pts;
static long long g_last_pts = -1;
static int       g_frame90  = 3003;      /* 29.97 fps until told */

/* One 2 KB slice of bitstream, taken from the source. */
static unsigned char g_feed[2048] __attribute__((aligned(64)));

/*
 * Decoding runs on its own thread.
 *
 * Not for speed - the EE decodes faster than the display needs - but so a
 * stuck decoder cannot take the UI with it. MPEG_Picture has several
 * unbounded spins inside it, and while it ran on the UI thread any one of
 * them froze the console outright, with no way to see where.
 *
 * The UI signals a semaphore when it wants a picture; the worker waits on it
 * and sets g_have when one is ready. A semaphore rather than a polled flag
 * with a sleep: net_snooze cannot resolve finer than a vsync, so the worker
 * woke late, missed the draw its result would have been collected on, and a
 * ten-second file took twenty-three seconds.
 */
#define VIDEO_PRIO 0x40

static int  g_thread = -1;
static int  g_sema   = -1;
static char g_stack[32 * 1024] __attribute__((aligned(16)));   /* was 16 KB */

/*
 * Stack guard: the lowest 1 KB is filled with a pattern when the thread is
 * made. A stack grows down, so an overflow breaks the pattern before it
 * reaches anything else - and the music screen reports it.
 */
#define STACK_GUARD 1024

/* Fill the bottom of the decoder thread's stack with a known pattern. */
static void stack_guard_fill(char *stk)
{
	for (int i = 0; i < STACK_GUARD; i++)
		stk[i] = (char)0xA5;
}

/* Whether that pattern is intact (1) or the stack overran into it (0). */
static int stack_guard_ok(const char *stk)
{
	for (int i = 0; i < STACK_GUARD; i++)
		if (stk[i] != (char)0xA5)
			return 0;

	return 1;
}

/* The decoder thread's stack guard, for the DEBUG_UI readout. */
int video_stack_ok(void) { return stack_guard_ok(g_stack); }

static volatile int g_want;      /* UI: decode another picture */
static volatile int g_have;      /* worker: one is ready */
static volatile int g_fail;      /* worker: the stream ended or failed */
static volatile int g_busy;      /* worker: inside MPEG_Picture right now */

/*
 * The end-of-stream watchdog.
 *
 * At the end of some streams libmpeg never returns from MPEG_Picture once
 * the data has run out: the decoder spins, above the UI, and the console
 * froze on the last frame with every button dead. Rewriting the sequence
 * end code fixed one film but not another, so this catches the hang itself.
 *
 * On every vertical blank - an interrupt, so it runs however busy the CPU
 * is - it counts how long the decoder has been inside MPEG_Picture since
 * the data ran out. Past VIDEO_STUCK_BLANKS it drops the decoder below the
 * UI, which can then run again, sees the film as ended, and goes back to
 * its page. video_end then ends the stuck thread and starts a fresh one.
 */
#define VIDEO_STUCK_BLANKS 90          /* 1.5 s */
#define VIDEO_PARKED_PRIO  0x7E        /* below the UI */

static volatile int g_hold;         /* no more pictures: end between them */
static volatile int g_data_end;     /* data_cb has run out, this stream */
static volatile int g_busy_blanks;
static volatile int g_parked;       /* stuck at the end, pushed below the UI */
static volatile int g_cb_zero;      /* empty data_cb returns: diagnostics */
static volatile int g_parks;        /* times the watchdog fired, ever */

/*
 * Evidence for the end-of-stream hang, recorded so the watchdog's rescue
 * can report it (scr_video writes it to jfstuck.txt): what the hardware
 * was doing when the decoder was found stuck, and what it had been fed.
 */
static volatile int  g_cb_calls;    /* data_cb calls, this stream */
static volatile int  g_fed;         /* bytes handed to libmpeg, this stream */
static volatile int  g_last_n;      /* real bytes in the last block */
static unsigned char g_last_tail[48];   /* the last real bytes fed */
static volatile u32  g_snap[7];     /* registers when the watchdog fired */
static char          g_stuck[512];  /* the report; "" when nothing to tell */

static void stuck_watch(void)
{
	if (g_busy && g_data_end) {
		if (++g_busy_blanks >= VIDEO_STUCK_BLANKS && !g_parked &&
		    g_thread >= 0) {
			g_snap[0] = *R_IPU_CTRL;
			g_snap[1] = *R_IPU_BP;
			g_snap[2] = *R_D4_CHCR;
			g_snap[3] = *R_D4_QWC;
			g_snap[4] = *R_D3_CHCR;
			g_snap[5] = *R_D3_QWC;
			g_snap[6] = *R_D_ENABLER;

			iChangeThreadPriority(g_thread, VIDEO_PARKED_PRIO);
			g_parked = 1;
			g_parks++;
		}
	} else {
		g_busy_blanks = 0;
	}
}
static volatile int g_ticks;     /* worker: bumped on entry, to spot a stall */
static volatile int g_run;       /* worker: keep going */
static volatile int g_dec_ms;    /* worker: decode time of the last picture */
static volatile int g_dec_max;   /* worker: the slowest so far this film */

/*
 * libmpeg is torn down and set up again for every film.
 *
 * Keeping one session was a way around a reset that did not work: RST and
 * BCLR appeared to leave the IPU's input FIFO holding seven quadwords, so a
 * fresh MPEG_Initialize read a sequence header out of the leftovers and
 * reported 0x0. It turned out the reset was wrong rather than impossible -
 * see ipu_reset.
 *
 * Destroying every time matters because only a fresh session calls init_cb,
 * and init_cb is what tells us the new picture's size. A session that is
 * merely stopped leaves MPEG_Picture at _get_next_picture, which never does.
 */
static int g_mpeg_up;

/*
 * Set when the worker could not be got out of MPEG_Picture.
 *
 * Nothing can be done about it from outside: the IPU cannot be reset and the
 * library cannot be destroyed while a thread is inside it. Playback refuses
 * from then on, with a message, rather than producing a 0x0 sequence and a
 * frozen screen.
 */
static int g_lost;

/* The texture is still allocated after video_end_keep. */
static int g_tex_held;

/*
 * The texture's size, chosen per stream (video_expect_size): the picture
 * the transcode will deliver, in whole macroblocks plus one row of margin,
 * rather than always the largest supported. A 16:9 film at 512x288 then
 * takes about 0.6 MB of VRAM instead of 1.2. g_alloc_* is what was
 * reserved; init_cb refuses a picture larger than that.
 */
static int g_exp_w, g_exp_h;
static int g_alloc_w = VIDEO_MAX_W, g_alloc_h = VIDEO_MAX_H;

void video_expect_size(int w, int h)
{
	g_exp_w = w;
	g_exp_h = h;
}

/* video_end without giving the VRAM back. */
static int g_keep_on_end;

/* The last decoder error text, or ''. */
const char *video_error(void)   { return g_err; }

/* A short description of the stream (size, rate), for the readout. */
const char *video_info(void)    { return g_info; }

/* Milliseconds per picture, from the stream's frame rate. */
int  video_ms_per_frame(void)   { return g_ms_frame; }

/*
 * The picture's width: the visible (cropped) width if known, else the
 * coded one.
 */
int  video_width(void)          { return g_crop_w ? g_crop_w : g_img_w; }

/* The picture's height, likewise. */
int  video_height(void)         { return g_crop_h ? g_crop_h : g_img_h; }

/*
 * The visible picture size, when smaller than the coded one; see the
 * comment inside.
 */
void video_set_crop(int w, int h)
{
	/*
	 * The real picture, when it is smaller than the coded one.
	 *
	 * MPEG-2 codes in whole macroblocks, so a 640x346 picture is coded as
	 * 640x352 and libmpeg reports the larger size - the extra six rows
	 * being whatever the encoder left there. The caller knows the true
	 * size because it chose what to ask the transcoder for, so it can say.
	 */
	g_crop_w = (w > 0 && w <= VIDEO_MAX_W) ? w : 0;
	g_crop_h = (h > 0 && h <= VIDEO_MAX_H) ? h : 0;
}

/*
 * Where the decoder reads compressed video from (read_stream for the
 * network).
 */
void video_set_source(int (*cb)(void *, int)) { g_read = cb; }

/*
 * Where the decoder gets each picture's timestamp (take_pts in
 * scr_video.c).
 */
void video_set_pts_source(long long (*cb)(void)) { g_pts_cb = cb; }

/*
 * The picture duration in 90 kHz ticks (3754 at 23.976 fps), for
 * timestamping pictures that carry none.
 */
void video_set_frame90(int ticks)
{
	g_frame90 = (ticks >= 900 && ticks <= 9000) ? ticks : 3003;
}

/*
 * The readout line about the decoder (DEBUG_UI): busy, pictures started,
 * end, the end-of-stream data counters.
 */
const char *video_debug(void)
{
	/*
	 *   busy   1 if the worker is inside MPEG_Picture
	 *   ticks  pictures started, so a frozen number with busy 1 means one
	 *          that never finished
	 *   end    the stream reported its end
	 */
	static char buf[64];

	snprintf(buf, sizeof(buf), "dec%dms max%d busy%d end%d cb0 %d park%d",
	         g_dec_ms, g_dec_max, g_busy, g_fail, g_cb_zero, g_parks);
	return buf;
}

/*
 * libmpeg's data callback, called on the decoder thread whenever the IPU
 * needs more bytes: fill the 2 KB feed buffer from the source (zero-
 * padding the last, short block), flush it from the cache, and hand it
 * over. Returns the bytes, 0 at the end.
 */
static int data_cb(void *unused)
{
	(void)unused;

	if (!g_read)
		return 0;

	/*
	 * Fill the buffer completely before sending it.
	 *
	 * The transfer below always moves the whole 2048 bytes, because DMA
	 * works in quadwords and the IPU takes what it is given. So a short
	 * read leaves the tail holding the previous call's data, and that
	 * stale remainder goes into the bitstream as if it belonged there.
	 *
	 * It never showed with a file, whose buffered reader always returned
	 * exactly what was asked. A stream returns whatever the demuxer
	 * happens to yield, which is usually less - and the result was
	 * macroblock corruption throughout, plus more pictures decoded than
	 * the film contains.
	 */
	int n = 0;

	while (n < (int)sizeof(g_feed)) {
		int got = g_read(g_feed + n, (int)sizeof(g_feed) - n);

		if (got <= 0)
			break;

		n += got;
	}

	g_cb_calls++;

	if (n > 0) {
		int k = n < (int)sizeof(g_last_tail) ? n : (int)sizeof(g_last_tail);

		memcpy(g_last_tail, g_feed + n - k, k);
		if (k < (int)sizeof(g_last_tail))
			memset(g_last_tail + k, 0xEE, sizeof(g_last_tail) - k);
		g_fed   += n;
		g_last_n = n;
	}

	/* Anything short of a full block means the data has run out. */
	if (n < (int)sizeof(g_feed))
		g_data_end = 1;

	if (n <= 0) {
		g_cb_zero++;
		return 0;
	}

	/*
	 * The timestamp of the newest PES that started in these bytes.
	 *
	 * libmpeg copies g_cur_pts into the picture it is decoding, so this
	 * is how each picture learns its presentation time. The value lags or
	 * leads by at most the 2 KB in flight, a fraction of a frame at our
	 * bitrates.
	 */
	if (g_pts_cb) {
		long long p = g_pts_cb();

		if (p >= 0)
			g_cur_pts = p;
	}

	/* A genuinely short last read: pad rather than send stale bytes. */
	if (n < (int)sizeof(g_feed))
		memset(g_feed + n, 0, sizeof(g_feed) - n);

	SyncDCache(g_feed, g_feed + sizeof(g_feed));

	/*
	 * Bounded, not forever.
	 *
	 * A timeout of 0 means wait indefinitely, and this is the first thing
	 * done inside MPEG_Picture - so a channel left in an odd state by the
	 * previous film hangs here, which is exactly where the second
	 * playback froze. If the wait expires, reset the channel and carry
	 * on: a dropped 2 KB of bitstream costs a moment of corruption, which
	 * is a great deal better than a locked console.
	 */
	if (dma_channel_wait(DMA_CHANNEL_toIPU, 100000) < 0)
		dma_channel_initialize(DMA_CHANNEL_toIPU, NULL, 0);

	dma_channel_send_normal(DMA_CHANNEL_toIPU, g_feed,
	                        sizeof(g_feed) >> 4, 0, 0);
	return 1;
}

/*
 * Called once the sequence header is parsed, so the picture size is known.
 * Builds the transfer chain and returns the buffer pictures decode into.
 */
static void *init_cb(void *param, MPEGSequenceInfo *info)
{
	(void)param;                 /* the texture is reserved in video_begin */

	int mbw  = info->m_Width  >> 4;
	int mbh  = info->m_Height >> 4;

	g_ms_frame = info->m_MSPerFrame;

	/*
	 * The texture is already reserved, at VIDEO_MAX_W x VIDEO_MAX_H. It
	 * has to be: this callback cannot refuse. Returning NULL when the
	 * allocation failed left libmpeg writing to a null pointer, which
	 * froze the console with no message at all.
	 *
	 * Two widths matter here and they differ. The GS holds a buffer width
	 * in units of 64 pixels, so the allocation and the transfer use the
	 * width rounded up to 64, or every row is read at the wrong stride.
	 * Drawing samples the real width: at 352 the rounded width is 384,
	 * and sampling that far would show 32 columns of nothing.
	 */
	if (info->m_Width < 16 || info->m_Height < 16) {
		/*
		 * A sequence header read out of the previous stream's
		 * leftovers. Worth retrying: those leftovers have now been
		 * consumed, so a second attempt starts clean.
		 */
		snprintf(g_err, sizeof(g_err), "Bad sequence (%dx%d)",
		         info->m_Width, info->m_Height);
		g_fail = 1;
		return g_pic;
	}

	if (info->m_Width > g_alloc_w || info->m_Height > g_alloc_h) {
		snprintf(g_err, sizeof(g_err),
		         "Picture is %dx%d, larger than %dx%d",
		         info->m_Width, info->m_Height,
		         g_alloc_w, g_alloc_h);

		/*
		 * Say so, rather than failing quietly.
		 *
		 * This returns before building the transfer chain, so nothing
		 * can ever be drawn - and video_step only reports a problem
		 * when g_fail is set, which otherwise happens only at the end
		 * of a stream. The result was a blank screen with no
		 * explanation, which cost an evening to attribute.
		 */
		g_fail = 1;

		return g_pic;              /* never NULL: libmpeg would fault */
	}

	g_img_w = info->m_Width;
	g_img_h = info->m_Height;

	g_tex.Width  = (info->m_Width + 63) & ~63;
	g_tex.Height = info->m_Height;

	/*
	 * Two conversions, and both were wrong at first in ways that look like
	 * decoder faults.
	 *
	 * TBW is a buffer width in units of 64 pixels. gsKit puts it straight
	 * into TEX0 when drawing, and normally fills it in during
	 * gsKit_texture_upload - which we never call, because the picture
	 * arrives by DMA rather than from main memory. Left at zero the GS
	 * samples with the wrong stride and the image comes out in streaked
	 * diagonal bands.
	 *
	 * BITBLTBUF takes an address in units of 256 bytes, and gsKit stores
	 * Vram in bytes: it writes Texture->Vram/256 everywhere. The SDK
	 * sample shifts by 6 because graph_vram_allocate hands back a
	 * different unit entirely. Shifting by 6 here puts the transfer four
	 * times deeper into VRAM than the texture actually sits, over the
	 * framebuffers and the font atlas, so the UI is corrupt ever after.
	 */
	int tbw  = g_tex.Width >> 6;
	int base = g_tex.Vram >> 8;

	g_tex.TBW = tbw;

	/* A new sequence: the old chain described a different picture. */
	if (g_xfer) {
		packet_free(g_xfer);
		g_xfer = NULL;
	}

	g_xfer = packet_init((16 + 12 * mbw * mbh) >> 1, PACKET_NORMAL);

	if (!g_xfer) {
		snprintf(g_err, sizeof(g_err), "No memory for the transfer chain");
		return NULL;
	}

	qword_t *q   = g_xfer->data;
	char    *img = (char *)g_pic;

	DMATAG_CNT(q, 3, 0, 0, 0); q++;
	PACK_GIFTAG(q, GIF_SET_TAG(2, 0, 0, 0, 0, 1), GIF_REG_AD); q++;
	PACK_GIFTAG(q, GS_SET_TRXREG(16, 16), GS_REG_TRXREG); q++;
	PACK_GIFTAG(q, GS_SET_BITBLTBUF(0, 0, 0, base, tbw, GS_PSM_32),
	            GS_REG_BITBLTBUF); q++;

	for (int y = 0; y < info->m_Height; y += 16) {
		for (int x = 0; x < info->m_Width; x += 16, img += 1024) {
			DMATAG_CNT(q, 4, 0, 0, 0); q++;
			PACK_GIFTAG(q, GIF_SET_TAG(2, 0, 0, 0, 0, 1), GIF_REG_AD); q++;
			PACK_GIFTAG(q, GS_SET_TRXPOS(0, 0, x, y, 0), GS_REG_TRXPOS); q++;
			PACK_GIFTAG(q, GS_SET_TRXDIR(0), GS_REG_TRXDIR); q++;
			PACK_GIFTAG(q, GIF_SET_TAG(64, 1, 0, 0, 2, 0), 0); q++;
			DMATAG_REF(q, 64, (unsigned)img, 0, 0, 0); q++;
		}
	}

	/*
	 * End the chain. The SDK sample leaves this commented out, and without
	 * it the DMA controller reads past the packet and never completes.
	 */
	DMATAG_END(q, 0, 0, 0, 0); q++;

	g_xfer->qwc = q - g_xfer->data;

	return g_pic;
}

/*
 * Decode when asked, sleep otherwise.
 *
 * Only the decoding. The transfer to the GS deliberately stays on the UI
 * thread: gsKit drives DMA channel 2 from there for everything it draws,
 * and a second thread sending its own chains down the same channel wedged
 * the console just as thoroughly as the original bug.
 */
static void worker(void *arg)
{
	(void)arg;

	for (;;) {
		/* Blocks until the UI asks, with no polling latency. */
		WaitSema(g_sema);

		if (!g_run) {
			g_busy = 0;
			continue;
		}

		g_want = 0;
		g_busy = 1;
		g_ticks++;

		s64 pts;
		clock_t t0 = clock();

		if (!MPEG_Picture(g_pic, &pts)) {
			g_ended = 1;
			g_fail  = 1;
			g_busy  = 0;
			continue;
		}

		/*
		 * Never backwards.
		 *
		 * A picture whose PES carried no PTS gets the previous value
		 * again, and the file source gives none at all. Either way one
		 * frame after the last is the honest guess; a real timestamp
		 * takes over again as soon as one moves ahead.
		 */
		{
			long long t = (long long)pts;

			if (g_last_pts >= 0 && t <= g_last_pts)
				t = g_last_pts + g_frame90;

			g_last_pts = t;
			g_pic_pts  = t;
		}

		/*
		 * How long the picture took, including waiting for its data.
		 * Above a frame's time (42 ms at 23.976) the decoder cannot keep
		 * up, and pictures arrive late and are dropped.
		 */
		g_dec_ms = (int)((long long)(unsigned long)(clock() - t0) * 1000
		                 / CLOCKS_PER_SEC);

		if (g_dec_ms > g_dec_max)
			g_dec_max = g_dec_ms;

		g_have = 1;
		g_busy = 0;
	}
}

/*
 * Start decoding a new stream: set up libmpeg, the picture buffer, the
 * texture (taking the poster VRAM), and the decoder thread on first use.
 */
int video_begin(GSGLOBAL *gs)
{
	g_err[0]   = '\0';
	g_ms_frame = 33;

	/* A kept picture stays up until the first new one replaces it. */
	if (!g_tex_held)
		g_ready = 0;

	if (!g_read) {
		snprintf(g_err, sizeof(g_err), "No source set");
		return 0;
	}

	if (g_lost) {
		snprintf(g_err, sizeof(g_err),
		         "Decoder stuck; restart the app");
		return 0;
	}

	/*
	 * Artwork first: the texture needs more VRAM than is free while the
	 * poster slots are resident. Skipped, with the allocation below, when
	 * a pause or a seek kept the texture.
	 */
	if (!g_tex_held)
		poster_release(gs);

	/*
	 * Reserve the largest picture we support, before the decoder starts.
	 *
	 * It has to happen here rather than in init_cb, because that callback
	 * has no way to refuse: handing libmpeg a null buffer froze the
	 * console with nothing on screen. Here a failure is just a message.
	 */
	if (!g_tex_held) {
		/*
		 * Sized to the expected picture: coded sizes are whole 16x16
		 * macroblocks, plus one spare row in case the transcode comes
		 * out a little taller than predicted, never above the maximum.
		 * The GS stores texture rows in units of 64 pixels.
		 */
		int aw = VIDEO_MAX_W, ah = VIDEO_MAX_H;

		if (g_exp_w > 0 && g_exp_h > 0) {
			aw = (g_exp_w + 15) & ~15;
			ah = ((g_exp_h + 15) & ~15) + 16;

			if (aw > VIDEO_MAX_W) aw = VIDEO_MAX_W;
			if (ah > VIDEO_MAX_H) ah = VIDEO_MAX_H;
		}

		g_alloc_w = aw;
		g_alloc_h = ah;

		g_tex.Width    = (aw + 63) & ~63;
		g_tex.Height   = ah;
		g_tex.PSM      = GS_PSM_CT32;
		g_tex.Filter   = GS_FILTER_LINEAR;
		g_tex.Mem      = NULL;            /* filled by DMA, not by gsKit */
		g_tex.VramClut = 0;
		g_tex.Clut     = NULL;

		g_tex.Vram = gsKit_vram_alloc(gs,
		                 gsKit_texture_size(g_tex.Width, g_tex.Height,
		                                    GS_PSM_CT32),
		                 GSKIT_ALLOC_USERBUFFER);

		if (g_tex.Vram == GSKIT_ALLOC_ERROR) {
			snprintf(g_err, sizeof(g_err),
			         "No VRAM for %dx%d: %u KB used of 4096",
			         g_tex.Width, g_tex.Height,
			         (unsigned)(gs->CurrentPointer / 1024));
			poster_reinit(gs);
			return 0;
		}
	}

	g_tex_held = 0;

	/* Where the texture landed, so a leak shows up as this creeping up
	 * between films rather than as a refusal three films later. */
	snprintf(g_info, sizeof(g_info), "vram %u KB",
	         (unsigned)(gs->CurrentPointer / 1024));

	/*
	 * The picture buffer, big enough for the largest we allow. Allocated
	 * once and kept: libmpeg owns it once init_cb hands it over.
	 */
	if (!g_pic) {
		g_pic_size = VIDEO_MAX_W * VIDEO_MAX_H * 4;
		g_pic      = memalign(64, g_pic_size);

		if (!g_pic) {
			snprintf(g_err, sizeof(g_err), "No memory for a picture");
			poster_reinit(gs);
			return 0;
		}
	}

	memset(g_pic, 0, g_pic_size);
	SyncDCache(g_pic, (char *)g_pic + g_pic_size);

	/*
	 * Two ways to start, and which one depends on how the last film
	 * finished.
	 *
	 * Reaching the end of a stream latches libmpeg's EOF flag, and only
	 * MPEG_Initialize clears it - without that, every later playback
	 * reports the end immediately. Happily that is also the case where
	 * reinitialising works: the bitstream ran out cleanly, so the IPU's
	 * input FIFO is empty and there is nothing stale to resynchronise
	 * around.
	 *
	 * Stopping part way is the opposite. The FIFO still holds seven
	 * quadwords that neither RST nor BCLR clears, so a fresh
	 * MPEG_Initialize parses a sequence header out of the leftovers, gets
	 * 0x0, and asks the IPU to convert a picture with no macroblocks -
	 * which never returns. There the session has to be kept and the
	 * source simply rewound.
	 */
	/*
	 * What the IPU looks like before we touch it.
	 *
	 * The isolated harness proved this reset works: four rounds of
	 * decode-part-way-then-restart, with IPU_CTRL and IPU_BP both reading
	 * zero each time. Something in the app differs, so record the same
	 * registers here rather than guessing which.
	 *
	 * ctrl bits 0-3 are the input FIFO count, 4-7 the output. Anything
	 * other than zero after the reset means it did not take.
	 */
	snprintf(g_info, sizeof(g_info), "in %08x", (unsigned)*R_IPU_CTRL);

	if (!g_mpeg_up) {
		/*
		 * Clear whatever the last film left in the IPU, then start
		 * fresh. Both are needed: the reset because the input FIFO
		 * keeps quadwords that no amount of reinitialising clears, and
		 * a fresh MPEG_Initialize because only then does init_cb run
		 * and tell us the new picture's size.
		 */
		ipu_reset();

		dma_channel_initialize(DMA_CHANNEL_fromIPU, NULL, 0);
		dma_channel_initialize(DMA_CHANNEL_toIPU, NULL, 0);

		{
			char before[16];

			snprintf(before, sizeof(before), "%08x",
			         (unsigned)*R_IPU_CTRL);

			MPEG_Initialize(data_cb, NULL, init_cb, gs, &g_cur_pts);

			snprintf(g_info, sizeof(g_info),
			         "rst x%d  ctrl %s -> %08x",
			         g_reset_tries, before,
			         (unsigned)*R_IPU_CTRL);
		}

		g_mpeg_up = 1;
	}

	g_ended = 0;

	g_last_pts = -1;
	g_pic_pts  = 0;
	g_dec_ms   = 0;
	g_dec_max  = 0;
	g_cur_pts  = 0;

	g_want  = 0;
	g_have  = 0;
	g_fail  = 0;
	g_busy  = 0;
	g_ticks = 0;

	g_hold        = 0;
	g_data_end    = 0;
	g_busy_blanks = 0;
	g_parked      = 0;
	g_cb_zero     = 0;
	g_cb_calls    = 0;
	g_fed         = 0;
	g_last_n      = 0;
	net_set_vblank_hook(stuck_watch);

	if (g_sema < 0) {
		ee_sema_t sm;

		memset(&sm, 0, sizeof(sm));
		sm.init_count = 0;
		sm.max_count  = 1;

		g_sema = CreateSema(&sm);

		if (g_sema < 0) {
			snprintf(g_err, sizeof(g_err), "Could not create a semaphore");
			poster_reinit(gs);
			return 0;
		}
	}

	g_run = 1;

	if (g_thread < 0) {
		extern void *_gp;
		ee_thread_t th;

		memset(&th, 0, sizeof(th));

		th.func             = worker;
		stack_guard_fill(g_stack);
		th.stack            = g_stack;
		th.stack_size       = sizeof(g_stack);
		th.gp_reg           = &_gp;
		th.initial_priority = VIDEO_PRIO;

		g_thread = CreateThread(&th);
		freeze_register(g_thread, FZ_DECODER);

		if (g_thread < 0) {
			snprintf(g_err, sizeof(g_err), "Could not start decoder");
			poster_reinit(gs);
			return 0;
		}

		StartThread(g_thread, NULL);
	}

	g_active = 1;
	return 1;
}

/*
 * Unsynchronised presentation (no sound): show the decoded picture and ask
 * for the next.
 */
int video_step(int due)
{
	if (!g_active)
		return -1;

	if (g_fail)
		return g_err[0] ? -1 : 0;      /* failed, or simply ended */

	if (g_have) {
		g_have = 0;

		/*
		 * Scatter the macroblocks into the texture, here on the UI
		 * thread beside gsKit's own drawing.
		 *
		 * Both waits are bounded: gsKit drives this channel too, and a
		 * zero timeout spins forever. A timeout costs one torn frame,
		 * which is a fair price for a console that keeps running.
		 */
		if (dma_channel_wait(DMA_CHANNEL_GIF, 200000) >= 0) {
			dma_channel_send_chain(DMA_CHANNEL_GIF, g_xfer->data,
			                       g_xfer->qwc, 0, 0);
			dma_channel_wait(DMA_CHANNEL_GIF, 200000);
		}

		g_ready = 1;
		return 1;
	}

	/*
	 * Ask for the next one only when it is due.
	 *
	 * Collecting, above, happens on every call: pacing the collection as
	 * well as the request meant a finished picture waited up to two extra
	 * draws to be picked up, which halved the frame rate and looked like
	 * a slow decoder.
	 */
	if (due && !g_busy && !g_want && !g_hold) {
		g_want = 1;
		SignalSema(g_sema);
	}

	return 2;                          /* still working */
}

/* Whether a decoded picture is waiting, and its timestamp. */
int video_peek(long long *pts90)
{
	if (!g_active || g_fail || !g_have)
		return 0;

	if (pts90)
		*pts90 = g_pic_pts;

	return 1;
}

/*
 * Stop asking for pictures: the film ends after the one held, or the one
 * being decoded, without libmpeg ever being called into the tail of the
 * stream - where it hangs (see scr_video's near_the_end).
 */
void video_hold(void) { g_hold = 1; }

/* Ask for the next picture if the worker is idle and nothing is held. */
static void request_next(void)
{
	if (!g_have && !g_busy && !g_want && !g_hold) {
		g_want = 1;
		SignalSema(g_sema);
	}
}

/*
 * Synchronised presentation, driven by sync_step: act 1 shows the waiting
 * picture (scatter it into the texture) and asks for the next, -1 drops
 * it, 0 waits. Returns 0 at the end, 1 shown, 2 waiting, -1 on error.
 */
int video_step_sync(int act)
{
	if (!g_active)
		return -1;

	/* Stuck at the end and set aside by the watchdog: the film is over. */
	if (g_parked) {
		g_ended = 1;
		return 0;
	}

	if (g_fail)
		return g_err[0] ? -1 : 0;

	if (g_have) {
		if (act == 0)
			return 2;                  /* not its time yet */

		if (act < 0) {
			/*
			 * Late: never uploaded. g_pic is free again as soon
			 * as the flag drops, so the worker can decode into it.
			 */
			g_have = 0;
			request_next();
			return 3;
		}

		/* The same transfer video_step does, bounded the same way. */
		if (dma_channel_wait(DMA_CHANNEL_GIF, 200000) >= 0) {
			dma_channel_send_chain(DMA_CHANNEL_GIF, g_xfer->data,
			                       g_xfer->qwc, 0, 0);
			dma_channel_wait(DMA_CHANNEL_GIF, 200000);
		}

		g_ready = 1;

		/*
		 * The picture is in VRAM now, so g_pic can be reused: start on
		 * the next one straight away. That is what lets it be ready,
		 * and waiting, by the time it is due.
		 */
		g_have = 0;
		request_next();
		return 1;
	}

	/* Held, and the last picture is out: ended, between pictures. */
	if (g_hold && !g_busy && !g_want) {
		g_ended = 1;
		return 0;
	}

	request_next();
	return 2;
}

/*
 * Whether the decoder has been inside one picture for far too long; see
 * the comment inside.
 */
int video_stalled(void)
{
	/*
	 * A picture taking far longer than any picture should.
	 *
	 * MPEG_Picture contains several unbounded spins - _MPEG_WaitBDEC waits
	 * for D3_QWC to reach zero, for one - and a decoder left in a bad
	 * state sits in one of them forever. On the UI thread that was a dead
	 * console; here it is a number that stops changing.
	 */
	static int last_ticks, still;

	if (!g_busy) {
		still = 0;
		return 0;
	}

	if (g_ticks != last_ticks) {
		last_ticks = g_ticks;
		still      = 0;
		return 0;
	}

	return ++still > 180;              /* three seconds on one picture */
}

/* Draw the current picture as a textured sprite filling the given box. */
void video_draw(GSGLOBAL *gs, float x, float y, float w, float h)
{
	if (!g_ready)
		return;

	/* Layout space to canvas (layout.h), then canvas to framebuffer. */
	x = LAY_X(x);
	y = LAY_Y(y) + SCR_Y0;
	w = LAY_W(w);
	h = LAY_H(h);

	/*
	 * Sample half a texel inside the picture's edges. The texture is
	 * drawn smoothed (bilinear), which at an edge blends in half a texel
	 * beyond it - and past the picture's last row are the spare rows of
	 * the reservation, holding whatever that VRAM held before (a poster,
	 * a previous film). That showed as a thin coloured line under 16:9
	 * films. Half a texel at each edge is invisible.
	 */
	gsKit_prim_sprite_texture(gs, &g_tex,
	                          x, y, 0.5f, 0.5f,
	                          x + w, y + h,
	                          (float)video_width() - 0.5f,
	                          (float)video_height() - 0.5f,
	                          2, GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0));
}

/*
 * Give the VRAM back without touching a wedged decoder; see the comment
 * inside.
 */
void video_abandon(GSGLOBAL *gs)
{
	/*
	 * Give the VRAM back without touching the decoder.
	 *
	 * For when it is wedged inside MPEG_Picture: MPEG_Destroy would spin
	 * waiting for a colour conversion that will never finish, so the
	 * library is left as it is and only our own resources are recovered.
	 *
	 * Skipping this is what leaked the texture. Each abandoned playback
	 * allocated another 1.25 MB on top of the last, and the third one
	 * reported 3584 KB of 4096 used and refused.
	 */
	if (!g_active)
		return;

	g_run    = 0;
	g_want   = 0;
	g_ready  = 0;
	g_active = 0;
	g_tex_held = 0;

	/*
	 * Abandoned, so the session is left as it is: MPEG_Destroy would spin
	 * on a colour conversion that will never finish. The next start
	 * resets the IPU and initialises afresh regardless.
	 */
	g_mpeg_up = 0;

	poster_reinit(gs);
}

/*
 * Stop decoding: let the decoder finish its picture, replace it if it was
 * set aside, reset the IPU and destroy libmpeg, then give the VRAM back to
 * the posters.
 */
void video_end(GSGLOBAL *gs)
{
	if (!g_active) {
		video_release(gs);           /* a picture kept from a pause */
		return;
	}

	/*
	 * Stand the worker down and let it finish the picture it is on.
	 *
	 * The source is still readable at this point, deliberately: closing
	 * it first leaves the worker inside MPEG_Picture with nothing to
	 * read, which is what wedges it.
	 */
	g_run  = 0;
	g_want = 0;

	/*
	 * A decoder the watchdog set aside is still spinning inside libmpeg
	 * and never will leave. End it, so the IPU and the library can be
	 * reset safely below, and so the next film starts a fresh thread.
	 */
	if (g_parked && g_thread >= 0) {
		/* What it was stuck on, before anything is reset. */
		{
			int o = snprintf(g_stuck, sizeof(g_stuck),
			    "decoder stuck at end of stream\n"
			    "ipu_ctrl %08x ipu_bp %08x\n"
			    "d4(toIPU) chcr %08x qwc %u  d3(fromIPU) chcr %08x qwc %u\n"
			    "d_enable %08x\n"
			    "cb calls %d, empty %d, fed %d bytes, last block %d bytes\n"
			    "last bytes fed:",
			    (unsigned)g_snap[0], (unsigned)g_snap[1],
			    (unsigned)g_snap[2], (unsigned)g_snap[3],
			    (unsigned)g_snap[4], (unsigned)g_snap[5],
			    (unsigned)g_snap[6],
			    g_cb_calls, g_cb_zero, g_fed, g_last_n);

			for (int i = 0; i < (int)sizeof(g_last_tail) &&
			     o < (int)sizeof(g_stuck) - 4; i++)
				o += snprintf(g_stuck + o, sizeof(g_stuck) - o,
				              " %02x", g_last_tail[i]);

			if (o < (int)sizeof(g_stuck) - 2)
				snprintf(g_stuck + o, sizeof(g_stuck) - o, "\n");
		}

		TerminateThread(g_thread);
		DeleteThread(g_thread);
		g_thread = -1;
		g_busy   = 0;
		g_parked = 0;
	}

	/* Wake it so it notices, rather than waiting on a signal that will
	 * never come. */
	if (g_sema >= 0)
		SignalSema(g_sema);

	/*
	 * Wait for it to leave MPEG_Picture. Two seconds, because a stall
	 * detector allows three.
	 */
	for (int i = 0; i < 200 && g_busy; i++)
		net_snooze(10);

	/*
	 * If it is still in there, leave libmpeg alone.
	 *
	 * Resetting the IPU and destroying the session underneath a thread
	 * sitting inside the library is worse than not tidying up: the next
	 * MPEG_Initialize then fails to run init_cb, and the following film
	 * inherits this one's size and freezes. It happened on the second
	 * stop but not the first, which is the signature of a race.
	 *
	 * g_mpeg_up is cleared either way, so the next start resets the IPU
	 * and initialises afresh - which is now reliable, and is what
	 * actually recovers the state.
	 */
	if (g_busy) {
		/*
		 * Still inside libmpeg after two seconds.
		 *
		 * Nothing here can safely touch the IPU or the library now,
		 * and the next start must not either - resetting under a
		 * thread that is inside MPEG_Picture is what produced a
		 * sequence of 0x0. So the thread is marked as lost and the
		 * next start refuses rather than corrupting things further.
		 */
		snprintf(g_info, sizeof(g_info), "decoder busy at stop");
		g_lost = 1;
	} else {
		ipu_reset();
		MPEG_Destroy();

		if (g_xfer) {
			packet_free(g_xfer);
			g_xfer = NULL;
		}
	}

	/*
	 * g_pic stays allocated regardless: libmpeg owns it once init_cb has
	 * handed it over, and freeing it as well corrupts the heap.
	 */
	g_mpeg_up = 0;
	g_active  = 0;

	if (g_keep_on_end) {
		/* Keep the texture and what is in it: g_ready stays as it was. */
		g_keep_on_end = 0;
		g_tex_held    = 1;
		return;
	}

	g_ready   = 0;

	/* The texture's VRAM goes back with the slots' - it was allocated in
	 * the same reclaimed space. */
	poster_reinit(gs);
}

/* video_end, keeping the last picture on screen (a seek or a pause). */
void video_end_keep(GSGLOBAL *gs)
{
	if (!g_active)
		return;

	g_keep_on_end = 1;
	video_end(gs);
	g_keep_on_end = 0;
}

/* Release a picture kept by video_end_keep and give its VRAM back. */
void video_release(GSGLOBAL *gs)
{
	if (!g_tex_held)
		return;

	g_tex_held = 0;
	g_ready    = 0;
	poster_reinit(gs);
}

/* Whether there is a picture to draw. */
int video_has_picture(void) { return g_ready; }
/* The last watchdog rescue's evidence, once: "" afterwards. */
const char *video_take_stuck_report(void)
{
	static char out[sizeof(g_stuck)];

	memcpy(out, g_stuck, sizeof(out));
	g_stuck[0] = '\0';
	return out;
}

/*
 * The freeze safety net set the decoder aside (freeze.c): treat it as the
 * end-of-stream watchdog's parked decoder, so video_end ends the thread
 * and the next film starts a fresh one.
 */
void video_mark_stuck(void)
{
	if (g_thread >= 0 && g_busy)
		g_parked = 1;
}
