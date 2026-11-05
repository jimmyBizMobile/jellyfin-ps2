/*
 * Text subtitles: downloaded once per film, parsed into timed lines,
 * converted to Latin-1, and looked up by position for drawing.
 *
 * Two readers. SRT (and anything Jellyfin converts to it) is read whole,
 * as before. ASS and SSA are read directly (since v58), line by line as
 * they arrive: Jellyfin's conversion to SRT drops positions and drawing
 * tags, so typesetting (signs redrawn every frame, vector shapes) came
 * through as garbage - lines of coordinates, flicker, dialogue pushed off
 * the screen. Read as ASS, typesetting is recognisable and skipped, and
 * each line keeps its place: top or bottom of the screen.
 */

#include "subs.h"
#include "config.h"
#include "jf.h"
#include "net.h"
#include "text.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
	int  start, end;       /* ms */
	int  off;              /* into g_buf: Latin-1, '\n' between lines */
	int  top;              /* 1: drawn at the top of the screen */
} Cue;

static char *g_buf;        /* SRT: the file, then the cue text in place.
                              ASS: only the kept lines' text. */
static int   g_used;       /* bytes of g_buf in use (ASS) */
static Cue  *g_cue;
static int  *g_reach;      /* latest end among cues 0..k: how far back to look */
static int   g_n;          /* cues loaded, 0 for none */
static int   g_cur;        /* where the last lookup ended: a hint */

static char  g_item[40];
static int   g_index = -1;

/* Free the loaded track and forget which one it was. */
void subs_clear(void)
{
	free(g_buf);
	free(g_cue);
	free(g_reach);
	g_buf   = NULL;
	g_cue   = NULL;
	g_reach = NULL;
	g_n     = 0;
	g_used  = 0;
	g_cur   = 0;
	g_item[0] = '\0';
	g_index   = -1;
}

/* Whether a track is loaded and it belongs to item_id. */
int subs_active_for(const char *item_id)
{
	return g_n > 0 && item_id && strcmp(g_item, item_id) == 0;
}

/* ------------------------------------------------------------------ */
/* Parsing helpers                                                    */
/* ------------------------------------------------------------------ */

/* "01:02:03,456" or "02:03.456" (WebVTT drops the hours): ms, or -1. */
static int parse_time(const char *p, const char **end)
{
	int part[3] = { 0, 0, 0 };
	int n = 0;

	while (*p == ' ')
		p++;

	for (;;) {
		if (*p < '0' || *p > '9')
			return -1;

		int v = 0;

		while (*p >= '0' && *p <= '9')
			v = v * 10 + (*p++ - '0');

		if (n < 3)
			part[n++] = v;

		if (*p == ':') { p++; continue; }
		break;
	}

	int ms = 0;

	if (*p == ',' || *p == '.') {
		int digits = 0;

		p++;
		while (*p >= '0' && *p <= '9') {
			if (digits < 3) { ms = ms * 10 + (*p - '0'); digits++; }
			p++;
		}
		while (digits++ < 3)
			ms *= 10;
	}

	int h = 0, m, s;

	if (n == 3)      { h = part[0]; m = part[1]; s = part[2]; }
	else if (n == 2) { m = part[0]; s = part[1]; }
	else             return -1;

	if (end)
		*end = p;

	return ((h * 60 + m) * 60 + s) * 1000 + ms;
}

/*
 * Clean one cue's text where it lies: styling tags out (<i>, {\an8},
 * <font ...>), \N to a line break, \h (a hard space) to a space, UTF-8 to
 * Latin-1, blank lines and stray spaces trimmed. soft_nl: whether \n is a
 * line break too (SRT converted from ASS writes it so) or a space (ASS,
 * where it only breaks in a wrapping mode nobody uses). The result is
 * never longer, so it is written over the original.
 */
