/*
 * ============================================================
 *  The segment bug is fixed in the build image, not here.
 *
 *  This stack lost or altered bytes in maximum-size Ethernet
 *  frames. Proven with a packet capture: the server sent every
 *  response complete, the console acknowledged all of it, and
 *  the application still received a hole.
 *
 *  The console invited those frames itself, advertising
 *  "mss 1460" in its SYN. Every runtime route to change that is
 *  closed, each one tested rather than assumed:
 *
 *    netif MTU at runtime  a capture still showed mss 1460
 *    TCP_MAXSEG            the constant does not exist here
 *    SO_RCVBUF             rejected on hardware
 *    custom lwipopts.h     our build never compiles lwIP
 *
 *  Dockerfile.mss rebuilds ps2sdk with TCP_MSS at 1260, and
 *  pins PBUF_POOL_BUFSIZE at 1536 so the receive buffers stop
 *  following the MSS down and can still hold a whole 1514-byte
 *  frame. Without that pin nothing is received at all: ARP
 *  resolves forever and TCP never starts.
 *
 *  Confirmed on hardware. The SYN now says mss 1260 and the
 *  largest segment the server sends is around 900 bytes, at a
 *  normal 1500 MTU.
 *
 *  So the settings below are no longer defending against
 *  anything and are tuned for a healthy link. Building with the
 *  stock ps2dev image brings the bug back, and the values noted
 *  against each one are what it needs then.
 * ============================================================
 */

#ifndef CONFIG_H
#define CONFIG_H

/* Set by the Makefile (VERSION); this is only the fallback. */
#ifndef JFPS2_VERSION
#define JFPS2_VERSION "dev"
#endif

/* The development build (Makefile BUILD), "" for a release. */
#ifndef JFPS2_BUILD
#define JFPS2_BUILD ""
#endif

/*
 * Diagnostics for development. 0 for anything handed to users: no readout
 * over the video (Square does nothing), no technical lines on error
 * screens, and no jfdebug.txt or jfstuck.txt written to their drive. Set
 * to 1 when building from source to investigate a problem.
 */
#define DEBUG_UI 0

/*
 * Test 16-bit framebuffers in 480i and 480p (1080i needs them; there is no
 * other way to try them on a 4:3 display). 1: those modes use 16-bit
 * colour with dithering, and L2 on the connect screen shows colour ramps
 * to judge the banding. Always 0 for users.
 */
#define TEST_16BIT_FB 0

/*
 * Built for real hardware, launched from a USB drive with uLaunchELF.
 *
 * Nothing in here needs editing for normal use: the network and server
 * come from settings.txt beside the ELF (see settings.h), and so do the
 * sign-ins made on the user page; a server typed on the connect screen is
 * saved back to it too.
 */

/* ---- video: 480i, what most PS2 games use ---- */
/*
 * The screen.
 *
 * The framebuffer is 640x480: the full NTSC picture. 640x480 with the
 * GS's line spacing is exactly 4:3 with square pixels, so a film drawn
 * over it has its true shape. (With the old 448 lines, the area drawn on
 * was 640:448, about 7% wider than 4:3, and films came out that much too
 * wide.)
 *
 * The menus are still laid out on 640x448, SCR_W x SCR_H, centred in the
 * 480 lines: a CRT hides a few lines at the top and bottom (overscan), so
 * text and buttons stay where every TV shows them. The drawing functions
 * add SCR_Y0 to every y. What should reach the edges - the video, the
 * background, full-screen overlays, the waves - spans SCR_TOP to
 * SCR_BOTTOM in the same coordinates, which is the whole 480 lines.
 */
/*
 * Since v50 these come from the current display profile (display.h), so
 * they are values read at run time, not constants: they cannot size a
 * static array or initialise a file-scope variable.
 */
#include "display.h"
#include "layout.h"

/*
 * Layout space to canvas, for the drawing functions (see layout.h). The
 * standard layout is the identity, so these change nothing in 480i/480p.
 */
