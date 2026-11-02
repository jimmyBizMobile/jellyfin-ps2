/*
 * Video playback. Opens the stream, runs the network reader thread that
 * demuxes it into the video and audio rings, starts the decoder (video.c)
 * and sound (vaudio.c), presents pictures against the sound clock, and
 * handles the bar, pause, seek, subtitles, the end of the film and
 * stopping.
 */

#include "screens.h"
#include "freeze.h"
#include "ui.h"
#include "app.h"
#include "video.h"
#include "config.h"
#include "jf.h"
#include "net.h"
#include "ts.h"
#include "vaudio.h"
#include "subs.h"
#include "settings.h"

#include <kernel.h>       /* the reader thread */
#include <libpad.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/*
 * Video playback, streamed from Jellyfin.
 *
 * The server transcodes to MPEG-2 in a transport stream, so what arrives
 * carries video and audio interleaved and both have to be unpicked. A file
 * on USB is still supported and is how the decoder was proven; it remains
 * the way to test with the network out of the picture, but it carries no
 * sound.
 */

/*
 * Where the video comes from.
 *
 * A file on USB was how the decoder was proven, and it is kept because it
 * removes the network from the picture when something goes wrong. But USB
 * reads at about 190 KB/s here, which caps the bitrate; the server
 * transcodes at ten times that, so streaming is both faster and sharper.
 */
#define VIDEO_PATH "mass:/test.bin"

static int   g_fd   = -1;    /* the file, when playing from USB */
static int   g_sock = -1;    /* the socket, when streaming */

/*
 * Jellyfin sends a transport stream, not raw MPEG-2, so what arrives has to
 * be unpicked before the decoder sees it. The demuxer keeps its own state
 * because packets arrive split across reads.
 */
static TsDemux      g_ts;
static unsigned char g_raw[8192];   /* as it comes off the socket */
static int   g_playing;
static int   g_frames;
static int   g_due;        /* thousandths of a ms owed toward the next picture */
static char  g_msg[128];
static char  g_stall_dbg[112];
static char  g_stall_adbg[64];
static int   g_ms_shown;   /* the frame time in use, for the readout */
static int   g_drift;      /* clock minus the picture's time, ms: + is late */
static int   g_vlate;      /* pictures dropped for being late */

/* Playback controls: see the section above input(). */
static int       g_live_paused;     /* paused in place, stream still open */
static long long g_pause_wall;      /* when, 90 kHz wall */
static int       g_seek_pending;    /* presses gathering */
static long long g_seek_target;     /* where they add up to, 100 ns */
static long long g_seek_at;         /* when to go, 90 kHz wall */
static int       g_recovered;       /* reloads after a dropped stream */
static int       g_any_shown;       /* a picture went up during this film */

/* The bar's three buttons, left to right. */
enum { VB_BACK, VB_PLAY, VB_FWD, VB_N };
static int       g_vbtn = VB_PLAY;  /* the focused one */

static int bar_visible(void);
static long long g_open_start;      /* where the next film starts: 0 or resume */


/*
 * Feed the decoder from the file, buffered.
 *
 * This runs inside MPEG_Picture, on the decoder thread, so a read stalls
 * decoding - which is why it reads 64 KB at a time rather than the 2 KB the
 * decoder asks for. A 2 KB read from FAT costs about as much as a 64 KB one,
 * so one read now serves thirty-two requests.
 *
 * A reader thread and ring buffer would remove the stall altogether, and
 * were tried: see the note in video.c. Both stalled the decoder in a way a
 * full ring could not explain, because GIF access cannot be shared between
 * threads without a lock that reaches into ui.c.
 */
#define CHUNK (64 * 1024)

static unsigned char g_buf[CHUNK] __attribute__((aligned(64)));
static int           g_have;      /* bytes in the buffer */
static int           g_used;      /* how many of them are gone */

/*
 * One read from the socket feeds both streams.
 *
 * The transport stream interleaves video and audio, so a single read carries
 * some of each and one pass over the packets splits them by PID. Each lands
 * in its own ring, because the two are consumed at very different rates:
 * video takes about 187 KB/s and audio 24 KB/s.
 *
 * One producer and one consumer per ring - the pump only advances a head,
 * each consumer only its own tail - so no locking is needed.
 */
/*
 * Seconds, not fractions of one.
 *
 * These were 32 KB and 64 KB: about a sixth of a second of picture. The
 * reader stops when either ring is short of room, so any hiccup in decoding
 * stopped the socket - and the sound with it. Sized as SMS sizes them, the
 * video ring holds about ten seconds at 1.5 Mbit/s and the audio ring about
 * twenty at 192 kbit/s, so the sound always has its own supply in hand.
 */
#define VRING (2 * 1024 * 1024)
#define ARING (512 * 1024)

/*
 * Set to make the source report the end without touching the socket.
 *
 * Closing a socket does not wake a thread already blocked in recv on this
 * stack. The socket has a one-second receive timeout, so the worker surfaces
 * regularly - and when it does, this tells it to stop rather than read again.
 */
static volatile int  g_stop_src;

static TsDemux       g_ats;              /* the audio demuxer */
static unsigned char g_vring[VRING];
static unsigned char g_aring[ARING];
static volatile int  g_vhead, g_vtail;
static volatile int  g_ahead, g_atail;

/* Bytes thrown away for want of room. Should stay at zero now. */
static volatile int  g_adropped;
static volatile int  g_vdropped;

/*
 * Which timestamp the video bytes belong to.
 *
 * The reader records, for each PES that carried a PTS, its position in the
 * video stream counted from the first byte. The decoder side counts what it
 * has taken, so after each 2 KB block it can say which PES starts it has
 * passed - and the newest of those is the timestamp libmpeg stamps on the
 * picture it is decoding. One writer (the reader), one reader (the decoder
 * thread). About ten seconds of pictures fit in the ring; 1024 marks is
 * three times that.
 */
#define VMARKS 1024

static volatile long long g_vmark_pos[VMARKS];
static volatile long long g_vmark_pts[VMARKS];
static volatile int       g_vmark_n;      /* marks written */
static int                g_vmark_seen;   /* decoder: marks passed */
static long long          g_vin;          /* reader: video bytes put */

/*
 * Receive timeouts in a row, for the end-of-stream test. Per stream: it was
 * a static inside pump and survived from one stream to the next, so after a
 * film had run to its end (which leaves it at the limit) the next stream
 * gave up on its very first timeout. Streams that answer at once never
 * noticed; a transcode burning in subtitles always did.
 */
static int                g_idle;

/*
 * The last three video bytes put in the ring, so a sequence end code split
 * across two feeds is still caught (see defuse_seq_end).
 */
static unsigned int       g_vtail3;

/*
 * Whether the stream's closing start code has been handed to the decoder
 * (see end_of_data).
 */
static int                g_vclosed;

/*
 * Defuse MPEG-2's sequence end code, 00 00 01 B7.
 *
 * A transcode that runs to its end finishes with one, and libmpeg does not
 * return from MPEG_Picture on it: it spins, inside the decoder thread, which
 * runs above the UI - so the whole console froze on the film's last frame,
 * buttons dead (found on hardware: stream ended, clock handed over, sound
 * idle, decoder about to read the end; ORBIT's client notes the same hang).
 * Without the code the data simply runs out, and MPEG_Picture returns 0.
 *
 * The B7 is rewritten as B2, a user-data start code, rather than removed:
 * removing bytes would shift the timestamp marks recorded against the
 * stream, and user data with nothing after it is skipped harmlessly.
 */
static void defuse_seq_end(unsigned char *p, int n)
{
	unsigned int w = g_vtail3;

	for (int i = 0; i < n; i++) {
		w = ((w << 8) | p[i]) & 0xFFFFFFFFu;

		if (w == 0x000001B7u) {
			p[i] = 0xB2;
			w = 0x000001B2u;
		}
	}

	g_vtail3 = w & 0x00FFFFFFu;
}
static long long          g_vout;         /* decoder: video bytes taken */

/* The PTS of the first audio byte in the ring; -1 until then. */
static volatile long long g_apts0 = -1;

/* The first picture is up: the sound may start. */
static volatile int g_first_shown;

/*
 * Record that the video byte at stream position pos starts a picture with
 * timestamp pts (see ts.c's marks).
 */
static void add_vmark(long long pos, long long pts)
{
	int k = g_vmark_n & (VMARKS - 1);

	g_vmark_pos[k] = pos;
	g_vmark_pts[k] = pts;
	g_vmark_n++;
}

/* For video.c, after each block: the newest PTS passed, or -1. */
static long long take_pts(void)
{
	long long best = -1;
	int n = g_vmark_n;

	if (n - g_vmark_seen > VMARKS)
		g_vmark_seen = n - VMARKS;   /* overwritten: skip ahead */

	while (g_vmark_seen < n) {
		int k = g_vmark_seen & (VMARKS - 1);

		if (g_vmark_pos[k] >= g_vout)
			break;

		best = g_vmark_pts[k];
		g_vmark_seen++;
	}

	return best;
}

/*
 * Bytes waiting in a ring (head written by the reader, tail by the
 * consumer).
 */
static int ring_used(int head, int tail, int size)
{
	int n = head - tail;
	return n < 0 ? n + size : n;
}

/*
 * Copy n bytes into a ring at head, wrapping. The caller has checked there
 * is room.
 */
static void ring_put(unsigned char *ring, int size, volatile int *head,
                     const unsigned char *src, int n)
{
	int h = *head;
	int first = size - h;

	if (first > n)
		first = n;

	memcpy(ring + h, src, first);

	if (n > first)
		memcpy(ring, src + first, n - first);

	*head = (h + n) % size;
}

/*
 * Copy up to max bytes out of a ring at tail, wrapping. Returns the count.
 */
static int ring_take(const unsigned char *ring, int size, volatile int *tail,
                     int head, unsigned char *dst, int max)
{
	int avail = ring_used(head, *tail, size);

	if (avail > max)
		avail = max;

	int t = *tail;
	int first = size - t;

	if (first > avail)
		first = avail;

	memcpy(dst, ring + t, first);

	if (avail > first)
		memcpy(dst + first, ring, avail - first);

	*tail = (t + avail) % size;

	return avail;
}

/*
 * Read once, and hand what comes out to both rings.
 *
 * Returns 0 when the stream has ended, 1 otherwise.
 */
