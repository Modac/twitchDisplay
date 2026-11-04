#pragma once
#include "Arduino.h"
#include <vector>
class GFXcanvas16 {
  int16_t w,h; std::vector<uint16_t> buf;
public:
  GFXcanvas16(int16_t w,int16_t h):w(w),h(h),buf(w*h,0){}
  void fillScreen(uint16_t c){ std::fill(buf.begin(),buf.end(),c); }
  int cy = 0, tsz = 1; std::string lastText;
  void setCursor(int16_t,int16_t y){ cy = y; } void setTextWrap(bool){} void setTextColor(uint16_t){} void setTextSize(uint8_t s){ tsz = s; } void cp437(bool=true){}
  // text rendering is not simulated: the text's rows (8 px * size from the cursor) get a marker colour
  void band(uint16_t c){ for(int y = std::max(0, cy); y < std::min<int>(h, cy + 8*tsz); y++) for(int x = 0; x < w; x++) buf[y*w + x] = c; }
  void print(const char* t){ lastText = t; band(0x1234); }
  int printf(const char* f, ...){ char b[64]; va_list ap; va_start(ap, f); vsnprintf(b, sizeof b, f, ap); va_end(ap); lastText = b; band(0x4321); return 0; }
  void drawRGBBitmap(int16_t,int16_t,const uint16_t* bm,int16_t bw,int16_t bh){ std::copy(bm,bm+bw*bh,buf.begin()); }
  uint16_t* getBuffer(){ return buf.data(); }
  int16_t width() const { return w; } int16_t height() const { return h; }
  void drawPixel(int16_t x, int16_t y, uint16_t c){ if(x >= 0 && x < w && y >= 0 && y < h) buf[y*w + x] = c; }
  void drawRect(int16_t x, int16_t y, int16_t rw, int16_t rh, uint16_t c){ for(int i = 0; i < rw; i++){ drawPixel(x+i, y, c); drawPixel(x+i, y+rh-1, c); } for(int j = 0; j < rh; j++){ drawPixel(x, y+j, c); drawPixel(x+rw-1, y+j, c); } }
};
// 1-bit canvas: drawChar is not simulated either, a character other than a
// space fills its 5x7 cell (times the size)
class GFXcanvas1 {
  int16_t w, h; std::vector<uint8_t> bits;
public:
  GFXcanvas1(int16_t w, int16_t h):w(w),h(h),bits(w*h,0){}
  void fillScreen(uint16_t c){ std::fill(bits.begin(), bits.end(), c ? 1 : 0); }
  void setTextWrap(bool){} void cp437(bool=true){}
  void drawChar(int16_t x, int16_t y, unsigned char c, uint16_t color, uint16_t, uint8_t size){
    if(c == ' ') return;
    for(int j = 0; j < 7*size; j++) for(int i = 0; i < 5*size; i++) if(x+i < w && y+j < h) bits[(y+j)*w + x+i] = color ? 1 : 0;
  }
  bool getPixel(int16_t x, int16_t y) const { return x >= 0 && x < w && y >= 0 && y < h && bits[y*w + x]; }
};
