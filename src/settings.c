/*
 * settings.txt beside the ELF: reading it (server, network, video, signed-
 * in users) and saving changes safely on the PS2's USB driver - update in
 * place, a settings.bak backup, and a read-back check.
 */

#include "settings.h"
#include "config.h"
#include "net.h"       /* net_snooze, to let writes land */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

Settings settings;

static void backup_path(char *out, int max);
static int  read_file(const char *path, char *buf, int max);
static int  write_file(const char *path, const char *data, int len);

static char g_path[288];
static int  g_found;
static int  g_ip_from_file;   /* a valid ip= line was read */
static char g_notice[320];

/* Where settings.txt is (beside the ELF). */
const char *settings_path(void)   { return g_path; }

/* Whether settings.txt existed at start-up (or has been written since). */
int         settings_found(void)  { return g_found; }

/*
 * The first problem found in settings.txt, or a failed save, for the
 * connect screen.
 */
const char *settings_notice(void) { return g_notice; }

/* Keep the first problem only: later ones would hide it. */
static void note(const char *fmt, const char *a)
{
	if (g_notice[0])
		return;                     /* the first problem is enough */

	snprintf(g_notice, sizeof(g_notice), fmt, a);
}

/* The directory of the ELF, plus name. */
static void beside_elf(char *out, int max, int argc, char **argv,
                       const char *name)
{
	const char *base = (argc > 0 && argv[0]) ? argv[0] : "";
	int cut = -1;

	for (int i = 0; base[i]; i++)
		if (base[i] == '/' || base[i] == '\\' || base[i] == ':')
			cut = i;

	if (cut < 0) {
		snprintf(out, max, "mass:/%s", name);
		return;
	}

	int n = cut + 1;

	if (n > max - 1)
		n = max - 1;

	memcpy(out, base, n);
	out[n] = '\0';
	strncat(out, name, max - strlen(out) - 1);
}

/* Strip spaces and line ends from both ends, in place. */
static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;

	int n = strlen(s);

	while (n > 0 && (unsigned char)s[n - 1] <= ' ')
		s[--n] = '\0';

	return s;
}

/* Case-insensitive comparison of a key with a lower-case name. */
static int key_is(const char *k, const char *want)
{
	for (; *k && *want; k++, want++)
		if (tolower((unsigned char)*k) != *want)
			return 0;

	return *k == '\0' && *want == '\0';
}

/* Four numbers from 0 to 255, separated by dots. */
static int is_ip(const char *s)
{
	int parts = 0;

	while (*s) {
		int v = 0, digits = 0;

		while (*s >= '0' && *s <= '9') {
			v = v * 10 + (*s++ - '0');
			digits++;
		}

		if (digits == 0 || digits > 3 || v > 255)
			return 0;

		parts++;

		if (*s == '.')
			s++;
		else if (*s)
			return 0;
	}

	return parts == 4;
}

/* Every setting at its default, before the file is read. */
static void defaults(void)
{
	memset(&settings, 0, sizeof(settings));

	settings.network = NET_AUTO;

	/* No address of anyone's: ip= and gateway= come from the file. */
	snprintf(settings.mask, sizeof(settings.mask), "255.255.255.0");

	settings.end_margin_kb = VIDEO_TAIL_BYTES / 1024;
	settings.end_early_ms  = VIDEO_END_EARLY_MS;
}

/*
 * Apply one key=value line: validate it, store it, or note the line number
 * of a bad value. Unknown keys are ignored.
 */
