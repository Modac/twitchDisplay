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
};
