// Fonts for the scrolling titles: reads font.bin (Noto Sans and monochrome
// Noto Emoji, built by tools/make_fonts.py) and renders single characters
// with stb_truetype. Independent of the display, so it runs in the host
// simulation as well.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

// font.bin layout (all numbers little endian):
//   0  "TDFONT01"            magic and format version
//   8  u32 total length      header + data
//   12 u32 CRC-32            of the bytes from offset 16 to the end
//   16 u32 entry count
//   20 entries: u32 id, u32 offset, u32 length (offset from the file start)
// Entry ids: 1 = text font (TrueType), 2 = monochrome emoji font (TrueType).
#define FONT_BIN_MAGIC     "TDFONT01"
#define FONT_ENTRY_TEXT    1
#define FONT_ENTRY_EMOJI   2

// Standard CRC-32 (as zlib), continued from crc
uint32_t fontCrc32(const uint8_t* data, size_t len, uint32_t crc = 0);

struct FontBin {
  const uint8_t* text = nullptr;  size_t textLen = 0;
  const uint8_t* emoji = nullptr; size_t emojiLen = 0;
  uint32_t crc = 0;               // the CRC in the header (identifies the version)
  size_t totalLen = 0;
};

// Checks font.bin in data (len = available bytes, e.g. the partition size).
// Returns nullptr if it is complete and intact, otherwise the reason.
const char* parseFontBin(const uint8_t* data, size_t len, FontBin& out);

// A rendered character: 8-bit coverage, origin at the baseline (xoff/yoff of
// the top-left pixel relative to the pen position)
struct GlyphBitmap {
  int16_t w = 0, h = 0, xoff = 0, yoff = 0;
  std::vector<uint8_t> alpha;
};

class TitleFont {
public:
  TitleFont();
  ~TitleFont();
  bool begin(const FontBin& bin);   // false if a font cannot be read
  void end();
  bool ready() const { return isReady; }

  enum Source : uint8_t { NONE = 0, TEXT = 1, EMOJI = 2 };
  Source sourceFor(uint32_t cp) const;              // which font has cp (text font first)
  int advance(uint32_t cp, float em) const;         // pixels; a box (0.6 em) without glyph
  bool render(uint32_t cp, float em, GlyphBitmap& out) const;  // false: no glyph (draw a box)
  // As render(), but the glyph fits into maxAbove pixels above and maxBelow
  // below the baseline: a character taller than that (e.g. Vietnamese capitals
  // with two stacked marks) keeps its body, and only the part above the
  // capital height (the marks) is squeezed into the room left; below the
  // baseline the part under it. 0: no limit.
  bool render(uint32_t cp, float em, GlyphBitmap& out, int maxAbove, int maxBelow) const;

  // Renders cp from any TrueType font in memory (e.g. a downloaded CJK
  // piece); advance in pixels. False if the font has no glyph for cp.
  static bool renderFrom(const uint8_t* ttf, uint32_t cp, float em, GlyphBitmap& out, int& advance);

private:
  void* textInfo;    // stbtt_fontinfo
  void* emojiInfo;
  int capHeight = 0;   // font units ('H')
  bool hasEmoji = false;
  bool isReady = false;
};

// A character of a title: a code point, or an emoji (sequence) with the file
// name of its Twemoji image ("1f468-200d-1f4bb") and, for the monochrome
// fallback, its first code point (0 for flags: nothing to draw without colour).
struct TitleChar {
  uint32_t cp = 0;
  std::string emoji;    // empty: not an emoji
};

// Squeezes the rows of g above the line capPx pixels above the baseline into
// maxAbove - capPx rows (area average) if g reaches higher than maxAbove, and
// the rows below the baseline into maxBelow rows if it reaches lower.
void fitGlyph(GlyphBitmap& g, int capPx, int maxAbove, int maxBelow);

// UTF-8 text -> title characters. Control characters become spaces; emoji
// sequences (flags, keycaps, skin tones, zero width joiner sequences, tag
// sequences) are kept together; stray variation selectors, joiners,
// modifiers, tags and regional indicators are dropped. Invalid bytes are
// skipped.
std::vector<TitleChar> titleChars(const char* utf8);

// The code points to draw without colour emoji: titleChars() with each emoji
// as its first code point (flags left out)
std::vector<uint32_t> titleCodepoints(const char* utf8);
