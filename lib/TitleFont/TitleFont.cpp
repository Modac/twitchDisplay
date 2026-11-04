#include "TitleFont.h"

#include <math.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

uint32_t fontCrc32(const uint8_t* data, size_t len, uint32_t crc){
  crc = ~crc;
  for(size_t i = 0; i < len; i++){
    crc ^= data[i];
    for(int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
  }
  return ~crc;
}

static uint32_t rd32(const uint8_t* p){
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const char* parseFontBin(const uint8_t* data, size_t len, FontBin& out){
  out = FontBin();
  if(!data || len < 20 || memcmp(data, FONT_BIN_MAGIC, 8) != 0) return "no font.bin";
  uint32_t total = rd32(data + 8), count = rd32(data + 16);
  if(total < 20 || total > len || count > 16 || 20 + count * 12 > total) return "font.bin damaged";
  uint32_t crc = rd32(data + 12);
  if(fontCrc32(data + 16, total - 16) != crc) return "font.bin damaged (checksum)";
  for(uint32_t i = 0; i < count; i++){
    const uint8_t* e = data + 20 + i * 12;
    uint32_t id = rd32(e), off = rd32(e + 4), n = rd32(e + 8);
    if(off > total || n > total - off) return "font.bin damaged";
    if(id == FONT_ENTRY_TEXT){ out.text = data + off; out.textLen = n; }
    if(id == FONT_ENTRY_EMOJI){ out.emoji = data + off; out.emojiLen = n; }
  }
  if(!out.text) return "font.bin has no text font";
  out.crc = crc;
  out.totalLen = total;
  return nullptr;
}

TitleFont::TitleFont() : textInfo(new stbtt_fontinfo()), emojiInfo(new stbtt_fontinfo()) {}

TitleFont::~TitleFont(){
  delete (stbtt_fontinfo*)textInfo;
  delete (stbtt_fontinfo*)emojiInfo;
}

bool TitleFont::begin(const FontBin& bin){
  end();
  if(!bin.text || !stbtt_InitFont((stbtt_fontinfo*)textInfo, bin.text, 0)) return false;
  hasEmoji = bin.emoji && stbtt_InitFont((stbtt_fontinfo*)emojiInfo, bin.emoji, 0);
  isReady = true;
  return true;
}

void TitleFont::end(){
  isReady = false;
  hasEmoji = false;
}

TitleFont::Source TitleFont::sourceFor(uint32_t cp) const {
  if(!isReady) return NONE;
  if(stbtt_FindGlyphIndex((const stbtt_fontinfo*)textInfo, (int)cp)) return TEXT;
  if(hasEmoji && stbtt_FindGlyphIndex((const stbtt_fontinfo*)emojiInfo, (int)cp)) return EMOJI;
  return NONE;
}

int TitleFont::advance(uint32_t cp, float em) const {
  Source s = sourceFor(cp);
  if(s == NONE) return (int)(em * 0.6f);
  const stbtt_fontinfo* f = (const stbtt_fontinfo*)(s == TEXT ? textInfo : emojiInfo);
  int adv, lsb;
  stbtt_GetGlyphHMetrics(f, stbtt_FindGlyphIndex(f, (int)cp), &adv, &lsb);
  return (int)lroundf(adv * stbtt_ScaleForMappingEmToPixels(f, em));
}

bool TitleFont::render(uint32_t cp, float em, GlyphBitmap& out) const {
  out = GlyphBitmap();
  Source s = sourceFor(cp);
  if(s == NONE) return false;
  const stbtt_fontinfo* f = (const stbtt_fontinfo*)(s == TEXT ? textInfo : emojiInfo);
  int gi = stbtt_FindGlyphIndex(f, (int)cp);
  float scale = stbtt_ScaleForMappingEmToPixels(f, em);
  int x0, y0, x1, y1;
  stbtt_GetGlyphBitmapBox(f, gi, scale, scale, &x0, &y0, &x1, &y1);
  out.xoff = (int16_t)x0; out.yoff = (int16_t)y0;
  if(x1 <= x0 || y1 <= y0) return true;   // e.g. space: nothing to draw
  out.w = (int16_t)(x1 - x0); out.h = (int16_t)(y1 - y0);
  out.alpha.assign((size_t)out.w * out.h, 0);
  stbtt_MakeGlyphBitmap(f, out.alpha.data(), out.w, out.h, out.w, scale, scale, gi);
  return true;
}

bool TitleFont::renderFrom(const uint8_t* ttf, uint32_t cp, float em, GlyphBitmap& out, int& advance){
  out = GlyphBitmap();
  stbtt_fontinfo f;
  if(!ttf || !stbtt_InitFont(&f, ttf, 0)) return false;
  int gi = stbtt_FindGlyphIndex(&f, (int)cp);
  if(!gi) return false;
  float scale = stbtt_ScaleForMappingEmToPixels(&f, em);
  int adv, lsb, x0, y0, x1, y1;
  stbtt_GetGlyphHMetrics(&f, gi, &adv, &lsb);
  advance = (int)lroundf(adv * scale);
  stbtt_GetGlyphBitmapBox(&f, gi, scale, scale, &x0, &y0, &x1, &y1);
  out.xoff = (int16_t)x0; out.yoff = (int16_t)y0;
  if(x1 <= x0 || y1 <= y0) return true;
  out.w = (int16_t)(x1 - x0); out.h = (int16_t)(y1 - y0);
  out.alpha.assign((size_t)out.w * out.h, 0);
  stbtt_MakeGlyphBitmap(&f, out.alpha.data(), out.w, out.h, out.w, scale, scale, gi);
  return true;
}

static std::vector<uint32_t> decodeUtf8(const char* utf8){
  std::vector<uint32_t> out;
  if(!utf8) return out;
  const uint8_t* p = (const uint8_t*)utf8;
  while(*p){
    uint32_t cp;
    int len;
    if(*p < 0x80)                { cp = *p;        len = 1; }
    else if((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
    else if((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
    else if((*p & 0xF8) == 0xF0) { cp = *p & 0x07; len = 4; }
    else { p++; continue; }
    int k = 1;
    for(; k < len && (p[k] & 0xC0) == 0x80; k++) cp = (cp << 6) | (p[k] & 0x3F);
    if(k < len){ p++; continue; }
    p += len;
    if(cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) cp = ' ';
    out.push_back(cp);
  }
  return out;
}

static bool isRegional(uint32_t c){ return c >= 0x1F1E6 && c <= 0x1F1FF; }
static bool isModifier(uint32_t c){ return c >= 0x1F3FB && c <= 0x1F3FF; }
static bool isTag(uint32_t c){ return c >= 0xE0020 && c <= 0xE007F; }
static bool isKeycapBase(uint32_t c){ return (c >= '0' && c <= '9') || c == '#' || c == '*'; }

// Emoji by default (no variation selector needed)
static bool isEmojiDefault(uint32_t c){
  return (c >= 0x1F000 && c <= 0x1FAFF && !isRegional(c) && !isModifier(c))
      || (c >= 0x2300 && c <= 0x23FF) || (c >= 0x2600 && c <= 0x27BF) || (c >= 0x2B00 && c <= 0x2BFF)
      || (c >= 0x25AA && c <= 0x25FE) || c == 0x24C2 || c == 0x3030 || c == 0x303D || c == 0x3297 || c == 0x3299;
}
// Text by default, emoji only with U+FE0F
static bool isEmojiWithSelector(uint32_t c){
  return c == 0xA9 || c == 0xAE || c == 0x203C || c == 0x2049 || c == 0x2122 || c == 0x2139
      || (c >= 0x2190 && c <= 0x21FF) || c == 0x2934 || c == 0x2935;
}

static std::string twemojiName(const std::vector<uint32_t>& seq){
  bool zwj = false;
  for(uint32_t c : seq) if(c == 0x200D) zwj = true;
  std::string name;
  char hex[12];
  for(uint32_t c : seq){
    if(c == 0xFE0F && !zwj) continue;   // Twemoji's file names keep U+FE0F only in joiner sequences
    snprintf(hex, sizeof(hex), "%s%x", name.empty() ? "" : "-", (unsigned)c);
    name += hex;
  }
  return name;
}

std::vector<TitleChar> titleChars(const char* utf8){
  std::vector<uint32_t> in = decodeUtf8(utf8);
  std::vector<TitleChar> out;
  size_t n = in.size();
  auto at = [&](size_t i){ return i < n ? in[i] : 0; };
  for(size_t i = 0; i < n; i++){
    uint32_t c = in[i];
    TitleChar tc;
    std::vector<uint32_t> seq;
    if(isRegional(c) && isRegional(at(i + 1))){                 // flag
      seq = {c, in[i + 1]}; i++;
      tc.cp = 0;
    } else if(isKeycapBase(c) && (at(i + 1) == 0x20E3 || (at(i + 1) == 0xFE0F && at(i + 2) == 0x20E3))){
      seq.push_back(c);
      if(at(i + 1) == 0xFE0F){ seq.push_back(0xFE0F); i++; }
      seq.push_back(0x20E3); i++;
      tc.cp = c;
    } else if((isEmojiDefault(c) && at(i + 1) != 0xFE0E) || (isEmojiWithSelector(c) && at(i + 1) == 0xFE0F)){
      tc.cp = c;
      seq.push_back(c);
      for(;;){
        if(at(i + 1) == 0xFE0F){ seq.push_back(0xFE0F); i++; }
        if(isModifier(at(i + 1))){ seq.push_back(in[i + 1]); i++; }
        while(isTag(at(i + 1))){ seq.push_back(in[i + 1]); i++; }
        uint32_t next = at(i + 2);
        if(at(i + 1) == 0x200D && (isEmojiDefault(next) || isEmojiWithSelector(next))){
          seq.push_back(0x200D); seq.push_back(next); i += 2;
          continue;
        }
        break;
      }
    } else {
      // stray selectors, joiners, keycap marks, modifiers, tags and single regional indicators
      if(c == 0xFE0E || c == 0xFE0F || c == 0x200D || c == 0x20E3 || isModifier(c) || isTag(c) || isRegional(c)) continue;
      tc.cp = c;
      out.push_back(tc);
      continue;
    }
    tc.emoji = twemojiName(seq);
    out.push_back(tc);
  }
  return out;
}

std::vector<uint32_t> titleCodepoints(const char* utf8){
  std::vector<uint32_t> out;
  for(const TitleChar& c : titleChars(utf8)) if(c.cp) out.push_back(c.cp);
  return out;
}