static int clean(char *t, int len, int soft_nl)
{
	int o = 0;

	for (int i = 0; i < len; i++) {
		char c = t[i];

		if (c == '<') {                      /* <i>, </font> ... */
			int j = i;
			while (j < len && t[j] != '>')
				j++;
			if (j < len) { i = j; continue; }
		}

		if (c == '{') {                      /* {\an8}, {\i1} ... */
			int j = i;
			while (j < len && t[j] != '}')
				j++;
			if (j < len) { i = j; continue; }
		}

		if (c == '\\' && i + 1 < len) {
			char d = t[i + 1];

			if (d == 'N' || (d == 'n' && soft_nl)) {
				t[o++] = '\n';
				i++;
				continue;
			}
			if (d == 'n' || d == 'h') {
				t[o++] = ' ';
				i++;
				continue;
			}
		}

		if (c == '\r' || c == '\t') {
			if (c == '\t')
				t[o++] = ' ';
			continue;
		}

		t[o++] = c;
	}

	o = text_utf8_to_latin1(t, o, t);

	/* Trim spaces around each line, squeeze runs of them, drop empty lines. */
	int w = 0, line_start = 0;

	for (int i = 0; i <= o; i++) {
		if (i == o || t[i] == '\n') {
			int a = line_start, b = i;

			while (a < b && t[a] == ' ') a++;
			while (b > a && t[b - 1] == ' ') b--;

			if (b > a) {
				if (w > 0)
					t[w++] = '\n';
				for (int k = a; k < b; k++) {
					if (t[k] == ' ' && w > 0 && t[w - 1] == ' ')
						continue;
					t[w++] = t[k];
				}
			}
			line_start = i + 1;
		}
	}

	t[w] = '\0';
	return w;
}

/*
 * An ASS vector drawing that lost its tag on the way through a converter:
 * nothing but drawing commands (m n l b s p c) and numbers, starting with
 * a move. "m 0 0 l 400 0 400 80 0 80" - shown as text, a line of numbers.
 */
static int is_drawing(const char *t)
{
	int digits = 0;

	if (t[0] != 'm' || t[1] != ' ')
		return 0;

	for (const char *p = t; *p; p++) {
		char c = *p;

		if (c >= '0' && c <= '9') { digits++; continue; }
		if (c == ' ' || c == '\n' || c == '.' || c == '-')
			continue;
		if (c == 'm' || c == 'n' || c == 'l' || c == 'b' ||
		    c == 's' || c == 'p' || c == 'c')
			continue;

		return 0;
	}

	return digits >= 4;
}

/*
 * Where a line goes, from an alignment tag in its raw text: {\an7..9} (or
 * SSA's {\a5..7}) mean the top. Returns 1 top, 0 bottom, -1 no tag.
 */
static int tag_top(const char *t, int len)
{
	for (int i = 0; i + 2 < len; i++) {
		if (t[i] != '\\' || t[i + 1] != 'a')
			continue;

		if (t[i + 2] == 'n' && i + 3 < len &&
		    t[i + 3] >= '1' && t[i + 3] <= '9')
			return t[i + 3] >= '7';

		if (t[i + 2] >= '1' && t[i + 2] <= '9') {
			int v = t[i + 2] - '0';

			if (i + 3 < len && t[i + 3] >= '0' && t[i + 3] <= '9')
				v = v * 10 + (t[i + 3] - '0');
			return v >= 5 && v <= 7;
		}
	}

	return -1;
}

/* A new cue; its text is already at g_buf + off. */
static void add_cue(int st, int en, int off, int top)
{
	if (g_n >= SUBS_MAX_CUES)
		return;

	g_cue[g_n].start = st;
	g_cue[g_n].end   = en;
	g_cue[g_n].off   = off;
	g_cue[g_n].top   = top;
	g_n++;
}

/* ------------------------------------------------------------------ */
/* SRT                                                                */
/* ------------------------------------------------------------------ */

/*
 * The whole file into g_buf. HTTP/1.0, so the server closes when done and
 * there is no length to trust. Returns bytes, or negative.
 */
static int fetch_srt(int s)
{
	g_buf = malloc(SUBS_MAX_BYTES + 1);

	if (!g_buf)
		return -20;

	int used = 0;

	while (used < SUBS_MAX_BYTES) {
		int n = net_stream_read(s, g_buf + used, SUBS_MAX_BYTES - used);

		if (n <= 0)
			break;               /* closed: the end. Or a stall. */

		used += n;
	}

	g_buf[used] = '\0';
	return used;
}

/*
 * Blocks of: a number, a "start --> end" line, text lines, a blank line.
 * Only the arrow line is relied on, so the numbers may be missing or
 * wrong and WebVTT's header is skipped as noise.
 */
