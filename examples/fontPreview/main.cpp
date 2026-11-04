/**
 * Test firmware: previews Unicode text on the display before it goes into
 * the real firmware.
 *
 *  - the built-in 5x7 font (CP437, scaled 8x) next to Noto Sans, static and
 *    scrolling, in two sizes and three weights, with Latin Extended, Greek
 *    and Cyrillic
 *  - emoji: monochrome from Noto Emoji (in the firmware) above the colour
 *    Twemoji images (downloaded)
 *  - Japanese, Korean and Chinese, rendered from pieces of Noto Sans JP/KR/SC
 *    downloaded on demand
 *  - a page with the measured times (glyph rendering, downloads) and memory
 *
 * Fonts are rendered with stb_truetype. The text fonts and the monochrome
 * emoji are subsets built by tools/make_preview_fonts.py; the colour emoji and
 * the CJK pieces come from cdn.jsdelivr.net (Twemoji, fontsource).
 *
 * WiFi: uses the network stored by the main firmware.
 *
 * Install it through the web interface's Update page. It never confirms
 * itself, so pressing RESET (or the automatic restart after 15 minutes) makes
 * the bootloader return to the main firmware (rollback). If it was flashed
 * over USB instead, flash the main firmware again afterwards.
 *
 * A short press on BOOT shows the next page; pages also advance by themselves.
 */
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <LogoImage.h>

#include <string>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define TFT_CS     7
#define TFT_RST   10
#define TFT_DC     1
#define TFT_BK     0
#define BUTTON_PIN 9

#define ROW_W      320
#define ROW_H      64            // a title row
#define EM         54.0f         // text size that fits a row with accents and descenders
#define BASELINE   51            // baseline within a row for EM
#define PAGE_TIME  15000         // ms per page
#define SCROLL_PX_PER_S 278      // speed of the real firmware's title
#define RESTART_AFTER (15UL * 60 * 1000)

#define CDN "https://cdn.jsdelivr.net"

// Fonts in the firmware (board_build.embed_files)
extern const uint8_t font600[]  asm("_binary_examples_fontPreview_fonts_NotoSans_600_ttf_start");
extern const uint8_t font400[]  asm("_binary_examples_fontPreview_fonts_NotoSans_400_ttf_start");
extern const uint8_t font700[]  asm("_binary_examples_fontPreview_fonts_NotoSans_700_ttf_start");
extern const uint8_t fontEmoji[] asm("_binary_examples_fontPreview_fonts_NotoEmoji_600_ttf_start");

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 row(ROW_W, ROW_H);
WiFiClientSecure tls;

stbtt_fontinfo sans600, sans400, sans700, emojiFont;
bool fontsOk = false;
bool wifiOk = false;

// measurements for the summary page
struct Stats {
  uint32_t glyphs = 0, glyphUs = 0, glyphMaxUs = 0;       // Noto Sans glyphs
  uint32_t cjkGlyphs = 0, cjkGlyphUs = 0;
  uint32_t emojiGlyphs = 0, emojiGlyphUs = 0;             // monochrome
  uint32_t pngOk = 0, pngFail = 0, pngBytes = 0, pngMs = 0, pngMaxMs = 0;
  uint32_t pieces = 0, pieceFail = 0, pieceBytes = 0, pieceMs = 0, pieceMaxBytes = 0;
  uint32_t scrollFrames = 0, scrollUs = 0, scrollMaxUs = 0;
  String lastError;
} stats;

// Never confirm this firmware: the next restart returns to the main firmware
extern "C" bool verifyRollbackLater(){
  return true;
}

// ---------- helpers

