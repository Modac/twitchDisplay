#!/usr/bin/env python3
"""Builds the font subsets for the font preview firmware (examples/fontPreview).

Downloads Noto Sans and Noto Emoji (SIL Open Font License) from fontsource on
jsDelivr, merges the pieces and keeps only the needed characters:

  fonts/NotoSans-600.ttf   SemiBold: Latin-1, Latin Extended-A/B, Greek,
                           Cyrillic, punctuation, euro, trademark (the set
                           planned for the real firmware)
  fonts/NotoSans-400.ttf   Regular and Bold, Latin-1 and punctuation only,
  fonts/NotoSans-700.ttf   for comparing weights
  fonts/NotoEmoji-600.ttf  monochrome emoji, only those on the preview pages

Needs fontTools (pip install fonttools). Run from the repository root:
  python3 tools/make_preview_fonts.py
"""
import io
import os
import urllib.request

from fontTools import subset
from fontTools.merge import Merger
from fontTools.ttLib import TTFont

CDN = "https://cdn.jsdelivr.net/fontsource/fonts"
OUT = os.path.join("examples", "fontPreview", "fonts")


def fetch(name):
    with urllib.request.urlopen(f"{CDN}/{name}", timeout=60) as r:
        return io.BytesIO(r.read())


def r(a, b):
    return list(range(a, b + 1))


LATIN1 = r(0x20, 0x7E) + r(0xA0, 0xFF)
PUNCT = r(0x2010, 0x2027) + r(0x2030, 0x203A) + [0x20AC, 0x2122]
FULL = LATIN1 + PUNCT + r(0x100, 0x24F) + r(0x370, 0x3FF) + r(0x400, 0x4FF)

# emoji on the preview pages (sequences as their single code points)
EMOJI = [0x1F600, 0x1F602, 0x1F60D, 0x1F914, 0x1F62D, 0x1F44D, 0x2764, 0x1F525, 0x1F3AE, 0x1F3C6,
         0x1F389, 0x1F4AF, 0x2705, 0x1F680, 0x1F355, 0x2615, 0x1F440, 0x1F916,
         0x1F1E9, 0x1F1EA, 0x1F468, 0x200D, 0x1F4BB, 0xFE0F]


def save_subset(font, unicodes, path):
    opts = subset.Options()
    opts.hinting = False            # stb_truetype ignores hinting
    opts.notdef_outline = True
    opts.layout_features = []       # no shaping on the display
    opts.drop_tables += ["GSUB", "GPOS", "GDEF", "STAT", "vhea", "vmtx"]
    s = subset.Subsetter(opts)
    s.populate(unicodes=unicodes)
    s.subset(font)
    font.save(path)
    print(f"{path}: {os.path.getsize(path)} bytes")


def noto_sans(weight, pieces):
    fonts = [TTFont(fetch(f"noto-sans@latest/{p}-{weight}-normal.ttf")) for p in pieces]
    if len(fonts) == 1:
        return fonts[0]
    tmp = []
    for i, f in enumerate(fonts):       # Merger wants files
        path = os.path.join(OUT, f".piece{i}.ttf")
        f.save(path)
        tmp.append(path)
    merged = Merger().merge(tmp)
    for path in tmp:
        os.remove(path)
    return merged


def main():
    os.makedirs(OUT, exist_ok=True)
    save_subset(noto_sans(600, ["latin", "latin-ext", "greek", "cyrillic"]), FULL, os.path.join(OUT, "NotoSans-600.ttf"))
    for w in (400, 700):
        save_subset(noto_sans(w, ["latin"]), LATIN1 + PUNCT, os.path.join(OUT, f"NotoSans-{w}.ttf"))
    save_subset(TTFont(fetch("noto-emoji@latest/emoji-600-normal.ttf")), EMOJI, os.path.join(OUT, "NotoEmoji-600.ttf"))


if __name__ == "__main__":
    main()