static int pump(void)
{
	if (g_sock < 0 || g_stop_src)
		return 0;

	/*
	 * Static, not on the stack. Two 8 KB buffers is 16 KB of locals, and
	 * this runs on the decoder thread whose stack is exactly that - it
	 * overflowed, and the symptom was zero frames decoded rather than
	 * anything pointing here.
	 *
	 * The 256 bytes of headroom matter too: a read of 8192 can yield 8096
	 * bytes of video, past where a same-sized buffer stops accepting, and
	 * whatever is left unread is discarded. That was blocks of corruption
	 * across the picture.
	 */
	static unsigned char es[8192 + 256];
	static unsigned char aes[8192 + 256];

	int n = net_stream_read(g_sock, g_raw, (int)sizeof(g_raw));

	if (n == 0)
		return 0;                      /* the server closed */

	if (n < 0) {
		/*
		 * A timeout. When a transcode finishes, Jellyfin stops
		 * sending without closing the socket, so waiting forever on
		 * one hangs the decoder at the last frame. Five seconds of
		 * silence is the end, counted in short timeouts.
		 *
		 * Only once something has arrived. Before the first byte the
		 * server is still starting the transcode - burning subtitles
		 * in can take a long while - and ST_BUFFER has its own, much
		 * longer limit for that.
		 */
		if (g_stop_src)
			return 0;

		if (g_vin > 0 && ++g_idle >= 5000 / VIDEO_RECV_MS)
			return 0;

		return 1;
	}

	g_idle = 0;

	int v = 0, a = 0;

	ts_feed2(&g_ts, &g_ats, g_raw, n,
	         es, sizeof(es), &v,
	         aes, sizeof(aes), &a);

	if (v > 0) {
		int room = VRING - 1 - ring_used(g_vhead, g_vtail, VRING);

		if (v <= room) {
			/* Timestamps first, positioned in the stream. */
			for (int k = 0; k < g_ts.nmark; k++)
				add_vmark(g_vin + g_ts.mark_off[k],
				          g_ts.mark_pts[k]);

			defuse_seq_end(es, v);
			ring_put(g_vring, VRING, &g_vhead, es, v);
			g_vin += v;
		} else {
			g_vdropped += v;
		}
	}

	if (a > 0) {
		const unsigned char *src = aes;
		int len = a;

		/*
		 * Start the audio ring on a timestamp.
		 *
		 * The clock is the first PTS plus the samples played, so the
		 * first byte in the ring has to be the one that PTS belongs
		 * to. Anything before the first stamped PES is a fragment of
		 * a frame anyway.
		 */
		if (g_apts0 < 0) {
			if (g_ats.nmark == 0) {
				len = 0;
			} else {
				src  += g_ats.mark_off[0];
				len  -= g_ats.mark_off[0];
				g_apts0 = g_ats.mark_pts[0];
			}
		}

		if (len > 0) {
			int room = ARING - 1 - ring_used(g_ahead, g_atail, ARING);

			/* Only the reader moves the head, so this can only
			 * refuse - and with the reader checking for room
			 * first, it should not. */
			if (len <= room)
				ring_put(g_aring, ARING, &g_ahead, src, len);
			else
				g_adropped += len;
		}
	}

	return 1;
}

/*
 * The socket has its own thread.
 *
 * Reading used to happen inside data_cb, on the decoder thread - so the
 * audio ring only filled when video asked for bytes. Everything about the
 * sound was then downstream of the picture: a pause starved it, and holding
 * a frame to correct drift stopped it entirely, which deadlocked both.
 *
 * With the reading separate, each decoder takes from its own ring at its own
 * pace and neither can starve the other. TCP provides the backpressure: when
 * both rings are full the thread stops reading and the sender waits.
 */
static int  g_reader = -1;
static char g_rstack[32 * 1024] __attribute__((aligned(16)));  /* was 8 KB */

/*
 * Stack guard: the lowest 1 KB is filled with a pattern when the thread is
 * made. A stack grows down, so an overflow breaks the pattern before it
 * reaches anything else - and the music screen reports it.
 */
#define STACK_GUARD 1024

/* Fill the bottom of a thread stack with a known pattern. */
static void stack_guard_fill(char *stk)
{
	for (int i = 0; i < STACK_GUARD; i++)
		stk[i] = (char)0xA5;
}

/*
 * Whether that pattern is still intact (1) or the stack overran into it
 * (0).
 */
static int stack_guard_ok(const char *stk)
{
	for (int i = 0; i < STACK_GUARD; i++)
		if (stk[i] != (char)0xA5)
			return 0;

	return 1;
}

/* The network reader thread's stack guard, for the DEBUG_UI readout. */
int scr_video_stack_ok(void) { return stack_guard_ok(g_rstack); }

static volatile int g_src_done;   /* the stream has ended */
static volatile int g_reader_run;
static volatile int g_reader_busy; /* inside pump right now */

static void reader(void *arg)
{
	(void)arg;

	for (;;) {
		if (!g_reader_run || g_src_done) {
			net_snooze(20);
			continue;
		}

		/* Room in both, or there is nowhere to put what arrives. */
		int vroom = VRING - 1 - ring_used(g_vhead, g_vtail, VRING);
		int aroom = ARING - 1 - ring_used(g_ahead, g_atail, ARING);

		if (vroom < 8704 || aroom < 8704) {
			net_snooze(2);
			continue;
		}

		g_reader_busy = 1;

		if (!pump())
			g_src_done = 1;

		g_reader_busy = 0;
	}
}

/*
 * What the decoder gets once the stream has run out: a start code, once,
 * then nothing.
 *
 * An MPEG-2 slice has no length: it ends where the next start code begins.
 * Every picture's last slice is ended by the next picture - except the
 * film's very last one, which the stream simply stops after (seen in a
 * Jellyfin transcode: "00 00 01 1E 12 70 10 33 80" and then nothing). The
 * IPU, mid-slice, waited for bits that never came, and libmpeg never
 * returned: the decoder spun above the UI and the console froze on the
 * last frame.
 *
 * So the slice is closed with a start code of our own. User data (B2),
 * the same harmless code the sequence end becomes, is skipped by libmpeg,
 * which then meets the end of data between pictures - where it returns 0
 * cleanly.
 */
static int end_of_data(void *buf, int len)
{
	static const unsigned char close_code[4] = { 0x00, 0x00, 0x01, 0xB2 };

	if (g_vclosed || g_stop_src || len < 4)
		return 0;

	g_vclosed = 1;
	memcpy(buf, close_code, 4);
	return 4;
}

/*
 * The decoder's data source (data_cb in video.c): video bytes from the
 * ring, waiting for the reader if it is empty; at the end of the stream,
 * the closing start code once, then 0.
 */
static int read_stream(void *buf, int len)
{
	/*
	 * Wait for the reader, rather than reading here.
	 *
	 * Bounded, so a stream that stops leaves the decoder rather than
	 * hanging it. Ten seconds is far longer than any hiccup and is only
	 * reached when the source has genuinely gone.
	 */
	if (!VIDEO_AUDIO_ENABLE) {
		/* No reader thread, so fill the ring here. */
		while (ring_used(g_vhead, g_vtail, VRING) == 0) {
			if (!pump())
				return end_of_data(buf, len);
		}
	} else {
		for (int i = 0; i < 500; i++) {
			if (ring_used(g_vhead, g_vtail, VRING) > 0)
				break;

			if (g_src_done || g_stop_src)
				return end_of_data(buf, len);

			net_snooze(20);
		}
	}

	int got = ring_take(g_vring, VRING, &g_vtail, g_vhead, buf, len);

	g_vout += got;

	return got;
}

/*
 * Demuxed audio, for vaudio to decode.
 *
 * Nothing is ever skipped here any more. The old version trimmed the ring
 * to half on every read, to drop a startup backlog - but it did so all film
 * long, and any time the picture ran a little ahead of the sound a second
 * or so of sound was thrown away. With timestamps doing the pacing there is
 * no backlog to drop: the sound starts at the first PTS, the picture waits
 * for it, and every byte is played.
 */
int scr_video_audio_read(void *buf, int len)
{
	return ring_take(g_aring, ARING, &g_atail, g_ahead, buf, len);
}

/* Audio bytes waiting, for the sound thread. */
int scr_video_audio_avail(void) { return ring_used(g_ahead, g_atail, ARING); }

/*
 * Timestamp of the first audio byte of this stream, for aligning sound
 * with the first picture.
 */
long long scr_video_audio_pts0(void) { return g_apts0; }

/* Whether the first picture is on screen yet (the sound waits for it). */
int scr_video_first_shown(void) { return g_first_shown; }
long long scr_video_first_vpts(void);

/* Whether the stream has ended or is being stopped. */
int scr_video_source_done(void) { return g_src_done || g_stop_src; }

/*
 * THE CLOCK
 *
 * The sound's position when it is playing, the wall clock otherwise. The
 * wall clock is kept continuous with the sound - each audio reading resets
 * its offset - so falling back to it, or back again, is not a jump.
 *
 * clock() is read as differences only and accumulated into 64 bits, so its
 * width and rate do not matter here.
 */
static clock_t   g_wall_last;
static long long g_wall_acc;
static long long g_wall_off;     /* stream time minus wall time */
static int       g_wall_ok;      /* g_wall_off means something */
static int       g_audio_seen;   /* the audio clock has run this film */
static int       g_expect_audio; /* there is a sound decoder to wait for */
static long long g_first_wall;   /* when the first picture went up */
static long long g_last_show;    /* when a picture last went up */
static int       g_frame90 = 3003;

/*
 * Where in the film we are, ORBIT's way: the stream started at
 * g_start_ticks (100 ns, as Jellyfin counts), and the picture on screen is
 * that much plus its PTS less the first picture's. This is what a time bar
 * shows, what a seek adds to, and what the stop report sends.
 */
static long long g_start_ticks;          /* where this stream was asked from */
static long long g_first_vpts = -1;      /* PTS of the first picture shown */
static long long g_pos_ticks;            /* the picture on screen, 100 ns */

long long scr_video_first_vpts(void) { return g_first_vpts; }
static char      g_csrc = '-';    /* last clock used: A audio, W wall, H hold */

