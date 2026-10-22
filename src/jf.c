/*
 * The Jellyfin client: every API request the app makes. Connecting, users
 * and signing in, library pages, details, HOME, stream URLs, images, and
 * the background thread that sends stop reports and resume points.
 * Requests run on the screen thread inside jobs, except the reports.
 */

#include "jf.h"
#include "freeze.h"
#include "settings.h"
#include "net.h"
#include "json.h"
#include "config.h"

#include <kernel.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>

char jf_url[128] = DEFAULT_URL;
char jf_server_name[64] = "";
char jf_version[32]     = "";

JfUser jf_users[JF_MAX_USERS];
int    jf_user_count = 0;
char   jf_user_id[40]   = "";
char   jf_user_name[64] = "";

JfItem jf_items[JF_PAGE_MAX];
int     jf_item_count  = 0;
int     jf_total_count = 0;

JfMusicLevel jf_music_level = JF_MUSIC_ARTISTS;
char         jf_artist_name[128];
char         jf_artist_id[40];

/* Music: go into an artist (their albums). */
void jf_music_enter_artist(const char *id, const char *name)
{
	snprintf(jf_artist_id,   sizeof(jf_artist_id),   "%s", id   ? id   : "");
	snprintf(jf_artist_name, sizeof(jf_artist_name), "%s", name ? name : "");
	jf_music_level = JF_MUSIC_ALBUMS;
	jf_start_index = 0;
}

JfTvLevel jf_tv_level = JF_TV_SHOWS;
char      jf_show_id[40];
char      jf_show_name[128];

/* Series: go into a show (its seasons). */
void jf_tv_enter_show(const char *id, const char *name)
{
	snprintf(jf_show_id,   sizeof(jf_show_id),   "%s", id   ? id   : "");
	snprintf(jf_show_name, sizeof(jf_show_name), "%s", name ? name : "");
	jf_tv_level    = JF_TV_SEASONS;
	jf_start_index = 0;
}

/* Series: back to the list of shows. */
void jf_tv_up(void)
{
	jf_show_id[0]   = '\0';
	jf_show_name[0] = '\0';
	jf_tv_level     = JF_TV_SHOWS;
	jf_start_index  = 0;
}

/* Music: back to the list of artists. */
void jf_music_up(void)
{
	jf_artist_name[0] = '\0';
	jf_artist_id[0]   = '\0';
	jf_music_level    = JF_MUSIC_ARTISTS;
	jf_start_index    = 0;
}

JfTrack jf_tracks[JF_TRACK_MAX];
int     jf_track_count = 0;
char    jf_album_name[112]  = "";
char    jf_album_artist[80] = "";
char    jf_album_desc[512]  = "";
int     jf_album_year = 0;

JfTab    jf_tab = JF_TAB_MOVIES;
char     jf_query[64] = "";
JfDetail jf_detail;

static int g_library_total;      /* unfiltered count, for letter jumps */
int     jf_start_index = 0;

/*
 * The signed-in user's access token: the only credential the app holds.
 * Empty until someone signs in on the user page, and nothing but the
 * server's public information and user list can be read without it.
 */
static char g_key[192]      = "";

static char g_host[96] = "";
static int  g_port     = 8096;

static int  g_net_up = 0;

static char g_err_title[64]  = "";
static char g_err_body[320]  = "";

/* ------------------------------------------------------------------ */

const char *jf_err_title(void) { return g_err_title; }

/* The last error's explanation, for the error screen. */
const char *jf_err_body(void)  { return g_err_body;  }

/*
 * Record an error title and printf-style explanation; returns -1 so
 * callers can 'return err(...)'.
 */
static int err(const char *title, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_err_body, sizeof(g_err_body), fmt, ap);
	va_end(ap);

	snprintf(g_err_title, sizeof(g_err_title), "%s", title);
	return -1;
}

/* A file beside settings.txt, for diagnostics. */
static void beside_settings(char *out, int max, const char *name)
{
	const char *sp  = settings_path();
	const char *cut = strrchr(sp, '/');

	if (!cut)
		cut = strrchr(sp, ':');

	int n = cut ? (int)(cut - sp) + 1 : 0;

	snprintf(out, max, "%.*s%s", n, sp, name);
}

/*
 * Dump the whole failing response to the USB stick. Reading 48 bytes of
 * tail off a CRT is not a diagnosis; opening the file on a desktop is.
 */
/* Error screens mention the dump only when one is written (DEBUG_UI). */
#if DEBUG_UI
#define DUMP_NOTE " Written to jfdebug.txt on the drive."
#else
#define DUMP_NOTE ""
#endif

/*
 * Development builds (DEBUG_UI): write the last raw response to
 * jfdebug.txt beside settings.txt. Does nothing otherwise.
 */
static void dump_reply(const char *label)
{
#if !DEBUG_UI
	(void)label;
	return;              /* nothing written to users' drives */
#endif
	char path[320];
	beside_settings(path, sizeof(path), "jfdebug.txt");

	FILE *f = fopen(path, "w");
	if (!f)
		return;

	fprintf(f, "=== %s ===\n", label);
	fprintf(f, "host %s port %d\n", g_host, g_port);
	fprintf(f, "raw bytes %d\n", net_last_raw_len());

	/* The sum of the recv sizes should equal the raw length. If it does,
	 * bytes went missing inside a single recv rather than between calls. */
	int sum = 0, calls = net_recv_count();
	fprintf(f, "recv calls %d:", calls);
	for (int i = 0; i < calls; i++) {
		int r = net_recv_at(i);
		fprintf(f, " %d", r);
		if (r > 0) sum += r;
	}
	fprintf(f, "\nrecv sum %d\n\n", sum);

	fwrite(net_last_raw(), 1, net_last_raw_len(), f);
	fputc('\n', f);

	/*
	 * fclose alone left the file empty: the FAT driver buffers and the data
	 * had not reached the stick. Flush explicitly, then yield long enough
	 * for the write to land.
	 */
	fflush(f);
	fclose(f);
	net_snooze(1500);
}

/*
 * One HTTP attempt, retried on a transport-level failure.
 *
 * -5 is a reply with no usable headers, -6 a body that ends mid-document.
 * Both are transient, so three goes with a pause between.
 */
static int http_try(const char *path, int auth, int *status, char **body)
{
	const char *tok = auth ? g_key : NULL;
	int rc = -1;

	for (int attempt = 0; attempt < 2; attempt++) {
		rc = http_get(g_host, g_port, path, tok, status, body);

		/* -3 is a connect that never completed. On a cold stack that is
		 * usually ARP losing the race, and it succeeds a moment later. */
		if (rc != -3 && rc != -5 && rc != -6)
			break;

		/* Yielding, not spinning: the stack needs EE time to recover. */
		net_snooze(300);
	}

	return rc;
}

/*
 * Fetch and parse, retrying when the document will not parse.
 *
 * This stack sometimes drops a handful of bytes from the middle of a
 * response, when one recv spans a pbuf boundary. The body still ends
 * correctly, so neither the length nor a completeness check notices, and
 * only the parser does. The loss is intermittent, so another attempt
 * almost always succeeds.
 *
 * Returns the token count, or negative on failure. *status carries the
 * HTTP result either way.
 */
#define GET_ATTEMPTS JSON_ATTEMPTS

/* Keep transport failures distinct from jsmn's overlapping error codes. */
static int g_json_transport_rc;

/*
 * Fetch a resource of any size by asking for it in slices small enough that
 * no TCP segment is ever full. See HTTP_SAFE_BYTES in config.h for why.
 *
 * Returns the assembled length, or negative. The buffer is static and valid
 * until the next call.
 */
__attribute__((unused))
static char g_asm[HTTP_RAW_MAX];

__attribute__((unused))

/*
 * Fetch a large reply in byte ranges (http_get_range) small enough to
 * arrive intact, reassembling it in g_asm; each slice is retried on its
 * own.
 */
static int fetch_sliced(const char *path, int auth, int *status)
{
	long total = -1;
	int  got   = 0;

	while (got < HTTP_RAW_MAX - 1) {
		int want = HTTP_SAFE_BYTES;

		if (total > 0 && got + want > total)
			want = (int)(total - got);
		if (want <= 0)
			break;

		/*
		 * Retry each slice individually.
		 *
		 * A short slice is directly detectable: we asked for `want` bytes
		 * and are not at the end of the resource, so anything less means
		 * the segment lost data. Much better than waiting for the parse to
		 * fail, because we know exactly which piece to ask for again.
		 */
		int n = -1;

		for (int try = 0; try < 3; try++) {
			n = http_get_range(g_host, g_port, path, auth ? g_key : NULL,
			                   got, want, g_asm + got, status, &total);
			if (n < 0) {
				net_snooze(200);
				continue;
			}

			/* Full slice, or a short one that reaches the end. */
			if (n == want || (total > 0 && got + n >= total))
				break;

			char m[64];
			snprintf(m, sizeof(m), "slice short: %d of %d", n, want);
			net_say(m);

			n = -1;
			net_snooze(200);
		}

		if (n <= 0)
			return -1;

		got += n;

		if (total > 0 && got >= total)
			break;

		/* The server ignored Range and sent everything, so we are done. */
		if (*status == 200 && total < 0)
			break;
	}

	g_asm[got] = '\0';
	return got;
}

/*
 * Fetch and parse.
 *
 * A plain request is tried first because it is one round trip and most of
 * ours are small enough to be safe. If the reply will not parse, the cause
 * is almost always a full TCP segment losing its tail, so the retry
 * reassembles from Range slices, which cannot produce a full segment.
 */
/*
 * Binary sibling of fetch_sliced, into a caller-independent buffer. Same
 * per-slice verification: a short slice that is not the last one means the
 * segment lost data, so ask again.
 */
static unsigned char g_img[IMAGE_MAX];

/*
 * A JPEG proves its own completeness: it opens with FF D8 and closes with
 * FF D9. That is a much stronger check than anything available for JSON, so
 * use it rather than trusting lengths.
 */
static int jpeg_ok(const unsigned char *d, int n)
{
	return n > 4 &&
	       d[0] == 0xFF && d[1] == 0xD8 &&
	       d[n - 2] == 0xFF && d[n - 1] == 0xD9;
}

/*
 * One attempt at one width. Returns bytes, 0 for no artwork, -1 on damage.
 */