#define LAY_X(x) (lay->ox + (x) * lay->sx)
#define LAY_Y(y) (lay->oy + (y) * lay->sy)
#define LAY_W(w) ((w) * lay->sx)
#define LAY_H(h) ((h) * lay->sy)

/*
 * Since v53 SCR_W, SCR_H, SCR_FB_H, SCR_TOP and SCR_BOTTOM are in LAYOUT
 * units (layout.h): the screens are designed on 640x448 whatever the
 * canvas, and the drawing functions scale. In the standard layout one
 * layout unit is one canvas pixel, so nothing changes there. SCR_Y0 alone
 * is in canvas pixels: the offset the drawing functions add last.
 */
#define SCR_W      (lay->w)                          /* layout width: 640, or 853 wide */
#define SCR_H      448.0f                            /* layout height */
#define SCR_FB_H   (disp->fb_h / lay->sy)            /* framebuffer, layout units */
#define SCR_Y0     (disp->safe_y0)                   /* canvas pixels */
#define SCR_TOP    (-SCR_Y0 / lay->sy)               /* first line, layout units */
#define SCR_BOTTOM (SCR_H + SCR_Y0 / lay->sy)        /* one past the last */

/*
 * The whole framebuffer's width in layout units, and where it starts.
 * Every current layout covers the framebuffer exactly (0 and SCR_W), but
 * full-screen elements - backgrounds, waves, overlays, the video - use
 * these, so a layout that does not reach the edges still fills them.
 */
#define SCR_LEFT   (-lay->ox / lay->sx)
#define SCR_FULL_W (disp->fb_w / lay->sx)

/* ---- network ---- */

/*
 * How the PS2 gets its address comes from settings.txt (network=auto, dhcp
 * or static, with ip=, mask=, gateway=). Nothing personal is compiled in:
 * with no file, or no ip= line, the PS2 asks the router and says so if
 * none answers, rather than guessing an address for someone else's network.
 *
 * A direct cable between the PS2 and a Mac has no router, so both ends are
 * set by hand. On the Mac, for example:
 *   sudo networksetup -setmanual "AX88179A" 192.168.50.1 255.255.255.0
 * and in settings.txt: network=static, ip=192.168.50.90,
 * gateway=192.168.50.1.
 */

/*
 * Wait for the Ethernet PHY before asking for a lease. Needed with DHCP:
 * a request sent over a down link never gets answered, so without this
 * you wait out the lease timeout instead of being told the cable is out.
 */
#define REQUIRE_LINK 1

/* Seconds to wait. Both report a clear error rather than hanging. */
#define LINK_TIMEOUT 8
#define DHCP_TIMEOUT 12

/* network=auto asks the router only this long before using the static
 * lines: on a direct cable, this is what every start waits. */
#define AUTO_DHCP_TIMEOUT 5

/*
 * Force lwIP's interface up regardless of what SMAP reports. Only needed
 * under an emulator, which never raises a PHY link. Leave at 0.
 */
#define FORCE_NETIF_UP 0

/* ---- files ---- */

/*
 * Remember the server URL between launches, written to settings.txt
 * after a successful connect. Typing a URL on a controller once is fine.
 * Twice is not.
 */
#define SAVE_URL 1

/*
 * Prefill the URL field when settings.txt has no server= line. Empty: the
 * address is typed once and kept in settings.txt from then on.
 */
#define DEFAULT_URL ""   /* none: typed once on the connect screen, then kept in settings.txt */

/* ---- sizes, measured against a real reply (2446 bytes for 14 items) ---- */
/*
 * Items per page. Kept small so the reply stays under the receive mailbox
 * limit described above: 14 items is roughly 2.4 KB and gets truncated, 6
 * is about 1 KB and arrives intact. Six is also what the poster grid wants.
 */
#define JF_PAGE_MAX  10                          /* sizes the item array */
#define JF_PAGE      (lay->grid_cols * 2)        /* 6, or 10 in the wide layout */

