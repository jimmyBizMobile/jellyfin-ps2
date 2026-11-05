#ifndef SETTINGS_H
#define SETTINGS_H

/*
 * settings.txt, beside the ELF: the server, the PS2's network setup and
 * the video mode, as key=value lines. # starts a comment, blank lines and
 * unknown keys are ignored, keys are not case-sensitive, and anything
 * missing takes its default - so a file from an older version still reads.
 *
 *   server=192.168.50.1:8096
 *   network=auto          auto, dhcp or static
 *   ip=192.168.50.90      static, or auto's fallback
 *   mask=255.255.255.0
 *   gateway=192.168.50.1
 *   video=480i            480i, 480p, 240p or 1080i (the others ask to confirm)
 *   remote=auto           auto (DVD remote, built-in receiver) or off
 *   user=Name|id|token|device   one per signed-in user, written by the app
 *   end_margin_kb=32      for tuning how films end (see tuto.txt)
 *   end_early_ms=200
 *
 * With no file, or no ip= line: the router is asked, and nothing is made
 * up if it does not answer. With no server= line: the connect screen asks.
 * Missing mask: 255.255.255.0. Missing gateway: the server's address, else
 * the .1 of the PS2's own network.
 */

enum { NET_AUTO = 0, NET_DHCP, NET_STATIC };

/*
 * A user signed in on this PS2:
 *   user=Name|user id|access token|device id
 * written by the app when someone signs in, removed when they sign out or
 * the server stops accepting the token. The password is never kept.
 */
#define SETTINGS_MAX_USERS 8

/* The display mode settings.txt asks for (video=). */
enum { VIDEO_480I = 0, VIDEO_480P, VIDEO_240P, VIDEO_1080I };

/* The DVD remote (remote=): auto reads the built-in receiver if the
 * console has one; off never touches it. */
enum { REMOTE_AUTO = 0, REMOTE_OFF };

typedef struct {
	char name[64];
	char id[40];
	char token[96];
	char device[48];
} SavedUser;

typedef struct {
	char server[128];   /* as written: "192.168.50.1:8096"; "" if none */
	int  network;       /* NET_AUTO, NET_DHCP, NET_STATIC */
	char ip[16];        /* "" if none */
	char mask[16];
	char gateway[16];
	int  video_mode;    /* VIDEO_480I, VIDEO_480P or VIDEO_240P (main.c) */
	int  remote;        /* REMOTE_AUTO or REMOTE_OFF (remote.c) */
	int  end_margin_kb; /* end once the stream is over and less is left */
	int  end_early_ms;  /* end this long before the runtime; 0 = off */

	SavedUser users[SETTINGS_MAX_USERS];
	int       user_count;
} Settings;

extern Settings settings;

/*
 * Read settings.txt. Before mods_load: the IOP reset in
 * there takes the launcher's USB drivers with it until ours are loaded.
 */
void settings_load(int argc, char **argv);

/* Where the file is, and whether it was there. */
const char *settings_path(void);
int         settings_found(void);

/* Lines that were ignored, or a failed save; "" when all is well. */
const char *settings_notice(void);

/*
 * Write server= into the file - replacing the line, or adding one - and
 * leave every other line as it was, comments included. Creates the file
 * if there is none. 0 on success.
 */
int settings_save_server(const char *server);

/* The saved sign-in of the user called name, or NULL. */
const SavedUser *settings_user(const char *name);

/* Add or replace u's line; remove name's line. 0 on success. */
int settings_save_user(const SavedUser *u);
int settings_remove_user(const char *name);

#endif
