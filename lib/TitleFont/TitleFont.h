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

private:
  void* textInfo;    // stbtt_fontinfo
  void* emojiInfo;
  bool hasEmoji = false;
  bool isReady = false;
};

// UTF-8 text -> code points for the title. Control characters become spaces;
// variation selectors, zero width joiners, skin tone modifiers, tag
// characters, keycap marks and regional indicators (flags) are dropped, so an
// emoji sequence shows as its single emoji. Invalid bytes are skipped.
std::vector<uint32_t> titleCodepoints(const char* utf8);