static long long wall90(void)
{
	clock_t now = clock();

	g_wall_acc += (unsigned long)(now - g_wall_last);
	g_wall_last = now;

	return g_wall_acc * 90000 / CLOCKS_PER_SEC;
}

/* Reset the playback clock for a new stream. */
static void clock_reset(void)
{
	g_wall_last  = clock();
	g_wall_acc   = 0;
	g_wall_off   = 0;
	g_wall_ok    = 0;
	g_audio_seen = 0;
	g_first_wall = 0;
	g_last_show  = 0;
}

/*
 * Where the film is, at 90 kHz, or -1 to hold the picture.
 *
 * vp is the picture waiting to be shown. When nothing has set the clock
 * yet, that first picture does: it goes up at once and the wall clock
 * starts from its timestamp.
 */
static long long av_clock(long long vp)
{
	long long w = wall90();
	long long a;

	/*
	 * Once the stream has ended and every byte of sound has been taken,
	 * the sound clock stops: the sound thread pads with silence, which
	 * keeps the clock fresh but never moves it. The last pictures were
	 * then never due, the decoder never reached the end, and the film
	 * sat on its last frame instead of returning to its page.
	 *
	 * From there the wall clock carries on, from the offset the sound
	 * last gave it, so the hand-over is seamless.
	 */
	int sound_over = g_src_done &&
	                 ring_used(g_ahead, g_atail, ARING) < 1024;

	if (g_expect_audio && !sound_over && vaudio_clock90(&a)) {
		g_csrc       = 'A';
		g_wall_off   = a - w;
		g_wall_ok    = 1;
		g_audio_seen = 1;
		return a;
	}

	if (!g_wall_ok) {
		g_csrc       = 'W';
		g_wall_off   = vp - w;
		g_wall_ok    = 1;
		g_first_wall = w;
		return vp;
	}

	/*
	 * The first picture is up and the sound is starting. Hold the next
	 * rather than run ahead on the wall clock, but not for ever: a film
	 * with no sound track plays on the wall clock after two seconds.
	 */
	if (g_expect_audio && !g_audio_seen && w - g_first_wall < 2 * 90000) {
		g_csrc = 'H';
		return -1;
	}

	g_csrc = 'W';
	return w + g_wall_off;
}

/*
 * The state of the whole pipeline in one line, for the overlay and for the
 * stall message: ring levels (KB), whether the audio ring found its first
 * timestamp, the bytes the reader has put, the clock in use and the wall
 * clock in seconds (it must count up, or clock() is not working here), then
 * the audio worker's own state.
 */
static const char *pipe_debug(void)
{
	static char buf[112];

	snprintf(buf, sizeof(buf),
	         "vr%dK ar%dK p0%c in%dK end%d clk%c w%d",
	         ring_used(g_vhead, g_vtail, VRING) >> 10,
	         ring_used(g_ahead, g_atail, ARING) >> 10,
	         g_apts0 >= 0 ? '+' : '-',
	         (int)(g_vin >> 10), g_src_done, g_csrc,
	         (int)(wall90() / 90000));
	return buf;
}

/*
 * Show the waiting picture when its time comes, drop it when it is more
 * than two frames late. The sound is never adjusted.
 *
 * Dropping is skipped if nothing has been shown for a quarter second, so a
 * decoder that is slow throughout still shows a picture now and then rather
 * than a frozen frame.
 */
/*
 * Whether the film should end after the picture at vp, rather than ask
 * libmpeg for another.
 *
 * libmpeg does not come back from the tail of a stream: whatever the last
 * bytes are (a sequence end code, a slice left open, a start code of our
 * own), some streams left it spinning with data still unread. ORBIT's
 * client found the same and ends between pictures half a second before
 * the runtime; so does this, and also once the stream has ended and only
 * a little is left - for films whose runtime runs past their content.
 * The cost is the last half-second, almost always black.
 */
/*
 * How the last film ended, for tuning end_margin_kb and end_early_ms: shown
 * on the readout's last line, so the next film tells you about the last.
 *   reason  R  the runtime rule, T  the tail rule
 *   cut     how much of the runtime was left unshown, ms
 *   left    video still unread when it ended
 */
/*
 * The end of a film that plays out, softened.
 *
 * Only the picture has to stop early - libmpeg is the video decoder - so
 * once the last picture is out the sound carries on to its own end, under
 * the last picture held on screen (almost always black by then). When the
 * sound has run dry, the picture fades to black, and only then the film's
 * page. Stop (Circle) still stops at once.
 *   0 playing   1 the sound playing out   2 fading
 */
#define OUTRO_SOUND_MAX_MS 2500     /* never wait longer for the sound */
#define OUTRO_FADE_MS      300

static int       g_outro;
static long long g_outro_t;         /* when the current phase began, 90 kHz */
static int       g_outro_sil;       /* silence count when the sound began to play out */
static int       g_held_end;        /* this stream was ended by near_the_end */

static char g_end_why = '-';
static int  g_end_cut_ms;
static int  g_end_left;

/*
 * Whether the film should end after the picture at vp (the time and data
 * rules), recording why for the readout.
 */
static int near_the_end(long long vp)
{
	long long len = jf_detail.runtime_ticks;
	long long pos = -1;

	if (g_first_vpts >= 0)
		pos = g_start_ticks + (vp - g_first_vpts) * 1000 / 9;

	char why = 0;

	if (len > 0 && pos >= 0 && settings.end_early_ms > 0 &&
	    pos >= len - settings.end_early_ms * 10000LL)
		why = 'R';
	else if (g_src_done &&
	         ring_used(g_vhead, g_vtail, VRING) < settings.end_margin_kb * 1024)
		why = 'T';

	if (!why)
		return 0;

	g_held_end   = 1;
	g_end_why    = why;
	g_end_cut_ms = (len > 0 && pos >= 0) ? (int)((len - pos) / 10000) : -1;
	g_end_left   = ring_used(g_vhead, g_vtail, VRING);
	return 1;
}

/* The readout line about how the last film ended (DEBUG_UI). */
static const char *end_debug(void)
{
	static char buf[96];

	snprintf(buf, sizeof(buf),
	         "end: margin %dK early %dms | last %c cut %dms left %dK",
	         settings.end_margin_kb, settings.end_early_ms,
	         g_end_why, g_end_cut_ms, g_end_left >> 10);
	return buf;
}

/*
 * Present pictures against the clock: show the held picture when due, drop
 * it if too late, wait if early; returns what video_step_sync reported (0
 * end, 1 shown, 2 waiting).
 */
/*
 * Timing diagnostics for the DEBUG_UI readout: why pictures are dropped.
 * The clock at the first picture versus that picture's PTS, the last
 * dropped picture's lateness, the largest, and the spacing between the
 * last pictures' timestamps (a steady frame period when all is well).
 */
static long long g_dbg_clk0 = -1;     /* clock - PTS at the first picture */
static int       g_dbg_late_last, g_dbg_late_max;   /* ms */
static long long g_dbg_prev_vp = -1;
static int       g_dbg_gap[4];        /* ms between recent PTS */
static int       g_dbg_gap_n;

/* Frames the screen thread drew in the last second (diagnostics): 60
 * when it keeps up with the display, less when something holds it. */
static int       g_dbg_ui_fps, g_dbg_ui_count;
static long long g_dbg_ui_t0 = -1;

static void count_ui_frame(void)
{
	long long w = wall90();

	if (g_dbg_ui_t0 < 0 || w < g_dbg_ui_t0)
		g_dbg_ui_t0 = w;               /* first frame, or a new stream's clock */

	g_dbg_ui_count++;

	if (w - g_dbg_ui_t0 >= 90000) {
		g_dbg_ui_fps   = g_dbg_ui_count;
		g_dbg_ui_count = 0;
		g_dbg_ui_t0    = w;
	}
}

static int sync_step(void)
{
	long long vp;
	int act = 0;

	if (video_peek(&vp)) {
		if (near_the_end(vp))
			video_hold();

		long long now = av_clock(vp);

		if (now >= 0) {
			long long w = wall90();

			g_drift = (int)((now - vp) / 90);

			/* Diagnostics: the clock against the picture, the first
			 * time both exist (biased so that 0 can be stored). */
			if (g_dbg_clk0 < 0)
				g_dbg_clk0 = now - vp + 1000000;

			if (now - vp > 2LL * g_frame90 &&
			    w - g_last_show < 90000 / 4) {
				act = -1;                  /* late: drop */

				g_dbg_late_last = (int)((now - vp) / 90);
				if (g_dbg_late_last > g_dbg_late_max)
					g_dbg_late_max = g_dbg_late_last;
			}
			else if (vp - now <= 8 * 90)
				act = 1;                   /* due: show */
		}
	}

	int r = video_step_sync(act);

	if (r == 1 || r == -1 || act == -1) {
		/* Spacing of consecutive picture timestamps. */
		if (g_dbg_prev_vp >= 0 && vp != g_dbg_prev_vp) {
			g_dbg_gap[g_dbg_gap_n & 3] = (int)((vp - g_dbg_prev_vp) / 90);
			g_dbg_gap_n++;
		}
		g_dbg_prev_vp = vp;
	}

	if (r == 1) {
		/* The PTS first: the sound reads it as soon as it sees the
		 * picture is up, to start at the same point. */
		if (g_first_vpts < 0)
			g_first_vpts = vp;

		g_first_shown = 1;
		g_any_shown   = 1;
		g_last_show   = wall90();

		/* 90 kHz to 100 ns: x 1000 / 9. */
		g_pos_ticks = g_start_ticks + (vp - g_first_vpts) * 1000 / 9;
	}

	return r;
}

/*
 * Development data source: with no item to play, video is read from the
 * file VIDEO_PATH (mass:/test.bin) instead of the network - a test harness
 * for the decoder that needs no server.
 */
static int read_file(void *buf, int len)
{
	if (g_fd < 0)
		return 0;

	if (g_used >= g_have) {
		g_have = read(g_fd, g_buf, sizeof(g_buf));
		g_used = 0;

		if (g_have <= 0) {
			g_have = 0;
			return 0;              /* end of file */
		}
	}

	int n = g_have - g_used;

	if (n > len)
		n = len;

	memcpy(buf, g_buf + g_used, n);
	g_used += n;

	return n;
}

