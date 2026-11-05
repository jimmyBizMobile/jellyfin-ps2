#!/usr/bin/env python3
"""Check packed font cells against independently rasterized glyphs."""
import contextlib
import io
from pathlib import Path
import re
import sys
import tempfile
from unittest.mock import patch

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import genfont


def check_atlas():
    with tempfile.TemporaryDirectory() as root:
        Path(root, "tools").mkdir()
        with patch.object(genfont, "ROOT", root), contextlib.redirect_stdout(io.StringIO()):
            source = genfont.build(genfont.FONT_SIZE, "font_test", "preview.png")

    metadata = source.split("const FontAtlasData font_test = {", 1)[1]
    dimensions = list(map(int, re.findall(r"\d+", metadata.splitlines()[1])))
    atlas_w, atlas_h, cell_w, cell_h, line_h = dimensions[:5]
    top_pad = dimensions[5] if len(dimensions) > 5 else 0
    table = source.split("static const FontGlyph", 1)[1].split("};", 1)[0]
    entries = [tuple(map(int, m)) for m in re.findall(r"\{\s*(\d+),\s*(\d+),\s*(\d+)\s*\}", table)]
    packed = source.split("static const unsigned char font_test_bits[] = {", 1)[1].split("};", 1)[0]
    bits = bytes(map(int, re.findall(r"\d+", packed)))
    assert len(bits) == atlas_h * ((atlas_w + 7) // 8)
    atlas = Image.frombytes("1", (atlas_w, atlas_h), bits).convert("L")
    font = ImageFont.truetype(genfont.TTF, genfont.FONT_SIZE)
    draw = ImageDraw.Draw(Image.new("L", (1, 1)))

    # Start with the reported symptom: the cell below w used to paint two
    # diaeresis dots into its bottom rows, despite w having no descender.
    for code in [ord("w")] + [c for c in genfont.CODES if c != ord("w")]:
        cx, cy, advance = entries[code - genfont.FIRST]
        bbox = draw.textbbox((0, 0), chr(code), font=font)
        isolated = Image.new("L", (cell_w, cell_h))
        ImageDraw.Draw(isolated).text((1 - bbox[0], 1 + top_pad), chr(code), font=font, fill=255)
        isolated = isolated.point(lambda alpha: 255 if alpha >= genfont.THRESH else 0)
        actual = atlas.crop((cx, cy, cx + cell_w, cy + cell_h))
        assert actual.tobytes() == isolated.tobytes(), f"Neighbor glyph pixels in {chr(code)!r} (0x{code:02x})"
        assert advance == int(round(draw.textlength(chr(code), font=font))) + 1
        # Transparent borders prevent both clipped accents/descenders and
        # texture sampling from touching another cell's ink.
        assert actual.crop((0, 0, cell_w, 1)).getbbox() is None, hex(code)
        assert actual.crop((0, cell_h - 1, cell_w, cell_h)).getbbox() is None, hex(code)
        assert actual.crop((0, 0, 1, cell_h)).getbbox() is None, hex(code)
        assert actual.crop((cell_w - 1, 0, cell_w, cell_h)).getbbox() is None, hex(code)

    bounds = [draw.textbbox((0, 0), chr(c), font=font) for c in genfont.CODES]
    assert top_pad >= -min(b[1] for b in bounds)
    assert cell_h >= top_pad + max(b[3] for b in bounds) + 2
    assert line_h == sum(font.getmetrics())  # Keep existing line spacing.
    assert len(entries) == genfont.LAST - genfont.FIRST + 1
    for code in range(genfont.FIRST, genfont.LAST + 1):
        if code not in genfont.CODES:
            assert entries[code - genfont.FIRST] == entries[ord("?") - genfont.FIRST]
    print(f"Font atlas tests passed ({len(genfont.CODES)} isolated glyphs, accents, descenders, advances, borders)")


if __name__ == "__main__":
    check_atlas()
