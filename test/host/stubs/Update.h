#pragma once
#include "Arduino.h"
#include <vector>
#define UPDATE_SIZE_UNKNOWN 0xFFFFFFFF
#define U_FLASH 0
extern std::vector<uint8_t> g_flashed; extern bool g_updateRunning, g_updateEndOk; extern int g_updateBegins, g_updateAborts, g_updateEnds;
struct UpdateClass {
  bool begin(size_t=UPDATE_SIZE_UNKNOWN, int=U_FLASH){ g_updateBegins++; g_updateRunning=true; g_flashed.clear(); return true; }
  size_t write(uint8_t* d, size_t n){ g_flashed.insert(g_flashed.end(), d, d+n); return n; }
  bool end(bool){ g_updateEnds++; g_updateRunning=false; return g_updateEndOk; }
  void abort(){ g_updateAborts++; g_updateRunning=false; }
  bool isRunning(){ return g_updateRunning; }
  const char* errorString(){ return "Image verification failed"; }
};
extern UpdateClass Update;