/*
 * Startup is staged so a freeze says where it happened.
 *
 * Everything here used to run in enter(), which draws nothing: when the
 * second playback hung there was no way to tell whether it was opening the
 * file, releasing the artwork, starting the decoder or decoding the first
 * picture. Now each step happens on a separate frame, with the last one
 * completed shown on screen, so a hang leaves its position visible.
 */
typedef enum {
	ST_OPEN,        /* about to open the file */
	ST_BUFFER,      /* streaming: waiting for the first data */
	ST_BEGIN,       /* about to release artwork and start the decoder */
	ST_FIRST,       /* about to decode the first picture */
	ST_RUNNING,
	ST_PAUSED,      /* stream ended on purpose, last picture up */
	ST_DONE
} Stage;

static Stage g_stage;

/*
 * Frames to hold each step before attempting it.
 *
 * Half a second, purely so the sequence can be read. Without it the steps
 * flash past and a working start is indistinguishable from a hang at the
 * first one.
 */
#define STAGE_HOLD 0   /* was 30: the readout now says where a start stops */

static int g_hold;

/*
 * How much video to have before starting the decoder: about a second and
 * a half at 1.5 Mbit/s, which the server delivers in well under a second
 * once it is going.
 */
#define PREBUFFER_BYTES (256 * 1024)

static long long g_buffer_since;   /* when ST_BUFFER began, 90 kHz */

static void stop(void);

/* A stage's name, for the readout. */
static const char *stage_name(Stage s)
{
	switch (s) {
	case ST_OPEN:    return "connecting";
	case ST_BUFFER:  return "loading";
	case ST_BEGIN:   return "starting decoder";
	case ST_FIRST:   return "first picture";
	case ST_PAUSED:  return "paused";
	case ST_RUNNING: return "running";
	default:         return "done";
	}
}

/*
 * The picture's shape when the viewer chose one (Triangle), as
 * width/height x 1000, or 0 to use jf_detail.aspect_milli. Only offered
 * when that shape is a guess (aspect_sure 0); kept across seeks and
 * pauses of the same film, reset for each new one.
 */
static int       g_shape_choice;
static long long g_shape_note_until;     /* show the choice until (wall90) */

/* Clear every per-stream variable before a new stream starts. */
static void reset_stream(void)
{
	/*
	 * The shape note's end time is on this stream's clock, which starts
	 * again from zero: left alone, it showed again at the start of every
	 * later film.
	 */
	g_shape_note_until = 0;

	/* Timing diagnostics, per stream (DEBUG_UI readout). */
	g_dbg_clk0      = -1;
	g_dbg_late_last = 0;
	g_dbg_late_max  = 0;
	g_dbg_prev_vp   = -1;
	g_dbg_gap_n     = 0;
	memset(g_dbg_gap, 0, sizeof(g_dbg_gap));

	/*
	 * Nothing may survive from the last stream.
	 *
	 * Belt and braces against a path that returned without closing:
	 * whatever is still open here would otherwise be read as though it
	 * belonged to what is about to play.
	 */
	if (g_fd >= 0) {
		close(g_fd);
		g_fd = -1;
	}

	if (g_sock >= 0) {
		net_stream_close(g_sock);
		g_sock = -1;
	}

	g_playing = 0;
	g_frames  = 0;
	g_msg[0]  = '\0';
	g_stall_dbg[0] = '\0';
	g_stall_adbg[0] = '\0';
	g_csrc    = '-';
	g_due     = 0;
	g_have    = 0;
	g_used    = 0;
	g_vhead    = 0;
	g_vtail    = 0;
	g_ahead    = 0;
	g_atail    = 0;
	g_adropped = 0;
	g_vdropped = 0;
	g_drift    = 0;
	g_vlate    = 0;
	g_stop_src = 0;

	g_vmark_n     = 0;
	g_vmark_seen  = 0;
	g_vin         = 0;
	g_idle        = 0;
	g_vtail3      = 0xFFFFFFu;    /* nothing before the first byte */
	g_vclosed     = 0;
	g_held_end    = 0;
	g_outro       = 0;
	g_vout        = 0;
	g_apts0       = -1;
	g_first_shown = 0;
	g_src_done    = 0;
	g_expect_audio = 0;
	g_first_vpts   = -1;
	g_pos_ticks    = g_start_ticks;
	g_live_paused  = 0;
	g_seek_pending = 0;
	clock_reset();
	g_stage    = ST_OPEN;
	g_hold    = STAGE_HOLD;
}

/* Playback controls; see the section below input(). */
static int       g_stopping;       /* Circle seen: draw a frame, then stop */
static long long g_bar_until;      /* show the bar until then, 90 kHz wall */
static int       g_show_debug = 0; /* square toggles the readout
                                       (SELECT already quits the app) */

static void show_bar(void) { g_bar_until = wall90() + 3LL * 90000; }

/* A new film: from its beginning. */
static void enter(void)
{
	g_start_ticks = g_open_start;     /* 0, or a resume point */
	g_open_start  = 0;
	g_stopping    = 0;
	g_recovered   = 0;
	g_any_shown   = 0;
	reset_stream();
	show_bar();
}

/*
 * Which item to stream.
 *
 * Set before the screen opens. Empty means play mass:/test.bin instead,
 * which is how the decoder was proven and remains the way to test it with
 * the network out of the picture.
 */
static char g_item[40];

static int g_audio_index = -1;   /* -1 leaves the choice to Jellyfin */
static int g_sub_index   = -1;   /* -1 for no subtitles */

void scr_video_open_at(const char *item_id, int audio_index, int sub_index,
                       long long start_ticks)
{
	if (strcmp(g_item, item_id ? item_id : "") != 0)
		g_shape_choice = 0;         /* a new film: its own shape */

	g_shape_note_until = 0;

	snprintf(g_item, sizeof(g_item), "%s", item_id ? item_id : "");

	g_audio_index = audio_index;
	g_sub_index   = sub_index;
	g_open_start  = start_ticks > 0 ? start_ticks : 0;
}

/* One startup step at a time, held long enough to read. */
static void advance_startup(void)
{
	if (g_hold > 0) {
		g_hold--;
		return;
	}

	g_hold = STAGE_HOLD;

	switch (g_stage) {
	case ST_OPEN:
		if (g_item[0]) {
			ts_init(&g_ts,  VIDEO_TS_PID);
			ts_init(&g_ats, AUDIO_TS_PID);

			g_sock = net_stream_open(jf_host(), jf_port(),
			                         jf_video_stream_path_at(
			                                 g_item, g_audio_index,
			                                 g_sub_index, g_start_ticks),
			                         NULL);

			if (g_sock < 0) {
				snprintf(g_msg, sizeof(g_msg),
				         "Could not connect (%d)", g_sock);
				g_stage = ST_DONE;
				return;
			}

			/* So the reader notices a stop within VIDEO_RECV_MS. */
			net_stream_set_timeout(g_sock, VIDEO_RECV_MS);

			/*
			 * Tell the decoder the real picture size, which is not
			 * always what was asked for: Jellyfin fits the aspect
			 * and rounds to even, then the encoder pads up to a
			 * macroblock. Without this those few rows are drawn.
			 */
			video_set_crop(jf_video_pic_w, jf_video_pic_h);
			video_expect_size(jf_video_pic_w, jf_video_pic_h);

			video_set_source(read_stream);
			video_set_pts_source(take_pts);

			/*
			 * The new stream is on its way: now end the one it
			 * replaces (a seek or a resume). Jellyfin is starting
			 * the transcode meanwhile, so this costs nothing.
			 */
			jf_video_stop_flush();

			/* One frame at 90 kHz, for pictures with no PTS. */
			g_frame90 = jf_video_fps_milli > 0
			          ? (int)(90000000LL / jf_video_fps_milli)
			          : 3003;
			video_set_frame90(g_frame90);

			/*
			 * Sound starts here, not at the first picture.
			 *
			 * Nothing drains the audio ring until this runs, and
			 * the staged startup takes over a second - long enough
			 * for video's reads to fill 64 KB and overflow it. The
			 * dropped bytes were permanent gaps, heard as skips.
			 */
			/*
			 * The socket's own thread, and the sound - both only
			 * when audio is on.
			 *
			 * With it off, reading inside data_cb is the
			 * arrangement that has played reliably all day, and
			 * nothing else is waiting on the ring.
			 */
			if (VIDEO_AUDIO_ENABLE) {
				g_src_done   = 0;
				g_reader_run = 1;

				if (g_reader < 0) {
					extern void *_gp;
					ee_thread_t th;

					memset(&th, 0, sizeof(th));

					th.func             = reader;
					stack_guard_fill(g_rstack);
					th.stack            = g_rstack;
					th.stack_size       = sizeof(g_rstack);
					th.gp_reg           = &_gp;
					th.initial_priority = AUDIO_READER_PRIO;

					g_reader = CreateThread(&th);
					freeze_register(g_reader, FZ_VREADER);

					if (g_reader >= 0)
						StartThread(g_reader, NULL);
				}

				if (vaudio_start() == 0)
					g_expect_audio = 1;
			}
		} else {
			g_fd = open(VIDEO_PATH, O_RDONLY);

			if (g_fd < 0) {
				snprintf(g_msg, sizeof(g_msg),
				         "Could not open %s", VIDEO_PATH);
				g_stage = ST_DONE;
				return;
			}

			video_set_crop(0, 0);   /* whatever the file says */
			video_expect_size(0, 0);
			video_set_source(read_file);
			video_set_pts_source(NULL);
			video_set_frame90(3003);
			g_frame90 = 3003;
			jf_video_fps_milli = 0;  /* no request: pace by the file */
		}

		/*
		 * Streaming waits for data before the decoder starts, as ORBIT
		 * does. Jellyfin needs a moment to start a transcode, longer
		 * if an old one is still being torn down, and starting the
		 * decoder first put it inside libmpeg with nothing to read -
		 * where the stall detector called it after three seconds.
		 */
		g_stage = g_sock >= 0 ? ST_BUFFER : ST_BEGIN;
		g_buffer_since = wall90();
		return;

	case ST_BUFFER: {
		int have = ring_used(g_vhead, g_vtail, VRING);

		g_hold = 0;                    /* look again next frame */

		if (have >= PREBUFFER_BYTES || (g_src_done && have > 0)) {
			g_stage = ST_BEGIN;
			return;
		}

		/* Burning subtitles in, Jellyfin renders before it sends:
		 * give it a minute rather than twenty seconds. */
		long long limit = (g_sub_index >= 0 ? 60LL : 20LL) * 90000;

		if (g_src_done || wall90() - g_buffer_since > limit) {
			snprintf(g_msg, sizeof(g_msg),
			         g_vin ? "The stream stopped while loading"
			               : "No data from the server");
			stop();
			g_stage = ST_DONE;
		}

		return;
	}

	case ST_BEGIN:
		if (!video_begin(ui_gs())) {
			snprintf(g_msg, sizeof(g_msg), "%s", video_error());
			close(g_fd);
			g_fd    = -1;
			g_stage = ST_DONE;
			return;
		}

		g_stage = ST_FIRST;
		return;

	case ST_FIRST:
		g_playing = 1;
		g_stage   = ST_RUNNING;
		return;

	default:
		return;
	}
}

