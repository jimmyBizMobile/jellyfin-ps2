/*
 * Text rendering. The font is generated at build time from
 * assets/mania.ttf into a 1-bit atlas (font_data.c, Latin-1); this unpacks
 * it into a texture once and draws strings as one sprite per glyph.
 */

#include "font.h"
#include "config.h"

#include <malloc.h>
#include <string.h>

/*
 * The atlas in VRAM: its generated data, the unpacked 4-bit
 * pixels kept for re-uploading, the palette and the texture.
 */
typedef struct {
	const FontAtlasData *d;
	GSTEXTURE tex;
	u32      *pixels;
	u32       clut[16] __attribute__((aligned(64)));
	int       ready;
} Atlas;

static Atlas g_at[1] = { { &font_normal } };
static int   g_ready;

/* The atlas text is drawn with. */
static Atlas *cur(void) { return &g_at[0]; }

/*
 * Layout units per atlas pixel at scale 1. One atlas pixel is one layout
 * unit; the layout's scale (lay->sy) then shrinks it on the canvas - half
 * size in 240p, with NEAREST sampling, as the 240p test suite draws.
 */
static float unit(void) { return 1.0f; }

/*
 * Unpack one generated 1-bit atlas into a 4-bit palette texture (PSMT4):
 * two pixels per byte, the left one in the low nibble, indexing a 16-entry
 * palette of which two entries are used - transparent and white. The text
 * is tinted when drawn. 4 bits instead of 32 per pixel: the normal atlas
 * takes about 34 KB of VRAM instead of 270 KB.
 */
static void atlas_init(GSGLOBAL *gs, Atlas *a)
{
	if (a->ready)
		return;

	int w = a->d->atlas_w, h = a->d->atlas_h;
	int stride = (w + 7) / 8;

	a->pixels = memalign(128, (w * h + 1) / 2);
	if (!a->pixels)
		return;

	unsigned char *px = (unsigned char *)a->pixels;

	memset(px, 0, (w * h + 1) / 2);

	for (int y = 0; y < h; y++) {
		const unsigned char *row = &a->d->bits[y * stride];

		for (int x = 0; x < w; x++) {
			if (!(row[x >> 3] & (0x80 >> (x & 7))))
				continue;

			int i = y * w + x;

			px[i >> 1] |= (i & 1) ? 0x10 : 0x01;
		}
	}

	/* 16 entries: 0 transparent, 1 white (CT32 is ABGR, 0x80 opaque). */
	for (int i = 0; i < 16; i++)
		a->clut[i] = 0x00FFFFFF;
	a->clut[1] = 0x80FFFFFF;

	a->tex.Width   = w;
	a->tex.Height  = h;
	a->tex.PSM     = GS_PSM_T4;
	a->tex.ClutPSM = GS_PSM_CT32;
	a->tex.Filter  = GS_FILTER_NEAREST;
	a->tex.Mem     = a->pixels;
	a->tex.Clut    = a->clut;

	a->ready = 1;
}

/* Reserve one atlas's VRAM and upload it. */
static void atlas_upload(GSGLOBAL *gs, Atlas *a)
{
	if (!a->ready)
		return;

	a->tex.Vram     = gsKit_vram_alloc(
	                      gs, gsKit_texture_size(a->tex.Width, a->tex.Height,
	                                             GS_PSM_T4),
	                      GSKIT_ALLOC_USERBUFFER);
	a->tex.VramClut = gsKit_vram_alloc(
	                      gs, gsKit_texture_size(8, 2, GS_PSM_CT32),
	                      GSKIT_ALLOC_USERBUFFER);

	gsKit_texture_upload(gs, &a->tex);
}

/*
 * Unpack the atlas and put it in VRAM, once. Idempotent: later calls
 * do nothing.
 */
void font_init(GSGLOBAL *gs)
{
	if (g_ready)
		return;

	atlas_init(gs, &g_at[0]);
	atlas_upload(gs, &g_at[0]);

	g_ready = 1;
}

/*
 * After a display mode change: the framebuffers were set up again and
 * everything after them in VRAM is gone. Reserve the atlas again and
 * upload it from the copy kept in main memory.
 */
void font_reload(GSGLOBAL *gs)
{
	if (!g_ready) {
		font_init(gs);
		return;
	}

	atlas_upload(gs, &g_at[0]);
}

/* The atlas entry for a Latin-1 byte; anything outside the table is drawn
 * as '?'. */
static const FontGlyph *glyph_for(const FontAtlasData *d, unsigned char c)
{
	int i = (int)c - d->first;
	if (i < 0 || i >= d->count)
		i = '?' - d->first;
	return &d->glyphs[i];
}

/* Width of s in layout units at the given scale: the sum of each glyph's
 * advance. */
float font_width(float scale, const char *s)
{
	const FontAtlasData *d = cur()->d;
	float w = 0.0f;

	for (; *s; s++)
		w += glyph_for(d, (unsigned char)*s)->adv * scale;

	return w * unit();
}

/* Line height in layout units at the given scale. */
float font_height(float scale)
{
	return cur()->d->line_h * scale * unit();
}

/*
 * Draw s with its top-left corner at (x, y), in layout space: one textured
 * sprite per glyph from the atlas, tinted with color. Spaces only
 * advance the pen.
 */
void font_draw(GSGLOBAL *gs, float x, float y, float scale,
               u64 color, const char *s)
{
	if (!g_ready)
		return;

	Atlas *a = cur();
	const FontAtlasData *d = a->d;

	/* Layout space to canvas (layout.h), then canvas to framebuffer.
	 * Glyphs shrink with the layout: half size in 240p. */
	x      = LAY_X(x);
	y      = LAY_Y(y) + SCR_Y0;
	scale *= unit() * lay->sy;
	/* The atlas includes ink outside the font's advertised vertical metrics.
	 * Compensate its extra top padding to preserve existing text placement. */
	y -= d->top_pad * scale;

	float pen = x;
	float cw  = d->cell_w * scale;
	float ch  = d->cell_h * scale;

	for (; *s; s++) {
		const FontGlyph *g = glyph_for(d, (unsigned char)*s);

		if (*s != ' ') {          /* nothing to draw, just advance */
			gsKit_prim_sprite_texture(gs, &a->tex,
			                          pen,      y,
			                          g->x,     g->y,
			                          pen + cw, y + ch,
			                          g->x + d->cell_w,
			                          g->y + d->cell_h,
			                          3, color);
		}
		pen += g->adv * scale;
	}
}