static void apply(const char *key, char *val, int lineno)
{
	char where[24];

	snprintf(where, sizeof(where), "line %d", lineno);

	if (key_is(key, "server")) {
		/* http:// is optional; the rest of the app adds it. */
		if (strncmp(val, "http://", 7) == 0)
			val += 7;

		int n = strlen(val);

		while (n > 0 && val[n - 1] == '/')
			val[--n] = '\0';

		snprintf(settings.server, sizeof(settings.server), "%s", val);
		return;
	}

	if (key_is(key, "network")) {
		for (char *p = val; *p; p++)
			*p = tolower((unsigned char)*p);

		if      (strcmp(val, "auto")   == 0) settings.network = NET_AUTO;
		else if (strcmp(val, "dhcp")   == 0) settings.network = NET_DHCP;
		else if (strcmp(val, "static") == 0) settings.network = NET_STATIC;
		else note("settings.txt: network= not understood (%s)", where);
		return;
	}

	if (key_is(key, "ip") || key_is(key, "mask") || key_is(key, "gateway")) {
		if (!is_ip(val)) {
			note("settings.txt: not an address (%s)", where);
			return;
		}

		if (key_is(key, "ip"))
			g_ip_from_file = 1;

		char *dst = key_is(key, "ip")   ? settings.ip :
		            key_is(key, "mask") ? settings.mask : settings.gateway;

		snprintf(dst, 16, "%s", val);
		return;
	}

	if (key_is(key, "video")) {
		for (char *p = val; *p; p++)
			*p = tolower((unsigned char)*p);

		if      (strcmp(val, "480i") == 0) settings.video_mode = VIDEO_480I;
		else if (strcmp(val, "480p") == 0) settings.video_mode = VIDEO_480P;
		else if (strcmp(val, "240p") == 0) settings.video_mode = VIDEO_240P;
		else if (strcmp(val, "1080i") == 0) settings.video_mode = VIDEO_1080I;
		else note("settings.txt: video= must be 480i, 480p, 240p or 1080i (%s)",
		          where);
		return;
	}

	if (key_is(key, "remote")) {
		for (char *p = val; *p; p++)
			*p = tolower((unsigned char)*p);

		if      (strcmp(val, "auto") == 0) settings.remote = REMOTE_AUTO;
		else if (strcmp(val, "off")  == 0) settings.remote = REMOTE_OFF;
		else note("settings.txt: remote= must be auto or off (%s)", where);
		return;
	}

	if (key_is(key, "end_margin_kb") || key_is(key, "end_early_ms")) {
		int v = 0, digits = 0;

		for (const char *p = val; *p >= '0' && *p <= '9'; p++, digits++)
			v = v * 10 + (*p - '0');

		int max = key_is(key, "end_margin_kb") ? 1024 : 5000;

		if (digits == 0 || val[digits] != '\0' || v > max) {
			note("settings.txt: not a number in range (%s)", where);
			return;
		}

		if (key_is(key, "end_margin_kb"))
			settings.end_margin_kb = v;
		else
			settings.end_early_ms = v;
		return;
	}

	if (key_is(key, "user")) {
		if (settings.user_count >= SETTINGS_MAX_USERS)
			return;

		SavedUser u;
		char *f[4];
		int   k = 0;

		memset(&u, 0, sizeof(u));
		f[k++] = val;

		for (char *p = val; *p && k < 4; p++) {
			if (*p == '|') {
				*p = '\0';
				f[k++] = p + 1;
			}
		}

		if (k < 4 || !f[0][0] || !f[1][0] || !f[2][0] || !f[3][0]) {
			note("settings.txt: a user= line is incomplete (%s)", where);
			return;
		}

		snprintf(u.name,   sizeof(u.name),   "%s", f[0]);
		snprintf(u.id,     sizeof(u.id),     "%s", f[1]);
		snprintf(u.token,  sizeof(u.token),  "%s", f[2]);
		snprintf(u.device, sizeof(u.device), "%s", f[3]);
		settings.users[settings.user_count++] = u;
		return;
	}

	/* Unknown keys are ignored, so newer files work with older builds. */
}

/*
 * Read settings.txt beside the ELF into 'settings', restoring it from
 * settings.bak if it is empty. Called before the IOP reset, while the
 * launcher's USB driver still works.
 */
