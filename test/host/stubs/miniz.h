#pragma once
// Host stand-in for the ESP32 ROM inflater (miniz tinfl), built on zlib.
// Only supports the way the firmware calls it: zlib header, non-wrapping output buffer.
#include <zlib.h>
#include <cstddef>
#include <cstring>
typedef unsigned char mz_uint8;
typedef unsigned int mz_uint32;
enum { TINFL_FLAG_PARSE_ZLIB_HEADER = 1, TINFL_FLAG_HAS_MORE_INPUT = 2, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF = 4 };
typedef enum { TINFL_STATUS_BAD_PARAM = -3, TINFL_STATUS_ADLER32_MISMATCH = -2, TINFL_STATUS_FAILED = -1,
               TINFL_STATUS_DONE = 0, TINFL_STATUS_NEEDS_MORE_INPUT = 1, TINFL_STATUS_HAS_MORE_OUTPUT = 2 } tinfl_status;
struct tinfl_decompressor { z_stream zs; int started; char sameSizeAsTheRealOne[10900]; };
#define tinfl_init(r) do { (r)->started = 0; } while(0)
inline tinfl_status tinfl_decompress(tinfl_decompressor* r, const mz_uint8* in, size_t* inSize, mz_uint8* outStart,
                                     mz_uint8* outNext, size_t* outSize, mz_uint32 flags){
  if(!(flags & TINFL_FLAG_PARSE_ZLIB_HEADER) || !(flags & TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF) || outNext < outStart) return TINFL_STATUS_BAD_PARAM;
  if(!r->started){ memset(&r->zs, 0, sizeof(r->zs)); if(inflateInit(&r->zs) != Z_OK) return TINFL_STATUS_FAILED; r->started = 1; }
  r->zs.next_in = (Bytef*)in; r->zs.avail_in = (uInt)*inSize;
  r->zs.next_out = outNext; r->zs.avail_out = (uInt)*outSize;
  int rc = inflate(&r->zs, Z_NO_FLUSH);
  *inSize -= r->zs.avail_in; *outSize -= r->zs.avail_out;
  if(rc == Z_STREAM_END){ inflateEnd(&r->zs); r->started = 0; return TINFL_STATUS_DONE; }
  if((rc == Z_OK || rc == Z_BUF_ERROR) && r->zs.avail_out == 0){ inflateEnd(&r->zs); r->started = 0; return TINFL_STATUS_HAS_MORE_OUTPUT; }
  if((rc == Z_OK || rc == Z_BUF_ERROR) && (flags & TINFL_FLAG_HAS_MORE_INPUT)) return TINFL_STATUS_NEEDS_MORE_INPUT;
  inflateEnd(&r->zs); r->started = 0;
  return TINFL_STATUS_FAILED;
}