/*
 * End the network side: let the reader leave recv, close the socket, and
 * tell the server. Used by every way out of playback.
 */
static void close_stream(int report_now)
{
	g_stop_src   = 1;
	g_reader_run = 0;

	/*
	 * Long enough for recv's timeout (VIDEO_RECV_MS), with plenty to spare.
	 *
	 * This was 30 x net_snooze(5), and net_snooze cannot sleep less than
	 * 16 ms: under half a second, against a recv that can take a whole
	 * one. Whenever a stop landed mid-read the socket was closed under
	 * the reader, which never came back from recv - and the next film
	 * started with nobody reading it: "Stalled at 0 frames", in0K.
	 */
	for (int i = 0; i < 150 && g_reader_busy; i++)
		net_snooze(16);

	if (g_sock >= 0) {
		/*
		 * Still inside recv after two and a half seconds: leave the
		 * socket open rather than close it under the thread. A leaked
		 * socket is the lesser fault.
		 */
		if (!g_reader_busy)
			net_stream_close(g_sock);

		g_sock = -1;

		/*
		 * The server's side: the stop report, and the transcode. For
		 * a seek or a pause it waits until the next stream has been
		 * asked for, so it is not in the way of the reload.
		 */
		if (report_now)
			jf_video_stopped(g_item, g_pos_ticks);
		else
			jf_video_stop_later(g_item, g_pos_ticks);
	}
}

static void teardown(int keep);

/*
 * The watchdog's evidence, beside settings.txt, with what this side knew:
 * how much arrived, whether the stream had ended, whether the closing
 * start code went out, and what was left unread.
 */
static void save_stuck_report(void)
{
#if !DEBUG_UI
	video_take_stuck_report();      /* consumed, not written */
	return;
#endif
	const char *r = video_take_stuck_report();

	if (!r[0])
		return;

	char path[300];
	const char *sp = settings_path();
	const char *cut = strrchr(sp, '/');

	if (!cut)
		cut = strrchr(sp, ':');

	int n = cut ? (int)(cut - sp) + 1 : 0;

	snprintf(path, sizeof(path), "%.*sjfstuck.txt", n, sp);

	FILE *f = fopen(path, "a");

	if (!f)
		return;

	fprintf(f, "---\nfilm %s  %s\n%s"
	           "in %lld bytes, unread %d, stream ended %d, closed %d, "
	           "pictures shown %d\n",
	        g_item, jf_detail.name, r, (long long)g_vin,
	        ring_used(g_vhead, g_vtail, VRING), g_src_done, g_vclosed,
	        g_frames);
	fclose(f);
}

/*
 * Stop playback (Circle, end of film, or the safety net): tear everything
 * down, report the stop, save the resume point.
 */
static void stop(void)
{
	teardown(0);
	jf_video_stop_flush();       /* a seek's report, if one is waiting */

	/*
	 * Where this user got to, for the film page's Resume button - only
	 * if something was actually watched, so a failed start does not
	 * wipe a resume point it never reached. The page uses the kept
	 * value at once rather than asking the server again.
	 */
	if (g_any_shown && g_item[0] &&
	    strcmp(g_item, jf_detail.id) == 0) {
		jf_detail.resume_ticks = jf_save_position(g_item, g_pos_ticks,
		                                          jf_detail.runtime_ticks);
		g_any_shown = 0;
	}
}

/*
 * End the stream. keep: a pause or a seek, so the decoder stops but the
 * texture and the last picture in it stay, to sit under the bar while the
 * next stream loads. Otherwise everything goes, VRAM included.
 */
static void teardown(int keep)
{
	freeze_arm(0);                 /* teardown waits on threads, briefly */
	/*
	 * Tell the source to stop, then wait for the decoder, then close.
	 *
	 * The order took several attempts. Closing first strands the worker
	 * inside recv, which closing does not interrupt on this stack. Not
	 * closing at all leaves it blocked there just the same. A flag plus
	 * the socket's receive timeout reaches it either way: within a second
	 * the read surfaces, sees the flag, and reports the end - which takes
	 * libmpeg down its end-of-stream path and lets the session be
	 * destroyed.
	 */
	g_stop_src   = 1;
	g_reader_run = 0;

	/*
	 * Sound first: it holds no IPU state and its thread must be out of
	 * libmad before anything else is torn down.
	 */
	if (VIDEO_AUDIO_ENABLE)
		vaudio_stop();

	if (g_playing) {
		if (keep)
			video_end_keep(ui_gs());
		else
			video_end(ui_gs());

		g_playing = 0;
	} else if (!keep) {
		video_release(ui_gs());      /* a picture kept by a pause */
	}

	if (g_fd >= 0) {
		close(g_fd);
		g_fd = -1;
	}

	/*
	 * Let the reader notice before the socket goes.
	 *
	 * It surfaces from recv within the socket's one-second timeout, and
	 * g_stop_src makes pump return at once. Closing underneath a thread
	 * sitting in recv is what stranded the decoder before.
	 */
	close_stream(!keep);

	g_vhead = 0;
	g_vtail = 0;
	g_ahead = 0;
	g_atail = 0;

	/* After a watchdog rescue: what it found, for jfstuck.txt. */
	save_stuck_report();
}

/*
 * PLAYBACK CONTROLS, ORBIT's way
 *
 * Nothing pauses or seeks inside a stream. A pause or a seek ends this
 * stream - reporting the position to Jellyfin and ending its transcode -
 * and the next one is asked for from the new position with
 * startTimeTicks. The last picture stays up under the bar meanwhile.
 *
 *   the bar       -10 s, Play/Pause, +20 s: left and right move between
 *                 them, X presses the focused one
 *   X / START     with the bar hidden, pause or play at once
 *   square        readout on / off
 *   circle        back to the film's page
 *
 * While paused, left and right move the position without loading
 * anything; X plays from there.
 */
#define SEEK_BACK_TICKS (10LL * 10000000LL)
#define SEEK_FWD_TICKS  (20LL * 10000000LL)

/* How long after the last seek press the reload starts: 90 kHz. */
#define SEEK_SETTLE     (90000LL / 2)

/*
 * A pause longer than this becomes a stop: back to the film's page, with
 * the position saved for Resume. 90 kHz; 30 s.
 */
#define LIVE_PAUSE_MAX  (30LL * 90000)


/*
 * Pause in place: the stream stays open, the sound thread plays silence
 * and the clock stops.
 */
static void live_pause(void)
{
	g_live_paused = 1;
	g_pause_wall  = wall90();

	if (g_expect_audio)
		vaudio_pause(1);
}

/* Resume from a live pause. */
static void live_resume(void)
{
	g_live_paused = 0;

	if (g_expect_audio)
		vaudio_pause(0);

	/* Without sound the wall clock paces: take the pause out of it. */
	g_wall_off -= wall90() - g_pause_wall;
}

/*
 * +20 s is refused when it would land in the last SEEK_END_GUARD of the
 * runtime: the button greys out and the bar stays put. Runtimes can run
 * past the real content, and a reload past the true end gets nothing.
 */
#define SEEK_END_GUARD (30LL * 10000000LL)

/* Whether +20 s is allowed: not within the last 30 s of the runtime. */
static int fwd_allowed(void)
{
	long long len  = jf_detail.runtime_ticks;
	long long base = g_seek_pending ? g_seek_target :
	                 (g_stage == ST_PAUSED ? g_start_ticks : g_pos_ticks);

	return len <= 0 || base + SEEK_FWD_TICKS <= len - SEEK_END_GUARD;
}

/* Never before the start - so -10 s in the first ten seconds restarts the
 * film - and (like ORBIT) not within 10 s of the end. */
static long long clamp_pos(long long t)
{
	long long len = jf_detail.runtime_ticks;

	if (len > 0 && t > len - 10LL * 10000000LL)
		t = len - 10LL * 10000000LL;

	return t < 0 ? 0 : t;
}

/*
 * Start a new stream from g_start_ticks: reset, then the stages from
 * ST_OPEN.
 */
static void begin_stream(void)
{
	reset_stream();
	g_hold = 0;
	show_bar();
}

/* End this stream and either pause or go on from pos. */
static void restart_at(long long pos, int pause)
{
	teardown(1);

	g_start_ticks = clamp_pos(pos);
	g_pos_ticks   = g_start_ticks;
	show_bar();

	if (pause)
		g_stage = ST_PAUSED;
	else
		begin_stream();
}

/*
 * Per frame: arm the freeze net while running, handle stop, the outro, the
 * start-up stages, picture presentation, and the buttons (pause, -10 s,
 * +20 s, the bar).
 */
