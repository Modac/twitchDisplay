// Generated test data, so the tests need no build outputs or third-party images.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <zlib.h>

// Deterministic filler bytes
inline void fillPattern(std::vector<uint8_t>& v, size_t from, uint32_t seed){
  for(size_t i = from; i < v.size(); i++){ seed = seed * 1103515245u + 12345u; v[i] = (uint8_t)(seed >> 16); }
}

// ESP32-C3 application image: magic 0xE9, chip id 5, app description magic 0xABCD5432 at offset 32
inline std::vector<uint8_t> makeAppImage(size_t size, uint32_t seed = 1){
  std::vector<uint8_t> v(size);
  fillPattern(v, 0, seed);
  v[0] = 0xE9; v[1] = 3; v[12] = 0x05; v[13] = 0x00;
  const uint8_t appDesc[4] = {0x32, 0x54, 0xCD, 0xAB};
  memcpy(&v[32], appDesc, 4);
  return v;
}

// Bootloader image: same image header, but no app description in its first segment
inline std::vector<uint8_t> makeBootloaderImage(size_t size){
  std::vector<uint8_t> v = makeAppImage(size, 2);
  v[32] = 0x50; v[33] = 0x00; v[34] = 0x00; v[35] = 0x00;
  return v;
}

// Merged USB image: starts with the bootloader at offset 0
inline std::vector<uint8_t> makeMergedImage(size_t size){
  std::vector<uint8_t> v(size, 0xFF);
  std::vector<uint8_t> boot = makeBootloaderImage(20000);
  memcpy(v.data(), boot.data(), boot.size());
  std::vector<uint8_t> app = makeAppImage(size - 0x10000, 3);
  memcpy(v.data() + 0x10000, app.data(), app.size());
  return v;
}

// Partition table: starts with 0xAA 0x50
inline std::vector<uint8_t> makePartitionTable(){
  std::vector<uint8_t> v(3072, 0xFF);
  v[0] = 0xAA; v[1] = 0x50; v[2] = 0x01;
  return v;
}

inline void pngChunk(std::string& out, const char* type, const std::string& body){
  uint32_t n = (uint32_t)body.size();
  out += (char)(n >> 24); out += (char)(n >> 16); out += (char)(n >> 8); out += (char)n;
  std::string td = std::string(type, 4) + body;
  out += td;
  uint32_t crc = (uint32_t)crc32(0, (const Bytef*)td.data(), (uInt)td.size());
  out += (char)(crc >> 24); out += (char)(crc >> 16); out += (char)(crc >> 8); out += (char)crc;
}

// RGBA PNG (8 bit, filter 0, image data split into IDAT chunks of idatSize bytes)
inline std::string makePng(int w, int h, const std::vector<uint8_t>& rgba, size_t idatSize = 8192){
  std::string raw;
  for(int y = 0; y < h; y++){ raw += '\0'; raw.append((const char*)&rgba[(size_t)y * w * 4], (size_t)w * 4); }
  uLongf zlen = compressBound((uLong)raw.size());
  std::string z(zlen, '\0');
  compress2((Bytef*)&z[0], &zlen, (const Bytef*)raw.data(), (uLong)raw.size(), 9);
  z.resize(zlen);
  std::string png = "\x89PNG\r\n\x1a\n";
  std::string ihdr;
  for(int v : {w, h}){ ihdr += (char)(v >> 24); ihdr += (char)(v >> 16); ihdr += (char)(v >> 8); ihdr += (char)v; }
  ihdr += (char)8; ihdr += (char)6; ihdr += '\0'; ihdr += '\0'; ihdr += '\0';
  pngChunk(png, "IHDR", ihdr);
  for(size_t i = 0; i < z.size(); i += idatSize) pngChunk(png, "IDAT", z.substr(i, idatSize));
  pngChunk(png, "IEND", "");
  return png;
}

// A 70x70 test avatar (like Twitch's small avatar size): colour gradient with
// a pattern depending on the seed and partly transparent corners
inline std::string makeAvatar(uint32_t seed){
  const int n = 70;
  std::vector<uint8_t> px((size_t)n * n * 4);
  for(int y = 0; y < n; y++) for(int x = 0; x < n; x++){
    uint8_t* p = &px[((size_t)y * n + x) * 4];
    p[0] = (uint8_t)(x * 3 + seed * 40);
    p[1] = (uint8_t)(y * 3 + seed * 70);
    p[2] = (uint8_t)(((x / (4 + seed % 5)) + (y / 6)) % 2 ? 230 : 20);
    int dx = x - n / 2, dy = y - n / 2;
    p[3] = (dx * dx + dy * dy > 34 * 34) ? 0 : 255;   // round avatar, transparent outside
  }
  return makePng(n, n, px, 1000 + seed * 500);
}