/*
 * Items per HTTP request, as opposed to per screen.
 *
 * This is the real fix for the segment bug, and it does not need Range.
 * Jellyfin ignores Range everywhere, but Limit and StartIndex are ordinary
 * query parameters it has always supported, so they can do the same job.
 *
 * Measured on a real reply: about 400 bytes per item, plus 60 of wrapper and
 * 167 of headers. So
 *
 *     6 items -> ~2613 raw, one full 1460 segment plus a remainder
 *     3 items -> ~1421 raw, under the boundary but only by 39 bytes
 *     2 items -> ~1023 raw, 437 bytes of headroom
 *
 * Two it is. A page becomes three small requests that cannot produce a full
 * segment, instead of one large one that always does.
 */
/*
 * All six in one request. Uncontrolled network: 2, which keeps each reply
 * inside a single undersized segment.
 */
#define JF_FETCH_ITEMS 6
#define JF_MAX_USERS 8

/* Tracks or episodes held for one album or season. Longer lists are capped. */
#define JF_TRACK_MAX 256

/* Episode requests stay bounded independently of music/library browsing.
 * Smaller pages may help unreliable networks; retries still validate JSON. */
#define JF_FETCH_EPISODES 6

#define HTTP_RAW_MAX 32768   /* whole response, headers included */

/*
 * Bytes per recv call. Any large value is fine; this is not the bottleneck.
 */
#define HTTP_CHUNK 8192

/*
 * THE constraint on this hardware.
 *
 * This stack drops the last 10 to 26 bytes of any full-size TCP segment.
 * Every corrupted reply we logged put the hole at raw offset ~1450, and the
 * Ethernet MSS is 1460. A response that fits inside one undersized segment
 * arrives perfectly; anything larger loses a piece of its middle, with the
 * end intact, so only a failed parse reveals it.
 *
 * So no single request may exceed this, headers included. Anything bigger
 * is fetched as a series of Range requests by http_get_range.
 *
 * 800 rather than 1200 for margin: a 206 reply carries Content-Range on top
 * of the usual headers, so 1200 bytes of body landed near 1400 and was close
 * enough to the boundary to still fail sometimes.
 */
#define HTTP_SAFE_BYTES 800

/*
 * Force lwIP's interface up regardless of what SMAP reports. An emulator
 * escape hatch; 0 on hardware.
 */

/*
 * On lowering the MSS with a socket option, which does not work.
 *
 * The truncation bug only bites full 1460-byte segments, so if the console
 * advertised a smaller MSS the server would never send one and the problem
 * would disappear for every request, images included.
 *
 * It is not reachable from here. Neither TCP_MAXSEG nor TCP_MSS appears
 * anywhere in $PS2SDK/{ee,common,ports}/include, so there is no socket
 * option to set: lwIP fixes its MSS at build time, inside ps2ip.
 *
 * Changing it would mean rebuilding that part of the SDK, which would fix
 * this for every PS2 homebrew using this stack but is a separate project.
 * Everything else here is a workaround for not having done that.
 */

/*
 * Fetch JSON in Range slices rather than as one request.
 *
 * Off, because Jellyfin does not honour Range: asking for bytes 0-799 returns
 * 200 OK with the whole document, so a "slice" is really the first 800 bytes
 * of a truncated body. Slicing looked like it worked earlier only because the
 * plain request that ran first was quietly succeeding.
 *
 * Left in place because some servers do support it, and because the code path
 * is shared. Turn it on only after confirming a 206 with curl -r.
 */
#define HTTP_ALWAYS_SLICE 0

/*
 * Attempts per JSON request.
 *
 * The same strategy images use: a failed parse proves the reply was damaged,
 * so ask again.
 *
 * 20 rather than 8, because the observed loss rate is far worse than the
 * coin flip originally assumed. The detail request is the awkward one: at
 * 1277 bytes plus 167 of headers it sits 16 bytes under the boundary, so a
 * slightly longer overview pushes it over and it fails at random. Each
 * attempt is one small request, so the insurance is cheap.
 */
/*
 * Retries should never fire. Kept as insurance rather than as the mechanism.
 * Uncontrolled network: 20.
 */
