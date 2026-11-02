/*
 * The Jellyfin client: shared state (items, tracks, detail, users, HOME)
 * and every request; see jf.c.
 */

#ifndef JF_H
#define JF_H

#include "config.h"

typedef enum {
	JF_TAB_MOVIES,
	JF_TAB_SERIES,
	JF_TAB_MUSIC,
	JF_TAB_SEARCH,
	JF_TAB_COUNT
} JfTab;

typedef struct { char id[40]; char name[64]; int has_pw; }  JfUser;
typedef struct { char id[40]; char name[112]; int year; } JfItem;

/*
 * One item's full detail. Fetched on demand rather than prefetched with the
 * page: overviews run to hundreds of bytes each, and six of them would push
 * the listing past the ~2 KB the receive mailbox can hold.
 */
typedef struct {
	char id[40];
	char name[112];
	char overview[512];
	char type[16];
	char series_id[40];   /* episodes: the show, for finding the next one */
	int  year;
	int  runtime_min;
	long long runtime_ticks;   /* exact, 100 ns units; 0 if unknown */

	/*
	 * Where this user left off, from Jellyfin's UserData; 0 for none.
	 * Updated locally when playback stops, so the page is right at once.
	 */
	long long resume_ticks;

	/*
	 * The source picture size, for choosing what to ask the transcoder
	 * for. Zero when the item is not a video.
	 */
	int  width;
	int  height;

	/*
	 * The picture's intended shape on screen, as width/height x 1000
	 * (1333 for 4:3, 1778 for 16:9), and whether it is known for sure.
	 *
	 * DVDs and TV captures store their picture at 720x480 or 720x576 with
	 * pixels that are not square: the file carries a note saying how wide
	 * it is meant to be shown, which Jellyfin reports as AspectRatio. The
	 * picture is drawn with this shape, not with its stored size.
	 *
	 * aspect_sure is 0 for a DVD-sized file whose note was lost (it claims
	 * square pixels): the shape is then a guess, 4:3, and only then may
	 * the viewer change it during playback (Triangle). See decide_aspect.
	 */
	int  aspect_milli;
	int  aspect_sure;

	/*
	 * Frames per second, times 1000, from the video stream. The rate
	 * actually asked for is jf_video_rate's, within what the decoder
	 * keeps up with.
	 *
	 * Needed in the transcode request, not merely useful: once a subtitle
	 * filter is in the chain ffmpeg loses the rate and falls back to the
	 * container time base, which for these files is 1000 - and MPEG-2
	 * cannot express 1000 fps, so the encoder refuses outright.
	 */
	int  fps_milli;
} JfDetail;

/*
 * One selectable audio or subtitle track.
 *
 * The index is the stream's own, as ffmpeg numbers it, which is what the
 * transcode request wants.
 */
typedef struct {
	int  index;
	char label[40];       /* what to show: language, and title if present */
	char codec[12];       /* "ass", "pgssub", "aac"... */
	int  is_text;         /* subtitles only: text (drawn by the PS2), not
	                         pictures (PGS, VobSub: burned in) */
} JfStream;

#define JF_STREAM_MAX 12

extern JfStream jf_audio[JF_STREAM_MAX];
extern int      jf_audio_count;

extern JfStream jf_subs[JF_STREAM_MAX];
extern int      jf_sub_count;

/* ---- session ---- */
extern char jf_url[128];
extern char jf_server_name[64];
extern char jf_version[32];

extern JfUser jf_users[JF_MAX_USERS];
extern int    jf_user_count;
extern char   jf_user_id[40];
extern char   jf_user_name[64];

/* One track in an album. */
typedef struct {
	char id[40];
	char name[112];
	int  seconds;

	/*
	 * "mp3", "flac", "m4a"... which decides how the track is requested.
	 * libmad decodes MP3 and nothing else, so anything else has to be
	 * transcoded by the server.
	 */
	char container[12];
} JfTrack;

/* ---- browsing ---- */
extern JfTab jf_tab;
extern char  jf_query[64];          /* search term, used by the Search tab */

extern JfItem jf_items[JF_PAGE_MAX];
extern int    jf_item_count;
extern int    jf_total_count;       /* matches the current filter */
extern int    jf_start_index;

extern JfDetail jf_detail;

/* ---- the album currently open ---- */
/*
 * Where we are inside the Music tab.
 *
 * Artists first, then that artist's albums, then its tracks. Only the first
 * two use the browse grid; tracks have their own screen.
 */
typedef enum { JF_MUSIC_ARTISTS, JF_MUSIC_ALBUMS } JfMusicLevel;

