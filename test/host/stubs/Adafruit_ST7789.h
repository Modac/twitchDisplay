#pragma once
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
extern uint16_t g_fb[170][320];
extern int g_bitmapDraws;   // number of 64x64 bitmap draws (pic tiles)
struct TextBox { int x, y, w, h; std::string text; };
extern std::vector<TextBox> g_textBoxes; // text printed since the last fillScreen (6x8 px font * size, with wrapping)
class Adafruit_ST7789 {
  int16_t ax,ay,aw,ah; long apos;
  void px(int x,int y,uint16_t c){ if(x>=0&&x<320&&y>=0&&y<170) g_fb[y][x]=c; else { fprintf(stderr,"OFFSCREEN %d,%d\n",x,y); } }
public:
  Adafruit_ST7789(int,int,int){}
  void init(int,int){} void setRotation(int){} void setSPISpeed(long){}
  void fillScreen(uint16_t c){ for(auto& r:g_fb) for(auto& p:r) p=c; g_textBoxes.clear(); }
  void fillRect(int16_t x,int16_t y,int16_t w,int16_t h,uint16_t c){
    if(w<0){x+=w+1;w=-w;} if(h<0){y+=h+1;h=-h;}
    for(int j=y;j<y+h;j++) for(int i=x;i<x+w;i++) px(i,j,c); }
  void drawRGBBitmap(int16_t x,int16_t y,uint16_t* bm,int16_t w,int16_t h){
    g_bitmapDraws++; for(int j=0;j<h;j++) for(int i=0;i<w;i++) px(x+i,y+j,bm[j*w+i]); }
  void startWrite(){} void endWrite(){}
  int cx=0, cy=0, tsz=1; bool wrap=true;
  void setTextWrap(bool w){ wrap=w; } void setTextSize(int s){ tsz=s; } void setTextColor(uint16_t){} void setCursor(int x,int y){ cx=x; cy=y; }
  void print(const char* t){
    int cw=6*tsz, ch=8*tsz, n=strlen(t), x=cx, y=cy, minx=cx, maxx=cx;
    for(int i=0;i<n;i++){ if(wrap && x+cw>320){ x=0; y+=ch; minx=0; } x+=cw; if(x>maxx) maxx=x; }
    g_textBoxes.push_back({minx, cy, maxx-minx, y+ch-cy, t}); cx=x; cy=y; }
  void drawRect(int16_t x,int16_t y,int16_t w,int16_t h,uint16_t c){ fillRect(x,y,w,1,c); fillRect(x,y+h-1,w,1,c); fillRect(x,y,1,h,c); fillRect(x+w-1,y,1,h,c); }
  void setAddrWindow(int16_t x,int16_t y,int16_t w,int16_t h){ ax=x;ay=y;aw=w;ah=h;apos=0; }
  void writePixels(uint16_t* p,uint32_t n){ for(uint32_t k=0;k<n;k++,apos++) px(ax+apos%aw, ay+apos/aw, p[k]); }
};