void settings_load(int argc, char **argv)
{
	defaults();
	g_notice[0]    = '\0';
	g_ip_from_file = 0;

	beside_elf(g_path, sizeof(g_path), argc, argv, "settings.txt");

	static char text[8192];
	int n = read_file(g_path, text, sizeof(text));

	/*
	 * Empty or missing, but a backup from the last save is there: a save
	 * was cut short (power off right after signing in, say). Use the
	 * backup, and put it back as settings.txt. Judged by reading, not by
	 * seeking to the end: the size a seek reports cannot be trusted on
	 * every driver.
	 */
	if (n <= 0) {
		char bak[300];

		backup_path(bak, sizeof(bak));

		int b = read_file(bak, text, sizeof(text));

		if (b > 0) {
			n = b;

			if (write_file(g_path, text, n) == 0)
				snprintf(g_notice, sizeof(g_notice),
				         "settings.txt was empty: restored from settings.bak");
		}
	}

	g_found = (n >= 0);

	if (n <= 0)
		return;

	text[n] = '\0';

	char *p      = text;
	int   lineno = 0;

	while (*p) {
		char *nl = strchr(p, '\n');

		if (nl)
			*nl = '\0';

		lineno++;

		char line[256];
		int  ll = strlen(p);

		if (ll > (int)sizeof(line) - 1)
			ll = sizeof(line) - 1;
		memcpy(line, p, ll);
		line[ll] = '\0';
		p = nl ? nl + 1 : p + strlen(p);

		char *s = trim(line);

		if (!*s || *s == '#')
			continue;

		char *eq = strchr(s, '=');

		if (!eq) {
			char where[24];
			snprintf(where, sizeof(where), "line %d", lineno);
			note("settings.txt: no '=' on %s", where);
			continue;
		}

		*eq = '\0';

		char *key = trim(s);
		char *val = trim(eq + 1);

		apply(key, val, lineno);
	}

	/*
	 * static means the file's own address. The build's default would put
	 * the console somewhere nobody asked for, so with no valid ip= line
	 * there is no address at all, and connecting says so.
	 */
	if (settings.network == NET_STATIC && !g_ip_from_file) {
		settings.ip[0] = '\0';
		note("settings.txt: network=static needs a valid ip= line%s", "");
	}

	/*
	 * No gateway: the server, if it is an address, else the .1 of the
	 * PS2's own network. On a direct cable it goes nowhere anyway, but
	 * lwIP wants a plausible one.
	 */
	if (settings.ip[0] && !settings.gateway[0]) {
		char host[32];
		int  i = 0;

		while (settings.server[i] && settings.server[i] != ':' &&
		       i < (int)sizeof(host) - 1) {
			host[i] = settings.server[i];
			i++;
		}
		host[i] = '\0';

		if (is_ip(host)) {
			snprintf(settings.gateway, sizeof(settings.gateway), "%.15s", host);
		} else {
			snprintf(settings.gateway, sizeof(settings.gateway), "%s",
			         settings.ip);
			char *dot = strrchr(settings.gateway, '.');
			if (dot)
				snprintf(dot + 1, 4, "1");
		}
	}
}

/* ------------------------------------------------------------------ */
/* Saving                                                             */
/* ------------------------------------------------------------------ */

/*
 * Rewrite settings.txt with one kind of line changed, leaving every other
 * line exactly as it was, comments included:
 *   key   the line's key, "server" or "user"
 *   who   for user lines, the name the line must start with; NULL for any
 *   line  the replacement line, or NULL to remove the matching line(s)
 * A replacement with nothing to replace is added at the end. The file is
 * small, so it is read whole, then written back.
 */
/* The same folder, another name: settings.bak. */
static void backup_path(char *out, int max)
{
	snprintf(out, max, "%s", g_path);

	int n = strlen(out);

	if (n > 4 && strcmp(out + n - 4, ".txt") == 0)
		snprintf(out + n - 4, max - (n - 4), ".bak");
	else
		strncat(out, ".bak", max - n - 1);
}