static int fetch_image_at(const char *item_id, int width, int quality)
{
	static char path[320];
	int   status;
	char *body;

	/*
	 * Bound the height too. maxWidth alone constrains one axis, so artwork
	 * with an unusual ratio comes back taller than the VRAM slot and the
	 * decode is refused.
	 *
	 * Film posters are nominally 2:3; album covers are square, and asking
	 * for a 3:2 height simply leaves them smaller than they could be, which
	 * is harmless. Jellyfin fits within both bounds and preserves the ratio.
	 */
	int height = width * 3 / 2;
	if (height > POSTER_MAX_H)
		height = POSTER_MAX_H;

	snprintf(path, sizeof(path),
	         "/Items/%s/Images/Primary"
	         "?maxWidth=%d&maxHeight=%d&quality=%d&format=Jpg",
	         item_id, width, height, quality);

	int rc = http_get(g_host, g_port, path, g_key, &status, &body);

	if (rc != 0)
		return -1;                     /* transport failure */

	if (status == 404)
		return 0;                      /* no artwork, a normal answer */

	if (status != 200)
		return -1;

	int n = net_last_body_len();

	if (n <= 0 || n >= IMAGE_MAX)
		return -1;

	/*
	 * Too large to trust. Above roughly this size images arrive with the
	 * right byte count but altered contents, which nothing downstream can
	 * detect. Refuse it so the caller drops to a smaller rung.
	 */
	if (n > IMAGE_SAFE_BYTES)
		return -3;

	/*
	 * A cheap pre-filter only. It catches a body that was cut short, but a
	 * hole in the middle leaves both markers in place, so passing this does
	 * not mean the image is sound. Only the decoder can say that.
	 */
	if (!jpeg_ok((const unsigned char *)body, n))
		return -1;

	memcpy(g_img, body, n);
	return n;
}

/*
 * An item's poster as JPEG bytes, width-limited and with the given
 * quality; *out points to the shared image buffer. Returns the byte count
 * or negative.
 */
int jf_fetch_image(const char *item_id, int width, int quality,
                   const unsigned char **out)
{
	*out = g_img;
	return fetch_image_at(item_id, width, quality);
}

/*
 * GET a JSON reply and parse it, retrying a few times on transport errors
 * or truncated JSON. Returns the token count (or negative); *status and
 * *body as for http_get.
 */
static int get_json(const char *path, int auth, int *status, char **body)
{
	int last = -1;
	int toks = -1;
	g_json_transport_rc = 0;

	for (int attempt = 0; attempt < GET_ATTEMPTS; attempt++) {
		/*
		 * Say which attempt this is. Without it a run of retries is
		 * indistinguishable from a hang, since the busy screen is
		 * painted once before the job starts.
		 */
		if (attempt > 0) {
			char msg[80];
			snprintf(msg, sizeof(msg), "retry %d/%d: %.40s",
			         attempt + 1, GET_ATTEMPTS, path);
			net_say(msg);
		}

		/*
		 * Plain request, every attempt.
		 *
		 * A failed parse is proof the reply lost bytes to a full segment,
		 * and the loss is a coin flip, so simply asking again is the whole
		 * defence. Artwork does the same thing, but validates by decoding.
		 */
		int rc = http_try(path, auth, status, body);
		if (rc < 0) {
			g_json_transport_rc = rc;
			return rc;
		}

		toks = json_parse(*body);
		if (toks > 0) {
			if (attempt > 0) {
				char m[48];
				snprintf(m, sizeof(m), "ok on attempt %d", attempt + 1);
				net_say(m);
			}
			return toks;
		}

		last = toks;

#if HTTP_ALWAYS_SLICE
		/* Only useful against a server that answers 206. */
		{
			int n = fetch_sliced(path, auth, status);

			if (n > 0) {
				toks = json_parse(g_asm);
				if (toks > 0) {
					*body = g_asm;
					return toks;
				}
			}
		}
#endif

		net_snooze(200);
	}

	return last;
}

/* ------------------------------------------------------------------ */
/* Server address                                                     */
/* ------------------------------------------------------------------ */

/* The server from settings.txt, into the connect screen's field. */
void jf_init(void)
{
	if (settings.server[0])
		snprintf(jf_url, sizeof(jf_url), "http://%.120s", settings.server);
}

/* The server's host, parsed from jf_url. */
const char *jf_host(void) { return g_host; }

/* The server's port, parsed from jf_url. */
int         jf_port(void) { return g_port; }

/* The signed-in user's access token, '' before sign-in. */
const char *jf_key(void)  { return g_key;  }

/*
 * The URL path of a music track's audio stream: MP3 as is, anything else
 * transcoded to MP3 by the server.
 */
const char *jf_track_stream_path(const char *track_id)
{
	static char path[512];

	/*
	 * MP3 goes across untouched; anything else is transcoded to MP3 by the
	 * server.
	 *
	 * libmad decodes MP3 and nothing else, so a FLAC or AAC track would
	 * otherwise reach it as noise it cannot sync to. Asking for the file as
	 * it is remains the better path when it already is MP3: no work for the
	 * server, a real Content-Length, and no transcode to go wrong.
	 *
	 * PCM would have avoided needing a decoder at all, but Jellyfin's PCM
	 * path is broken: it builds an ffmpeg command with a bare "-ar" and no
	 * value, and every wav request returns 500. The mp3 path works.
	 */
	const char *container = "";

	for (int i = 0; i < jf_track_count; i++) {
		if (strcmp(jf_tracks[i].id, track_id) == 0) {
			container = jf_tracks[i].container;
			break;
		}
	}

	int is_mp3 = (container[0] == 'm' || container[0] == 'M') &&
	             (container[1] == 'p' || container[1] == 'P') &&
	              container[2] == '3';

	if (is_mp3)
		snprintf(path, sizeof(path),
		         "/Audio/%s/stream?static=true", track_id);
	else
		snprintf(path, sizeof(path),
		         "/Audio/%s/stream.mp3"
		         "?audioCodec=mp3&audioBitRate=192000",
		         track_id);

	return path;
}

/*
 * Save the server address to settings.txt after a successful connect (only
 * if it changed).
 */
void jf_save_url(void)
{
#if SAVE_URL
	if (jf_url[0])
		settings_save_server(jf_url);   /* only if it changed */
#endif
}

/* ------------------------------------------------------------------ */

static int parse_url(const char *url)
{
	const char *p = url;

	if (strncmp(p, "http://", 7) == 0)
		p += 7;
	else if (strncmp(p, "https://", 8) == 0)
		return err("HTTPS not supported",
		           "The PS2 cannot do TLS. Use a plain http:// address on "
		           "your LAN.");

	g_port = 8096;

	int i = 0;
	while (*p && *p != ':' && *p != '/' && i < (int)sizeof(g_host) - 1)
		g_host[i++] = *p++;
	g_host[i] = '\0';

	if (i == 0)
		return err("Bad address",
		           "Could not read a host from that URL. It must be an IP "
		           "address, not a hostname.");

	if (*p == ':') {
		p++;
		int v = 0;
		while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
		if (v > 0 && v < 65536) g_port = v;
	}
	return 0;
}

/* ------------------------------------------------------------------ */

