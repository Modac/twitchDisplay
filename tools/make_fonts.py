#!/usr/bin/env python3
"""Builds fonts/font.bin, the font data of the display's "font" partition.

Contents (see lib/TitleFont/TitleFont.h for the layout):
  1  Noto Sans SemiBold: Latin-1, Latin Extended-A/B, Latin Extended
     Additional (Vietnamese), combining diacritics, Greek, Cyrillic, general
     punctuation, euro, dong and trademark signs
  2  Noto Emoji SemiBold (monochrome): the emoji listed in
     tools/emoji_common.txt (the most used ones, about 200 KB)

The fonts (SIL Open Font License, fonts/OFL.txt) are downloaded from
fontsource on jsDelivr. The firmware is built with the CRC of the committed
fonts/font.bin (tools/version.py), so rebuild and commit both together.

Needs fontTools (pip install fonttools). Run from the repository root:
  python3 tools/make_fonts.py
"""
import io
import os
import struct
import tempfile
import urllib.request
import zlib

from fontTools import subset
from fontTools.merge import Merger
from fontTools.ttLib import TTFont

CDN = "https://cdn.jsdelivr.net/fontsource/fonts"
OUT = os.path.join("fonts", "font.bin")
EMOJI_LIST = os.path.join("tools", "emoji_common.txt")


def fetch(name):
    with urllib.request.urlopen(f"{CDN}/{name}", timeout=60) as r:
        return io.BytesIO(r.read())


def r(a, b):
    return list(range(a, b + 1))


TEXT_CHARS = (r(0x20, 0x7E) + r(0xA0, 0xFF) + r(0x100, 0x24F) + r(0x370, 0x3FF) + r(0x400, 0x4FF)
              + r(0x2010, 0x2027) + r(0x2030, 0x203A) + [0x20AC, 0x2122]
              + r(0x1E00, 0x1EFF) + r(0x300, 0x36F) + [0x20AB])   # Vietnamese (and other Latin) letters, diacritics, dong


def subset_font(font, unicodes):
    opts = subset.Options()
    opts.hinting = False            # stb_truetype ignores hinting
    opts.notdef_outline = True
    opts.layout_features = []       # no shaping on the display
    opts.drop_tables += ["GSUB", "GPOS", "GDEF", "STAT", "vhea", "vmtx", "name", "post"]
    opts.name_IDs = []
    s = subset.Subsetter(opts)
    s.populate(unicodes=unicodes)
    s.subset(font)
    out = io.BytesIO()
    font.save(out)
    return out.getvalue()


def text_font():
    with tempfile.TemporaryDirectory() as tmp:
        paths = []
        for piece in ("latin", "latin-ext", "greek", "cyrillic", "vietnamese"):
            path = os.path.join(tmp, piece + ".ttf")
            with open(path, "wb") as f:
                f.write(fetch(f"noto-sans@latest/{piece}-600-normal.ttf").getvalue())
            paths.append(path)
        merged = Merger().merge(paths)
    return subset_font(merged, TEXT_CHARS)


def emoji_font():
    with open(EMOJI_LIST) as f:
        cps = [int(line, 16) for line in f if line.strip() and not line.startswith("#")]
    return subset_font(TTFont(fetch("noto-emoji@latest/emoji-600-normal.ttf")), cps), len(cps)


def pack(entries):
    """entries: list of (id, bytes) -> font.bin"""
    header_len = 20 + 12 * len(entries)
    offset = (header_len + 3) & ~3
    table, data = b"", b""
    for eid, blob in entries:
        table += struct.pack("<III", eid, offset + len(data), len(blob))
        data += blob + b"\0" * (-len(blob) % 4)
    body = struct.pack("<I", len(entries)) + table + b"\0" * (offset - header_len) + data
    total = 16 + len(body)
    return b"TDFONT01" + struct.pack("<II", total, zlib.crc32(body)) + body


def main():
    text = text_font()
    emoji, count = emoji_font()
    blob = pack([(1, text), (2, emoji)])
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(blob)
    print(f"text font {len(text)} bytes, {count} emoji {len(emoji)} bytes")
    print(f"{OUT}: {len(blob)} bytes, CRC {zlib.crc32(blob[16:]):08x}")


if __name__ == "__main__":
    main()