/* Stock ps2dev image: 20, where retries were the mechanism rather than
 * insurance. */
#define JSON_ATTEMPTS 2

/* ---- artwork ---- */

/*
 * Load artwork at all. Set to 0 to take a page from forty-odd HTTP requests
 * down to one, which is how to tell whether posters are behind a failure
 * elsewhere.
 */
/*
 * What to ask Jellyfin for when streaming video.
 *
 * 1.5 Mbit/s at 640x480 is comfortable: the server transcodes at around
 * 2 MB/s, roughly ten times what playback consumes, so the bitrate is
 * limited by taste rather than by the link. It was 1 Mbit/s from USB only
 * because that drive reads at about 190 KB/s.
 */
#define VIDEO_BITRATE 1500000

/*
 * The largest picture supported, and so the size of the reserved texture.
 *
 * 640x480 because that is what Jellyfin sends for 4:3 material. The VRAM
 * cost is the same as 448 would be: a texture's height rounds up to a power
 * of two, so both become 512 and the allocation is 1.25 MB either way.
 *
 * It is 1200 macroblocks, past the 1023 a stock libmpeg converts in one
 * pass, so the patched library is required rather than merely preferable.
 * See the note in video.h.
 *
 * Also used to choose what to ask the transcoder for, which is why it lives
 * here rather than in video.c.
 */
#define VIDEO_MAX_W 640
#define VIDEO_MAX_H 480

/*
 * Which transport stream PID carries the video.
 *
 * Jellyfin's transcodes put video on 0x0100 and audio on 0x0101, confirmed
 * by reading the program map of a real one. The map is parsed rather than
 * assumed if this is set to 0, which is not implemented yet.
 */
#define VIDEO_TS_PID 0x0100

/*
 * And the audio, MPEG-1 layer II, which libmad already decodes.
 *
 * Confirmed by reading the program map of a real transcode. Timestamps on
 * the two streams share a clock, so they can be played in step.
 */
#define AUDIO_TS_PID 0x0101

/*
 * Whether to decode and play the film's sound.
 *
 * A switch because the two interact in ways that are hard to attribute:
 * video decode, audio decode and the UI share the DMA controller and the
 * IOP, and a freeze in one looks like a fault in another. Turning this off
 * answers "is video still sound on its own?" in one build, which is the
 * question that was taking six attempts to guess at.
 *
 * The demuxing stays on either way, so the audio ring still fills and drops
 * are still counted - only the decoding stops.
 */
/*
 * ON, with the sound as the clock.
 *
 * It was off because three attempts failed, each by coupling the sound's
 * supply to the picture: reading inside data_cb, holding frames to correct
 * drift, and a reader thread fed from rings a sixth of a second deep. The
 * rebuild follows what ORBIT (the other PS2 Jellyfin client), Pocketfin,
 * the PS3 client and Ufin all do:
 *
 *   - the reader fills rings measured in seconds, so the sound always has
 *     its own supply in hand;
 *   - the sound plays continuously and is never trimmed, held or hurried;
 *   - its clock is the first audio PTS plus the samples audsrv has played;
 *   - each picture carries its PTS and is shown when the clock reaches it,
 *     or dropped when more than two frames late.
 *
 * Set to 0 for the silent player exactly as it was.
 */
#define VIDEO_AUDIO_ENABLE 1

/*
 * Divide the frame time, to play deliberately fast.
 *
 * 1 is correct speed. 2 runs at double, which is what the old broken pacing
 * did by accident - and under it films finished before the freeze had a
 * chance to appear. If the freeze moves to a different point in the film
 * when this changes, it is tied to elapsed time or to a position in the
 * stream, and was not introduced today at all.
 */
#define VIDEO_SPEEDUP 1


#define POSTER_ENABLE 1

/*
 * Print each poster's decoded size and byte count over the artwork.
 * Deliberately ugly; it is for working out why a particular image displays
 * wrongly when others are fine.
 */
#define DEBUG_POSTER_INFO 0