int jf_connect(void)
{
	if (parse_url(jf_url) < 0)
		return -1;

	/*
	 * lwIP is started here rather than at boot, so a hang in ps2ipInit
	 * costs you a connect attempt instead of the whole app.
	 */
	if (!g_net_up) {
		int rc = net_start();
		if (rc == -4)
			return err("No address",
			           "Ethernet is up but no router answered. Check its "
			           "DHCP server, or on a direct cable set network=static "
			           "(or auto) and ip= in settings.txt.");

		if (rc == -5)
			return err("No address set",
			           "settings.txt says network=static but has no ip= "
			           "line. Add one, e.g. ip=192.168.50.90.");

		if (rc == -2)
			return err("No network link",
			           "Ethernet never came up. Check the cable, and that "
			           "the network adaptor is seated if this is a fat "
			           "console.");

		if (rc < 0 && rc != -100)
			return err("Network failed",
			           "Init returned %d before any request was made.", rc);
		g_net_up = 1;
	}

	int   status;
	char *body;

	/* No credentials needed: is this a Jellyfin server at all? */
	int rc = get_json("/System/Info/Public", 0, &status, &body);
	if (rc < 0 && status == 0) {
		const char *why;
		switch (rc) {
		case -1: why = "socket() failed";                     break;
		case -2: why = "address is not a dotted quad";        break;
		case -3: why = "connect() refused or timed out";      break;
		case -4: why = "send() failed";                       break;
		case -5: why = "connected, but no HTTP reply";        break;
		default: why = "unknown";                             break;
		}
		return err("Cannot reach server",
		           "%s:%d  rc=%d %s.  %s",
		           g_host, g_port, rc, why, net_status());
	}

	if (status != 200)
		return err("Not a Jellyfin server",
		           "%s:%d answered with HTTP %d instead of server info.",
		           g_host, g_port, status);

	jf_server_name[0] = '\0';
	jf_version[0]     = '\0';

	if (rc > 0) {          /* get_json already parsed it */
		json_str(json_get(0, "ServerName"), jf_server_name,
		         sizeof(jf_server_name));
		json_str(json_get(0, "Version"), jf_version, sizeof(jf_version));
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Users and signing in                                               */
/* ------------------------------------------------------------------ */

/*
 * The users Jellyfin lists on its login page. Needs no credentials, which
 * is the point: it is how the user page is drawn before anyone has signed
 * in. Accounts hidden from the login page are not in it; "Other user" on
 * the page covers those.
 */
int jf_load_users(void)
{
	int   status;
	char *body;

	jf_user_count = 0;

	int toks = get_json("/Users/Public", 0, &status, &body);

	if (status != 200 || toks < 1 || !json_is_array(0)) {
		dump_reply("GET /Users/Public");
		return err("Could not list users",
		           "%s." DUMP_NOTE,
		           net_last_info());
	}

	int j = 1;

	for (int n = 0; n < json_size(0) && jf_user_count < JF_MAX_USERS; n++) {
		int vn = json_get(j, "Name");
		int vi = json_get(j, "Id");

		if (vn >= 0 && vi >= 0) {
			JfUser *u = &jf_users[jf_user_count++];
			char    hp[8] = "";

			json_str(vn, u->name, sizeof(u->name));
			json_str(vi, u->id,   sizeof(u->id));
			json_str(json_get(j, "HasPassword"), hp, sizeof(hp));
			u->has_pw = (hp[0] == 't');
		}
		j = json_skip(j);
	}

	return 0;
}

/* Back to nobody: the user page clears this before anyone is chosen. */
void jf_sign_out_local(void)
{
	g_key[0]        = '\0';
	jf_user_id[0]   = '\0';
	jf_user_name[0] = '\0';
	net_set_device_id(NULL);
}

/* Make a saved sign-in the current user: token, id, name and device id. */
static void become(const SavedUser *u)
{
	snprintf(g_key,        sizeof(g_key),        "%s", u->token);
	snprintf(jf_user_id,   sizeof(jf_user_id),   "%s", u->id);
	snprintf(jf_user_name, sizeof(jf_user_name), "%s", u->name);
	net_set_device_id(u->device);
}

/*
 * A device id for a user signing in on this PS2 for the first time. Each
 * user has their own: Jellyfin keeps one sign-in per device, so a shared
 * id would let one user signing in sign the other out. Mixed from the CPU
 * cycle counter and the clock, so two consoles never pick the same one.
 */
static void new_device_id(char *out, int max)
{
	unsigned count;

	__asm__ volatile("mfc0 %0, $9" : "=r"(count));

	unsigned stamp = count ^ ((unsigned)clock() * 2654435761u);

	snprintf(out, max, "ps2-%08x", stamp);
}

/* name as a JSON string body: Latin-1 back to UTF-8, quotes escaped. */
static int json_put(char *out, int max, const char *s)
{
	int o = 0;

	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (o > max - 8)
			break;

		if (*p == '"' || *p == '\\') {
			out[o++] = '\\';
			out[o++] = (char)*p;
		} else if (*p < 0x20) {
			out[o++] = ' ';
		} else if (*p < 0x80) {
			out[o++] = (char)*p;
		} else {
			out[o++] = (char)(0xC0 | (*p >> 6));
			out[o++] = (char)(0x80 | (*p & 0x3F));
		}
	}

	out[o] = '\0';
	return o;
}

/*
 * Sign in with a name and password; see jf.h. Saves the sign-in to
 * settings.txt.
 */
int jf_sign_in(const char *name, const char *password)
{
	SavedUser u;
	const SavedUser *had = settings_user(name);

	memset(&u, 0, sizeof(u));

	/* The same device as before, if this user was ever signed in here. */
	if (had)
		snprintf(u.device, sizeof(u.device), "%s", had->device);
	else
		new_device_id(u.device, sizeof(u.device));

	jf_sign_out_local();
	net_set_device_id(u.device);

	char jn[160], jp[160], body[400];

	json_put(jn, sizeof(jn), name);
	json_put(jp, sizeof(jp), password ? password : "");
	snprintf(body, sizeof(body), "{\"Username\":\"%s\",\"Pw\":\"%s\"}", jn, jp);

	int   status = 0;
	char *reply;
	int   rc = -1;

	for (int attempt = 0; attempt < 2; attempt++) {
		rc = http_post(g_host, g_port, "/Users/AuthenticateByName", NULL,
		               body, &status, &reply);
		if (rc != -3 && rc != -5 && rc != -6)
			break;
		net_snooze(300);
	}

	if (rc < 0 && status == 0)
		return err("Cannot reach server", "Signing in failed (%d).", rc);

	if (status == 401 || status == 403)
		return -2;                       /* wrong name or password */

	if (status != 200 || json_parse(reply) < 1)
		return err("Sign in failed", "The server answered HTTP %d.", status);

	json_str(json_get(0, "AccessToken"), u.token, sizeof(u.token));

	int user = json_get(0, "User");

	if (user >= 0) {
		json_str(json_get(user, "Id"),   u.id,   sizeof(u.id));
		json_str(json_get(user, "Name"), u.name, sizeof(u.name));
	}

	if (!u.token[0] || !u.id[0])
		return err("Sign in failed", "The reply had no access token.");

	if (!u.name[0])
		snprintf(u.name, sizeof(u.name), "%s", name);

	/* A name with '|' would break its settings line. */
	for (char *c = u.name; *c; c++)
		if (*c == '|')
			*c = '/';

	settings_save_user(&u);
	become(settings_user(u.name) ? settings_user(u.name) : &u);
	return 0;
}

/* Use a saved sign-in, checking the server still accepts it; see jf.h. */
int jf_resume_user(const char *name)
{
	const SavedUser *u = settings_user(name);

	if (!u)
		return -2;

	become(u);

	/* Is the saved sign-in still good? Any small authenticated read. */
	char path[96];
	int  status = 0;
	char *body;

	snprintf(path, sizeof(path), "/Users/%s", u->id);

	if (http_try(path, 1, &status, &body) < 0 && status == 0)
		return err("Cannot reach server", "Could not check the sign-in.");

	if (status == 401 || status == 403) {
		/* Signed out on the server: forget it here too. */
		char n[64];

		snprintf(n, sizeof(n), "%s", name);
		settings_remove_user(n);
		jf_sign_out_local();
		return -2;
	}

	if (status != 200) {
		jf_sign_out_local();
		return err("Sign in failed", "The server answered HTTP %d.", status);
	}

	return 0;
}

/*
 * End a saved sign-in on the server (/Sessions/Logout) and remove it from
 * settings.txt.
 */
void jf_sign_out(const char *name)
{
	const SavedUser *u = settings_user(name);

	if (!u)
		return;

	/* End the sign-in on the server too, so the token stops working. */
	char path[160];

	net_set_device_id(u->device);
	snprintf(path, sizeof(path), "/Sessions/Logout?api_key=%s", u->token);
	net_http_send(g_host, g_port, "POST", path, NULL);

	char n[64];

	snprintf(n, sizeof(n), "%s", name);
	settings_remove_user(n);
	jf_sign_out_local();
}

/* ------------------------------------------------------------------ */
/* Browsing                                                           */
/* ------------------------------------------------------------------ */

static const char *tab_types(void)
{
	switch (jf_tab) {
	case JF_TAB_SERIES: return "Series";

	/*
	 * Albums rather than individual tracks: a grid of six covers is a
	 * useful screen, a grid of six song titles is not.
	 */
	case JF_TAB_MUSIC:  return "MusicAlbum";

	/*
	 * Films only.
	 *
	 * The grid can show anything, but Cross on a result always opens the
	 * film detail page, so an album found here landed on a screen with a
	 * blank overview and a Play button that does nothing. Routing by item
	 * type would mean fetching Type with each result and sending albums to
	 * the track list instead; until that exists, searching one kind of
	 * thing is honest and searching three is not.
	 */
	case JF_TAB_SEARCH: return "Movie";
	default:            return "Movie";
	}
}

/* Percent-encode anything that would break a query string. */
static void url_escape(const char *in, char *out, int max)
{
	static const char hex[] = "0123456789ABCDEF";
	int o = 0;

	for (; *in && o < max - 4; in++) {
		unsigned char c = (unsigned char)*in;

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
			out[o++] = (char)c;
		} else {
			out[o++] = '%';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 0x0F];
		}
	}
	out[o] = '\0';
}

/*
 * Fields stay minimal on purpose. Overview is fetched per item by
 * jf_load_detail instead: six overviews would push this reply past the
 * ~2 KB the receive mailbox can hold, and the rest of the document would
 * be silently dropped.
 */
/*
 * One request for a run of items, appending to jf_items.
 *
 * Returns how many were added, or negative on failure. Kept small on purpose:
 * see JF_FETCH_ITEMS in config.h.
 */
static int fetch_item_run(int start_index, int limit)
{
	int   status;
	char *body;
	static char path[640];

	if (jf_tab == JF_TAB_MUSIC && jf_music_level == JF_MUSIC_ARTISTS) {
		/*
		 * Artists come from their own endpoint, not /Items. It returns
		 * MusicArtist entries with their own ids and images.
		 */
		snprintf(path, sizeof(path),
		         "/Artists?userId=%s"
		         "&SortBy=SortName&SortOrder=Ascending"
		         "&EnableImages=false&EnableUserData=false"
		         "&Limit=%d&StartIndex=%d",
		         jf_user_id, limit, start_index);
	} else if (jf_tab == JF_TAB_MUSIC && jf_music_level == JF_MUSIC_ALBUMS) {
		/*
		 * The artist's albums are its children.
		 *
		 * Neither AlbumArtistIds nor ArtistIds matches an album, because
		 * the album items' own artist arrays are empty; and AlbumArtist by
		 * name is ignored outright, returning every album in the library
		 * whichever name is given. ParentId is the one that works: it
		 * returns exactly this artist's albums, one entry each.
		 */
		snprintf(path, sizeof(path),
		         "/Items?userId=%s"
		         "&ParentId=%s"
		         "&IncludeItemTypes=MusicAlbum"
		         "&SortBy=ProductionYear,SortName&SortOrder=Ascending"
		         "&EnableImages=false&EnableUserData=false"
		         "&Fields=ProductionYear"
		         "&Limit=%d&StartIndex=%d",
		         jf_user_id, jf_artist_id, limit, start_index);
	} else if (jf_tab == JF_TAB_SERIES && jf_tv_level == JF_TV_SEASONS) {
		/*
		 * Seasons have a dedicated endpoint, and carry Primary images, so
		 * they sit in the same poster grid as everything else.
		 */
		snprintf(path, sizeof(path),
		         "/Shows/%s/Seasons?userId=%s"
		         "&EnableImages=false&EnableUserData=false"
		         "&Fields=ProductionYear",
		         jf_show_id, jf_user_id);
	} else if (jf_tab == JF_TAB_SEARCH) {
		char q[192];
		url_escape(jf_query, q, sizeof(q));

		snprintf(path, sizeof(path),
		         "/Items?userId=%s"
		         "&Recursive=true"
		         "&IncludeItemTypes=%s"
		         "&searchTerm=%s"
		         "&SortBy=SortName&SortOrder=Ascending"
		         "&EnableImages=false&EnableUserData=false"
		         "&Fields=ProductionYear"
		         "&Limit=%d&StartIndex=%d",
		         jf_user_id, tab_types(), q, limit, start_index);
	} else {
		snprintf(path, sizeof(path),
		         "/Items?userId=%s"
		         "&Recursive=true"
		         "&IncludeItemTypes=%s"
		         "&SortBy=SortName&SortOrder=Ascending"
		         "&EnableImages=false&EnableUserData=false"
		         "&Fields=ProductionYear"
		         "&Limit=%d&StartIndex=%d",
		         jf_user_id, tab_types(), limit, start_index);
	}

	int toks = get_json(path, 1, &status, &body);

	if (status != 200)
		return err("Could not list items",
		           "GET /Items failed. %s", net_last_info());

	if (toks < 1) {
		int fp = json_fail_pos();
		dump_reply("GET /Items");
		return err("Item list would not parse",
		           "/Items. %s. byte %d of %d recv calls.",
		           net_last_info(), fp, net_recv_count());
	}

	jf_total_count = json_int(json_get(0, "TotalRecordCount"));

	/* Remember the unfiltered size so letter jumps can compute an offset. */
	if (jf_tab != JF_TAB_SEARCH)
		g_library_total = jf_total_count;

	int arr = json_get(0, "Items");
	if (arr < 0 || !json_is_array(arr)) {
		dump_reply("GET /Items, no Items array");
		return err("Bad item list", "%s", net_last_info());
	}

	int added = 0;
	int j = arr + 1;

	/*
	 * The Seasons endpoint ignores Limit and StartIndex and returns every
	 * season each time, so paging has to happen here instead. Everything
	 * else has already been paged by the server.
	 */
	int skip = (jf_tab == JF_TAB_SERIES && jf_tv_level == JF_TV_SEASONS)
	           ? start_index : 0;

	for (int n = 0; n < json_size(arr) && jf_item_count < JF_PAGE; n++) {
		if (skip > 0) {
			skip--;
			j = json_skip(j);
			continue;
		}

		int vn = json_get(j, "Name");
		int vi = json_get(j, "Id");
		int vy = json_get(j, "ProductionYear");

		if (vn >= 0 && vi >= 0) {
			JfItem *it = &jf_items[jf_item_count++];
			json_str(vn, it->name, sizeof(it->name));
			json_str(vi, it->id,   sizeof(it->id));
			it->year = (vy >= 0) ? json_int(vy) : 0;   /* null is normal */
			added++;
		}
		j = json_skip(j);
	}

	return added;
}