static void input(u32 p)
{
	/* The freeze net watches playback proper only (freeze.h). */
	freeze_arm(g_stage == ST_RUNNING && !g_stopping);

	count_ui_frame();                 /* diagnostics: screen frame rate */

	/*
	 * Stopping takes a moment - the decoder, the reader and two short
	 * requests to the server - and the screen used to sit on the last
	 * frame meanwhile with no sign the button had been seen. So Circle
	 * only marks it: this frame draws "Stopping...", and the next does
	 * the work while that frame stays up.
	 */
	if (g_stopping) {
		g_stopping = 0;
		stop();
		app_goto(SCR_DETAIL);
		return;
	}

	/*
	 * The bar comes up with any button and hides three seconds after
	 * the last one. While it is hidden a press only brings it up, with
	 * Play/Pause focused - except X, which pauses or plays at once, so
	 * the most common thing never needs two presses.
	 */
	int was_visible = bar_visible();

	if (p)
		show_bar();

	if (p && !was_visible)
		g_vbtn = VB_PLAY;

	if (p & PAD_CIRCLE) {
		g_stopping = 1;
		return;
	}

	/*
	 * Triangle: the picture's shape, 4:3 or 16:9 - offered only when it
	 * is a guess (a DVD-sized file that lost its note; see aspect_sure).
	 * A known shape is never changed.
	 */
	if ((p & PAD_TRIANGLE) && g_item[0] && !jf_detail.aspect_sure) {
		int now = g_shape_choice ? g_shape_choice : jf_detail.aspect_milli;

		g_shape_choice      = (now == 1778) ? 1333 : 1778;
		g_shape_note_until  = wall90() + 2LL * 90000;
		return;
	}

#if DEBUG_UI
	/* The readout, for development builds only. */
	if (p & PAD_SQUARE)
		g_show_debug ^= 1;
#endif

	/* Left and right move between the three buttons, as in the music
	 * player's transport. */
	if (was_visible) {
		if ((p & PAD_LEFT)  && g_vbtn > 0)        g_vbtn--;
		if ((p & PAD_RIGHT) && g_vbtn < VB_N - 1) g_vbtn++;
	}

	/* Which button this press works, if any. */
	int act = -1;

	if (p & PAD_CROSS)
		act = was_visible ? g_vbtn : VB_PLAY;

	if (p & PAD_START)
		act = VB_PLAY;

	/* Paused with the stream handed back: as before. */
	if (g_stage == ST_PAUSED) {
		if (act == VB_PLAY) {
			begin_stream();
		} else if (act == VB_BACK) {
			g_start_ticks = clamp_pos(g_start_ticks - SEEK_BACK_TICKS);
			g_pos_ticks   = g_start_ticks;
		} else if (act == VB_FWD && fwd_allowed()) {
			g_start_ticks = clamp_pos(g_start_ticks + SEEK_FWD_TICKS);
			g_pos_ticks   = g_start_ticks;
		}

		return;
	}

	int live    = g_stage == ST_RUNNING && g_playing && g_first_shown;
	int loading = g_stage == ST_OPEN || g_stage == ST_BUFFER ||
	              g_stage == ST_BEGIN || g_stage == ST_FIRST;

	/*
	 * Pause in place. The stream stays open, the rings stay full, the
	 * decoder holds its next picture and the sound plays silence that
	 * does not move the clock - so pausing is immediate and playing
	 * again carries on from the very next frame and sample.
	 */
	if (live && act == VB_PLAY) {
		if (g_live_paused)
			live_resume();
		else
			live_pause();

		return;
	}

	/*
	 * Seeks gather. Each press of -10 or +20 moves the target on the bar
	 * at once, and the reload starts only when the presses stop, so
	 * three presses are one reload of +60 s rather than three of +20 s.
	 * Playback carries on meanwhile.
	 */
	{
		int d = (act == VB_BACK) ? -1 : (act == VB_FWD) ? 1 : 0;

		if (d > 0 && !fwd_allowed())
			d = 0;                     /* too near the end: refused */

		if (d && (live || loading)) {
			long long base = g_seek_pending ? g_seek_target : g_pos_ticks;

			g_seek_target  = clamp_pos(base + (d < 0 ? -SEEK_BACK_TICKS
			                                         : SEEK_FWD_TICKS));
			g_seek_pending = 1;
			g_seek_at      = wall90() + SEEK_SETTLE;
		}
	}

	if (g_seek_pending && wall90() >= g_seek_at) {
		int was_paused = g_live_paused;

		g_seek_pending = 0;
		g_live_paused  = 0;

		/* Seeking while paused stays paused, at the new place. */
		restart_at(g_seek_target, was_paused);
		return;
	}

	if (g_live_paused) {
		/*
		 * A long pause is a stop. Holding the stream open longer
		 * risks the server closing a connection nobody reads, and
		 * keeps a transcode running on the Mac for nothing. So after
		 * LIVE_PAUSE_MAX it stops as Circle does - position saved -
		 * and goes back to the film's page, where Resume (or
		 * Triangle) carries on from here.
		 */
		if (wall90() - g_pause_wall > LIVE_PAUSE_MAX) {
			g_live_paused = 0;
			g_stopping    = 1;     /* "Stopping..." for a frame, then out */
			show_bar();
		}

		return;                        /* nothing to pace while paused */
	}

	if (g_stage != ST_RUNNING) {
		advance_startup();
		return;
	}

	if (!g_playing)
		return;

	/*
	 * Pace with an accumulator, not a whole number of draws.
	 *
	 * Only the asking is paced. video_step is called every frame
	 * regardless, because it also collects whatever the decoder has
	 * finished - pacing that too meant a finished picture waited another
	 * draw or two to be picked up, halving the frame rate.
	 *
	 * The old arithmetic was (ms * 60) / 1000 - 1, which is a count of
	 * draws to skip - and integer division rounds it badly wrong. A 33 ms
	 * frame gave 0, meaning a picture every draw: twice realtime. A 42 ms
	 * frame gave 1, or 33 ms, a quarter too fast.
	 *
	 * Playing fast is not only a picture problem. It drags the whole
	 * stream through faster than realtime, and the audio ring - which
	 * cannot be drained faster than the sound card plays it - overflows.
	 * That was 222 KB of audio thrown away, heard as skips.
	 *
	 * So: add a draw's worth of time each frame, and ask for a picture
	 * whenever a frame's worth has built up. The remainder carries, which
	 * is what keeps 23.976 and 29.97 honest over time.
	 */
	int due = 0;

	{
		/*
		 * The frame time we asked the server for, not the one the
		 * decoder reports.
		 *
		 * jf_video_fps_milli is the rate put in the transcode request,
		 * fitted to the decoder (jf_video_rate), so it is exactly what
		 * is arriving. m_MSPerFrame from the sequence header has to be
		 * right too, but it comes back as 0 for some streams - and the
		 * clamp below then turns that into 16 ms, a picture every
		 * draw, which is double speed for 30 fps material and drags
		 * the whole stream through twice as fast as it should.
		 */
		int ms = 0;

		if (jf_video_fps_milli > 0)
			ms = 1000000 / jf_video_fps_milli;

		if (ms < 16)
			ms = video_ms_per_frame();

		if (ms < 16)
			ms = 33;               /* neither known: assume 30 fps */

		ms /= VIDEO_SPEEDUP;

		if (ms < 16)
			ms = 16;

		g_ms_shown = ms;

		g_due += 1000;                 /* thousandths of a millisecond */

		if (g_due >= ms * 60) {        /* 1000/60 ms per draw, scaled */
			g_due -= ms * 60;
			due    = 1;

			/* After a stall, do not try to catch up in a burst. */
			if (g_due > ms * 60)
				g_due = 0;
		}
	}

	/*
	 * A picture that never arrives.
	 *
	 * The decoder is on its own thread, so this shows up as a message
	 * rather than a dead console.
	 */
	if (video_stalled()) {
		if (g_sock >= 0 && g_vin == 0)
			snprintf(g_msg, sizeof(g_msg),
			         "No data from the server (stalled at 0 frames)");
		else
			snprintf(g_msg, sizeof(g_msg), "Stalled at %d frames", g_frames);

		/* Where everything was, kept on screen with the message. */
		snprintf(g_stall_dbg, sizeof(g_stall_dbg), "%s", pipe_debug());
		snprintf(g_stall_adbg, sizeof(g_stall_adbg), "%s", vaudio_debug());
		g_playing = 0;
		g_stage   = ST_DONE;

		video_abandon(ui_gs());

		/* The sound, which may still be waiting on its first picture. */
		if (VIDEO_AUDIO_ENABLE)
			vaudio_stop();

		/*
		 * Close the socket too, not just the file.
		 *
		 * A leaked connection is not merely untidy here. The next
		 * playback gets the same descriptor number back with the old
		 * stream's data still queued in it, and plays that instead -
		 * one film's id and title on screen with another film's
		 * picture underneath. It looked like the server sending the
		 * wrong thing; the server was blameless.
		 */
		if (g_fd >= 0) { close(g_fd); g_fd = -1; }

		/* Through close_stream, which waits for the reader first. */
		close_stream(1);

		return;
	}

	/*
	 * Two ways to pace.
	 *
	 * With sound, the sound is the clock: each picture is shown when the
	 * audio clock reaches its timestamp, or dropped when it is late (see
	 * sync_step). The old arrangement - a free-running count of draws, at
	 * an assumed 60.000 Hz, against sound paced by the SPU2's own crystal -
	 * let the two drift apart, and the rings absorbed the difference as
	 * dropped or padded sound.
	 *
	 * Without sound, the count of draws is all there is, and it is the
	 * arrangement that has played reliably: left exactly as it was.
	 */
	/* Ending softly: no more pictures, just the sound and the fade. */
	if (g_outro) {
		long long now = wall90();

		if (g_outro == 1) {
			/* The sound has run dry once it is padding with silence. */
			int done = !g_expect_audio ||
			           vaudio_underruns() >= g_outro_sil + 2 ||
			           now - g_outro_t > OUTRO_SOUND_MAX_MS * 90LL;

			if (done) {
				g_outro   = 2;
				g_outro_t = now;
			}
			return;
		}

		if (now - g_outro_t >= OUTRO_FADE_MS * 90LL) {
			stop();

			/*
			 * Really the end - sound finished, picture faded. An
			 * episode with one after it counts down to that one;
			 * anything else goes back to its page.
			 */
			if (jf_find_next())
				app_goto(SCR_NEXT);
			else
				app_goto(SCR_DETAIL);
		}
		return;
	}

	int r;

	if (VIDEO_AUDIO_ENABLE && g_sock >= 0)
		r = sync_step();
	else
		r = video_step(due);

	if (r == 3) {
		g_vlate++;
		return;                  /* dropped; the last frame stays up */
	}

	if (r == 2)
		return;                  /* still decoding; last frame stays up */

	if (r == 0) {
		/*
		 * Back to the film's page by itself.
		 *
		 * There is nothing to read on a "finished" message and nothing
		 * to decide, so making someone press a button to dismiss it is
		 * a step for its own sake.
		 */
		/*
		 * Ended well short of the runtime: the server dropped the
		 * stream rather than finishing it. Pick up from here, a few
		 * times at most, rather than leave the film half watched.
		 */
		long long len = jf_detail.runtime_ticks;

		if (g_sock >= 0 && len > 0 && !g_stop_src && g_recovered < 3 &&
		    g_pos_ticks + 30LL * 10000000LL < len) {
			g_recovered++;
			restart_at(g_pos_ticks, 0);
			return;
		}

		/* Ended between pictures on purpose: let the sound finish. */
		if (g_held_end && !g_stop_src) {
			g_outro     = 1;
			g_outro_t   = wall90();
			g_outro_sil = vaudio_underruns();
			return;
		}

		stop();
		app_goto(SCR_DETAIL);
		return;
	} else if (r < 0) {
		snprintf(g_msg, sizeof(g_msg), "%s", video_error());
		stop();
		g_stage = ST_DONE;            /* so the message is shown */
	} else {
		g_frames++;
	}
}

