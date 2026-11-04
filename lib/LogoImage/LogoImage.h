// Converts a channel avatar (PNG) into a square RGB565 logo for the display.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define LOGO_SIZE        64   // logo edge length in pixels
#define LOGO_MAX_SOURCE  80   // largest accepted source image edge (Twitch's avatars are fetched as 70x70)

// Decodes the PNG in png/len, blends transparency onto black and scales it to
// LOGO_SIZE x LOGO_SIZE (box filter: every output pixel is the area-weighted
// average of the source pixels it covers). out must hold LOGO_SIZE*LOGO_SIZE
// values in the native byte order of the display buffers.
//
// Supports 8-bit grayscale, RGB, palette, grayscale+alpha and RGBA images,
// and 1/2/4-bit grayscale and palette images, without interlacing (what
// Twitch and Twemoji serve). Memory: the unpacked image
// (height * (1 + width * bytes per pixel), at most ~26 KB) plus ~11 KB for
// the inflater, freed again before returning. No separate 32 KB zlib window
// is needed because the whole image is unpacked into one buffer.
//
// Returns nullptr on success, otherwise a short error description.
const char* pngToLogo(const uint8_t* png, size_t len, uint16_t* out);

// The same for any output size (e.g. emoji): out must hold outW*outH values.
const char* pngToRgb565(const uint8_t* png, size_t len, uint16_t* out, int outW, int outH);

// Scales an RGB565 image (srcW x srcH) to dstW x dstH with the same box
// filter (works for shrinking and enlarging). Exposed for testing.
void scaleRgb565(const uint16_t* src, int srcW, int srcH, uint16_t* dst, int dstW, int dstH);