/*
 * A page, assembled from several small requests.
 *
 * The screen shows JF_PAGE items but no single reply may approach 1460 raw
 * bytes, so the page is fetched JF_FETCH_ITEMS at a time. Jellyfin's own
 * Limit and StartIndex do the work that Range would have done, and unlike
 * Range they are actually supported.
 */
int jf_load_items(void)
{
	jf_item_count = 0;

	if (jf_tab == JF_TAB_SEARCH && !jf_query[0]) {
		jf_total_count = 0;
		return 0;                      /* nothing typed yet */
	}

	while (jf_item_count < JF_PAGE) {
		int want = JF_PAGE - jf_item_count;
		if (want > JF_FETCH_ITEMS)
			want = JF_FETCH_ITEMS;

		{
			char m[48];
			snprintf(m, sizeof(m), "Loading %d/%d",
			         jf_item_count + 1, JF_PAGE);
			net_say(m);
		}

		int added = fetch_item_run(jf_start_index + jf_item_count, want);

		if (added < 0)
			return -1;                 /* error already set */

		if (added == 0)
			break;                     /* end of the library */

		/* Past the end of the filtered set. */
		if (jf_start_index + jf_item_count >= jf_total_count)
			break;
	}

	return 0;
}

/*
 * Two requests, both tiny.
 *
 * The first counts items sorting at or after the letter. Subtracting from
 * the library total gives the index of the first one, which becomes the new
 * StartIndex. The letter is then discarded, so L1 and R1 keep working across
 * the whole library instead of being trapped inside a filter.
 */
int jf_jump_to_letter(char letter)
{
	int   status;
	char *body;
	static char path[640];

	if (jf_tab == JF_TAB_SEARCH)
		return 0;                       /* no rail on search results */

	/*
	 * No rail inside an artist. One artist's albums are few enough to page
	 * through, and jumping by letter within them would need the same
	 * AlbumArtist filter as the listing, for no real benefit.
	 */
	if (jf_tab == JF_TAB_MUSIC && jf_music_level == JF_MUSIC_ALBUMS)
		return 0;

	if (g_library_total <= 0)
		return 0;                       /* nothing loaded yet */

	if (jf_tab == JF_TAB_MUSIC) {
		/* Artists have their own endpoint, so the count does too. */
		snprintf(path, sizeof(path),
		         "/Artists?userId=%s"
		         "&SortBy=SortName&SortOrder=Ascending"
		         "&NameStartsWithOrGreater=%c"
		         "&EnableImages=false&EnableUserData=false"
		         "&Limit=1",
		         jf_user_id, letter);
	} else {
		snprintf(path, sizeof(path),
		         "/Items?userId=%s"
		         "&Recursive=true"
		         "&IncludeItemTypes=%s"
		         "&SortBy=SortName&SortOrder=Ascending"
		         "&NameStartsWithOrGreater=%c"
		         "&EnableImages=false&EnableUserData=false"
		         "&Limit=1",
		         jf_user_id, tab_types(), letter);
	}

	int toks = get_json(path, 1, &status, &body);

	if (status != 200 || toks < 1)
		return err("Jump failed",
		           "Could not count items from %c. %s", letter,
		           net_last_info());

	int at_or_after = json_int(json_get(0, "TotalRecordCount"));

	int offset = g_library_total - at_or_after;
	if (offset < 0) offset = 0;

	/* Land on a page boundary so paging stays predictable. */
	jf_start_index = (offset / JF_PAGE) * JF_PAGE;

	return jf_load_items();
}

/* ------------------------------------------------------------------ */

/*
 * RunTimeTicks is 100 ns units and overflows 32 bits after about three and a
 * half minutes, so it has to be read as text and divided as a 64-bit value.
 */
static int ticks_to_seconds(int tok)
{
	if (tok < 0)
		return 0;

	char buf[24];
	json_str(tok, buf, sizeof(buf));

	long long t = 0;
	for (const char *c = buf; *c >= '0' && *c <= '9'; c++) {
		if (t > (LLONG_MAX - (*c - '0')) / 10)
			return 0;
		t = t * 10 + (*c - '0');
	}

	return t / 10000000LL <= INT_MAX ? (int)(t / 10000000LL) : 0;
}

/* One request for a run of tracks, appending to jf_tracks. */
static int fetch_track_run(const char *album_id, int start_index, int limit)
{
	int   status;
	char *body;
	static char path[640];

	/*
	 * AlbumArtist has to be asked for; it is not among the fields Jellyfin
	 * returns by default for a track, which is why the artist line came up
	 * blank on both the track list and the player.
	 */
	snprintf(path, sizeof(path),
	         "/Items?userId=%s"
	         "&ParentId=%s"
	         "&SortBy=IndexNumber&SortOrder=Ascending"
	         "&Fields=AlbumArtist,Container"
	         "&EnableImages=false&EnableUserData=false"
	         "&Limit=%d&StartIndex=%d",
	         jf_user_id, album_id, limit, start_index);

	int toks = get_json(path, 1, &status, &body);

	if (status != 200)
		return err("Could not list tracks",
		           "GET /Items failed. %s", net_last_info());

	if (toks < 1) {
		dump_reply("GET /Items tracks");
		return err("Track list would not parse", "%s", net_last_info());
	}

	int arr = json_get(0, "Items");
	if (arr < 0 || !json_is_array(arr))
		return err("Bad track list", "%s", net_last_info());

	int added = 0;
	int j = arr + 1;

	for (int n = 0; n < json_size(arr) && jf_track_count < JF_TRACK_MAX; n++) {
		int vn = json_get(j, "Name");
		int vi = json_get(j, "Id");

		if (vn >= 0 && vi >= 0) {
			JfTrack *t = &jf_tracks[jf_track_count++];
			json_str(vn, t->name, sizeof(t->name));
			json_str(vi, t->id,   sizeof(t->id));
			t->seconds = ticks_to_seconds(json_get(j, "RunTimeTicks"));

			/*
			 * The container decides how the track is requested. libmad
			 * decodes MP3 and nothing else, so anything else has to be
			 * transcoded by the server.
			 */
			int vc = json_get(j, "Container");

			if (vc >= 0)
				json_str(vc, t->container, sizeof(t->container));
			else
				t->container[0] = '\0';
			added++;
		}

		/*
		 * The album artist is the same on every track, so take the first
		 * one that carries it, from whichever shape the library happens to use.
		 * Jellyfin exposes three, and which are populated depends on how
		 * the files were tagged:
		 *
		 *   AlbumArtist    a plain string, often absent
		 *   AlbumArtists   an array of objects, each with a Name
		 *   Artists        an array of plain strings
		 *
		 * All three come back empty for untagged files, in which case
		 * there is genuinely nothing to show.
		 */
		if (!jf_album_artist[0]) {
			int va = json_get(j, "AlbumArtist");

			if (va >= 0)
				json_str(va, jf_album_artist, sizeof(jf_album_artist));
		}

		if (!jf_album_artist[0]) {
			int va = json_get(j, "AlbumArtists");

			if (va >= 0 && json_is_array(va) && json_size(va) > 0) {
				int name = json_get(va + 1, "Name");

				if (name >= 0)
					json_str(name, jf_album_artist,
					         sizeof(jf_album_artist));
			}
		}

		if (!jf_album_artist[0]) {
			int va = json_get(j, "Artists");

			if (va >= 0 && json_is_array(va) && json_size(va) > 0)
				json_str(va + 1, jf_album_artist,
				         sizeof(jf_album_artist));
		}

		j = json_skip(j);
	}

	return added;
}

/*
 * An album's tracks into jf_tracks, in pages, with the album's artist and
 * description.
 */
int jf_load_tracks(const char *album_id, const char *album_name, int year)
{
	jf_track_count     = 0;
	jf_album_artist[0] = '\0';
	jf_album_desc[0]   = '\0';
	jf_album_year      = year;

	snprintf(jf_album_name, sizeof(jf_album_name), "%s", album_name);

	while (jf_track_count < JF_TRACK_MAX) {
		int want = JF_TRACK_MAX - jf_track_count;
		if (want > JF_FETCH_ITEMS)
			want = JF_FETCH_ITEMS;

		int added = fetch_track_run(album_id, jf_track_count, want);

		if (added < 0)
			return -1;                 /* error already set */

		if (added < want)
			break;                     /* end of the album */
	}

	return 0;
}

/* Fetch one bounded episode page, returning its SOURCE record count.
 * Missing IDs/titles are skipped without changing the next source offset. */