/*
 * What the bar should say, or NULL for nothing. Something to say keeps
 * the bar up; otherwise it shows for three seconds after a button.
 */
static const char *bar_status(void)
{
	static char buf[32];

	int loading = g_stage == ST_OPEN || g_stage == ST_BUFFER ||
	              g_stage == ST_BEGIN || g_stage == ST_FIRST;

	if (g_stopping)
		return "Stopping...";

	if (g_seek_pending)
		return "Seeking...";

	if (g_stage == ST_PAUSED || g_live_paused)
		return "Paused";

	if (loading) {
		if (g_stage == ST_BUFFER && g_sub_index >= 0 && g_vin == 0)
			return "Preparing subtitles...";

		if (g_stage == ST_BUFFER) {
			snprintf(buf, sizeof(buf), "Loading %d KB",
			         ring_used(g_vhead, g_vtail, VRING) >> 10);
			return buf;
		}

		return "Loading...";
	}

	return NULL;
}

/*
 * Whether the control bar is showing (always while loading or paused, else
 * for 3 s after a press).
 */
static int bar_visible(void)
{
	return bar_status() != NULL || wall90() < g_bar_until;
}

/*
 * Fit the picture, rather than filling the screen.
 *
 * Work in display aspect, not in pixels: the framebuffer is 640x448, which
 * the monitor shows as 4:3, and the transcode is square-pixel, so the
 * picture's aspect is simply its width over its height. Wider than 4:3 is
 * letterboxed, narrower pillarboxed.
 */
static void draw_picture(void)
{
	int vw = video_width();
	int vh = video_height();

	if (vw <= 0 || vh <= 0) {
		video_draw(ui_gs(), SCR_LEFT, SCR_TOP, SCR_FULL_W, SCR_FB_H);
		return;
	}

	/* The screen's shape, from the display profile (4:3 today). */
	const float screen = disp->screen_shape_milli / 1000.0f;

	/*
	 * The intended shape, not the stored size: a DVD's 720x480 is meant
	 * to be seen as 4:3 or 16:9 (see aspect_milli in jf.h). The GS
	 * stretches the picture to whatever box it is drawn into, so this is
	 * all it takes. The stored size remains the fallback, for the test
	 * file and anything without a shape.
	 */
	int   shape = g_shape_choice ? g_shape_choice : jf_detail.aspect_milli;
	float pic   = (g_item[0] && shape > 0) ? shape / 1000.0f : (float)vw / vh;
	/*
	 * The whole 640x480 framebuffer, which is exactly 4:3 (see config.h),
	 * from SCR_TOP: the picture uses all 480 lines, not the menus' 448.
	 */
	/* The whole screen, not the menus' area: in 1080i that is the full
	 * 16:9 width. */
	float fw = SCR_FULL_W;
	float w  = fw;
	float h  = SCR_FB_H;

	if (pic > screen)
		h = SCR_FB_H * screen / pic;
	else if (pic < screen)
		w = fw * pic / screen;

	ui_dither(1);                  /* pictures are dithered (16-bit only) */
	video_draw(ui_gs(), SCR_LEFT + (fw - w) * 0.5f,
	           SCR_TOP + (SCR_FB_H - h) * 0.5f, w, h);
	ui_dither(0);
}

/* The diagnostic readout, top left. Square hides it. */
static void draw_readout(void)
{
	char d[80];

	/*
	 * f pictures shown, late pictures dropped, a audio frames decoded,
	 * sil silence chunks padded in, drop bytes refused by a full ring
	 * (should stay 0), av how late the last picture was in ms.
	 */
	snprintf(d, sizeof(d), "%dx%d f%d late%d a%d sil%d drop%d av%+d",
	         video_width(), video_height(), g_frames, g_vlate,
	         vaudio_frames(), vaudio_underruns(),
	         g_adropped + g_vdropped, g_drift);
	ui_text(8, 8, 1.0f, C_WHITE, d);

	/*
	 * Timing: a0-v0 is how far the sound's first PTS is from the first
	 * picture's (ms, positive: sound starts later); skip is the samples
	 * the sound skipped (negative: silence added); clk0 is the clock
	 * minus the picture's PTS when the clock first ran; late last/max
	 * are how far behind dropped pictures were (ms); gaps are the last
	 * PTS spacings (ms).
	 */
	{
		long long a0 = scr_video_audio_pts0();
		long long v0 = g_first_vpts;

		snprintf(d, sizeof(d),
		         "a0-v0 %d skip %d clk0 %d late %d/%d gap %d %d %d %d ui %d",
		         (a0 >= 0 && v0 >= 0) ? (int)((a0 - v0) / 90) : 0,
		         (int)vaudio_skip(),
		         g_dbg_clk0 >= 0 ? (int)((g_dbg_clk0 - 1000000) / 90) : 0,
		         g_dbg_late_last, g_dbg_late_max,
		         g_dbg_gap[0], g_dbg_gap[1], g_dbg_gap[2], g_dbg_gap[3],
		         g_dbg_ui_fps);
		ui_text(8, 162, 1.0f, C_WHITE, d);
	}

	/* Stack guards of the three playback threads: 1 intact, 0 overrun. */
	snprintf(d, sizeof(d), "stack decoder %d sound %d reader %d",
	         video_stack_ok(), vaudio_stack_ok(), scr_video_stack_ok());
	ui_text(8, 140, 1.0f, C_WHITE, d);

	/* What was asked for, and under which session: a wrong film shows
	 * here as a title that does not match the picture. */
	{
		char who[72];

		snprintf(who, sizeof(who), "id %.12s  %.20s  %s",
		         g_item, jf_detail.name, jf_play_session());
		ui_text(8, 30, 1.0f, C_DIM, who);
	}

	ui_text(8, 52, 1.0f, C_WHITE, pipe_debug());
	ui_text(8, 74, 1.0f, C_WHITE, vaudio_debug());
	ui_text(8, 96, 1.0f, C_WHITE, video_debug());
	ui_text(8, 118, 1.0f, C_WHITE, end_debug());
}

