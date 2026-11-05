#pragma once
#include "Arduino.h"
#include <map>
extern std::map<std::string, std::map<std::string, std::string>> g_nvs;
class Preferences {
  std::string ns; bool ro = false, started = false;
public:
  bool begin(const char* n, bool readOnly = false, const char* = nullptr){ ns = n; ro = readOnly; if(readOnly && !g_nvs.count(ns)) return started = false; g_nvs[ns]; return started = true; }
  void end(){ started = false; }
  String getString(const char* k, String def = String()){ if(!started) return def; auto& m = g_nvs[ns]; auto it = m.find(k); return it == m.end() ? def : String(it->second); }
  size_t putString(const char* k, const String& v){ if(!started || ro) return 0; g_nvs[ns][k] = v; return v.size() + 1; }
  size_t putString(const char* k, const char* v){ return putString(k, String(v)); }
  bool remove(const char* k){ if(!started || ro) return false; return g_nvs[ns].erase(k) > 0; }
  bool clear(){ if(!started || ro) return false; g_nvs[ns].clear(); return true; }
  uint8_t getUChar(const char* k, uint8_t def = 0){ String v = getString(k, ""); return v.empty() ? def : (uint8_t)atoi(v.c_str()); }
  size_t putUChar(const char* k, uint8_t v){ return putString(k, String(std::to_string(v).c_str())); }
  bool isKey(const char* k){ return started && g_nvs[ns].count(k) > 0; }
};