static int fetch_episode_run(const char *series_id, const char *season_id,
                             int start, int limit, int *total)
{
	int status = 0;
	char *body;
	static char path[640];

	snprintf(path, sizeof(path),
	         "/Shows/%s/Episodes?userId=%s"
	         "&seasonId=%s"
	         "&EnableImages=false&EnableUserData=false"
	         "&Fields=RunTimeTicks&Limit=%d&StartIndex=%d",
	         series_id, jf_user_id, season_id, limit, start);

	int toks = get_json(path, 1, &status, &body);
	if (net_last_raw_len() >= HTTP_RAW_MAX - 1) {
		dump_reply("GET /Episodes");
		return err("Episode reply too large",
		           "HTTP response buffer exhausted. Reduce JF_FETCH_EPISODES. %s",
		           net_last_info());
	}
	if (status != 200 && !g_json_transport_rc)
		return err("Could not list episodes", "HTTP request failed. %s", net_last_info());
	if (toks < 1) {
		dump_reply("GET /Episodes");
		if (g_json_transport_rc)
			return err("Could not load episodes",
			           "HTTP transport failed (%d); response may be incomplete. %s",
			           g_json_transport_rc, net_last_info());
		if (toks == -1) /* JSMN_ERROR_NOMEM */
			return err("Episode reply too complex",
			           "JSON token limit exhausted. Reduce JF_FETCH_EPISODES. %s",
			           net_last_info());
		return err("Episode list would not parse",
		           "Malformed or incomplete JSON at byte %d. %s",
		           json_fail_pos(), net_last_info());
	}
	int arr = json_get(0, "Items");
	if (arr < 0 || !json_is_array(arr))
		return err("Bad episode list", "Missing or invalid Items array. %s", net_last_info());

	int count = json_size(arr), offset, page_total;
	int vs = json_get(0, "StartIndex");
	int vt = json_get(0, "TotalRecordCount");
	if (count > limit || count > INT_MAX - start ||
	    (vs >= 0 && (!json_uint(vs, &offset) || offset != start)) ||
	    (vt >= 0 && (!json_uint(vt, &page_total) || page_total < start + count ||
	                 (*total >= 0 && page_total != *total))))
		return err("Unexpected episode page", "Invalid count, total or StartIndex at %d.", start);

	if (vt >= 0) *total = page_total;
	if (*total >= 0 && start + count > *total)
		return err("Unexpected episode page", "Records exceed TotalRecordCount at %d.", start);

	/* A short/empty page before a promised total is an incomplete response. */
	if (count < limit && *total >= 0 && start + count < *total)
		return err("Unexpected episode page", "Episode page ended before TotalRecordCount at %d.", start);

	int before = jf_track_count;
	int j = arr + 1;
	for (int n = 0; n < count; n++, j = json_skip(j)) {
		int vn = json_get(j, "Name"), vi = json_get(j, "Id");
		if (!json_is_string(vn) || !json_is_string(vi))
			continue;

		JfTrack t = {0};
		json_str(vn, t.name, sizeof(t.name));
		/* Detect overlong IDs instead of silently opening a different item. */
		char id[sizeof(t.id) + 1];
		json_str(vi, id, sizeof(id));
		if (!t.name[0] || !id[0] || strlen(id) >= sizeof(t.id))
			continue;
		strcpy(t.id, id);
		for (int i = 0; i < jf_track_count; i++)
			if (strcmp(jf_tracks[i].id, t.id) == 0)
				return err("Unexpected episode page", "Repeated episode ID at StartIndex %d.", start);
		t.seconds = ticks_to_seconds(json_get(j, "RunTimeTicks"));
		jf_tracks[jf_track_count++] = t;
	}

	/* Every nonempty page must make progress: at most JF_TRACK_MAX pages,
	 * even if the server never supplies totals and sends unusable records. */
	if (count && jf_track_count == before)
		return err("Bad episode list", "No usable episode IDs and titles at StartIndex %d.", start);
	return count;
}

/* Episodes reuse the numbered track list, in the server's season order. */
int jf_load_episodes(const char *series_id, const char *season_id,
                     const char *season_name)
{
	jf_track_count     = 0;
	jf_album_artist[0] = '\0';
	jf_album_desc[0]   = '\0';
	jf_album_year      = 0;

	snprintf(jf_album_name, sizeof(jf_album_name), "%s", season_name);

	/*
	 * The season's own synopsis, for the list header. A separate request:
	 * the episodes endpoint describes episodes, not the season, and ours
	 * have no overviews of their own anyway.
	 */
	{
		static char dpath[320];

		snprintf(dpath, sizeof(dpath),
		         "/Items?userId=%s&Ids=%s&Fields=Overview&Limit=1",
		         jf_user_id, season_id);

		int dstatus;
		char *dbody;

		if (get_json(dpath, 1, &dstatus, &dbody) > 0 && dstatus == 200) {
			int darr = json_get(0, "Items");

			if (darr >= 0 && json_is_array(darr) && json_size(darr) > 0) {
				int vo = json_get(darr + 1, "Overview");

				if (vo >= 0)
					json_str(vo, jf_album_desc,
					         sizeof(jf_album_desc));
			}
		}
	}

	int start = 0, total = -1;
	while (jf_track_count < JF_TRACK_MAX) {
		int want = JF_TRACK_MAX - jf_track_count;
		if (want > JF_FETCH_EPISODES) want = JF_FETCH_EPISODES;
		int count = fetch_episode_run(series_id, season_id, start, want, &total);
		if (count < 0) {
			jf_track_count = 0; /* Never expose a partial season as success. */
			return -1;
		}
		start += count; /* validated against overflow by fetch_episode_run */
		if (count < want || (total >= 0 && start >= total)) break;
	}
	return 0;
}

/* The picture Jellyfin will actually produce, given a box. */
int jf_video_pic_w, jf_video_pic_h;

JfStream jf_audio[JF_STREAM_MAX];
int      jf_audio_count;

JfStream jf_subs[JF_STREAM_MAX];
int      jf_sub_count;

/*
 * A three-letter code to something a person recognises.
 *
 * Jellyfin returns ISO 639-2, and only a handful appear in practice. Anything
 * unlisted is shown as it came, which is unhelpful but honest - better than
 * calling it "Unknown" and hiding which track it is.
 */
static const char *language_name(const char *code)
{
	static const struct { const char *code, *name; } t[] = {
		{ "eng", "English" },  { "fre", "French" },   { "fra", "French" },
		{ "jpn", "Japanese" }, { "spa", "Spanish" },  { "ger", "German" },
		{ "deu", "German" },   { "ita", "Italian" },  { "por", "Portuguese" },
		{ "rus", "Russian" },  { "kor", "Korean" },   { "chi", "Chinese" },
		{ "zho", "Chinese" },  { "dut", "Dutch" },    { "nld", "Dutch" },
		{ "ara", "Arabic" },   { "pol", "Polish" },   { "swe", "Swedish" },
		{ "nor", "Norwegian" },{ "dan", "Danish" },   { "fin", "Finnish" },
		{ "tur", "Turkish" },  { "hun", "Hungarian" },{ "cze", "Czech" },
		{ "ces", "Czech" },    { "gre", "Greek" },    { "heb", "Hebrew" },
		{ "tha", "Thai" },     { "vie", "Vietnamese" },
	};

	if (!code || !code[0])
		return NULL;

	for (unsigned i = 0; i < sizeof(t) / sizeof(*t); i++)
		if (strcmp(code, t[i].code) == 0)
			return t[i].name;

	return NULL;
}

/*
 * What to show for one track.
 *
 * Language first, because that is what anyone is choosing by, and the title
 * after it when there is one - a file can carry two French subtitle tracks,
 * one full and one for signs and songs, and the language alone cannot tell
 * them apart. With neither, the index at least identifies it.
 */
static void stream_label(int obj, int index, char *out, int len)
{
	char lang[16] = "", title[40] = "";

	json_str(json_get(obj, "Language"), lang,  sizeof(lang));
	json_str(json_get(obj, "Title"),    title, sizeof(title));

	const char *name = language_name(lang);

	if (name && title[0])
		snprintf(out, len, "%s (%s)", name, title);
	else if (name)
		snprintf(out, len, "%s", name);
	else if (title[0])
		snprintf(out, len, "%s", title);
	else if (lang[0])
		snprintf(out, len, "%s", lang);
	else
		snprintf(out, len, "Track %d", index);
}

/* Round down to an even number (video dimensions must be even). */
static int even_down(int v) { return (v / 2) * 2; }

/*
 * Jellyfin's own scale expression, worked out here.
 *
 *   scale=trunc(min(max(iw,ih*a),min(bw,bh*a))/2)*2
 *       : trunc(min(max(iw/a,ih),min(bw/a,bh))/2)*2
 *
 * It fits the source aspect inside the box and rounds to EVEN, not to 16 -
 * so asking for 624x352 on 16:9 material yields 624x350, which the encoder
 * then pads back up to 352. Knowing the answer in advance means the two rows
 * of padding can be cropped when drawing rather than shown.
 */
static void predict(int sw, int sh, int bw, int bh, int *pw, int *ph)
{
	if (sw <= 0 || sh <= 0) {
		*pw = bw;
		*ph = bh;
		return;
	}

	float a = (float)sw / sh;

	float cw = ((float)sw > sh * a) ? (float)sw : sh * a;
	float lw = ((float)bw < bh * a) ? (float)bw : bh * a;

	float ch = ((float)sw / a > (float)sh) ? (float)sw / a : (float)sh;
	float lh = ((float)bw / a < (float)bh) ? (float)bw / a : (float)bh;

	/*
	 * ffmpeg evaluates this in double and truncates to even, so its
	 * answer can be two pixels under ours: asking for exactly 640x480 on
	 * 4:3 material gives 638, because its width limit works out at
	 * 639.99999... rather than 640.
	 *
	 * Left alone deliberately. The picture is coded in whole macroblocks
	 * either way, so 638 is padded to 640 and the crop samples two
	 * columns of that padding - invisible at this size. Biasing the
	 * prediction down to catch it would cost two pixels on every stream
	 * to fix one that nobody can see.
	 */
	*pw = even_down((int)((cw < lw) ? cw : lw));
	*ph = even_down((int)((ch < lh) ? ch : lh));
}

/*
 * The transcode size for a source picture: the largest box within 640x480
 * with the source's aspect, both sides whole macroblocks (multiples of
 * 16), so the PS2's decoder gets exactly what it handles best.
 */