/*
 * Reading and writing whole files.
 *
 * The USB FAT driver buffers writes: fclose alone has left files empty
 * (seen first with jfdebug.txt), so every write is flushed and then given
 * time to reach the stick.
 *
 * Two ways of replacing a file have gone wrong on hardware: opening an
 * existing settings.txt with "w" (which truncates it) left it empty, and
 * deleting it and creating it again left a FOLDER called settings.txt.
 * Creating a file that does not exist yet has always worked (settings.bak,
 * jfdebug.txt). So an existing file is never truncated or deleted: it is
 * opened for update ("r+") and written over from the start, and if the new
 * text is shorter, the rest is filled with blank lines, which the reader
 * skips. Only a file that does not exist is created. What was written is
 * then read back to make sure.
 */
static int read_file(const char *path, char *buf, int max)
{
	FILE *f = fopen(path, "r");

	if (!f)
		return -1;

	int n = 0;

	for (;;) {
		int r = fread(buf + n, 1, max - 1 - n, f);

		if (r <= 0)
			break;
		n += r;
		if (n >= max - 1)
			break;
	}

	fclose(f);
	buf[n] = '\0';
	return n;
}

/*
 * Flush and close a file, then wait for the USB FAT driver to actually
 * write it.
 */
static void settle(FILE *f)
{
	fflush(f);
	fclose(f);
	net_snooze(1500);
}

/* The file's length, by reading it: a seek's answer is not trusted. */
static int file_length(const char *path)
{
	static char tmp[8192];

	return read_file(path, tmp, sizeof(tmp));
}

/*
 * Write data to path: update an existing file in place (padding with blank
 * lines if shorter), or create a new one. Never truncates or deletes; see
 * the comment above.
 */
static int write_once(const char *path, const char *data, int len)
{
	int   old = file_length(path);          /* -1: no such file */
	FILE *f   = fopen(path, old >= 0 ? "r+" : "w");

	if (!f)
		return -1;

	int w = fwrite(data, 1, len, f);

	/* Shorter than before: blank lines over the rest. */
	for (int i = len; i < old; i++)
		fputc('\n', f);

	settle(f);
	return w == len ? 0 : -1;
}

/*
 * write_once, then read back and compare; once more if it did not stick. 0
 * on success, else a notice.
 */
static int write_file(const char *path, const char *data, int len)
{
	static char check[8192];

	for (int attempt = 0; attempt < 2; attempt++) {
		if (write_once(path, data, len) < 0)
			continue;

		int got = read_file(path, check, sizeof(check));

		/* What was written, then only padding after it. */
		int ok = got >= len && memcmp(check, data, len) == 0;

		for (int i = len; ok && i < got; i++)
			if (check[i] != '\n')
				ok = 0;

		if (ok)
			return 0;

		/* Not what was written: once more, after a longer pause. */
		net_snooze(1500);
	}

	snprintf(g_notice, sizeof(g_notice), "Could not save %s", path);
	return -1;
}

/*
 * Rewrite settings.txt with one line changed, added or removed, every
 * other line kept; the previous version goes to settings.bak first.
 */
