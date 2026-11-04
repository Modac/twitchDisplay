#include "LogoImage.h"

#include <new>
#include <stdlib.h>
#include <string.h>
#include <PNGdec.h>

namespace {

struct DecodeTarget {
  PNG* png;
  uint16_t* pixels;
  int width;
};

int drawLine(PNGDRAW* draw){
  DecodeTarget* t = (DecodeTarget*)draw->pUser;
  // 0x000000: blend transparent pixels onto black (the display background)
  t->png->getLineAsRGB565(draw, &t->pixels[draw->y * t->width], PNG_RGB565_LITTLE_ENDIAN, 0x000000);
  return 1;
}

inline void unpack(uint16_t c, uint32_t& r, uint32_t& g, uint32_t& b){
  r = (c >> 11) & 0x1F; r = (r << 3) | (r >> 2);
  g = (c >> 5) & 0x3F;  g = (g << 2) | (g >> 4);
  b = c & 0x1F;         b = (b << 3) | (b >> 2);
}

// Overlap of source cell i ([i*dst, (i+1)*dst)) with output cell o ([o*src, (o+1)*src))
inline uint32_t overlap(int i, int o, int src, int dst){
  long a = (long)i * dst, b = a + dst;
  long c = (long)o * src, d = c + src;
  long lo = a > c ? a : c, hi = b < d ? b : d;
  return hi > lo ? (uint32_t)(hi - lo) : 0;
}

} // namespace

void scaleRgb565(const uint16_t* src, int srcW, int srcH, uint16_t* dst, int dstW, int dstH){
  for(int oy = 0; oy < dstH; oy++){
    int y0 = (int)((long)oy * srcH / dstH);
    int y1 = (int)(((long)(oy + 1) * srcH - 1) / dstH);
    for(int ox = 0; ox < dstW; ox++){
      int x0 = (int)((long)ox * srcW / dstW);
      int x1 = (int)(((long)(ox + 1) * srcW - 1) / dstW);
      uint64_t sr = 0, sg = 0, sb = 0, sw = 0;
      for(int y = y0; y <= y1; y++){
        uint32_t wy = overlap(y, oy, srcH, dstH);
        for(int x = x0; x <= x1; x++){
          uint64_t w = (uint64_t)wy * overlap(x, ox, srcW, dstW);
          uint32_t r, g, b;
          unpack(src[y * srcW + x], r, g, b);
          sr += r * w; sg += g * w; sb += b * w; sw += w;
        }
      }
      uint32_t r = (uint32_t)((sr + sw / 2) / sw), g = (uint32_t)((sg + sw / 2) / sw), b = (uint32_t)((sb + sw / 2) / sw);
      // round to the nearest RGB565 value
      uint32_t r5 = (r * 31 + 127) / 255, g6 = (g * 63 + 127) / 255, b5 = (b * 31 + 127) / 255;
      dst[oy * dstW + ox] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
    }
  }
}

const char* pngToLogo(const uint8_t* data, size_t len, uint16_t* out){
  static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if(len < 8 || memcmp(data, signature, 8) != 0) return "Unsupported image format (not PNG)";

  PNG* png = new (std::nothrow) PNG(); // ~45 KB, too big for a task stack
  if(!png) return "Out of memory";
  const char* error = nullptr;
  uint16_t* pixels = nullptr;
  if(png->openRAM((uint8_t*)data, (int)len, drawLine) != PNG_SUCCESS){
    error = png->getLastError() == PNG_UNSUPPORTED_FEATURE ? "Unsupported PNG (interlaced or 16 bit)" : "Invalid PNG image";
  } else {
    int w = png->getWidth(), h = png->getHeight();
    if(w < 1 || h < 1) error = "Invalid PNG image";
    else if(w > LOGO_MAX_SOURCE || h > LOGO_MAX_SOURCE) error = "Image too large";
    else if(!(pixels = (uint16_t*)malloc((size_t)w * h * sizeof(uint16_t)))) error = "Out of memory";
    else {
      memset(pixels, 0, (size_t)w * h * sizeof(uint16_t));
      DecodeTarget target = {png, pixels, w};
      int rc = png->decode(&target, 0);
      if(rc != PNG_SUCCESS) error = "Invalid PNG image";
      else scaleRgb565(pixels, w, h, out, LOGO_SIZE, LOGO_SIZE);
    }
    png->close();
  }
  free(pixels);
  delete png;
  return error;
}