void jf_video_box(int src_w, int src_h, int *w, int *h)
{
	/*
	 * The size to ask Jellyfin for: just enough to fill what the picture
	 * covers on screen, no more.
	 *
	 * 1. Where the picture goes. The screen is the display profile's video
	 *    box (640x480 and 4:3 in 480i); a picture
	 *    of the film's intended shape (jf_detail.aspect_milli, see jf.h)
	 *    covers all of it (4:3), or the full width with bars (wider), or
	 *    the full height with bars (narrower). A 16:9 film: 640x360.
	 *
	 * 2. What to ask for. Jellyfin always keeps the file's STORED
	 *    proportions when it shrinks - which differ from the intended
	 *    ones for DVDs, whose pixels are not square. So the request has
	 *    the stored proportions, and is the smallest size of them that
	 *    covers the box from step 1 in both directions: every screen
	 *    column and line then gets its own picture pixel.
	 *
	 * 3. Within 640x480, the largest picture the decoder, its texture and
	 *    the stream are set up for.
	 *
	 * For ordinary files, stored and intended shapes are the same, so
	 * this is simply the screen box: 640x480 for 4:3, 640x360 for 16:9
	 * (it was 512x288, stretched by a quarter), 640x266 for 2.4:1. Sizes
	 * that are not whole 16-pixel macroblocks are coded with a few rows of
	 * padding, which the drawing crops (see predict and video_set_crop).
	 */
	*w = disp->video_box_w;
	*h = disp->video_box_h;

	if (src_w > 0 && src_h > 0) {
		float stored = (float)src_w / src_h;
		float shape  = jf_detail.aspect_milli > 0
		               ? jf_detail.aspect_milli / 1000.0f : stored;

		/* 1. The box on screen; 1.332 rather than 4/3, so a 4:3 shape
		 *    read as 1333/1000 counts as filling the width. */
		float dw, dh;

		if (shape >= 1.332f) {
			dw = disp->video_box_w;
			dh = disp->video_box_w / shape;
		} else {
			dh = disp->video_box_h;
			dw = disp->video_box_h * shape;
		}

		/* 2. Stored proportions, covering that box. */
		float rw = dh * stored > dw ? dh * stored : dw;
		float rh = rw / stored;

		/* 3. Within the maximum. */
		if (rw > disp->video_box_w) {
			rw = disp->video_box_w;
			rh = rw / stored;
		}
		if (rh > disp->video_box_h) {
			rh = disp->video_box_h;
			rw = rh * stored;
		}

		/* Even numbers, nearest. */
		*w = 2 * (int)(rw / 2.0f + 0.5f);
		*h = 2 * (int)(rh / 2.0f + 0.5f);

		if (*w > disp->video_box_w) *w = disp->video_box_w;
		if (*h > disp->video_box_h) *h = disp->video_box_h;
	}

	predict(src_w, src_h, *w, *h, &jf_video_pic_w, &jf_video_pic_h);

}

/* The session and device the last stream was requested with. */
static char g_play_session[24];
static char g_play_device[64];

/*
 * The URL path of a film's transcoded stream: MPEG-2 video and MP2 audio
 * in a transport stream, at jf_video_box's size, with the chosen audio and
 * burned-in subtitle tracks, from start_ticks, under a new PlaySessionId
 * and DeviceId.
 */
const char *jf_video_stream_path_at(const char *item_id, int audio_index,
                                    int sub_index, long long start_ticks)
{
	static char path[640];

	int w, h;

	jf_video_box(jf_detail.width, jf_detail.height, &w, &h);

	int n = snprintf(path, sizeof(path),
	                 "/Videos/%s/stream.ts"
	                 "?videoCodec=mpeg2video"
	                 "&audioCodec=mp2"
	                 "&audioChannels=2"
	                 "&audioSampleRate=48000"
	                 "&audioBitRate=192000"
	                 "&width=%d&height=%d"
	                 "&videoBitRate=%d",
	                 item_id, w, h, disp->video_bitrate);

	/*
	 * The frame rate, which is not optional.
	 *
	 * Without it ffmpeg takes the container's time base as the rate -
	 * 1000 for these files - and MPEG-2 cannot express that, so the
	 * encode is refused before it starts. It only bites once a subtitle
	 * filter is in the chain, but sending it always costs nothing.
	 */
	if (jf_detail.fps_milli > 0)
		n += snprintf(path + n, sizeof(path) - n, "&framerate=%d.%03d",
		              jf_detail.fps_milli / 1000,
		              jf_detail.fps_milli % 1000);

	if (audio_index >= 0)
		n += snprintf(path + n, sizeof(path) - n,
		              "&AudioStreamIndex=%d", audio_index);

	if (start_ticks > 0)
		n += snprintf(path + n, sizeof(path) - n,
		              "&startTimeTicks=%lld", start_ticks);

	/*
	 * Subtitles are burned into the picture, since there is no way to
	 * draw text over video here. Encode rather than the default, which
	 * would hand them to the client to render.
	 */
	if (sub_index >= 0)
		n += snprintf(path + n, sizeof(path) - n,
		              "&SubtitleStreamIndex=%d&SubtitleMethod=Encode",
		              sub_index);

	/*
	 * A fresh session for every playback.
	 *
	 * Jellyfin caches transcodes and will happily hand back the previous
	 * one when the request looks similar - it returned a byte-identical
	 * file for two different bitrates during testing. Choosing a
	 * different subtitle track then changed nothing on screen, because
	 * the old transcode was served again.
	 *
	 * The counter only has to differ from last time, so a simple one is
	 * enough; it survives as long as the app runs.
	 */
	static unsigned session;

	/*
	 * A fresh session AND a fresh device for every playback.
	 *
	 * Jellyfin reuses a transcode when a request looks like one it has
	 * already served - and with no DeviceId every request looks like the
	 * same device to it. The result was a Demon Slayer episode playing as
	 * a Spider-Man clip: the client asked for the right item, the id on
	 * screen proved it, and the server sent something else entirely.
	 *
	 * PlaySessionId alone was not enough to prevent it.
	 */
	++session;

	/*
	 * Unique across reboots, not just within one run.
	 *
	 * The counter alone restarts at 1 on every boot, so the first film
	 * after a power cycle asked for "ps21" again - a session id Jellyfin
	 * had already seen, possibly with another film's transcode behind it.
	 * That is how a Batman request came back as Spider-Man. ORBIT puts
	 * the time into its id for the same reason; here the CPU's cycle
	 * counter (COP0 Count) and clock() are mixed in, which differ on every
	 * boot and every playback.
	 */
	{
		unsigned count;

		__asm__ volatile("mfc0 %0, $9" : "=r"(count));

		unsigned stamp = count ^ ((unsigned)clock() * 2654435761u);

		snprintf(g_play_session, sizeof(g_play_session), "ps2%08x%04x",
		         stamp, session & 0xFFFF);
	}
	snprintf(g_play_device, sizeof(g_play_device), "ps2-%.32s-%s",
	         item_id, g_play_session + 3);

	n += snprintf(path + n, sizeof(path) - n,
	              "&PlaySessionId=%s&DeviceId=%s",
	              g_play_session, g_play_device);

	snprintf(path + n, sizeof(path) - n, "&api_key=%s", jf_key());

	return path;
}

/* The PlaySessionId of the stream last requested. */
const char *jf_play_session(void) { return g_play_session; }

/* ------------------------------------------------------------------ */
/* Reports to the server, sent in the background                      */
/* ------------------------------------------------------------------ */

/*
 * The stop report, ending the transcode and saving the resume point are
 * three requests, each on its own connection. Sent from the UI thread they
 * kept the film page waiting until all three had been answered, which was
 * most of the time Stop took.
 *
 * Nothing waits on their answers - the resume point is known here already -
 * so they are queued and a thread of their own sends them in order. Only
 * that thread calls net_http_send, whose request buffer is shared.
 */
#define REPORT_SLOTS 8

typedef struct {
	char method[8];
	char path[400];
	char alt[400];      /* tried on 404/405: older servers; "" for none */
	char body[192];
	int  is_save;       /* its status goes to jf_resume_status */
} Report;

static Report        g_rq[REPORT_SLOTS];
static volatile int  g_rq_head, g_rq_tail;   /* head: next free, tail: next to send */
static volatile int  g_rq_busy;              /* the thread is sending one */
static int           g_rq_sema   = -1;
static int           g_rq_thread = -1;
static u8            g_rq_stack[16 * 1024] __attribute__((aligned(16)));

static int g_resume_status;

/* HTTP status of the last resume-point save, for the film page. */
int jf_resume_status(void) { return g_resume_status; }

/*
 * Send one queued report, trying its alternative path on 404/405 (older
 * servers).
 */
static void report_send(const Report *r)
{
	int st = net_http_send(jf_host(), jf_port(), r->method, r->path,
	                       r->body[0] ? r->body : NULL);

	if ((st == 404 || st == 405) && r->alt[0])
		st = net_http_send(jf_host(), jf_port(), r->method, r->alt,
		                   r->body[0] ? r->body : NULL);

	if (r->is_save)
		g_resume_status = st;
}

/*
 * The background reports thread: waits on its semaphore, then sends queued
 * reports in order. The only caller of net_http_send for reports.
 */
static void reporter(void *arg)
{
	(void)arg;

	for (;;) {
		WaitSema(g_rq_sema);

		while (g_rq_tail != g_rq_head) {
			g_rq_busy = 1;
			report_send(&g_rq[g_rq_tail]);
			g_rq_tail = (g_rq_tail + 1) % REPORT_SLOTS;
			g_rq_busy = 0;
		}
	}
}

/* Create the reports thread and its semaphore, once, on first use. */
static void reporter_start(void)
{
	extern void *_gp;
	ee_sema_t   sem;
	ee_thread_t th;

	if (g_rq_thread >= 0)
		return;

	memset(&sem, 0, sizeof(sem));
	sem.init_count = 0;
	sem.max_count  = REPORT_SLOTS;
	g_rq_sema = CreateSema(&sem);

	if (g_rq_sema < 0)
		return;

	memset(&th, 0, sizeof(th));
	th.func             = reporter;
	th.stack            = g_rq_stack;
	th.stack_size       = sizeof(g_rq_stack);
	th.gp_reg           = &_gp;
	th.initial_priority = REPORT_PRIO;

	g_rq_thread = CreateThread(&th);
	freeze_register(g_rq_thread, FZ_REPORTS);

	if (g_rq_thread >= 0)
		StartThread(g_rq_thread, NULL);
}

/*
 * Queue one request. A full queue waits for a slot rather than sending from
 * here, since two threads in net_http_send would share its buffer. Only if
 * the thread never started is it sent here and now, as before.
 */
static void report(const char *method, const char *path, const char *alt,
                   const char *body, int is_save)
{
	reporter_start();

	int next = (g_rq_head + 1) % REPORT_SLOTS;
	Report *r;
	static Report now;

	if (g_rq_thread >= 0) {
		for (int t = 0; t < 5000 && next == g_rq_tail; t += 50)
			net_snooze(50);
	}

	int queued = (g_rq_thread >= 0 && next != g_rq_tail);

	if (g_rq_thread >= 0 && !queued)
		return;                        /* stuck for 5 s: drop this one */

	r = queued ? &g_rq[g_rq_head] : &now;

	snprintf(r->method, sizeof(r->method), "%s", method);
	snprintf(r->path,   sizeof(r->path),   "%s", path);
	snprintf(r->alt,    sizeof(r->alt),    "%s", alt  ? alt  : "");
	snprintf(r->body,   sizeof(r->body),   "%s", body ? body : "");
	r->is_save = is_save;

	if (!queued) {
		report_send(r);
		return;
	}

	g_rq_head = next;              /* published only once it is complete */
	SignalSema(g_rq_sema);
}