static int rewrite(const char *key, const char *who, const char *line)
{
	static char old[8192];
	static char out[8192];
	int klen = strlen(key);
	int o = 0;

	int used = read_file(g_path, old, sizeof(old));

	if (used < 0)
		used = 0;

	/*
	 * The file as it was, kept as settings.bak before it is rewritten. If
	 * a save is ever cut short, the next start restores from it.
	 */
	if (used > 0) {
		char bak[300];

		backup_path(bak, sizeof(bak));
		write_file(bak, old, used);
	}

#define PUT(...) do { if (o < (int)sizeof(out) - 1) \
	o += snprintf(out + o, sizeof(out) - o, __VA_ARGS__); } while (0)

	if (!used)
		PUT("# Jellyfin PS2 settings\n\n");

	int   done  = 0;
	int   blank = 0;                /* blank lines held back, see below */
	char *p     = old;

	while (*p) {
		char *nl  = strchr(p, '\n');
		int   len = nl ? (int)(nl - p) : (int)strlen(p);
		char *q   = p;

		/*
		 * Blank lines: a run of them is kept as one, and only when more
		 * text follows. The padding left by a shorter save (see
		 * write_once) is shed this way at the next one, instead of
		 * piling up.
		 */
		{
			int   empty = 1;

			for (char *c = p; c < p + len; c++)
				if (*c != ' ' && *c != '\t' && *c != '\r')
					empty = 0;

			if (empty) {
				blank = 1;
				if (!nl)
					break;
				p = nl + 1;
				continue;
			}

			if (blank && o > 0)
				PUT("\n");
			blank = 0;
		}

		while (q < p + len && (*q == ' ' || *q == '\t'))
			q++;

		/* key, optional spaces, '=' */
		int match = (p + len - q > klen && strncasecmp(q, key, klen) == 0);

		if (match) {
			char *r = q + klen;

			while (r < p + len && (*r == ' ' || *r == '\t'))
				r++;

			match = (r < p + len && *r == '=');

			if (match && who) {
				r++;
				while (r < p + len && (*r == ' ' || *r == '\t'))
					r++;

				int wl = strlen(who);

				match = (p + len - r > wl && strncmp(r, who, wl) == 0 &&
				         r[wl] == '|');
			}
		}

		if (match) {
			if (line && !done)
				PUT("%s\n", line);
			done = 1;                       /* duplicates are dropped */
		} else {
			PUT("%.*s\n", len, p);
		}

		if (!nl)
			break;
		p = nl + 1;
	}

	if (line && !done)
		PUT("%s\n", line);

#undef PUT

	if (o >= (int)sizeof(out))
		o = sizeof(out) - 1;

	if (write_file(g_path, out, o) < 0)
		return -1;

	g_found = 1;
	return 0;
}

/* Save server= if it changed. */
int settings_save_server(const char *server)
{
	const char *v = server;

	if (strncmp(v, "http://", 7) == 0)
		v += 7;

	/* Unchanged: nothing to write. */
	if (g_found && strcmp(settings.server, v) == 0)
		return 0;

	char line[160];

	snprintf(line, sizeof(line), "server=%s", v);

	if (rewrite("server", NULL, line) < 0)
		return -1;

	snprintf(settings.server, sizeof(settings.server), "%s", v);
	return 0;
}

/* The saved sign-in of a user, by name, or NULL. */
const SavedUser *settings_user(const char *name)
{
	for (int i = 0; i < settings.user_count; i++)
		if (strcmp(settings.users[i].name, name) == 0)
			return &settings.users[i];

	return NULL;
}

/* Add or replace a user= line, in the file and in memory. */
int settings_save_user(const SavedUser *u)
{
	char line[300];

	snprintf(line, sizeof(line), "user=%s|%s|%s|%s",
	         u->name, u->id, u->token, u->device);

	if (rewrite("user", u->name, line) < 0)
		return -1;

	/* And in memory. */
	for (int i = 0; i < settings.user_count; i++) {
		if (strcmp(settings.users[i].name, u->name) == 0) {
			settings.users[i] = *u;
			return 0;
		}
	}

	if (settings.user_count < SETTINGS_MAX_USERS)
		settings.users[settings.user_count++] = *u;

	return 0;
}

/* Remove a user= line, in the file and in memory. */
int settings_remove_user(const char *name)
{
	int rc = rewrite("user", name, NULL);

	for (int i = 0; i < settings.user_count; i++) {
		if (strcmp(settings.users[i].name, name) == 0) {
			settings.users[i] = settings.users[--settings.user_count];
			break;
		}
	}

	return rc;
}