/* Pause between posters, letting the stack reclaim sockets and buffers. */
/* Was 120 to let the stack recover between fetches. Stock image: 120. */
/*
 * The video stream's receive timeout once it is playing, ms. A thread
 * blocked in recv only notices Stop when this runs out, so it sets how
 * quickly Stop takes effect. Five seconds of timeouts still mean the
 * stream has ended.
 */
#define VIDEO_RECV_MS 200

/*
 * Where a film ends (scr_video's near_the_end): this long before the
 * runtime, or once the stream has ended with less than VIDEO_TAIL_BYTES
 * of video left - always between two pictures, so libmpeg is never asked
 * to decode into the tail of a stream, where it hangs.
 */
#define VIDEO_END_EARLY_MS 200
#define VIDEO_TAIL_BYTES   (32 * 1024)

#define POSTER_GAP_MS 30

/*
 * Poster request size.
 *
 * Small on purpose, and not for bandwidth. Jellyfin's image endpoint ignores
 * the Range header: asking for bytes 0-799 of a 6484-byte poster returns
 * 200 OK with all 6484. So slicing, which is what makes large JSON reliable
 * here, is not available for artwork.
 *
 * That leaves the segment lottery. Every full 1460-byte segment is a chance
 * to lose 10 to 26 bytes, so a 6.5 KB image gambles four times and arrives
 * intact less than half the time. 64px at quality 55 is 2 to 3 KB, one or
 * two segments, and usually survives.
 *
 * The cost is softness, upscaled into a 90px slot. On a CRT at 480i that is
 * a fair trade for artwork that actually appears.
 */
/*
 * Poster sizes, measured against a real reply rather than estimated.
 *
 * Headers on the image endpoint are 348 bytes, so the body budget under the
 * 1460 boundary is 1112:
 *
 *     w32 q55    856 body, 1204 total   safe, 256 spare
 *     w36 q40    941 body, 1289 total   safe, 171 spare
 *     w32 q80   1151 body, 1499 total   over
 *     w40 q55   1341 body, 1689 total   over
 *     w64 q55   2257 body, 2605 total   one full segment
 *
 * So anything that actually looks like a poster crosses the line. 36px is
 * the deterministic ceiling, and upscaled into a 90px slot it is a blur.
 *
 * The better trade is to ask for 64px, which reads as artwork, and lean on
 * retries: one full segment is roughly a coin flip, so six attempts is about
 * 98%, at a cost of two requests per poster on average. 36px stays as a
 * guaranteed fallback so a slot is never left empty.
 *
 * If the SDK's MSS is ever lowered, 64px becomes deterministic and the
 * fallback rung stops being used.
 */
/* Upper bound passed to the fetch; the ladder decides the actual size. */
#define POSTER_REQ_W   96

/*
 * Width and quality per rung, tried in order. The last one is sized to fit
 * inside a single short segment, so it cannot fail for size reasons.
 */
/*
 * An upper bound on a poster, not a workaround any more.
 *
 * This used to be 2100, because above roughly that size images arrived with
 * the right byte count and altered contents, which nothing downstream could
 * detect. With the segment bug fixed in the build image that no longer
 * happens, so this is only here to catch something absurd.
 *
 * Stock ps2dev image: 2100.
 */
#define IMAGE_SAFE_BYTES 8000

/*
 * Quality drops before width does. A softer 64px image still reads as a
 * poster; a sharp 36px one does not.
 */
/*
 * Sharp, with one fallback for items whose artwork is unusually large.
 * Uncontrolled network: { { 64, 55 }, { 64, 35 }, { 48, 45 }, { 36, 40 } }
 */
/*
 * One rung. With the segment bug gone there is nothing to fall back from:
 * a 96px poster arrives intact every time.
 *
 * Stock ps2dev image: { { 64, 55 }, { 64, 35 }, { 48, 45 }, { 36, 40 } }
 */