/*
 * Wait up to ms for queued reports to be sent (before quitting, or before
 * asking the server for HOME).
 */
void jf_reports_wait(int ms)
{
	for (int t = 0; t < ms && (g_rq_tail != g_rq_head || g_rq_busy); t += 50)
		net_snooze(50);
}

/* A stop waiting to be reported: see jf_video_stop_later. */
static char      g_stop_item[40];
static char      g_stop_session[24];
static char      g_stop_device[64];
static long long g_stop_pos;
static int       g_stop_pending;

/*
 * Remember a stream's stop report (item, position, session, device) to
 * send after the next stream has been requested; see the comment above.
 */
void jf_video_stop_later(const char *item_id, long long pos_ticks)
{
	if (!g_play_session[0])
		return;

	/* One already waiting goes first: two in a row must both be ended. */
	jf_video_stop_flush();

	snprintf(g_stop_item, sizeof(g_stop_item), "%s", item_id);
	snprintf(g_stop_session, sizeof(g_stop_session), "%s", g_play_session);
	snprintf(g_stop_device, sizeof(g_stop_device), "%s", g_play_device);
	g_stop_pos     = pos_ticks < 0 ? 0 : pos_ticks;
	g_stop_pending = 1;

	g_play_session[0] = '\0';
}

/* Queue the remembered stop report and the request to end its transcode. */
void jf_video_stop_flush(void)
{
	char path[400];
	char body[192];

	if (!g_stop_pending)
		return;

	g_stop_pending = 0;

	/*
	 * The stop report, as ORBIT sends it: the position lets Jellyfin
	 * keep a resume point.
	 */
	snprintf(path, sizeof(path), "/Sessions/Playing/Stopped?api_key=%s",
	         jf_key());
	snprintf(body, sizeof(body),
	         "{\"ItemId\":\"%s\",\"PositionTicks\":%lld,"
	         "\"PlayMethod\":\"Transcode\",\"PlaySessionId\":\"%s\"}",
	         g_stop_item, g_stop_pos, g_stop_session);
	report("POST", path, NULL, body, 0);

	/*
	 * And the transcode itself. Jellyfin keys it on the device and
	 * session given in the stream request; without this ffmpeg runs on
	 * after the socket closes.
	 */
	snprintf(path, sizeof(path),
	         "/Videos/ActiveEncodings?deviceId=%s&playSessionId=%s&api_key=%s",
	         g_stop_device, g_stop_session, jf_key());
	report("DELETE", path, NULL, NULL, 0);
}

/* Report a stream stopped now: stop_later then flush. */
void jf_video_stopped(const char *item_id, long long pos_ticks)
{
	jf_video_stop_later(item_id, pos_ticks);
	jf_video_stop_flush();
}

/*
 * Save the resume point (or mark the item played above 90%), in the
 * background. Returns the position kept, so the film page can show Resume
 * at once.
 */
long long jf_save_position(const char *item_id, long long pos_ticks,
                           long long runtime_ticks)
{
	char path[400];
	char alt[400];
	char body[96];

	int       played = 0;
	long long keep   = pos_ticks;

	if (runtime_ticks > 0 && pos_ticks > runtime_ticks * 9 / 10) {
		played = 1;                    /* watched: no resume point */
		keep   = 0;
	} else if (pos_ticks < 30LL * 10000000LL) {
		keep   = 0;                    /* barely started */
	}

	snprintf(body, sizeof(body),
	         "{\"PlaybackPositionTicks\":%lld%s}",
	         keep, played ? ",\"Played\":true" : "");

	/* Jellyfin 10.9 and later; the user is named in the query. */
	snprintf(path, sizeof(path),
	         "/UserItems/%s/UserData?userId=%s&api_key=%s",
	         item_id, jf_user_id, jf_key());

	/* Servers before 10.9: the same, under the user. */
	snprintf(alt, sizeof(alt),
	         "/Users/%s/Items/%s/UserData?api_key=%s",
	         jf_user_id, item_id, jf_key());

	/* In the background: the page needs only keep, known already. */
	report("POST", path, alt, body, 1);

	return keep;
}

/* ------------------------------------------------------------------ */
/* HOME                                                               */
/* ------------------------------------------------------------------ */

JfHomeItem jf_home_cw[JF_HOME_MAX];
int        jf_home_cw_count;
JfHomeItem jf_home_nu[JF_HOME_MAX];
int        jf_home_nu_count;

/* A 64-bit tick count, read as text: it overflows 32 bits. */
static long long json_ticks(int tok)
{
	char ticks[24];
	long long t = 0;

	if (tok < 0)
		return 0;

	json_str(tok, ticks, sizeof(ticks));

	for (const char *c = ticks; *c >= '0' && *c <= '9'; c++)
		t = t * 10 + (*c - '0');

	return t;
}

/* The Items array of a reply, into out. Returns how many. */
static int read_home_items(JfHomeItem *out, int max)
{
	int items = json_get(0, "Items");
	int n = 0;

	if (items < 0 || !json_is_array(items))
		return 0;

	int j = items + 1;

	for (int k = 0; k < json_size(items) && n < max; k++) {
		JfHomeItem *h = &out[n];
		char type[16] = "";

		memset(h, 0, sizeof(*h));
		json_str(json_get(j, "Id"),   h->id,   sizeof(h->id));
		json_str(json_get(j, "Name"), h->name, sizeof(h->name));
		json_str(json_get(j, "Type"), type,    sizeof(type));

		h->year          = json_int(json_get(j, "ProductionYear"));
		h->runtime_ticks = json_ticks(json_get(j, "RunTimeTicks"));

		int ud = json_get(j, "UserData");

		if (ud >= 0)
			h->pos_ticks = json_ticks(json_get(ud, "PlaybackPositionTicks"));

		if (strcmp(type, "Episode") == 0) {
			json_str(json_get(j, "SeriesName"), h->series, sizeof(h->series));
			json_str(json_get(j, "SeriesId"),   h->image_id,
			         sizeof(h->image_id));
			h->season  = json_int(json_get(j, "ParentIndexNumber"));
			h->episode = json_int(json_get(j, "IndexNumber"));
		}

		/* Episodes show their series' poster, films their own. */
		if (!h->image_id[0])
			snprintf(h->image_id, sizeof(h->image_id), "%s", h->id);

		if (h->id[0])
			n++;

		j = json_skip(j);
	}

	return n;
}

/* Continue Watching and Next Up for the signed-in user; see jf.h. */
int jf_load_home(void)
{
	static char path[384];
	int   status = 0;
	char *body;

	jf_home_cw_count = 0;
	jf_home_nu_count = 0;

	/*
	 * Started and not finished, most recent first. Jellyfin 10.9 moved
	 * this under /UserItems; older servers only have the /Users route.
	 * No images or extra fields: the posters are fetched on their own.
	 */
	snprintf(path, sizeof(path),
	         "/UserItems/Resume?userId=%s&Limit=%d&MediaTypes=Video"
	         "&EnableImages=false&EnableUserData=true&api_key=%s",
	         jf_user_id, JF_HOME_MAX, jf_key());

	int toks = get_json(path, 1, &status, &body);

	if (status == 404) {
		snprintf(path, sizeof(path),
		         "/Users/%s/Items/Resume?Limit=%d&MediaTypes=Video"
		         "&EnableImages=false&EnableUserData=true&api_key=%s",
		         jf_user_id, JF_HOME_MAX, jf_key());
		toks = get_json(path, 1, &status, &body);
	}

	if (status != 200 || toks < 1) {
		dump_reply("GET Resume");
		return err("Could not load Continue Watching",
		           "%s." DUMP_NOTE,
		           net_last_info());
	}

	jf_home_cw_count = read_home_items(jf_home_cw, JF_HOME_MAX);

	/*
	 * The next episode to watch in each show this user is following.
	 *
	 * NextUpDateCutoff is what Jellyfin's own web client sends: a year
	 * before today. Without it, Jellyfin left out shows with nothing
	 * watched yet - a newly added series, for one - and the row stayed
	 * empty where the web client showed them. Today is the server's date
	 * (net_server_date), not the PS2's clock, which is often wrong.
	 */
	{
		char cutoff[64] = "";
		int  y, m, d;

		if (net_server_date(&y, &m, &d))
			snprintf(cutoff, sizeof(cutoff),
			         "&NextUpDateCutoff=%04d-%02d-%02dT00%%3A00%%3A00Z",
			         y - 1, m, (m == 2 && d == 29) ? 28 : d);

		snprintf(path, sizeof(path),
		         "/Shows/NextUp?userId=%s&Limit=%d%s"
		         "&EnableImages=false&EnableUserData=false&api_key=%s",
		         jf_user_id, JF_HOME_MAX, cutoff, jf_key());
	}

	toks = get_json(path, 1, &status, &body);

	if (status != 200 || toks < 1) {
		dump_reply("GET NextUp");
		return err("Could not load Next Up",
		           "%s." DUMP_NOTE,
		           net_last_info());
	}

	jf_home_nu_count = read_home_items(jf_home_nu, JF_HOME_MAX);
	return 0;
}

JfHomeItem jf_next;

/* The episode after jf_detail's; see jf.h. */
int jf_find_next(void)
{
	static char path[512];
	int   status = 0;
	char *body;

	memset(&jf_next, 0, sizeof(jf_next));

	if (strcmp(jf_detail.type, "Episode") != 0 || !jf_detail.series_id[0])
		return 0;

	/*
	 * The show's episodes in order, starting from this one: the second
	 * is the next, whichever season it is in. Specials are left out, as
	 * Jellyfin's own players do.
	 */
	snprintf(path, sizeof(path),
	         "/Shows/%s/Episodes?userId=%s&StartItemId=%s&Limit=2"
	         "&IsSpecialSeason=false&EnableImages=false"
	         "&EnableUserData=false&api_key=%s",
	         jf_detail.series_id, jf_user_id, jf_detail.id, jf_key());

	int toks = get_json(path, 1, &status, &body);

	if (status != 200 || toks < 1)
		return 0;

	JfHomeItem two[2];
	int n = read_home_items(two, 2);

	/* The first is this episode; anything else means no next one. */
	if (n < 2 || strcmp(two[0].id, jf_detail.id) != 0)
		return 0;

	jf_next = two[1];
	return 1;
}