static void parse_srt(int len)
{
	char *p   = g_buf;
	char *end = g_buf + len;

	/* A UTF-8 byte order mark. */
	if (len >= 3 && (unsigned char)p[0] == 0xEF &&
	    (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
		p += 3;

	while (p < end && g_n < SUBS_MAX_CUES) {
		char *nl = memchr(p, '\n', end - p);
		char *le = nl ? nl : end;

		char *arrow = NULL;

		for (char *q = p; q + 2 < le; q++) {
			if (q[0] == '-' && q[1] == '-' && q[2] == '>') {
				arrow = q;
				break;
			}
		}

		if (!arrow) {
			p = nl ? nl + 1 : end;
			continue;
		}

		int st = parse_time(p, NULL);
		int en = parse_time(arrow + 3, NULL);

		p = nl ? nl + 1 : end;

		/* The text: every line up to a blank one. */
		char *ts = p;

		while (p < end) {
			char *n2 = memchr(p, '\n', end - p);
			char *e2 = n2 ? n2 : end;
			char *q  = p;

			while (q < e2 && (*q == ' ' || *q == '\r' || *q == '\t'))
				q++;

			if (q == e2) {               /* blank: the cue ends */
				p = n2 ? n2 + 1 : end;
				break;
			}

			p = n2 ? n2 + 1 : end;
		}

		char *te = p;

		while (te > ts && (te[-1] == '\n' || te[-1] == '\r' ||
		                   te[-1] == ' '))
			te--;

		if (st < 0 || en <= st || te <= ts)
			continue;

		int top = tag_top(ts, (int)(te - ts)) == 1;
		int n   = clean(ts, (int)(te - ts), 1);

		if (n == 0 || is_drawing(ts))
			continue;

		add_cue(st, en, (int)(ts - g_buf), top);
	}
}

/* ------------------------------------------------------------------ */
/* ASS and SSA                                                        */
/* ------------------------------------------------------------------ */

#define ASS_LINE   8192        /* longer lines are drawings: skipped */
#define ASS_STYLES 64
#define ASS_FIELDS 16

typedef struct {
	char name[32];
	int  top;
} AssStyle;

/* The reader's state while the file streams in. */
static struct {
	int      v4plus;               /* ASS; 0 for SSA (other alignments) */
	int      section;              /* 1 styles, 2 events, 0 other */
	int      st_name, st_align;    /* field positions in Style: lines */
	int      ev_start, ev_end, ev_style, ev_effect, ev_fields;
	AssStyle style[ASS_STYLES];
	int      styles;
} A;

/* Split "a, b,c" at commas into at most max fields (the last takes the
 * rest, commas and all). Leading spaces are skipped. Returns the count. */
static int split(char *s, char **f, int max)
{
	int n = 0;

	while (n < max) {
		while (*s == ' ')
			s++;
		f[n++] = s;

		if (n == max)
			break;

		char *c = strchr(s, ',');
		if (!c)
			break;
		*c = '\0';
		s = c + 1;
	}

	return n;
}

/* Strip trailing spaces in place. */
static void rtrim(char *s)
{
	int n = (int)strlen(s);

	while (n > 0 && s[n - 1] == ' ')
		s[--n] = '\0';
}

/* A Format: line: where each field is. */
static void ass_format(char *s)
{
	char *f[ASS_FIELDS * 2];
	int   n = split(s, f, ASS_FIELDS * 2);

	for (int i = 0; i < n; i++) {
		char *c = strchr(f[i], ',');
		if (c) *c = '\0';
		rtrim(f[i]);
	}

	if (A.section == 1) {
		for (int i = 0; i < n; i++) {
			if (!strcasecmp(f[i], "Name"))      A.st_name  = i;
			if (!strcasecmp(f[i], "Alignment")) A.st_align = i;
		}
	} else if (A.section == 2) {
		A.ev_fields = n;
		for (int i = 0; i < n; i++) {
			if (!strcasecmp(f[i], "Start"))  A.ev_start  = i;
			if (!strcasecmp(f[i], "End"))    A.ev_end    = i;
			if (!strcasecmp(f[i], "Style"))  A.ev_style  = i;
			if (!strcasecmp(f[i], "Effect")) A.ev_effect = i;
		}
	}
}

/* A Style: line: its name and whether it sits at the top. */
static void ass_style(char *s)
{
	char *f[ASS_FIELDS * 2];
	int   n = split(s, f, ASS_FIELDS * 2);

	if (A.styles >= ASS_STYLES || A.st_name >= n || A.st_align >= n)
		return;

	AssStyle *st = &A.style[A.styles++];

	char *c = strchr(f[A.st_name], ',');
	if (c) *c = '\0';
	rtrim(f[A.st_name]);
	snprintf(st->name, sizeof(st->name), "%s", f[A.st_name]);

	int v = atoi(f[A.st_align]);

	/* ASS: numpad layout, 7-9 the top row. SSA: 5-7 the top. */
	st->top = A.v4plus ? (v >= 7 && v <= 9) : (v >= 5 && v <= 7);
}

/*
 * Typesetting: anything placed, moved, clipped or drawn by hand. In real
 * fansub files dialogue never uses these and signs always do.
 */
static int is_typesetting(const char *t)
{
	for (const char *p = t; *p; p++) {
		if (*p != '\\')
			continue;

		const char *q = p + 1;

		if (!strncmp(q, "pos(", 4)  || !strncmp(q, "move(", 5)  ||
		    !strncmp(q, "clip(", 5) || !strncmp(q, "iclip(", 6) ||
		    !strncmp(q, "org(", 4))
			return 1;

		if (q[0] == 'p' && q[1] >= '1' && q[1] <= '9')
			return 1;               /* \p1: a drawing follows */
	}

	return 0;
}

/* A Dialogue: line: kept as a cue, or skipped as typesetting. */
static void ass_dialogue(char *s)
{
	char *f[ASS_FIELDS];
	int   want = A.ev_fields > 0 ? A.ev_fields : 10;

	if (want > ASS_FIELDS)
		return;

	int n = split(s, f, want);

	if (n < want)
		return;

	char *text = f[want - 1];

	/* Banner and scrolling effects are typesetting too. */
	if (A.ev_effect < want - 1 &&
	    (!strncasecmp(f[A.ev_effect], "Banner", 6) ||
	     !strncasecmp(f[A.ev_effect], "Scroll", 6)))
		return;

	if (is_typesetting(text))
		return;

	int st = parse_time(f[A.ev_start], NULL);
	int en = parse_time(f[A.ev_end], NULL);

	if (st < 0 || en <= st)
		return;

	/* The style's place, unless the line says otherwise. */
	rtrim(f[A.ev_style]);

	int top = 0;
	const char *sn = f[A.ev_style];

	if (*sn == '*')
		sn++;                       /* SSA's "*Default" */

	for (int i = 0; i < A.styles; i++) {
		if (!strcasecmp(A.style[i].name, sn)) {
			top = A.style[i].top;
			break;
		}
	}

	int len = (int)strlen(text);
	int t   = tag_top(text, len);

	if (t >= 0)
		top = t;

	/* The text, cleaned, into the kept-text buffer. */
	if (g_used + len + 1 > SUBS_MAX_BYTES || g_n >= SUBS_MAX_CUES)
		return;

	char *dst = g_buf + g_used;

	memcpy(dst, text, len);

	int k = clean(dst, len, 0);

	if (k == 0 || is_drawing(dst))
		return;

	add_cue(st, en, g_used, top);
	g_used += k + 1;
}

/* One complete line of the file. */
static void ass_line(char *s)
{
	int n = (int)strlen(s);

	while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == ' '))
		s[--n] = '\0';

	if (s[0] == '[') {
		if (!strncasecmp(s, "[V4+ Styles]", 12) ||
		    !strncasecmp(s, "[V4 Styles]", 11))
			A.section = 1;
		else if (!strncasecmp(s, "[Events]", 8))
			A.section = 2;
		else
			A.section = 0;
		return;
	}

	if (!strncasecmp(s, "ScriptType:", 11)) {
		A.v4plus = strstr(s, "+") != NULL;
		return;
	}

	if (!strncasecmp(s, "Format:", 7)) {
		ass_format(s + 7);
		return;
	}

	if (A.section == 1 && !strncasecmp(s, "Style:", 6)) {
		ass_style(s + 6);
		return;
	}

	if (A.section == 2 && !strncasecmp(s, "Dialogue:", 9))
		ass_dialogue(s + 9);
}

