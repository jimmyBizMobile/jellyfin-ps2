/*
 * The generated font atlas and the text drawing functions; see font.c.
 */

#ifndef FONT_H
#define FONT_H

#include <gsKit.h>

/*
 * Bitmap font atlas, rasterised at build time by tools/genfont.py from
 * assets/mania.ttf. Same approach as Rokusho: every glyph occupies a full
 * cell on a shared baseline, and carries its own advance.
 *
 * FONTM, the ROM font, has no width query, so text could only be centred
 * by guesswork. Here centring and wrapping are exact.
 *
 * Alpha is 1 bit. Anti-aliased edges produced coloured fringing on analog
 * GS output, so the generator thresholds them away. The filter is NEAREST
 * to match, which also suits a pixel typeface.
 */

typedef struct {
	unsigned short x, y;      /* cell origin in the atlas */
	unsigned short adv;       /* pen advance in pixels    */
} FontGlyph;

/* One generated atlas: its size, cell, line height, glyph table, bits. */
typedef struct {
	int atlas_w, atlas_h;
	int cell_w,  cell_h;
	int line_h;
	int top_pad;             /* atlas pixels above the original text origin */
	int first,   count;
	const FontGlyph     *glyphs;
	const unsigned char *bits;
} FontAtlasData;

/*
 * The menus' font. Every mode draws this one atlas: in 240p it is simply
 * drawn at half size (the layout's scale), as the Rokusho 240p test suite
 * does with its own - so text looks the same in every mode, and only the
 * interlace flicker differs.
 */
extern const FontAtlasData font_normal;

void  font_init(GSGLOBAL *gs);

/* After a display mode change: put the atlas back in VRAM (display.h). */
void  font_reload(GSGLOBAL *gs);

float font_width (float scale, const char *s);   /* exact */
float font_height(float scale);

/* y is the text origin; atlas padding must not move the shared baseline. */
void  font_draw(GSGLOBAL *gs, float x, float y, float scale,
                u64 color, const char *s);

#endif
