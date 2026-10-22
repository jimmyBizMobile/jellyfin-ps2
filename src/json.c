/*
 * Thin layer over jsmn.
 *
 * jsmn hands back a flat array of tokens in document order. A token holds
 * no text, only a byte range into the response buffer, plus `size`:
 *
 *   JSMN_OBJECT     size = number of key/value PAIRS
 *   JSMN_ARRAY      size = number of elements
 *   JSMN_STRING     size = 1 when used as a key (it owns the next token)
 *   JSMN_PRIMITIVE  size = 0   (numbers, true, false, null)
 *
 * A token's children follow it immediately in the array. That single fact
 * is what makes json_skip and json_get short.
 */

#include "json.h"
#include "text.h"
#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <limits.h>

#include "jsmn.h"

static jsmntok_t  g_tok[JSON_TOK_MAX];
static char      *g_js;
static int        g_count;

static int g_fail_pos;

/* Byte offset where the parse gave up. Compared against HTTP_CHUNK this
 * says whether a hole lands on a read boundary. */
int json_fail_pos(void) { return g_fail_pos; }

/*
 * Tokenise a JSON reply with jsmn into the module's token table. Returns
 * the number of tokens, or a negative jsmn error (too many tokens, or a
 * truncated reply). The body is not copied: tokens point into it, so it
 * must stay valid while the tokens are used.
 */
int json_parse(char *body)
{
	jsmn_parser p;
	jsmn_init(&p);

	g_js = body;

	/*
	 * Negative returns: JSMN_ERROR_NOMEM means raise JSON_TOK_MAX,
	 * JSMN_ERROR_PART means the response was truncated, so raise
	 * HTTP_RAW_MAX. Both surface as "Reply too large" in the UI.
	 */
	g_count = jsmn_parse(&p, body, strlen(body), g_tok, JSON_TOK_MAX);

	/* jsmn leaves its cursor where it stopped. */
	g_fail_pos = (g_count < 1) ? (int)p.pos : -1;

	return g_count;
}

/*
 * Children of token i: elements for an array, key/value PAIRS for an
 * object.
 */
int json_size(int i)      { return (i >= 0 && i < g_count) ? g_tok[i].size : 0; }

/* Whether token i is an array. */
int json_is_array(int i)  { return (i >= 0 && i < g_count) && g_tok[i].type == JSMN_ARRAY; }
int json_is_string(int i) { return (i >= 0 && i < g_count) && g_tok[i].type == JSMN_STRING; }

int json_uint(int tok, int *out)
{
	if (tok < 0 || tok >= g_count || g_tok[tok].type != JSMN_PRIMITIVE)
		return 0;

	int start = g_tok[tok].start, end = g_tok[tok].end, value = 0;
	if (start == end) return 0;
	for (int i = start; i < end; i++) {
		int digit = g_js[i] - '0';
		if (digit < 0 || digit > 9 || value > (INT_MAX - digit) / 10)
			return 0;
		value = value * 10 + digit;
	}
	*out = value;
	return 1;
}

/*
 * Step past every child, recursively. A leaf has size 0, never enters the
 * loop, and returns i+1 naturally. Because a key has size 1, skipping a
 * key also skips its value, which is what lets json_get walk keys only.
 */
int json_skip(int i)
{
	if (i < 0 || i >= g_count) return i + 1;

	int j = i + 1;
	int n = g_tok[i].size;
	while (n-- > 0)
		j = json_skip(j);
	return j;
}

/* Whether token i is the string s, exactly. */
static int tok_eq(int i, const char *s)
{
	int n = g_tok[i].end - g_tok[i].start;
	return g_tok[i].type == JSMN_STRING &&
	       (int)strlen(s) == n &&
	       strncmp(g_js + g_tok[i].start, s, n) == 0;
}

/*
 * The value token for key in object obj, or -1. Walks the object's pairs
 * with json_skip, so nested values are stepped over correctly.
 */
int json_get(int obj, const char *key)
{
	if (obj < 0 || obj >= g_count || g_tok[obj].type != JSMN_OBJECT)
		return -1;

	int j = obj + 1;                            /* first key */

	for (int n = 0; n < g_tok[obj].size; n++) { /* size = number of PAIRS */
		if (tok_eq(j, key))
			return j + 1;                       /* value is always next */
		j = json_skip(j);                       /* past key AND its value */
	}
	return -1;
}

/*
 * .NET escapes non-ASCII by default, so "Les Nouveaux Heros" arrives as
 * H\u00E9ros. Decode escapes here, uppercase or lowercase hex.
 */
void json_str(int tok, char *out, int max)
{
	out[0] = '\0';
	if (tok < 0 || tok >= g_count) return;

	int si = g_tok[tok].start, se = g_tok[tok].end, o = 0;

	while (si < se && o < max - 1) {
		char c = g_js[si];

		if (c != '\\') { out[o++] = c; si++; continue; }

		si++;
		if (si >= se) break;

		switch (g_js[si]) {
		case 'n': case 't': out[o++] = ' '; si++; break;
		case 'r': si++; break;

		case 'u': {
			if (si + 4 >= se) { si = se; break; }

			unsigned int cp = 0;
			for (int k = 1; k <= 4; k++) {
				char h = g_js[si + k];
				cp <<= 4;
				if      (h >= '0' && h <= '9') cp |= h - '0';
				else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
				else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
			}
			si += 5;

			/* Latin-1, which the font draws: see text.h. */
			if (o + 3 >= max) { si = se; break; }
			o += text_put_cp(cp, out + o);
			break;
		}

		default:
			out[o++] = g_js[si];
			si++;
			break;
		}
	}
	out[o] = '\0';
}

/*
 * A decimal number token as thousandths (23.976 -> 23976), parsed by hand;
 * see the comment inside.
 */
int json_milli(int tok)
{
	/*
	 * A decimal number as thousandths.
	 *
	 * For frame rates: 23.976025 becomes 23976. Parsed by hand rather
	 * than with atof, because the EE builds with -msingle-float and the
	 * value ends up in a URL as an integer anyway.
	 */
	if (tok < 0 || tok >= g_count) return 0;

	char b[24];
	int n = g_tok[tok].end - g_tok[tok].start;

	if (n > 23) n = 23;

	memcpy(b, g_js + g_tok[tok].start, n);
	b[n] = '\0';

	int whole = 0, frac = 0, scale = 1000, i = 0, neg = 0;

	if (b[i] == '-') { neg = 1; i++; }

	for (; b[i] >= '0' && b[i] <= '9'; i++)
		whole = whole * 10 + (b[i] - '0');

	if (b[i] == '.') {
		for (i++; b[i] >= '0' && b[i] <= '9' && scale > 1; i++) {
			scale /= 10;
			frac  += (b[i] - '0') * scale;
		}
	}

	int v = whole * 1000 + frac;

	return neg ? -v : v;
}

/* A number token as an int; 'null' and missing values give 0. */
int json_int(int tok)
{
	if (tok < 0 || tok >= g_count) return 0;

	char b[16];
	int n = g_tok[tok].end - g_tok[tok].start;
	if (n > 15) n = 15;
	memcpy(b, g_js + g_tok[tok].start, n);
	b[n] = '\0';

	return atoi(b);      /* "null" parses to 0, which is what we want */
}