/*
 * Read the file as it arrives, a line at a time, keeping only dialogue:
 * typesetting never takes memory, so a file of any size fits. Returns the
 * bytes read, or negative.
 */
static int fetch_ass(int s)
{
	static char chunk[4096];
	char *line = malloc(ASS_LINE + 1);

	g_buf = malloc(SUBS_MAX_BYTES + 1);

	if (!line || !g_buf) {
		free(line);
		return -20;
	}

	memset(&A, 0, sizeof(A));
	A.v4plus   = 1;                 /* until ScriptType says otherwise */
	A.st_name  = 0;
	A.st_align = 18;                /* the standard V4+ field order */
	A.ev_start = 1;
	A.ev_end   = 2;
	A.ev_style = 3;
	A.ev_effect = 8;
	A.ev_fields = 10;

	int  total = 0, ll = 0, skip = 0, first = 1;

	for (;;) {
		int n = net_stream_read(s, chunk, sizeof(chunk));

		if (n <= 0)
			break;

		total += n;

		for (int i = 0; i < n; i++) {
			char c = chunk[i];

			if (c == '\n') {
				line[ll] = '\0';

				char *p = line;

				/* A UTF-8 byte order mark. */
				if (first && ll >= 3 && (unsigned char)p[0] == 0xEF &&
				    (unsigned char)p[1] == 0xBB &&
				    (unsigned char)p[2] == 0xBF)
					p += 3;
				first = 0;

				if (!skip)
					ass_line(p);

				ll   = 0;
				skip = 0;
				continue;
			}

			if (ll < ASS_LINE)
				line[ll++] = c;
			else
				skip = 1;           /* a drawing, most likely */
		}
	}

	if (ll > 0 && !skip) {
		line[ll] = '\0';
		ass_line(line);
	}

	free(line);
	return total;
}

