#pragma once
#include "Arduino.h"
#define U_FLASH 0
typedef int ota_error_t;
enum { OTA_AUTH_ERROR, OTA_BEGIN_ERROR, OTA_CONNECT_ERROR, OTA_RECEIVE_ERROR, OTA_END_ERROR };
struct ArduinoOTAClass {
  void setHostname(const char*){} void setPassword(const char*){}
  ArduinoOTAClass& onStart(std::function<void()>){ return *this; }
  ArduinoOTAClass& onEnd(std::function<void()>){ return *this; }
  ArduinoOTAClass& onProgress(std::function<void(unsigned,unsigned)>){ return *this; }
  ArduinoOTAClass& onError(std::function<void(ota_error_t)>){ return *this; }
  void begin(){} void handle(){} int getCommand(){ return U_FLASH; }
};
extern ArduinoOTAClass ArduinoOTA;