std::vector<uint32_t> utf8(const char* s){
  std::vector<uint32_t> out;
  const uint8_t* p = (const uint8_t*)s;
  while(*p){
    uint32_t cp; int len;
    if(*p < 0x80)                { cp = *p;        len = 1; }
    else if((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
    else if((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
    else                         { cp = *p & 0x07; len = 4; }
    for(int k = 1; k < len; k++) cp = (cp << 6) | (p[k] & 0x3F);
    p += len;
    out.push_back(cp);
  }
  return out;
}

// The main firmware's conversion to the built-in font (CP437)
static const uint8_t latin1ToCp437[96] = {
  ' ',  0xAD, 0x9B, 0x9C, '*',  0x9D, '|',  'S',  '"',  'c',  0xA6, 0xAE, 0xAA, 0,    'R',  '-',
  0xF8, 0xF1, 0xFD, '3',  '\'', 0xE6, 'P',  0xFA, ',',  '1',  0xA7, 0xAF, 0xAC, 0xAB, '3',  0xA8,
  'A',  'A',  'A',  'A',  0x8E, 0x8F, 0x92, 0x80, 'E',  0x90, 'E',  'E',  'I',  'I',  'I',  'I',
  'D',  0xA5, 'O',  'O',  'O',  'O',  0x99, 'x',  'O',  'U',  'U',  'U',  0x9A, 'Y',  'P',  0xE1,
  0x85, 0xA0, 0x83, 'a',  0x84, 0x86, 0x91, 0x87, 0x8A, 0x82, 0x88, 0x89, 0x8D, 0xA1, 0x8C, 0x8B,
  'd',  0xA4, 0x95, 0xA2, 0x93, 'o',  0x94, 0xF6, 'o',  0x97, 0xA3, 0x96, 0x81, 'y',  'p',  0x98
};
std::string toCp437(const char* s){
  std::string out;
  for(uint32_t cp : utf8(s)){
    if(cp < 0x20) out += ' ';
    else if(cp < 0x7F) out += (char)cp;
    else if(cp >= 0xA0 && cp <= 0xFF){ if(latin1ToCp437[cp - 0xA0]) out += (char)latin1ToCp437[cp - 0xA0]; }
    else switch(cp){
      case 0x2013: case 0x2014: out += '-';  break;
      case 0x2018: case 0x2019: case 0x201A: out += '\''; break;
      case 0x201C: case 0x201D: case 0x201E: out += '"';  break;
      case 0x2022: out += (char)0xFA; break;
      case 0x2026: out += "..."; break;
      case 0x20AC: out += "EUR"; break;
      default: break;
    }
  }
  return out;
}

bool buttonPressed(){
  static bool last = false;
  bool now = digitalRead(BUTTON_PIN) == LOW;
  bool pressed = now && !last;
  last = now;
  if(pressed) delay(30);
  return pressed;
}

// Waits up to ms; true if BOOT was pressed (next page)
bool waitOrButton(unsigned long ms){
  unsigned long t0 = millis();
  while(millis() - t0 < ms){
    if(buttonPressed()) return true;
    if(millis() > RESTART_AFTER) ESP.restart();
    delay(10);
  }
  return false;
}

void label(int x, int y, const char* text, uint16_t color = 0x8410){
  tft.setTextSize(1);
  tft.setTextColor(color, ST77XX_BLACK);
  tft.setCursor(x, y);
  tft.print(text);
}

void showRow(int y){
  tft.drawRGBBitmap(0, y, row.getBuffer(), ROW_W, ROW_H);
}

// ---------- glyph rendering (stb_truetype)

struct Glyph {
  bool missing = false;
  int advance = 0, w = 0, h = 0, xoff = 0, yoff = 0;
  std::vector<uint8_t> alpha;
};

Glyph renderGlyph(const stbtt_fontinfo* f, uint32_t cp, float em, uint32_t* us = nullptr){
  Glyph g;
  unsigned long t0 = micros();
  int gi = stbtt_FindGlyphIndex(f, cp);
  float s = stbtt_ScaleForMappingEmToPixels(f, em);
  if(gi == 0){ g.missing = true; g.advance = (int)(em * 0.6f); return g; }
  int adv, lsb;
  stbtt_GetGlyphHMetrics(f, gi, &adv, &lsb);
  g.advance = (int)lroundf(adv * s);
  int x0, y0, x1, y1;
  stbtt_GetGlyphBitmapBox(f, gi, s, s, &x0, &y0, &x1, &y1);
  g.w = x1 - x0; g.h = y1 - y0; g.xoff = x0; g.yoff = y0;
  if(g.w > 0 && g.h > 0){
    g.alpha.resize(g.w * g.h);
    stbtt_MakeGlyphBitmap(f, g.alpha.data(), g.w, g.h, g.w, s, s, gi);
  }
  if(us) *us = micros() - t0;
  return g;
}

static inline uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t a){
  if(a == 255) return fg;
  uint32_t r = (((fg >> 11) & 31) * a + ((bg >> 11) & 31) * (255 - a)) / 255;
  uint32_t gr = (((fg >> 5) & 63) * a + ((bg >> 5) & 63) * (255 - a)) / 255;
  uint32_t b = ((fg & 31) * a + (bg & 31) * (255 - a)) / 255;
  return (uint16_t)(r << 11 | gr << 5 | b);
}

// Draws a glyph into the row canvas with its origin at (x, baseline)
void blit(const Glyph& g, int x, int baseline, float em, uint16_t color){
  if(g.missing){   // box for characters the font does not have
    int h = (int)(em * 0.7f);
    row.drawRect(x + 3, baseline - h, g.advance - 6, h, color);
    return;
  }
  uint16_t* buf = row.getBuffer();
  for(int j = 0; j < g.h; j++){
    int py = baseline + g.yoff + j;
    if(py < 0 || py >= ROW_H) continue;
    for(int i = 0; i < g.w; i++){
      int px = x + g.xoff + i;
      if(px < 0 || px >= ROW_W) continue;
      uint8_t a = g.alpha[j * g.w + i];
      if(a) buf[py * ROW_W + px] = blend565(buf[py * ROW_W + px], color, a);
    }
  }
}

// Draws text into the row canvas: Noto Sans, characters it lacks from the
// emoji font, otherwise a box. Returns the width.
int drawText(const stbtt_fontinfo* f, const char* text, float em, int x, int baseline, uint16_t color){
  int x0 = x;
  for(uint32_t cp : utf8(text)){
    if(cp == 0xFE0F || cp == 0x200D) continue;
    const stbtt_fontinfo* use = f;
    if(!stbtt_FindGlyphIndex(f, cp) && stbtt_FindGlyphIndex(&emojiFont, cp)) use = &emojiFont;
    uint32_t us = 0;
    Glyph g = renderGlyph(use, cp, em, &us);
    if(use == &emojiFont){ stats.emojiGlyphs++; stats.emojiGlyphUs += us; }
    else if(!g.missing){
      stats.glyphs++; stats.glyphUs += us;
      if(us > stats.glyphMaxUs) stats.glyphMaxUs = us;
    }
    blit(g, x, baseline, em, color);
    x += g.advance;
  }
  return x - x0;
}

void oldFontText(const char* text, int x){
  row.setTextSize(8);
  row.setTextWrap(false);
  row.cp437(true);
  row.setTextColor(ST77XX_CYAN);
  row.setCursor(x, 0);
  row.print(toCp437(text).c_str());
}

// ---------- downloads

// Downloads url into data (at most maxLen bytes). Returns "" or an error.
String download(const String& url, std::vector<uint8_t>& data, size_t maxLen, uint32_t* ms){
  data.clear();
  if(!wifiOk) return "no WiFi";
  unsigned long t0 = millis();
  HTTPClient http;
  http.useHTTP10(true);
  http.begin(tls, url);
  http.setUserAgent("twitchDisplay-fontPreview");
  http.setTimeout(10000);
  int code = http.GET();
  if(code != HTTP_CODE_OK){
    String e = "HTTP " + (code <= 0 ? http.errorToString(code) : String(code));
    http.end();
    return e;
  }
  int size = http.getSize();
  if(size <= 0 || size > (int)maxLen){ http.end(); return "size " + String(size); }
  if(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < (size_t)size + 4096){
    http.end();
    return "no RAM for " + String(size / 1024) + " KB";
  }
  data.resize(size);
  size_t got = 0;
  auto* stream = http.getStreamPtr();
  unsigned long last = millis();
  while(got < (size_t)size && millis() - last < 10000){
    size_t n = stream->available();
    if(!n){ delay(1); continue; }
    got += stream->readBytes(data.data() + got, std::min(n, (size_t)size - got));
    last = millis();
  }
  http.end();
  tls.stop();
  if(ms) *ms = millis() - t0;
  if(got != (size_t)size){ data.clear(); return "incomplete"; }
  return "";
}

// ---------- pages

void pageInfo(){
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(3); tft.setTextColor(ST77XX_CYAN); tft.setCursor(10, 10); tft.print("Font preview");
  tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(10, 50); tft.printf("Fonts: %s", fontsOk ? "OK" : "FAILED");
  tft.setCursor(10, 64); tft.printf("WiFi: %s", wifiOk ? WiFi.localIP().toString().c_str() : "not connected (no downloads)");
  tft.setCursor(10, 78); tft.printf("Free memory %u KB, largest block %u KB", (unsigned)(ESP.getFreeHeap() / 1024),
                                    (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024));
  tft.setCursor(10, 106); tft.print("BOOT: next page    RESET: back to the");
  tft.setCursor(10, 120); tft.print("normal firmware (also after 15 min)");
  waitOrButton(6000);
}

const char* SAMPLE = "Grüße – „Let’s Play“";
const char* SCROLL_TEXT = "Montags-Stream – „Let’s Play“ mit Ærø, Łódź & Šibenik … 50 € Spende? Привет! Γειά σου! 🔥🎮";

// built-in font above Noto Sans, same text
void pageCompare(){
  tft.fillScreen(ST77XX_BLACK);
  label(4, 2, "built-in font (5x7, CP437, 8x)");
  row.fillScreen(ST77XX_BLACK); oldFontText(SAMPLE, 0); showRow(12);
  label(4, 88, "Noto Sans SemiBold, 54 px");
  row.fillScreen(ST77XX_BLACK); drawText(&sans600, SAMPLE, EM, 0, BASELINE, ST77XX_CYAN); showRow(98);
  waitOrButton(PAGE_TIME);
}

// both fonts scroll like the title (278 px/s), Noto Sans glyphs cached
void pageScroll(){
  tft.fillScreen(ST77XX_BLACK);
  label(4, 2, "built-in font: scrolling");
  label(4, 88, "Noto Sans: scrolling (characters it lacks: emoji font, else box)");
  // lay out the Noto Sans line once: glyphs rendered when they come into view
  struct Item { uint32_t cp; const stbtt_fontinfo* f; int x; Glyph g; bool ready; };
  std::vector<Item> items;
  int x = ROW_W;
  for(uint32_t cp : utf8(SCROLL_TEXT)){
    if(cp == 0xFE0F || cp == 0x200D) continue;
    const stbtt_fontinfo* f = &sans600;
    if(!stbtt_FindGlyphIndex(f, cp) && stbtt_FindGlyphIndex(&emojiFont, cp)) f = &emojiFont;
    int gi = stbtt_FindGlyphIndex(f, cp), adv = (int)(EM * 0.6f), lsb;
    if(gi){ stbtt_GetGlyphHMetrics(f, gi, &adv, &lsb); adv = (int)lroundf(adv * stbtt_ScaleForMappingEmToPixels(f, EM)); }
    items.push_back({cp, f, x, Glyph(), false});
    x += adv;
  }
  int newWidth = x;
  int oldWidth = ROW_W + (int)toCp437(SCROLL_TEXT).size() * 48;
  int total = std::max(newWidth, oldWidth);
  unsigned long t0 = micros();
  stats.scrollFrames = 0; stats.scrollUs = 0; stats.scrollMaxUs = 0;
  while(true){
    unsigned long f0 = micros();
    int offset = (int)((uint64_t)(f0 - t0) * SCROLL_PX_PER_S / 1000000);
    if(offset > total || buttonPressed()) break;
    row.fillScreen(ST77XX_BLACK);
    oldFontText(SCROLL_TEXT, ROW_W - offset);
    showRow(12);
    row.fillScreen(ST77XX_BLACK);
    for(auto& it : items){
      int gx = it.x - offset;
      if(gx > ROW_W) break;
      if(gx + EM * 1.5f < 0){ if(it.ready){ it.g = Glyph(); it.ready = false; } continue; }   // passed: free
      if(!it.ready){
        uint32_t us = 0;
        it.g = renderGlyph(it.f, it.cp, EM, &us);
        it.ready = true;
        if(it.f == &sans600 && !it.g.missing){ stats.glyphs++; stats.glyphUs += us; if(us > stats.glyphMaxUs) stats.glyphMaxUs = us; }
      }
      blit(it.g, gx, BASELINE, EM, ST77XX_CYAN);
    }
    showRow(98);
    uint32_t us = micros() - f0;
    stats.scrollFrames++; stats.scrollUs += us;
    if(us > stats.scrollMaxUs) stats.scrollMaxUs = us;
    if(millis() > RESTART_AFTER) ESP.restart();
  }
}

void pageSizes(){
  const char* text = "Äpfel & Zögern – gj";
  tft.fillScreen(ST77XX_BLACK);
  label(4, 2, "54 px: fits the row (accents and descenders)");
  row.fillScreen(ST77XX_BLACK); drawText(&sans600, text, EM, 0, BASELINE, ST77XX_CYAN); showRow(12);
  label(4, 88, "64 px: larger, tops of accents and descenders cut off");
  row.fillScreen(ST77XX_BLACK); drawText(&sans600, text, 64, 0, 56, ST77XX_CYAN); showRow(98);
  waitOrButton(PAGE_TIME);
}

// three lines of 56 px at 46 px font size
void threeLines(const stbtt_fontinfo* f[3], const char* text[3]){
  tft.fillScreen(ST77XX_BLACK);
  for(int i = 0; i < 3; i++){
    row.fillScreen(ST77XX_BLACK);
    drawText(f[i], text[i], 46, 0, 44, ST77XX_CYAN);
    tft.drawRGBBitmap(0, i * 57, row.getBuffer(), ROW_W, 56);
  }
}

void pageWeights(){
  const stbtt_fontinfo* f[3] = {&sans400, &sans600, &sans700};
  const char* t[3] = {"400 Regular – Grüße", "600 SemiBold – Grüße", "700 Bold – Grüße"};
  threeLines(f, t);
  waitOrButton(PAGE_TIME);
}

void pageScripts(){
  const stbtt_fontinfo* f[3] = {&sans600, &sans600, &sans600};
  const char* t[3] = {"Łódź · Šibenik · Ærø", "Привет, мир!", "Γειά σου κόσμε"};
  threeLines(f, t);
  waitOrButton(PAGE_TIME);
}

struct Emoji { const char* text; const char* file; };
const Emoji EMOJI[] = {
  {"😀", "1f600"}, {"😂", "1f602"}, {"😍", "1f60d"}, {"🤔", "1f914"}, {"😭", "1f62d"},
  {"👍", "1f44d"}, {"❤️", "2764"},  {"🔥", "1f525"}, {"🎮", "1f3ae"}, {"🏆", "1f3c6"},
  {"🎉", "1f389"}, {"💯", "1f4af"}, {"✅", "2705"},  {"🚀", "1f680"}, {"🍕", "1f355"},
  {"☕", "2615"},  {"👀", "1f440"}, {"🤖", "1f916"}, {"🇩🇪", "1f1e9-1f1ea"}, {"👨‍💻", "1f468-200d-1f4bb"},
};

// monochrome (font) above colour (downloaded), five emoji per page
void pageEmoji(int first){
  tft.fillScreen(ST77XX_BLACK);
  label(4, 2, "monochrome: Noto Emoji (in the firmware)");
  label(4, 88, "colour: Twemoji 72x72 PNG (downloaded)");
  row.fillScreen(ST77XX_BLACK);
  for(int k = 0; k < 5; k++){
    const Emoji& e = EMOJI[first + k];
    std::vector<uint32_t> cps;
    for(uint32_t cp : utf8(e.text)) if(cp != 0xFE0F && cp != 0x200D) cps.push_back(cp);
    float em = EM / cps.size();     // sequences (flag, ZWJ) as their parts, side by side
    int x = k * 64 + 5;
    for(uint32_t cp : cps){
      uint32_t us = 0;
      Glyph g = renderGlyph(&emojiFont, cp, em, &us);
      stats.emojiGlyphs++; stats.emojiGlyphUs += us;
      blit(g, x, BASELINE, em, ST77XX_YELLOW);
      x += g.advance;
    }
  }
  showRow(12);
  static uint16_t logo[LOGO_SIZE * LOGO_SIZE], small[54 * 54];
  for(int k = 0; k < 5; k++){
    const Emoji& e = EMOJI[first + k];
    std::vector<uint8_t> png;
    uint32_t ms = 0;
    String err = download(String(CDN "/gh/jdecked/twemoji@latest/assets/72x72/") + e.file + ".png", png, 16384, &ms);
    const char* perr = err.length() ? nullptr : pngToLogo(png.data(), png.size(), logo);
    if(err.length() || perr){
      stats.pngFail++;
      stats.lastError = String(e.file) + ": " + (err.length() ? err : String(perr));
      label(k * 64 + 2, 120, "failed", ST77XX_RED);
      continue;
    }
    stats.pngOk++; stats.pngBytes += png.size(); stats.pngMs += ms;
    if(ms > stats.pngMaxMs) stats.pngMaxMs = ms;
    scaleRgb565(logo, LOGO_SIZE, LOGO_SIZE, small, 54, 54);
    tft.drawRGBBitmap(k * 64 + 5, 98 + 5, small, 54, 54);
  }
  waitOrButton(PAGE_TIME);
}

// Japanese, Korean, Chinese: each character from the fontsource piece that
// contains it (pieces looked up from fontsource's unicode-range lists)
struct CjkChar { const char* text; const char* font; int piece; };
const CjkChar CJK_LINES[3][5] = {
  {{"ゲ", "noto-sans-jp", 119}, {"ー", "noto-sans-jp", 119}, {"ム", "noto-sans-jp", 119}, {"配", "noto-sans-jp", 112}, {"信", "noto-sans-jp", 114}},
  {{"안", "noto-sans-kr", 118}, {"녕", "noto-sans-kr", 113}, {"하", "noto-sans-kr", 119}, {"세", "noto-sans-kr", 118}, {"요", "noto-sans-kr", 118}},
  {{"你", "noto-sans-sc", 118}, {"好", "noto-sans-sc", 118}, {"世", "noto-sans-sc", 116}, {"界", "noto-sans-sc", 116}, {nullptr, nullptr, 0}},
};
const char* CJK_NAMES[3] = {"Japanese", "Korean", "Chinese (simpl.)"};

void pageCjk(){
  tft.fillScreen(ST77XX_BLACK);
  for(int line = 0; line < 3; line++){
    row.fillScreen(ST77XX_BLACK);
    std::vector<uint8_t> piece;
    String loaded;
    stbtt_fontinfo info;
    int x = 0;
    String note;
    for(const CjkChar& c : CJK_LINES[line]){
      if(!c.text) break;
      String url = String(CDN "/fontsource/fonts/") + c.font + "@latest/" + c.piece + "-600-normal.ttf";
      if(url != loaded){
        piece.clear(); piece.shrink_to_fit();
        uint32_t ms = 0;
        String err = download(url, piece, 200000, &ms);
        loaded = url;
        if(err.length() || !stbtt_InitFont(&info, piece.data(), 0)){
          stats.pieceFail++;
          stats.lastError = String(c.font) + " " + c.piece + ": " + (err.length() ? err : "bad font");
          note = "piece " + String(c.piece) + " failed";
          piece.clear();
        } else {
          stats.pieces++; stats.pieceBytes += piece.size(); stats.pieceMs += ms;
          if(piece.size() > stats.pieceMaxBytes) stats.pieceMaxBytes = piece.size();
        }
      }
      Glyph g;
      if(piece.empty()){ g.missing = true; g.advance = 46; }
      else {
        uint32_t us = 0;
        g = renderGlyph(&info, utf8(c.text)[0], 46, &us);
        stats.cjkGlyphs++; stats.cjkGlyphUs += us;
      }
      blit(g, x, 44, 46, ST77XX_CYAN);
      x += g.advance;
    }
    tft.drawRGBBitmap(0, line * 57, row.getBuffer(), ROW_W, 56);
    label(240, line * 57 + 4, CJK_NAMES[line]);
    if(note.length()) label(240, line * 57 + 18, note.c_str(), ST77XX_RED);
  }
  waitOrButton(PAGE_TIME);
}

void pageStats(){
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE);
  int y = 2;
  auto line = [&](const String& s){ tft.setCursor(4, y); tft.print(s); Serial.println(s); y += 12; };
  line("Measurements");
  if(stats.glyphs) line("Noto Sans glyph 54px: avg " + String(stats.glyphUs / stats.glyphs / 1000.0, 1) + " ms, max "
                        + String(stats.glyphMaxUs / 1000.0, 1) + " ms (" + stats.glyphs + ")");
  if(stats.emojiGlyphs) line("Mono emoji glyph: avg " + String(stats.emojiGlyphUs / stats.emojiGlyphs / 1000.0, 1) + " ms");
  if(stats.cjkGlyphs) line("CJK glyph 46px: avg " + String(stats.cjkGlyphUs / stats.cjkGlyphs / 1000.0, 1) + " ms");
  if(stats.scrollFrames) line("Scrolling frame (both rows): avg " + String(stats.scrollUs / stats.scrollFrames / 1000.0, 1)
                              + " ms, max " + String(stats.scrollMaxUs / 1000.0, 1) + " ms");
  line("Colour emoji: " + String(stats.pngOk) + " ok, " + stats.pngFail + " failed"
       + (stats.pngOk ? ", avg " + String(stats.pngBytes / stats.pngOk) + " B, " + String(stats.pngMs / stats.pngOk) + " ms, max " + stats.pngMaxMs + " ms" : String("")));
  line("CJK pieces: " + String(stats.pieces) + " ok, " + stats.pieceFail + " failed"
       + (stats.pieces ? ", " + String(stats.pieceBytes / 1024) + " KB, avg " + String(stats.pieceMs / stats.pieces) + " ms, largest " + String(stats.pieceMaxBytes / 1024) + " KB" : String("")));
  line("Free memory " + String(ESP.getFreeHeap() / 1024) + " KB (lowest " + String(ESP.getMinFreeHeap() / 1024)
       + " KB, largest block " + String((unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024)) + " KB)");
  if(stats.lastError.length()) line("Last error: " + stats.lastError);
  waitOrButton(PAGE_TIME * 2);
}

// ---------- setup / loop

void connectWiFi(){
  Preferences prefs;
  prefs.begin("config", true);   // the main firmware's settings
  String ssid = prefs.getString("ssid", ""), pw = prefs.getString("wifipw", "");
  prefs.end();
  if(!ssid.length()) return;
  tft.setTextSize(2); tft.setTextColor(ST77XX_WHITE); tft.setCursor(10, 70);
  tft.print("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pw.c_str());
  unsigned long t0 = millis();
  while(WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(100);
  wifiOk = WiFi.status() == WL_CONNECTED;
}

void setup(){
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(TFT_BK, OUTPUT);
  digitalWrite(TFT_BK, HIGH);
  tft.init(170, 320);
  tft.setSPISpeed(80000000);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  fontsOk = stbtt_InitFont(&sans600, font600, 0) && stbtt_InitFont(&sans400, font400, 0)
            && stbtt_InitFont(&sans700, font700, 0) && stbtt_InitFont(&emojiFont, fontEmoji, 0);
  tls.useBuiltinCACertBundle();   // verify servers with the root certificates built into the core
  connectWiFi();
}

void loop(){
  pageInfo();
  if(!fontsOk) return;
  pageCompare();
  pageScroll();
  pageSizes();
  pageWeights();
  pageScripts();
  for(int first = 0; first < 20; first += 5) pageEmoji(first);
  pageCjk();
  pageStats();
}