/* ------------------------------------------------------------------ */
/* Loading                                                            */
/* ------------------------------------------------------------------ */

/* qsort order for cues: by start time. */
static int by_start(const void *a, const void *b)
{
	return ((const Cue *)a)->start - ((const Cue *)b)->start;
}

/*
 * After reading, for both formats:
 *
 * - Sorted by start time (converted ASS often is not in order).
 * - A line repeated back to back - typesetting redrawn every frame, in a
 *   file that did not mark it - becomes one line, so it no longer
 *   flickers.
 * - Lines shown for under SUBS_MIN_MS are dropped: nobody can read them,
 *   and a burst of them is what made the rest jump.
 * - g_reach[k], the latest end among cues 0..k, tells the lookup exactly
 *   how far back an overlapping line can have started.
 */
static void finish(void)
{
	qsort(g_cue, g_n, sizeof(Cue), by_start);

	int w = 0;

	for (int i = 0; i < g_n; i++) {
		Cue *c = &g_cue[i];
		int merged = 0;

		for (int k = w - 1; k >= 0 && k >= w - 8; k--) {
			Cue *p = &g_cue[k];

			if (p->top == c->top && c->start <= p->end + 50 &&
			    c->start >= p->start &&
			    !strcmp(g_buf + p->off, g_buf + c->off)) {
				if (c->end > p->end)
					p->end = c->end;
				merged = 1;
				break;
			}
		}

		if (!merged)
			g_cue[w++] = *c;
	}

	g_n = w;
	w = 0;

	for (int i = 0; i < g_n; i++)
		if (g_cue[i].end - g_cue[i].start >= SUBS_MIN_MS)
			g_cue[w++] = g_cue[i];

	g_n = w;

	g_reach = malloc(sizeof(int) * (g_n > 0 ? g_n : 1));

	if (!g_reach) {
		g_n = 0;
		return;
	}

	int r = 0;

	for (int i = 0; i < g_n; i++) {
		if (g_cue[i].end > r)
			r = g_cue[i].end;
		g_reach[i] = r;
	}
}

