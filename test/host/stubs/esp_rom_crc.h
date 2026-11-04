#pragma once
#include <cstdint>
// standard CRC-32 (like the ESP32 ROM function with crc = 0)
inline uint32_t esp_rom_crc32_le(uint32_t crc, uint8_t const* buf, uint32_t len){
  crc = ~crc;
  for(uint32_t i = 0; i < len; i++){ crc ^= buf[i]; for(int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1))); }
  return ~crc;
}
