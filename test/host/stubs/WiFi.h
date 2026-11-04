#pragma once
#include "WiFiMulti.h"
#include "Arduino.h"
extern bool g_wifiUp; extern int g_reconnects;
#include <vector>
#define WIFI_AP_STA 3
struct IPStub { const char* ip; String toString(){ return String(ip); } };
extern std::vector<std::string> g_scan; extern std::string g_beginSsid, g_beginPw, g_apSsid, g_apPw; extern int g_apStations;
struct WiFiClass { int status(){ return g_wifiUp ? WL_CONNECTED : 0; } void reconnect(){ g_reconnects++; }
  void setHostname(const char*){} String SSID(){ return String(g_beginSsid); } int RSSI(){ return -60; } IPStub localIP(){ return IPStub{"192.168.1.50"}; }
  void begin(const char* s, const char* p){ g_beginSsid=s; g_beginPw=p; }
  void disconnect(bool=false){} void mode(int){}
  int16_t scanNetworks(){ return g_scan.size(); } String SSID(uint8_t i){ return String(g_scan[i]); } void scanDelete(){}
  bool softAP(const char* s, const char* p){ g_apSsid=s; g_apPw=p; return true; }
  IPStub softAPIP(){ return IPStub{"192.168.4.1"}; } uint8_t softAPgetStationNum(){ return g_apStations; } };
extern WiFiClass WiFi;
