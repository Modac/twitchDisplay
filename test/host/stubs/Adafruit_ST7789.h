#pragma once
// ST7789 emulator for the host simulation.
//
// Models the controller memory (240 x 320), the commands MADCTL, CASET, RASET
// and RAMWR, and the 170x320 panel that shows memory columns 35..204. The
// address mapping follows the ST7789 data sheet: the host column (CASET) and
// row (RASET) counters are exchanged if MV is set, then MX mirrors the memory
// column and MY the memory row. setRotation() uses the same MADCTL values
// and offsets as the Adafruit library. What the panel shows is written to
// g_fb in the landscape orientation of rotation 1 (as mounted).
#include "Adafruit_GFX.h"
#include <vector>
#include <cstring>
#define ST77XX_BLACK 0x0000
#define ST77XX_GREEN 0x07E0
#define ST77XX_CYAN 0x07FF
#define ST77XX_RED 0xF800
#define ST77XX_MAGENTA 0xF81F
#define ST77XX_YELLOW 0xFFE0
#define ST77XX_WHITE 0xFFFF
#define ST77XX_CASET 0x2A
#define ST77XX_RASET 0x2B
#define ST77XX_RAMWR 0x2C
#define ST77XX_MADCTL 0x36
#define ST77XX_MADCTL_MY 0x80
#define ST77XX_MADCTL_MX 0x40
#define ST77XX_MADCTL_MV 0x20
#define ST77XX_MADCTL_RGB 0x00
extern uint16_t g_fb[170][320];
extern int g_bitmapDraws;   // number of 64x64 bitmap draws (pic tiles)
struct TextBox { int x, y, w, h; std::string text; };
extern std::vector<TextBox> g_textBoxes; // text printed since the last fillScreen (6x8 px font * size, with wrapping)

// emulator state, readable by the tests
extern uint16_t g_gram[320][240];      // controller memory [row Y][column X]
extern uint8_t g_madctl;               // current MADCTL value
extern bool g_recordWrites;            // record the screen position of every written pixel
extern std::vector<std::pair<int,int>> g_writeOrder; // (x, y) in landscape, in write order
extern int g_panelFrameRate;             // last FRCTRL2 (0xC6) value, -1 = never set

