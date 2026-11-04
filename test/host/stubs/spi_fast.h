#pragma once
// Host version of include/spi_fast.h: the pieces go to the display emulator.
#include <cstdint>
void spiFastPixels(const uint16_t* p, uint32_t n);   // test.cpp: into the emulator
extern int g_spiFastPieces;
static inline void spiFastWait(){}
static inline void spiFastSend(const uint32_t* words, uint32_t bytes){
  if(bytes == 0 || bytes > 64 || bytes % 2){ fprintf(stderr, "spiFastSend: bad length %u\n", bytes); return; }
  uint16_t px[32];
  for(uint32_t k = 0; k < bytes / 2; k++){
    uint16_t raw = (uint16_t)(words[k / 2] >> (16 * (k % 2)));
    px[k] = (uint16_t)(raw >> 8 | raw << 8);          // first byte on the wire is the high byte
  }
  spiFastPixels(px, bytes / 2);
  g_spiFastPieces++;
}
static inline uint32_t spiFastClockHz(){ return 80000000; }