/*
 * Where we are inside the Series tab: shows first, then that show's seasons.
 * Episodes have their own screen, like tracks.
 */
typedef enum { JF_TV_SHOWS, JF_TV_SEASONS } JfTvLevel;

extern JfTvLevel jf_tv_level;
extern char      jf_show_id[40];
extern char      jf_show_name[128];

/* Enter a series, or go back up to the series list. */
void jf_tv_enter_show(const char *id, const char *name);
void jf_tv_up(void);

extern JfMusicLevel jf_music_level;
extern char         jf_artist_name[128];
extern char         jf_artist_id[40];

/* Enter an artist, or go back up to the artist list. */
void jf_music_enter_artist(const char *id, const char *name);
void jf_music_up(void);

extern JfTrack jf_tracks[JF_TRACK_MAX];
extern int     jf_track_count;
extern char    jf_album_name[112];
extern char    jf_album_artist[80];

/*
 * A synopsis for the list currently open. Empty for albums, which have none
 * worth showing; a season's overview when the list is episodes.
 */
extern char    jf_album_desc[512];
extern int     jf_album_year;

/* The server address from settings.txt, into jf_url. Once, at start. */
void jf_init(void);

/* ---- calls. 0 on success, -1 with an error set. ---- */
int jf_connect(void);

/* The users on the server's login page, into jf_users. No sign-in needed. */
int jf_load_users(void);

/*
 * Signing in, as on any Jellyfin app. jf_sign_in asks the server with a
 * name and password and keeps the sign-in in settings.txt; jf_resume_user
 * uses a kept one, checking the server still accepts it. Both return 0 when
 * signed in, -2 when a password is needed (wrong, or the kept sign-in was
 * ended on the server - it is forgotten here then), -1 with an error set.
 * jf_sign_out ends a kept sign-in, on the server and here.
 */
int  jf_sign_in(const char *name, const char *password);
int  jf_resume_user(const char *name);
void jf_sign_out(const char *name);
void jf_sign_out_local(void);       /* forget who is signed in, in memory */

/* Uses jf_tab, jf_start_index and jf_query. */
int jf_load_items(void);

/*
 * Jump to a letter. Counts how many items sort at or after it, subtracts
 * from the library total, and sets jf_start_index to that offset. The letter
 * is not kept as a filter, so paging afterwards moves through the whole
 * library in both directions.
 */
int jf_jump_to_letter(char letter);

/*
 * Fetch one item's primary image, sliced so no TCP segment is ever full.
 * Returns the byte count and points *out at an internal buffer, or 0 when
 * the item has no artwork, or negative on failure.
 */
/*
 * One attempt at one size. Returns the byte count, 0 when the item has no
 * artwork, -3 when the reply is too large to be trusted, or -1 on failure.
 *
 * Single attempt on purpose. The only real proof a JPEG survived is that it
 * decodes: a hole in the middle leaves FF D8 and FF D9 intact, so the marker
 * check here passes and TurboJPEG rejects it later. The retry loop therefore
 * lives with the decoder, in scr_browse.c.
 */
int jf_fetch_image(const char *item_id, int width, int quality,
                   const unsigned char **out);

/*
 * Tracks of one album, into jf_tracks. Fetched a few at a time for the same
 * reason listings are: see HTTP_SAFE_BYTES.
 */
int jf_load_tracks(const char *album_id, const char *album_name, int year);

/*
 * Episodes of one season, into jf_tracks. They reuse the track list: a
 * numbered list of titles is what both want, and the episodes carry no
 * IndexNumber of their own, so their position in the reply is the number.
 */
int jf_load_episodes(const char *series_id, const char *season_id,
                     const char *season_name);

/*
 * The path for a video transcoded to something the IPU can decode.
 *
 * MPEG-2 video and MPEG-1 layer II audio in a transport stream. The size is
 * a bounding box, not a demand: Jellyfin fits the source's own aspect inside
 * it, so asking for 640x640 on 4:3 material yields 640x480, while asking for
 * 640x448 yields 596x448 - which is not a multiple of 16 and decodes as
 * 608x448.
 *
 * Returns a pointer to a static buffer.
 */

/*
 * The transcode size to ask for: the smallest picture, in the file's stored
 * proportions, that fills what the film covers on the 640x480 screen in its
 * intended shape (jf_detail.aspect_milli), within 640x480. 640x480 for 4:3,
 * 640x360 for 16:9, 640x426 for a DVD. Sizes that are not whole 16-pixel
 * macroblocks are padded by the encoder and cropped when drawn.
 *
 * Writes the box to *w and *h, and the picture Jellyfin will actually
 * produce to jf_video_pic_w and jf_video_pic_h.
 */