/* 100 ns ticks as h:mm:ss, or m:ss under an hour. */
static void fmt_time(char *out, int n, long long ticks)
{
	int t = (int)(ticks / 10000000LL);

	if (t < 0)
		t = 0;

	if (t >= 3600)
		snprintf(out, n, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
	else
		snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

/*
 * A circular arrow around a sign, for the -10 s and +20 s buttons: the
 * "replay" and "skip" icons of most players. dir -1 runs counter-
 * clockwise with a minus inside, +1 clockwise with a plus.
 *
 * The ring is dots along a circle, open at the top where the arrowhead
 * sits; the points are a fixed table, 15 degrees apart, so nothing is
 * worked out per frame.
 */
static void draw_seek_glyph(float cx, float cy, int dir, u64 col)
{
	/* cos and sin of k x 15 degrees, k = 0..23; y grows downwards. */
	static const float co[24] = {
		 1.000f,  0.966f,  0.866f,  0.707f,  0.500f,  0.259f,
		 0.000f, -0.259f, -0.500f, -0.707f, -0.866f, -0.966f,
		-1.000f, -0.966f, -0.866f, -0.707f, -0.500f, -0.259f,
		 0.000f,  0.259f,  0.500f,  0.707f,  0.866f,  0.966f };
	static const float si[24] = {
		 0.000f,  0.259f,  0.500f,  0.707f,  0.866f,  0.966f,
		 1.000f,  0.966f,  0.866f,  0.707f,  0.500f,  0.259f,
		 0.000f, -0.259f, -0.500f, -0.707f, -0.866f, -0.966f,
		-1.000f, -0.966f, -0.866f, -0.707f, -0.500f, -0.259f };

	const float r = 12.0f;

	/*
	 * k = 18 is straight up. The ring starts there, under the arrowhead,
	 * and runs right round to leave a gap on the side the arrow points
	 * to: left for counter-clockwise (-1), right for clockwise (+1).
	 */
	for (int k = 0; k < 24; k++) {
		int gap = dir < 0 ? (k >= 14 && k <= 17) : (k >= 19 && k <= 22);

		if (gap)
			continue;

		ui_box(cx + r * co[k] - 1.5f, cy + r * si[k] - 1.5f, 3, 3, col);
	}

	/*
	 * The arrowhead at the open end, pointing along the way round: left
	 * for counter-clockwise, right for clockwise. Rows of a small
	 * triangle, as the music player draws its glyphs.
	 */
	{
		float hy = cy - r;

		for (int i = 0; i < 11; i++) {
			float half = (i < 6) ? i : 10 - i;
			float w    = 1 + half * 1.3f;

			if (dir < 0)
				ui_box(cx + 1 - w, hy - 5 + i, w, 1, col);
			else
				ui_box(cx - 1, hy - 5 + i, w, 1, col);
		}
	}

	/* The sign inside. */
	ui_box(cx - 5, cy - 1, 10, 2, col);

	if (dir > 0)
		ui_box(cx - 1, cy - 5, 2, 10, col);
}

/* One button, in the music player's style: rounded, a purple ring when
 * focused. */
#define VBW 52.0f
#define VBH 42.0f
#define VBGAP 14.0f

/* One of the three playback buttons (-10 s, play/pause, +20 s). */
static void draw_vbutton(float x, float y, int kind, int focused)
{
	if (focused)
		ui_round(x - 3, y - 3, VBW + 6, VBH + 6, 7, C_PURPLE);

	ui_round(x, y, VBW, VBH, 5, C_ROWSEL);

	float cx = x + VBW * 0.5f;
	float cy = y + VBH * 0.5f;

	switch (kind) {
	case VB_BACK:
		draw_seek_glyph(cx, cy, -1, C_WHITE);
		break;

	case VB_FWD:
		/* Greyed when a press would be refused. */
		draw_seek_glyph(cx, cy, 1, fwd_allowed() ? C_WHITE : C_RULE);
		break;

	default:
		if (g_stage == ST_PAUSED || g_live_paused) {
			/* Play: a triangle in rows. */
			for (int i = 0; i < 20; i++) {
				float half = (i < 10) ? i : 19 - i;
				ui_box(cx - 7, cy - 10 + i, 4 + half * 1.6f, 1, C_WHITE);
			}
		} else {
			/* Pause: two bars. */
			ui_box(cx - 8, cy - 10, 6, 20, C_WHITE);
			ui_box(cx + 2, cy - 10, 6, 20, C_WHITE);
		}
		break;
	}
}

/*
 * The bar, in the music player's design: title centred in white, status
 * in dim under it, the three buttons (-10 s, Play/Pause, +20 s), then the
 * same 5 px track - C_RULE behind, C_PURPLE for what has played, x 110
 * across SCR_W - 220 - with the times under its two ends, and the hint
 * row at the bottom. A translucent panel keeps it readable over the
 * picture.
 */
static void draw_bar(const char *status)
{
	const float top = SCR_H - 182;
	const float bx  = 110.0f, bw = SCR_W - 220.0f;
	const float by  = top + 106;

	ui_box(SCR_LEFT, top, SCR_FULL_W, SCR_BOTTOM - top,
	       GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x60, 0x00));

	{
		char t[48];

		snprintf(t, sizeof(t), "%.40s", jf_detail.name);
		ui_text_c(SCR_W / 2, top + 6, 1.0f, C_WHITE, t);
	}

	if (status && status[0])
		ui_text_c(SCR_W / 2, top + 26, 1.0f, C_DIM, status);

	/* The buttons, centred. */
	{
		float total = VB_N * VBW + (VB_N - 1) * VBGAP;
		float x     = (SCR_W - total) * 0.5f;

		for (int i = 0; i < VB_N; i++) {
			draw_vbutton(x, top + 50, i, i == g_vbtn);
			x += VBW + VBGAP;
		}
	}

	{
		long long len = jf_detail.runtime_ticks;
		long long pos = g_seek_pending ? g_seek_target : g_pos_ticks;
		char      left[16], right[16];

		if (len > 0 && pos > len)
			pos = len;

		ui_box(bx, by, bw, 5, C_RULE);

		if (len > 0 && pos > 0)
			ui_box(bx, by, bw * (float)((double)pos / (double)len), 5,
			       C_PURPLE);

		fmt_time(left,  sizeof(left),  pos);
		fmt_time(right, sizeof(right), len);

		ui_text(bx, by + 12, 1.0f, C_DIM, left);

		if (len > 0)
			ui_text(bx + bw - ui_text_w(1.0f, right), by + 12, 1.0f,
			        C_DIM, right);
	}

	/* Triangle appears only when the picture's shape is a guess. */
	if (jf_detail.aspect_sure || !g_item[0]) {
		static const BtnGlyph g[] = { BTN_CROSS, BTN_DPAD, BTN_CIRCLE };
		static const char *l[] = { "Select", "Move", "Back" };

		ui_hint_row(g, l, 3);
	} else {
		static const BtnGlyph g[] = { BTN_CROSS, BTN_DPAD, BTN_TRIANGLE,
		                              BTN_CIRCLE };
		static const char *l[] = { "Select", "Move", "Shape", "Back" };

		ui_hint_row(g, l, 4);
	}
}

/*
 * Text subtitles, at the bottom of the screen - above the bar while it is
 * up. Each line is wrapped to fit, and drawn white over a black outline so
 * it reads on any picture.
 */
#define SUB_MAX_LINES 6

/*
 * The current subtitle line, wrapped, white on a black outline, above the
 * bar when it shows.
 */
static void draw_subs(void)
{
	const char *t = subs_at((int)(g_pos_ticks / 10000));

	if (!t)
		return;

	const float maxw = SCR_W - 80.0f;
	static char lines[SUB_MAX_LINES][160];
	int n = 0;

	/* Split at the line breaks, then wrap any line too wide at spaces. */
	while (*t && n < SUB_MAX_LINES) {
		const char *e = strchr(t, '\n');
		int len = e ? (int)(e - t) : (int)strlen(t);

		if (len > 159)
			len = 159;

		char src[160];
		memcpy(src, t, len);
		src[len] = '\0';

		char *p = src;

		while (*p && n < SUB_MAX_LINES) {
			int fit = (int)strlen(p);

			if (ui_text_w(1.0f, p) > maxw) {
				/* The longest run of whole words that fits. */
				int best = 0;

				for (int k = 1; p[k]; k++) {
					if (p[k] != ' ')
						continue;

					char c = p[k];
					p[k] = '\0';
					int ok = ui_text_w(1.0f, p) <= maxw;
					p[k] = c;

					if (!ok)
						break;
					best = k;
				}

				if (best > 0)
					fit = best;      /* else one long word: let it run */
			}

			memcpy(lines[n], p, fit);
			lines[n][fit] = '\0';
			n++;

			p += fit;
			while (*p == ' ')
				p++;
		}

		if (!e)
			break;
		t = e + 1;
	}

	const float lh     = 22.0f;
	const float bottom = bar_visible() ? (SCR_H - 182) - 6.0f
	                                   : SCR_H - 28.0f;
	const u64   black  = GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0x00);

	for (int i = 0; i < n; i++) {
		float y  = bottom - (n - i) * lh;
		float cx = SCR_W / 2;

		ui_text_c(cx - 2, y, 1.0f, black, lines[i]);
		ui_text_c(cx + 2, y, 1.0f, black, lines[i]);
		ui_text_c(cx, y - 2, 1.0f, black, lines[i]);
		ui_text_c(cx, y + 2, 1.0f, black, lines[i]);
		ui_text_c(cx, y,     1.0f, C_WHITE, lines[i]);
	}
}

/*
 * The playback screen: the picture, subtitles, the fade at the end, the
 * bar, and the readout (DEBUG_UI).
 */
static void draw(void)
{
	int pic     = video_has_picture();
	int loading = g_stage == ST_OPEN || g_stage == ST_BUFFER ||
	              g_stage == ST_BEGIN || g_stage == ST_FIRST;

	/*
	 * Black behind the picture, not the animated background: letterbox
	 * bars showed the waves through them, which looks like a fault.
	 */
	if (pic || g_playing)
		ui_box(SCR_LEFT, SCR_TOP, SCR_FULL_W, SCR_FB_H, GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0));
	else
		ui_waves();

	if (pic) {
		draw_picture();

		/*
		 * The shape just chosen with Triangle, for two seconds - only
		 * ever for a file whose shape is a guess. Worded by what it does
		 * on screen, since "4:3" alone reads wrong on a film letterboxed
		 * inside a 4:3 picture.
		 */
		if (!jf_detail.aspect_sure && wall90() < g_shape_note_until) {
			const char *t = g_shape_choice == 1778
			                ? "Picture: 16:9 (widescreen)"
			                : "Picture: 4:3 (full screen)";
			float tw = ui_text_w(1.0f, t);

			ui_round((SCR_W - tw) / 2 - 10, 18, tw + 20, 28, 5,
			         GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x60, 0x00));
			ui_text_c(SCR_W / 2, 22, 1.0f, C_WHITE, t);
		}
	}

	if (pic && subs_active_for(g_item) && g_stage != ST_DONE && !g_stopping &&
	    !g_outro)
		draw_subs();

	/* The fade at the end: black, from clear to solid. */
	if (g_outro == 2) {
		long long t = wall90() - g_outro_t;
		int a = (int)(t * 0x80 / (OUTRO_FADE_MS * 90LL));

		if (a > 0x80) a = 0x80;
		if (a < 0)    a = 0;

		ui_box(SCR_LEFT, SCR_TOP, SCR_FULL_W, SCR_FB_H, GS_SETREG_RGBAQ(0, 0, 0, a, 0));
	}

	if (DEBUG_UI && g_playing && g_show_debug)
		draw_readout();

	/* The first start of a film: nothing to show yet but the step. */
	if (!pic && loading && !g_stopping) {
		ui_text_c(SCR_W / 2, 200, 1.0f, C_WHITE, stage_name(g_stage));

		if (g_stage == ST_BUFFER) {
			char b[48];

			snprintf(b, sizeof(b), "%d KB",
			         ring_used(g_vhead, g_vtail, VRING) >> 10);
			ui_text_c(SCR_W / 2, 226, 1.0f, C_DIM, b);
		}

		static const BtnGlyph g[] = { BTN_CIRCLE };
		static const char *l[] = { "Back" };
		ui_hint_row(g, l, 1);
		return;
	}

	/* Finished or failed. */
	if (g_stage == ST_DONE && !g_stopping) {
		ui_text_c(SCR_W / 2, 200, 1.0f, C_WHITE,
		          g_msg[0] ? g_msg : "No video");

		if (video_info()[0])
			ui_text_c(SCR_W / 2, 226, 1.0f, C_DIM, video_info());

		if (DEBUG_UI && g_stall_dbg[0])
			ui_text_c(SCR_W / 2, 252, 1.0f, C_WHITE, g_stall_dbg);

		if (DEBUG_UI && g_stall_adbg[0])
			ui_text_c(SCR_W / 2, 274, 1.0f, C_WHITE, g_stall_adbg);

		static const BtnGlyph g[] = { BTN_CIRCLE };
		static const char *l[] = { "Back" };
		ui_hint_row(g, l, 1);
		return;
	}

	/* Playing, paused, reloading after a seek, or stopping. */
	if (bar_visible())
		draw_bar(bar_status());
}

const Screen scr_video = { enter, input, draw };
/*
 * The freeze safety net set a playback thread aside: stop as Circle does,
 * position saved, so the film can simply be played again.
 */
void scr_video_abort(char code)
{
	if (code == 'D')
		video_mark_stuck();

	stop();
}