class Adafruit_ST7789 {
  uint8_t cmd = 0; std::vector<uint8_t> data;
  int c0 = 0, c1 = 0, r0 = 0, r1 = 0, c = 0, r = 0; bool writing = false;
  // one pixel from the RAMWR stream at the current address
  void pixel(uint16_t color){
    int X = c, Y = r;
    if(g_madctl & ST77XX_MADCTL_MV){ X = r; Y = c; }
    if(g_madctl & ST77XX_MADCTL_MX) X = 239 - X;
    if(g_madctl & ST77XX_MADCTL_MY) Y = 319 - Y;
    if(X < 0 || X > 239 || Y < 0 || Y > 319){ fprintf(stderr, "OUT OF MEMORY RANGE %d,%d\n", X, Y); }
    else {
      g_gram[Y][X] = color;
      int x = 319 - Y, y = X - 35;  // panel (memory columns 35..204) seen in landscape
      if(y >= 0 && y < 170){ g_fb[y][x] = color; if(g_recordWrites) g_writeOrder.push_back({x, y}); }
      else fprintf(stderr, "OFFSCREEN memory column %d\n", X);
    }
    if(++c > c1){ c = c0; r++; }
  }
  void execute(){
    if(cmd == ST77XX_MADCTL && data.size() == 1) g_madctl = data[0];
    if(cmd == ST77XX_CASET && data.size() == 4){ c0 = data[0] << 8 | data[1]; c1 = data[2] << 8 | data[3]; }
    if(cmd == ST77XX_RASET && data.size() == 4){ r0 = data[0] << 8 | data[1]; r1 = data[2] << 8 | data[3]; }
  }
protected:
  int16_t _xstart = 0, _ystart = 0, _colstart = 35, _colstart2 = 35, _rowstart = 0, _rowstart2 = 0;
  uint8_t rotation = 0;
  void SPI_WRITE32(uint32_t v){ for(int s = 24; s >= 0; s -= 8) spiWrite((uint8_t)(v >> s)); }
public:
  Adafruit_ST7789(int,int,int){}
  void init(int,int){ setRotation(0); }
  void setSPISpeed(long){}
  void writeCommand(uint8_t command){ cmd = command; data.clear(); writing = (command == ST77XX_RAMWR); if(writing){ c = c0; r = r0; } }
  void spiWrite(uint8_t b){ data.push_back(b); execute(); }
  void writePixels(uint16_t* p, uint32_t n, bool = true, bool = false){ if(!writing){ fprintf(stderr, "pixels without RAMWR\n"); return; } for(uint32_t k = 0; k < n; k++) pixel(p[k]); }
  void startWrite(){} void endWrite(){}
  void sendCommand(uint8_t command, const uint8_t* d, uint8_t n){ writeCommand(command); for(uint8_t i = 0; i < n; i++) spiWrite(d[i]); if(command == 0xC6 && n == 1) g_panelFrameRate = d[0]; }
  // same as Adafruit_ST7789::setRotation() for a 170x320 panel
  void setRotation(uint8_t m){
    rotation = m & 3;
    uint8_t madctl = 0;
    switch(rotation){
      case 0: madctl = ST77XX_MADCTL_MX | ST77XX_MADCTL_MY; _xstart = _colstart; _ystart = _rowstart; break;
      case 1: madctl = ST77XX_MADCTL_MY | ST77XX_MADCTL_MV; _xstart = _rowstart; _ystart = _colstart2; break;
      case 2: madctl = 0; _xstart = _colstart2; _ystart = _rowstart2; break;
      case 3: madctl = ST77XX_MADCTL_MX | ST77XX_MADCTL_MV; _xstart = _rowstart2; _ystart = _colstart; break;
    }
    writeCommand(ST77XX_MADCTL); spiWrite(madctl);
  }
  // same as Adafruit_ST77xx::setAddrWindow()
  void setAddrWindow(int16_t x, int16_t y, int16_t w, int16_t h){
    x += _xstart; y += _ystart;
    writeCommand(ST77XX_CASET); SPI_WRITE32(((uint32_t)x << 16) | (uint32_t)(x + w - 1));
    writeCommand(ST77XX_RASET); SPI_WRITE32(((uint32_t)y << 16) | (uint32_t)(y + h - 1));
    writeCommand(ST77XX_RAMWR);
  }
  int16_t width() const { return (rotation & 1) ? 320 : 170; }
  int16_t height() const { return (rotation & 1) ? 170 : 320; }
  void fillScreen(uint16_t col){ fillRect(0, 0, width(), height(), col); g_textBoxes.clear(); }
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t col){
    if(w < 0){ x += w + 1; w = -w; } if(h < 0){ y += h + 1; h = -h; }
    if(x < 0){ w += x; x = 0; } if(y < 0){ h += y; y = 0; }
    if(x + w > width()) w = width() - x; if(y + h > height()) h = height() - y;
    if(w <= 0 || h <= 0) return;
    setAddrWindow(x, y, w, h);
    std::vector<uint16_t> line(w, col);
    for(int j = 0; j < h; j++) writePixels(line.data(), w);
  }
  void drawRGBBitmap(int16_t x, int16_t y, uint16_t* bm, int16_t w, int16_t h){
    g_bitmapDraws++;
    setAddrWindow(x, y, w, h);
    writePixels(bm, (uint32_t)w * h);
  }
  // rounded corners are not simulated
  void fillRoundRect(int16_t x,int16_t y,int16_t w,int16_t h,int16_t,uint16_t c){ fillRect(x,y,w,h,c); }
  void drawRoundRect(int16_t x,int16_t y,int16_t w,int16_t h,int16_t,uint16_t c){ drawRect(x,y,w,h,c); }
  void drawRect(int16_t x,int16_t y,int16_t w,int16_t h,uint16_t c){ fillRect(x,y,w,1,c); fillRect(x,y+h-1,w,1,c); fillRect(x,y,1,h,c); fillRect(x+w-1,y,1,h,c); }
  int cx=0, cy=0, tsz=1; bool wrap=true;
  void setTextWrap(bool w){ wrap=w; } void setTextSize(int s){ tsz=s; } void setTextColor(uint16_t){} void setCursor(int x,int y){ cx=x; cy=y; }
  void print(const char* t){
    int cw=6*tsz, ch=8*tsz, n=strlen(t), x=cx, y=cy, minx=cx, maxx=cx;
    for(int i=0;i<n;i++){ if(wrap && x+cw>320){ x=0; y+=ch; minx=0; } x+=cw; if(x>maxx) maxx=x; }
    g_textBoxes.push_back({minx, cy, maxx-minx, y+ch-cy, t}); cx=x; cy=y; }
};