void jf_video_box(int src_w, int src_h, int *w, int *h);

/*
 * The picture jf_video_box expects Jellyfin to produce, which is not always
 * the box: it fits the aspect and rounds to even, not to 16. The decoder
 * reports the padded size, so these are what should actually be drawn.
 */
extern int jf_video_pic_w, jf_video_pic_h;

/*
 * The frame rate to ask for: the source's, or the largest whole fraction
 * of it within VIDEO_MAX_FPS_MILLI (config.h). 59.94 becomes 29.97, 50
 * becomes 25; anything at or under the limit is unchanged.
 */
int jf_video_rate(int src_milli);

/*
 * The rate jf_video_stream_path_at last asked for: the player paces
 * against this, not the file's rate. 0 when no stream has been asked for.
 */
extern int jf_video_fps_milli;

/*
 * Path for streaming a track as raw PCM. Jellyfin does the decoding, so the
 * console only has to hand samples to the SPU2.
 */
const char *jf_track_stream_path(const char *track_id);

/* Host, port and key, for code that opens its own connection. */
const char *jf_host(void);
int         jf_port(void);
const char *jf_key(void);

/*
 * The stream request, starting at start_ticks (100 ns units, as Jellyfin
 * counts) rather than the beginning. A pause or a seek is a new request from
 * the new position: the server restarts the transcode there.
 */
const char *jf_video_stream_path_at(const char *item_id, int audio_index,
                                    int sub_index, long long start_ticks);

/*
 * Tell the server the last playback has stopped, at pos_ticks, and end the
 * transcode behind it. Without this ffmpeg runs on for every film started,
 * and a restart competes with the stream it replaces.
 */
void jf_video_stopped(const char *item_id, long long pos_ticks);

/*
 * The same, in two halves, for a seek or a pause: _later remembers which
 * session to end (the next stream request replaces the current one), and
 * _flush sends it once the new stream is already on its way, so the two
 * requests are not in the way of the reload.
 */
void jf_video_stop_later(const char *item_id, long long pos_ticks);
void jf_video_stop_flush(void);

/*
 * Reports to the server (stop, end transcode, resume point) are sent by a
 * background thread. Waits up to ms for any still queued, before quitting.
 */
void jf_reports_wait(int ms);

/*
 * Save where this user stopped, so the film page can offer to resume.
 *
 * Written into the signed-in user's data directly, alongside the stop
 * report, so the film page can offer it at once. Following
 * Jellyfin's own rule, past 90 % of the runtime the item counts as played
 * and keeps no resume point; under 30 s nothing is kept either.
 *
 * Returns the position actually kept (0 for none). jf_resume_status() is
 * the HTTP status of the save, for the readout: 2xx is saved.
 */
long long jf_save_position(const char *item_id, long long pos_ticks,
                           long long runtime_ticks);
int       jf_resume_status(void);

/* The play session the last stream was requested with, for the readout. */
const char *jf_play_session(void);

/* ---- HOME: Continue Watching and Next Up ---- */
#define JF_HOME_MAX 12

typedef struct {
	char id[40];
	char name[112];
	char series[64];      /* episodes: the show's name; "" for films */
	char image_id[40];    /* whose poster to show: the series' for episodes */
	int  season, episode; /* episodes; 0 if unknown */
	int  year;
	long long runtime_ticks;
	long long pos_ticks;  /* where this user left off; 0 for Next Up */
} JfHomeItem;

extern JfHomeItem jf_home_cw[JF_HOME_MAX];   /* Continue Watching */
extern int        jf_home_cw_count;
extern JfHomeItem jf_home_nu[JF_HOME_MAX];   /* Next Up */
extern int        jf_home_nu_count;

/* Both rows, for the signed-in user. 0, or -1 with an error set. */
int jf_load_home(void);

/*
 * The episode after jf_detail's, across seasons, into jf_next. 1 if there
 * is one, 0 at the end of the series or for anything not an episode.
 */
extern JfHomeItem jf_next;
int jf_find_next(void);

/* Full detail for one item, into jf_detail. */
int jf_load_detail(const char *id);

void jf_save_url(void);

/* Write received audio to the drive, for comparison against the original. */

const char *jf_err_title(void);
const char *jf_err_body(void);

/* First letter of the first visible item, 0 to 25. For the rail highlight. */
int jf_rail_letter(void);

#endif