/*
 * One item's full detail into jf_detail: overview, picture size, runtime,
 * resume point, and its audio and subtitle tracks.
 */
/*
 * Jellyfin's AspectRatio string as width/height x 1000: "4:3" -> 1333,
 * "16:9" -> 1778, "1.5:1" -> 1500, "2.35:1" -> 2350. Integers and up to
 * three decimals on either side, parsed by hand (the EE builds single-
 * float, and nothing here needs more). 0 if it cannot be read.
 */
static int parse_ratio(const char *s)
{
	long long part[2] = { 0, 0 };   /* each side, times 1000 */

	for (int side = 0; side < 2; side++) {
		long long whole = 0, frac = 0;
		int digits = 0, places = 0;

		while (*s >= '0' && *s <= '9') {
			whole = whole * 10 + (*s++ - '0');
			digits++;
		}

		if (*s == '.') {
			s++;
			while (*s >= '0' && *s <= '9') {
				if (places < 3) {
					frac = frac * 10 + (*s - '0');
					places++;
				}
				s++;
				digits++;
			}
		}

		while (places < 3) {
			frac *= 10;
			places++;
		}

		if (!digits)
			return 0;

		part[side] = whole * 1000 + frac;

		if (side == 0) {
			if (*s != ':')
				return 0;
			s++;
		}
	}

	if (part[1] <= 0)
		return 0;

	return (int)(part[0] * 1000 / part[1]);
}

/*
 * The shape to draw jf_detail's picture with; see aspect_milli in jf.h.
 *
 *   Jellyfin's AspectRatio, if it is sensible (1.0 to 2.5) - the normal
 *   case, and the one that fixes DVDs.
 *
 *   But a DVD-sized file (about 704-720 wide, 480 or 576 tall) whose
 *   AspectRatio is just its stored size - square pixels, which DVD video
 *   never has - lost its note when it was converted. Its real shape is
 *   unknown: assume 4:3, what most material of that era is, and let the
 *   viewer change it.
 *
 *   No usable AspectRatio on any other file: its stored size, as before.
 */
static void decide_aspect(int reported)
{
	int w = jf_detail.width, h = jf_detail.height;
	int stored = (w > 0 && h > 0) ? (int)((long long)w * 1000 / h) : 0;
	int a = (reported >= 1000 && reported <= 2500) ? reported : 0;

	int dvd_size = (w >= 700 && w <= 720) &&
	               ((h >= 470 && h <= 480) || (h >= 560 && h <= 576));

	/* Square pixels: the reported shape is the stored one, within 1%. */
	int square = (a == 0) ||
	             (stored > 0 && a * 100 >= stored * 99 && a * 100 <= stored * 101);

	if (dvd_size && square) {
		jf_detail.aspect_milli = 1333;
		jf_detail.aspect_sure  = 0;
	} else {
		jf_detail.aspect_milli = a ? a : stored;
		jf_detail.aspect_sure  = 1;
	}
}

int jf_load_detail(const char *id)
{
	int   status;
	char *body;
	static char path[640];

	memset(&jf_detail, 0, sizeof(jf_detail));
	snprintf(jf_detail.id, sizeof(jf_detail.id), "%s", id);

	/*
	 * Overview, and the picture size for choosing a transcode that lands
	 * on whole macroblocks. ProductionYear is already in jf_items from
	 * the listing, so it is left out - this reply used to land within
	 * about 16 bytes of the 1460 boundary, though the segment fix makes
	 * that far less pressing than it was.
	 */
	snprintf(path, sizeof(path),
	         "/Items?userId=%s&Ids=%s"
	         "&Fields=Overview,Width,Height,MediaStreams"
	         "&EnableImages=false&EnableUserData=true"
	         "&Limit=1",
	         jf_user_id, id);

	int toks = get_json(path, 1, &status, &body);

	if (status != 200 || toks < 1)
		return err("Could not load details",
		           "%s after %d attempts. A long overview pushes this reply "
		           "over the segment limit.",
		           net_last_info(), JSON_ATTEMPTS);

	int arr = json_get(0, "Items");
	if (arr < 0 || !json_is_array(arr) || json_size(arr) < 1)
		return err("Could not load details", "No item returned.");

	int obj = arr + 1;

	/*
	 * Check the reply is about the item we asked for.
	 *
	 * The id is set from the argument above, so it is always right - but
	 * everything else comes from the parsed body, and a stale one leaves
	 * the right id beside another item's name and dimensions. The stream
	 * request then carries the correct id with the wrong picture size:
	 * Spider-Man asked for at 512x288, which is a 16:9 box, because the
	 * dimensions came from a Kimetsu episode.
	 *
	 * Cheap to check, and it turns a wrong film into an honest error.
	 */
	{
		char got[40] = "";

		json_str(json_get(obj, "Id"), got, sizeof(got));

		if (got[0] && strcmp(got, id) != 0)
			return err("Wrong item returned",
			           "Asked for %.12s, got %.12s. A stale reply.",
			           id, got);
	}

	json_str(json_get(obj, "Name"),     jf_detail.name,     sizeof(jf_detail.name));
	json_str(json_get(obj, "Overview"), jf_detail.overview, sizeof(jf_detail.overview));
	json_str(json_get(obj, "Type"),     jf_detail.type,     sizeof(jf_detail.type));
	json_str(json_get(obj, "SeriesId"), jf_detail.series_id,
	         sizeof(jf_detail.series_id));

	int vy = json_get(obj, "ProductionYear");
	jf_detail.year = (vy >= 0) ? json_int(vy) : 0;

	/*
	 * The source picture size, which decides what to ask the transcoder
	 * for. Two small integers, so they cost the reply almost nothing.
	 */
	int vw = json_get(obj, "Width");
	int vh = json_get(obj, "Height");

	jf_detail.width  = (vw >= 0) ? json_int(vw) : 0;
	jf_detail.height = (vh >= 0) ? json_int(vh) : 0;

	/*
	 * The audio and subtitle tracks, and the frame rate.
	 *
	 * All three come from the same list, which is why it is worth the
	 * heavier reply: about 3.6 KB typically and 10.5 KB for the busiest
	 * file to hand, against a parser that has room for far more.
	 */
	jf_audio_count = 0;
	jf_sub_count   = 0;

	int reported_aspect = 0;     /* the video stream's AspectRatio */

	int streams = json_get(obj, "MediaStreams");

	if (streams >= 0 && json_is_array(streams)) {
		int k = streams + 1;

		for (int n = 0; n < json_size(streams); n++) {
			char type[16] = "";

			json_str(json_get(k, "Type"), type, sizeof(type));

			int vi  = json_get(k, "Index");
			int idx = (vi >= 0) ? json_int(vi) : -1;

			if (strcmp(type, "Video") == 0) {
				/*
				 * Frames per second, kept as thousandths so
				 * 23.976 survives without floating point in
				 * the URL.
				 */
				int vr = json_get(k, "RealFrameRate");

				if (vr < 0)
					vr = json_get(k, "AverageFrameRate");

				if (vr >= 0)
					jf_detail.fps_milli = json_milli(vr);

				/* The intended shape: see decide_aspect. */
				{
					char ar[24];

					json_str(json_get(k, "AspectRatio"), ar, sizeof(ar));
					reported_aspect = parse_ratio(ar);
				}
			} else if (strcmp(type, "Audio") == 0 &&
			           jf_audio_count < JF_STREAM_MAX && idx >= 0) {
				JfStream *a = &jf_audio[jf_audio_count++];

				a->index = idx;
				json_str(json_get(k, "Codec"), a->codec, sizeof(a->codec));
				stream_label(k, idx, a->label, sizeof(a->label));
			} else if (strcmp(type, "Subtitle") == 0 &&
			           jf_sub_count < JF_STREAM_MAX && idx >= 0) {
				JfStream *b = &jf_subs[jf_sub_count++];

				b->index = idx;
				json_str(json_get(k, "Codec"), b->codec, sizeof(b->codec));
				stream_label(k, idx, b->label, sizeof(b->label));

				/*
				 * Text or pictures. Jellyfin says so directly;
				 * the codec is the fallback for older servers.
				 */
				{
					char t[8] = "";

					json_str(json_get(k, "IsTextSubtitleStream"),
					         t, sizeof(t));

					if (t[0])
						b->is_text = (t[0] == 't');
					else
						b->is_text =
						    strstr("pgssub dvdsub dvbsub dvd_subtitle "
						           "hdmv_pgs_subtitle vobsub xsub",
						           b->codec[0] ? b->codec : "?") == NULL;
				}
			}

			k = json_skip(k);
		}
	}

	decide_aspect(reported_aspect);

	/* Not requested any more, so take it from the listing. */
	if (jf_detail.year == 0) {
		for (int i = 0; i < jf_item_count; i++) {
			if (strcmp(jf_items[i].id, id) == 0) {
				jf_detail.year = jf_items[i].year;
				break;
			}
		}
	}

	/*
	 * RunTimeTicks is 100 ns units and overflows 32 bits past about three
	 * and a half minutes, so read it as text and divide as a 64-bit value.
	 */
	int vt = json_get(obj, "RunTimeTicks");
	if (vt >= 0) {
		char ticks[24];
		json_str(vt, ticks, sizeof(ticks));

		long long t = 0;
		for (const char *c = ticks; *c >= '0' && *c <= '9'; c++)
			t = t * 10 + (*c - '0');

		jf_detail.runtime_min   = (int)(t / 600000000LL);
		jf_detail.runtime_ticks = t;
	}

	/*
	 * Where this user left off. Read as text like the runtime: it is a
	 * 64-bit count of 100 ns ticks.
	 */
	{
		int ud = json_get(obj, "UserData");
		int vp = (ud >= 0) ? json_get(ud, "PlaybackPositionTicks") : -1;

		if (vp >= 0) {
			char ticks[24];
			long long t = 0;

			json_str(vp, ticks, sizeof(ticks));

			for (const char *c = ticks; *c >= '0' && *c <= '9'; c++)
				t = t * 10 + (*c - '0');

			jf_detail.resume_ticks = t;
		}
	}

	return 0;
}

/* ------------------------------------------------------------------ */

int jf_rail_letter(void)
{
	if (jf_item_count < 1)
		return 0;

	char c = jf_items[0].name[0];

	if (c >= 'a' && c <= 'z') c -= 32;
	return (c >= 'A' && c <= 'Z') ? c - 'A' : 0;
}