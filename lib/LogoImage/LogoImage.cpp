#include "LogoImage.h"

#include <stdlib.h>
#include <string.h>
#include "miniz.h" // inflater (tinfl) in the ESP32 ROM

namespace {

inline uint32_t be32(const uint8_t* p){
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
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

// Box filter; pixel(x, y) returns the RGB565 source pixel
template<class Pixel>
void scaleBox(int srcW, int srcH, Pixel pixel, uint16_t* dst, int dstW, int dstH){
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
          unpack(pixel(x, y), r, g, b);
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

inline uint8_t paeth(int a, int b, int c){
  int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
  return (uint8_t)((pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c));
}

struct Image {
  int width = 0, height = 0, colorType = -1, bytesPerPixel = 0;
  int bitDepth = 8;          // 1, 2 or 4 only for grayscale and palette images (packed samples)
  size_t rowBytes = 0;       // per row, without the filter byte
  const uint8_t* palette = nullptr; int paletteSize = 0;      // PLTE entries
  const uint8_t* paletteAlpha = nullptr; int paletteAlphaSize = 0; // tRNS for palette images
  uint8_t* raw = nullptr; // unfiltered rows, each starting with its filter byte
};

// RGB565 of a pixel, transparency blended onto black
inline uint16_t pixel565(const Image& im, int x, int y){
  const uint8_t* row = im.raw + (size_t)y * (1 + im.rowBytes) + 1;
  const uint8_t* p = row + x * im.bytesPerPixel;
  uint8_t packed[1];
  if(im.bitDepth < 8){   // unpack the sample: grayscale scaled to 0..255, palette index as is
    int bit = x * im.bitDepth, mask = (1 << im.bitDepth) - 1;
    int v = (row[bit >> 3] >> (8 - im.bitDepth - (bit & 7))) & mask;
    packed[0] = (uint8_t)(im.colorType == 0 ? v * 255 / mask : v);
    p = packed;
  }
  uint32_t r, g, b, a = 255;
  switch(im.colorType){
    case 0: r = g = b = p[0]; break;
    case 2: r = p[0]; g = p[1]; b = p[2]; break;
    case 3: {
      int i = p[0];
      if(i < im.paletteSize){ r = im.palette[i * 3]; g = im.palette[i * 3 + 1]; b = im.palette[i * 3 + 2]; }
      else r = g = b = 0;
      if(i < im.paletteAlphaSize) a = im.paletteAlpha[i];
      break;
    }
    case 4: r = g = b = p[0]; a = p[1]; break;
    default: r = p[0]; g = p[1]; b = p[2]; a = p[3]; break; // 6: RGBA
  }
  if(a != 255){ r = r * a / 255; g = g * a / 255; b = b * a / 255; }
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Undoes the PNG row filters in place. False if a filter type is invalid.
bool unfilter(Image& im){
  size_t stride = 1 + im.rowBytes;
  int bpp = im.bytesPerPixel, n = (int)im.rowBytes;   // packed samples: filters work on whole bytes
  for(int y = 0; y < im.height; y++){
    uint8_t* row = im.raw + y * stride;
    uint8_t* cur = row + 1;
    const uint8_t* up = y ? row - stride + 1 : nullptr;
    switch(row[0]){
      case 0: break;
      case 1: for(int i = bpp; i < n; i++) cur[i] += cur[i - bpp]; break;
      case 2: if(up) for(int i = 0; i < n; i++) cur[i] += up[i]; break;
      case 3: for(int i = 0; i < n; i++) cur[i] += (uint8_t)(((i >= bpp ? cur[i - bpp] : 0) + (up ? up[i] : 0)) >> 1); break;
      case 4: for(int i = 0; i < n; i++) cur[i] += paeth(i >= bpp ? cur[i - bpp] : 0, up ? up[i] : 0, (up && i >= bpp) ? up[i - bpp] : 0); break;
      default: return false;
    }
  }
  return true;
}

} // namespace

void scaleRgb565(const uint16_t* src, int srcW, int srcH, uint16_t* dst, int dstW, int dstH){
  scaleBox(srcW, srcH, [&](int x, int y){ return src[y * srcW + x]; }, dst, dstW, dstH);
}

const char* pngToRgb565(const uint8_t* data, size_t len, uint16_t* out, int outW, int outH){
  static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if(len < 8 || memcmp(data, signature, 8) != 0) return "Unsupported image format (not PNG)";

  // first pass: header, palette, transparency; check that all chunks fit
  Image im;
  int bitDepth = 0, interlace = 0, idatChunks = 0;
  for(size_t pos = 8; pos + 12 <= len;){
    uint32_t chunkLen = be32(data + pos);
    const uint8_t* type = data + pos + 4;
    const uint8_t* body = data + pos + 8;
    if(chunkLen > len - pos - 12) return "Invalid PNG image";
    if(!memcmp(type, "IHDR", 4) && chunkLen >= 13){
      im.width = (int)be32(body); im.height = (int)be32(body + 4);
      bitDepth = body[8]; im.colorType = body[9]; interlace = body[12];
    } else if(!memcmp(type, "PLTE", 4)){
      im.palette = body; im.paletteSize = (int)(chunkLen / 3);
    } else if(!memcmp(type, "tRNS", 4)){
      im.paletteAlpha = body; im.paletteAlphaSize = (int)chunkLen;
    } else if(!memcmp(type, "IDAT", 4)){
      idatChunks++;
    } else if(!memcmp(type, "IEND", 4)) break;
    pos += 12 + chunkLen;
  }
  static const int bytesPerPixel[] = {1, 0, 3, 1, 2, 0, 4};
  if(im.width < 1 || im.height < 1 || im.colorType < 0 || im.colorType > 6 || !bytesPerPixel[im.colorType] || idatChunks == 0) return "Invalid PNG image";
  if(interlace) return "Unsupported PNG (interlaced)";
  bool packed = (bitDepth == 1 || bitDepth == 2 || bitDepth == 4) && (im.colorType == 0 || im.colorType == 3);
  if(bitDepth != 8 && !packed) return "Unsupported PNG (bit depth)";
  if(im.width > LOGO_MAX_SOURCE || im.height > LOGO_MAX_SOURCE) return "Image too large";
  if(im.colorType == 3 && !im.palette) return "Invalid PNG image";
  if(im.colorType != 3) im.paletteAlphaSize = 0; // tRNS colour keys of other types are ignored
  im.bytesPerPixel = bytesPerPixel[im.colorType];
  im.bitDepth = bitDepth;
  im.rowBytes = packed ? ((size_t)im.width * bitDepth + 7) / 8 : (size_t)im.width * im.bytesPerPixel;

  // second pass: unpack the IDAT chunks straight into the image buffer
  size_t rawSize = (size_t)im.height * (1 + im.rowBytes);
  tinfl_decompressor* inflator = (tinfl_decompressor*)malloc(sizeof(tinfl_decompressor));
  im.raw = (uint8_t*)malloc(rawSize);
  if(!inflator || !im.raw){ free(inflator); free(im.raw); return "Out of memory"; }
  tinfl_init(inflator);
  size_t outPos = 0;
  tinfl_status status = TINFL_STATUS_NEEDS_MORE_INPUT;
  int seen = 0;
  for(size_t pos = 8; pos + 12 <= len && status == TINFL_STATUS_NEEDS_MORE_INPUT;){
    uint32_t chunkLen = be32(data + pos);
    if(!memcmp(data + pos + 4, "IDAT", 4)){
      bool more = ++seen < idatChunks;
      size_t inSize = chunkLen, outSize = rawSize - outPos;
      status = tinfl_decompress(inflator, data + pos + 8, &inSize, im.raw, im.raw + outPos, &outSize,
        TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF | (more ? TINFL_FLAG_HAS_MORE_INPUT : 0));
      outPos += outSize;
    }
    pos += 12 + chunkLen;
  }
  free(inflator);

  const char* error = nullptr;
  if(status != TINFL_STATUS_DONE || outPos != rawSize || !unfilter(im)) error = "Invalid PNG image";
  else scaleBox(im.width, im.height, [&](int x, int y){ return pixel565(im, x, y); }, out, outW, outH);
  free(im.raw);
  return error;
}

const char* pngToLogo(const uint8_t* data, size_t len, uint16_t* out){
  return pngToRgb565(data, len, out, LOGO_SIZE, LOGO_SIZE);
}
