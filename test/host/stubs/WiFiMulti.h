#pragma once
#define WL_CONNECTED 3
struct WiFiMulti { void addAP(const char*,const char*){} int run(){ return WL_CONNECTED; } };
