/*
 * Text subtitles; see subs.c.
 */

#ifndef SUBS_H
#define SUBS_H

/*
 * Text subtitles, downloaded once per film and drawn by the PS2.
 *
 * Burning subtitles into the picture made Jellyfin render every frame
 * before sending any, which was slow to start and failed outright when
 * seeking. Instead the track is fetched - ASS and SSA as they are, so
 * typesetting can be told from dialogue and skipped, anything else as SRT
 * (Jellyfin converts WebVTT and embedded text tracks on request) - parsed
 * into timed lines, and the current lines are drawn over the picture.
 * A burned-in choice remains for ASS tracks whose signs matter.
 *
 * One track is held at a time, in memory only. Loading another replaces
 * it; loading the same one again costs nothing, so Resume after a Stop is
 * instant.
 */

/*
 * Fetch and parse track index of the item; ass: read it as ASS/SSA (its
 * codec is ass or ssa). Blocks: call from the UI thread with something on
 * screen. Returns the number of lines, or a negative number if the track
 * could not be had (the caller can fall back to burning it in).
 */
int  subs_load(const char *item_id, int index, int ass);

void subs_clear(void);
int  subs_active_for(const char *item_id);   /* ...and it is this film's */

/*
 * The text to show at the bottom at position ms (from the start of the
 * film), or NULL; *top (if top is not NULL) gets the text for the top of
 * the screen, or NULL. Latin-1, lines separated by '\n'.
 */
const char *subs_at(int ms, const char **top);

#endif