/* Fetch and read one track in one format. Returns cues, or negative. */
static int load(const char *item_id, int index, int ass)
{
	char path[256];

	/*
	 * The media source is the item itself for anything with one version,
	 * which is what the rest of the client assumes too.
	 */
	snprintf(path, sizeof(path),
	         "/Videos/%s/%s/Subtitles/%d/0/Stream.%s?api_key=%s",
	         item_id, item_id, index, ass ? "ass" : "srt", jf_key());

	/*
	 * Up to 20 s for the reply to start: the first request for a track
	 * inside a large file makes Jellyfin extract it before answering.
	 * It keeps the result, so the next time is quick.
	 */
	int s = net_stream_open_wait(jf_host(), jf_port(), path, NULL, 20000);

	if (s < 0)
		return s;

	net_stream_set_timeout(s, 5000);

	g_cue = malloc(sizeof(Cue) * SUBS_MAX_CUES);

	if (!g_cue) {
		net_stream_close(s);
		return -21;
	}

	g_n = 0;

	int len;

	if (ass) {
		len = fetch_ass(s);
		net_stream_close(s);
	} else {
		len = fetch_srt(s);
		net_stream_close(s);
		if (len > 0)
			parse_srt(len);
	}

	if (len <= 0)
		return len < 0 ? len : -22;

	finish();

	return g_n > 0 ? g_n : -23;
}

/*
 * Fetch and parse subtitle track index of item_id; see subs.h. A track
 * already loaded is kept without downloading again. An ASS track that
 * cannot be read as ASS is tried again as SRT.
 */
int subs_load(const char *item_id, int index, int ass)
{
	if (g_n > 0 && g_index == index && strcmp(g_item, item_id) == 0)
		return g_n;                  /* this one is loaded already */

	subs_clear();

	int n = load(item_id, index, ass);

	if (n <= 0 && ass) {
		subs_clear();
		n = load(item_id, index, 0);
	}

	if (n <= 0) {
		subs_clear();
		return n < 0 ? n : -23;
	}

	snprintf(g_item, sizeof(g_item), "%s", item_id);
	g_index = index;
	g_cur   = 0;

	return n;
}

/* ------------------------------------------------------------------ */
/* Lookup                                                             */
/* ------------------------------------------------------------------ */

/* Join up to max texts (oldest first) into out, a line break between. */
static const char *join(const char **t, int n, char *out, int size)
{
	if (n == 0)
		return NULL;

	int o = 0;

	out[0] = '\0';

	for (int i = 0; i < n && o < size - 1; i++)
		o += snprintf(out + o, size - o, "%s%s", i ? "\n" : "", t[i]);

	return out;
}

const char *subs_at(int ms, const char **top)
{
	static char bot_text[1024], top_text[512];

	if (top)
		*top = NULL;

	if (g_n <= 0 || ms < 0)
		return NULL;

	/*
	 * The last cue starting at or before ms. Playing forward that is the
	 * hint or just after it; after a seek, a binary search.
	 */
	int i = g_cur;

	if (i >= g_n)
		i = 0;

#define HOLDS(k) (g_cue[k].start <= ms && \
                  ((k) + 1 >= g_n || g_cue[(k) + 1].start > ms))

	if (HOLDS(i)) {
		/* still the same */
	} else if (i + 1 < g_n && HOLDS(i + 1)) {
		i++;
	} else {
		int lo = 0, hi = g_n - 1;

		i = -1;
		while (lo <= hi) {
			int mid = (lo + hi) / 2;

			if (g_cue[mid].start <= ms) { i = mid; lo = mid + 1; }
			else                         hi = mid - 1;
		}

		if (i < 0)
			return NULL;
	}

#undef HOLDS

	g_cur = i;

	/*
	 * Every line showing now: back from i while some earlier line can
	 * still be running (g_reach). Several speakers at once are shown
	 * together, the earliest highest, up to SUBS_MAX_SHOWN at the bottom
	 * and two at the top; past that the newest wait.
	 */
	int found[16];
	int nf = 0;

	for (int k = i; k >= 0 && g_reach[k] > ms && nf < 16; k--)
		if (g_cue[k].start <= ms && ms < g_cue[k].end)
			found[nf++] = k;

	const char *b[SUBS_MAX_SHOWN], *t[2];
	int nb = 0, nt = 0;

	for (int j = nf - 1; j >= 0; j--) {      /* oldest first */
		const Cue *c = &g_cue[found[j]];

		if (c->top) {
			if (nt < 2)
				t[nt++] = g_buf + c->off;
		} else if (nb < SUBS_MAX_SHOWN) {
			b[nb++] = g_buf + c->off;
		}
	}

	if (top)
		*top = join(t, nt, top_text, sizeof(top_text));

	return join(b, nb, bot_text, sizeof(bot_text));
}
