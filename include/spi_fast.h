// Fast pixel transfer straight to the SPI2 (FSPI) registers of the ESP32-C3,
// used for the scrolling title. The Arduino SPI driver sends at most 64 bytes
// at a time and waits for each piece; here the next piece is prepared while
// the current one is on the wire. The SPI transaction (clock, mode, CS, DC)
// stays with the Adafruit/Arduino driver.
// The host simulation (test/host) has its own version of this file.
#pragma once
#include <soc/spi_struct.h>

// Waits until the last piece has been sent.
static inline void spiFastWait(){
  while(GPSPI2.cmd.usr);
}

// Sends 1..64 bytes (words in the SPI's byte order: lowest byte first).
// Waits for the previous piece, starts this one and returns while it is sent.
static inline void spiFastSend(const uint32_t* words, uint32_t bytes){
  while(GPSPI2.cmd.usr);
  GPSPI2.ms_dlen.ms_data_bitlen = bytes * 8 - 1;
  for(uint32_t i = 0; i < (bytes + 3) / 4; i++) GPSPI2.data_buf[i] = words[i];
  GPSPI2.cmd.update = 1;          // registers into the SPI clock domain
  while(GPSPI2.cmd.update);
  GPSPI2.cmd.usr = 1;
}

// The SPI clock currently set (APB 80 MHz divided)
static inline uint32_t spiFastClockHz(){
  if(GPSPI2.clock.clk_equ_sysclk) return 80000000;
  return 80000000 / ((GPSPI2.clock.clkdiv_pre + 1) * (GPSPI2.clock.clkcnt_n + 1));
}
