// Converts a channel avatar (PNG) into a square RGB565 logo for the display.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define LOGO_SIZE        64   // logo edge length in pixels
#define LOGO_MAX_SOURCE  160  // largest accepted source image edge (RAM: edge^2 * 2 bytes)

// Decodes the PNG in png/len, blends transparency onto black and scales it to
// LOGO_SIZE x LOGO_SIZE (box filter: every output pixel is the area-weighted
// average of the source pixels it covers). out must hold LOGO_SIZE*LOGO_SIZE
// values in the native byte order of the display buffers.
// Returns nullptr on success, otherwise a short error description.
const char* pngToLogo(const uint8_t* png, size_t len, uint16_t* out);

// Scales an RGB565 image (srcW x srcH) to dstW x dstH with the same box
// filter (works for shrinking and enlarging). Exposed for testing.
void scaleRgb565(const uint16_t* src, int srcW, int srcH, uint16_t* dst, int dstW, int dstH);