#define POSTER_LADDER { { 96, 70 } }
/*
 * Attempts per rung.
 *
 * Sized for a normal 1500-byte MTU, where the driver bug is live. 64px is
 * 2605 bytes and contains one maximum-size segment, so it fails often and
 * needs the persistence. 36px is 1289 bytes, has no maximum-size segment,
 * and cannot fail for size.
 *
 * On a link where the MTU has been lowered nothing is lost at all and the
 * first attempt always wins, so a 96px rung could be added in front. See
 * the note in the README.
 */
/* Few attempts needed: the first should succeed every time.
 * Uncontrolled network: { 4, 4, 4, 3 } */
/* Two, and the second should never fire.
 * Stock ps2dev image: { 4, 4, 4, 3 } */
#define POSTER_TRIES  { 2 }


/*
 * VRAM slot size.
 *
 * The width MUST be a multiple of 64. The GS stores a texture's buffer width
 * in units of 64 pixels, so anything else is rounded up and the hardware
 * reads rows with a wider stride than the one we wrote them with. At 72 that
 * meant a stride of 128 against pixels laid out at 72, which looked exactly
 * like a corrupt JPEG: skewed bands of colour.
 *
 * 64 matches the widest rung of the ladder exactly. Height has no such
 * constraint.
 */
/* Width MUST be a multiple of 64: the GS stores texture buffer width in
 * units of 64 pixels, and a mismatch reads every row at the wrong offset. */
#define POSTER_MAX_W 128
#define POSTER_MAX_H 192

/*
 * The detail page slot. Also a multiple of 64, for the reason above.
 */
#define DETAIL_MAX_W 128
#define DETAIL_MAX_H 192

/* Working buffer for one encoded image. */
#define IMAGE_MAX 24576

/*
 * Ask for a bigger socket receive buffer. Only some lwIP builds honour it,
 * and it costs nothing to try.
 */
#define HTTP_RCVBUF 16384
/*
 * 3072 rather than 2048, for the stream lists.
 *
 * A page of items needs about 500 - roughly 35 tokens each. Asking for
 * MediaStreams is far heavier: the busiest file to hand, a 1080p VC-1 with
 * a dozen subtitle tracks, needs 896. That fits, but not with much to spare
 * for a library with more tracks than that.
 *
 * Each token is 16 bytes, so this costs 16 KB of static memory.
 */
#define JSON_TOK_MAX 3072

/* ---- audio ---- */

/*
 * Assumed sample rate, used only to turn bytes fed to the SPU2 into a
 * position for the progress bar.
 *
 * The real rate comes from the MP3 itself: audsrv is configured from the
 * first decoded frame header, since the file decides, not us. A 32 kHz track
 * will therefore play correctly but show a slightly optimistic position.
 */
#define AUDIO_RATE 44100

/*
 * Decoder buffers.
 *
 * The network read, the MP3 decode and the SPU2 feed all share one thread,
 * so every moment spent reading is a moment the SPU2 is not being fed. Larger
 * buffers mean fewer socket trips per frame decoded and more tolerance for a
 * slow read before the hardware runs dry and clicks.
 *
 * AUDIO_IN_BUF must comfortably exceed one MP3 frame, which is at most 1441
 * bytes.
 */
#define AUDIO_IN_BUF   32768
#define AUDIO_PCM_BUF  16384
#define AUDIO_CHUNK    2048
#define AUDIO_PREBUFFER 24576

/*
 * Ring buffer between the reader thread and the decoder.
 *
 * One thread reads the socket, another decodes and feeds the SPU2, and this
 * sits between them. Without it the two share a thread, so every socket read
 * is time the hardware is not being fed, and audsrv's own buffer is far too
 * small to cover the gap. That was the glitching.
 *
 * 256 KB is about sixteen seconds at 128 kbps, so the decoder can ride out a
 * long stall without noticing. Video will want the same arrangement.
 */
#define AUDIO_RING (256 * 1024)

/*
 * Use the reader thread and ring at all.
 *
 * 0 makes the decoder read the socket itself, which is the known-good
 * arrangement: glitchy, but it plays and input stays responsive.
 *
 * 1 adds the reader thread. Tried once and it broke input, most likely by
 * preempting the UI during its vsync busy-wait. Left in because video will
 * want this shape eventually, but it needs work before it is usable.
 */
