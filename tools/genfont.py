#!/usr/bin/env python3
"""
Rasterise assets/mania.ttf into a font atlas for the PS2.

Same approach as the Rokusho generator, with two changes:

  * output goes straight to src/font_data.c as 1-bit packed alpha, so
    there is no PNG to embed and no libpng at runtime
  * transparent pixels keep white RGB rather than black, so if the
    texture filter is ever switched to LINEAR the interpolation only
    touches alpha and cannot produce coloured fringes

Hard 1-bit alpha is kept from the Rokusho version: anti-aliased edge
pixels produced stray coloured dots over the menu background on analog
GS output, and thresholding removed them.

Also writes tools/font_preview.png so the result can be checked on a
desktop without involving a console.
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TTF  = os.path.join(ROOT, "assets", "mania.ttf")

FONT_SIZE   = 20          # drawn at scale 1.0 on a 448-line screen

# Printable ASCII plus the Latin-1 letters (French, Spanish, German,
# Italian, Portuguese). Text is kept as Latin-1 bytes, so a byte is a code.
# 127-159 are control codes: they get no cell and point at '?'.
FIRST, LAST = 32, 255
# AE and ae are left out: at 17 pixels they alone would widen every cell,
# and the atlas with it. Text spells them as two letters instead.
CODES       = [c for c in list(range(32, 127)) + list(range(160, 256))
               if c not in (0xC6, 0xE6)]
COLS        = 16
THRESH      = 128         # alpha cutoff for the 1-bit conversion


def build(size, prefix, preview_name):
    """One atlas of the font at this pixel size: returns the C source for
    it, every symbol named with prefix, and writes a preview PNG."""
    font = ImageFont.truetype(TTF, size)
    n = LAST - FIRST + 1          # table entries, one per byte value

    ascent, descent = font.getmetrics()
    line_h = ascent + descent

    tmp = Image.new("RGBA", (1, 1))
    d   = ImageDraw.Draw(tmp)

    metrics, max_w = [], 0
    min_y, max_y = 0, line_h
    for code in CODES:
        ch   = chr(code)
        bbox = d.textbbox((0, 0), ch, font=font)
        gw   = bbox[2] - bbox[0]
        adv  = d.textlength(ch, font=font)

        if gw <= 0:                      # space and friends
            gw = int(adv) if adv > 0 else size // 3

        metrics.append((code, gw, int(round(adv)), bbox[0]))
        max_w = max(max_w, gw)
        min_y = min(min_y, bbox[1])
        max_y = max(max_y, bbox[3])

    cell_w = max_w + 2
    # Some accents rise above the font's advertised ascent (negative y),
    # and some descenders exceed its descent. Include all ink plus a border;
    # drawing directly into undersized cells contaminates neighboring glyphs.
    top_pad = -min_y
    cell_h = max_y + top_pad + 2
    rows   = (len(CODES) + COLS - 1) // COLS

    def up8(v):
        return ((v + 7) // 8) * 8

    atlas_w, atlas_h = up8(COLS * cell_w), up8(rows * cell_h)

    atlas = Image.new("RGBA", (atlas_w, atlas_h), (0, 0, 0, 0))
    ad    = ImageDraw.Draw(atlas)

    cells = {}
    for i, (code, gw, adv, offx) in enumerate(metrics):
        cx = (i % COLS) * cell_w
        cy = (i // COLS) * cell_h

        ad.text((cx - offx + 1, cy + top_pad + 1), chr(code), font=font,
                fill=(255, 255, 255, 255))

        cells[code] = (cx, cy, adv + 1)

    entries = []
    for code in range(FIRST, LAST + 1):
        cx, cy, adv = cells.get(code, cells[ord("?")])
        entries.append((code, cx, cy, adv))

    px = atlas.load()
    for y in range(atlas_h):
        for x in range(atlas_w):
            a = px[x, y][3]
            px[x, y] = (255, 255, 255, 255 if a >= THRESH else 0)

    bg = Image.new("RGBA", (atlas_w, atlas_h), (16, 18, 42, 255))
    bg.alpha_composite(atlas)
    bg.save(os.path.join(ROOT, "tools", preview_name))

    packed = bytearray()
    for y in range(atlas_h):
        acc, bits = 0, 0
        for x in range(atlas_w):
            acc = (acc << 1) | (1 if px[x, y][3] else 0)
            bits += 1
            if bits == 8:
                packed.append(acc)
                acc, bits = 0, 0
        if bits:
            packed.append(acc << (8 - bits))

    out = []
    out.append(f"static const FontGlyph {prefix}_glyphs[] = {{\n")
    for code, cx, cy, adv in entries:
        name = chr(code) if 33 <= code <= 126 and code != 47 else \
               ("sp" if code == 32 else f"0x{code:02X}")
        out.append(f"\t{{ {cx}, {cy}, {adv} }},   /* {code} {name} */\n")
    out.append("};\n\n")

    out.append("/* 1 bit per pixel, MSB first, row-major. */\n")
    out.append(f"static const unsigned char {prefix}_bits[] = {{\n")
    for i in range(0, len(packed), 24):
        out.append("\t" + ",".join(str(b) for b in packed[i:i + 24]) + ",\n")
    out.append("};\n\n")

    out.append(f"const FontAtlasData {prefix} = {{\n"
               f"\t{atlas_w}, {atlas_h}, {cell_w}, {cell_h}, {line_h}, {top_pad},\n"
               f"\t{FIRST}, {n}, {prefix}_glyphs, {prefix}_bits\n}};\n\n")

    print(f"{prefix}: atlas {atlas_w}x{atlas_h}, cell {cell_w}x{cell_h}, "
          f"{n} glyphs, ascent {ascent} descent {descent}, top padding {top_pad}, "
          f"{len(packed)} bytes; preview -> tools/{preview_name}")
    return "".join(out)


def main():
    if not os.path.exists(TTF):
        sys.exit(f"missing {TTF}")

    normal = build(FONT_SIZE, "font_normal", "font_preview.png")

    out = os.path.join(ROOT, "src", "font_data.c")
    with open(out, "w") as f:
        f.write("/* Generated by tools/genfont.py from assets/mania.ttf.\n"
                "   Do not edit. */\n")
        f.write('#include "font.h"\n\n')
        f.write("/* The menus' font, at scale 1.0 on a 448-line screen; in 240p\n"
                "   the same atlas is drawn at half size (layout.h). */\n")
        f.write(normal)

    print("-> src/font_data.c")


if __name__ == "__main__":
    main()