#define AUDIO_READER_THREAD 0

/*
 * How long the audio threads sleep when they have nothing to do.
 *
 * Kept generous: the UI busy-waits for vsync, so every wakeup of a
 * higher-priority thread risks it missing a frame. Idle threads that wake
 * rarely are cheap; idle threads that wake constantly are not.
 */
#define AUDIO_IDLE_MS 100

/*
 * Thread priorities. On the EE a LOWER number wins.
 *
 * The decoder must outrank the reader. Feeding the SPU2 is the real-time
 * job; the reader only has to keep up on average and has the whole ring to
 * do it in. Getting this the wrong way round lets the reader preempt the
 * decoder whenever the ring has room, and the hardware starves.
 */
/*
 * Thread priorities, lowest number first - on the EE that means highest.
 *
 *   0x3F  music decode, and the film's sound
 *   0x40  the video decoder                    (VIDEO_PRIO, in video.c)
 *   0x45  the music and video network readers
 *   0x48  background reports
 *   0x56-0x59  SDK NetMan and TCP/IP threads
 *   0x60  the UI
 *
 * Sound sits above video deliberately: it must not be interrupted, whereas a
 * picture can be dropped or held without much notice. Getting this backwards
 * has caused both audible skips and a complete failure to start, so the
 * order is worth reading as a whole rather than one line at a time.
 */
#define AUDIO_DECODE_PRIO 0x3F

/*
 * The film's sound, between the music decoder and the video decoder.
 *
 * Above video (0x40) because sound must not be interrupted while a picture
 * can be dropped, and below the music decoder because only one of the two
 * ever runs.
 */
#define VIDEO_AUDIO_PRIO 0x3F
#define AUDIO_READER_PRIO 0x45

/*
 * Priority of the UI thread. On the EE a LOWER number wins.
 *
 * The main thread starts at 0. gsKit_sync_flip can busy-wait on the GS,
 * so the UI must sit below both playback and the SDK's network threads.
 * NetMan and TCP/IP use priorities 0x56-0x59: placing the UI at 0x50 lets
 * it delay packet processing and ACKs until the playback buffers run dry.
 * Keep it below those threads so streaming can continue while it waits.
 */
/* The thread sending stop reports and resume points: above the UI, which
 * can busy-wait on the GS, and below everything that plays. */
#define REPORT_PRIO 0x48

#define UI_THREAD_PRIO 0x60


/*
 * Text subtitles, drawn by the PS2 (subs.c). One track is held at a time,
 * in memory only; a film's subtitles are typically 50-150 KB.
 */
#define SUBS_MAX_BYTES (1024 * 1024)   /* the file as downloaded */
#define SUBS_MAX_CUES  6000            /* timed lines */

/* ---- startup ---- */

/*
 * Reset the IOP before loading our own modules. Required: it is what lets
 * sbv_patch_enable_lmb take effect, and that patch is what allows modules
 * to be loaded out of EE memory.
 *
 * uLaunchELF's own USB modules die with it, which is why settings.txt is
 * read before this happens. Ours are loaded straight afterwards, so mass:
 * works again for saving the URL.
 */
#define IOP_RESET 1


/*
 * Show the startup log and the network bring-up steps: the purple bar with
 * "loading dev9", "net: link 1/8" and so on.
 *
 * Off for a release build, which instead shows a splash while modules load
 * and a plain "Connecting..." while the network comes up. Turn it on when
 * something hangs, because the last line on screen names the call that
 * blocked.
 */
#define DEBUG_BOOT_LOG 0

/*
 * Show the network layer's internal chatter: "net: connect", "refetching in
 * slices", "slice short", and so on.
 *
 * Off by default. It is useful when a request misbehaves and noise the rest
 * of the time. User-facing progress, such as the artwork counter, goes
 * through app_progress instead and is always shown.
 */
#define DEBUG_NET_LOG 0


/* ---- debug switches. All 0 for a normal build. ---- */
#define SKIP_NETWORK 0
#define SKIP_PAD     0

#endif