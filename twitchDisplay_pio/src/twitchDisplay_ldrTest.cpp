
#include <Arduino.h>



#include <Adafruit_GFX.h>    // Core graphics library
#include <Adafruit_ST7789.h> // Hardware-specific library for ST7789
#include <SPI.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <set>

#include "LowPass.h"

#include "pics.h"
#include "secrets.h"

// https://stackoverflow.com/a/5459929
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

// PROGMEM and F() are pointless in ESP32
static const char DEBUG_TAG[] = "TwitchDisplay";
// inspiration from https://forum.arduino.cc/t/single-line-define-to-disable-code/636044/5

//#define TEST_SERVER

#define SERIAL_PORT Serial
#define USE_SERIAL true
#define DEBUG_ERROR true
#define DEBUG_WARNING true
#define DEBUG_INFO true
#define S if(USE_SERIAL)SERIAL_PORT
#define DEBUG_E if(DEBUG_ERROR)SERIAL_PORT
#define DEBUG_W if(DEBUG_WARNING)SERIAL_PORT
#define DEBUG_I if(DEBUG_INFO)SERIAL_PORT

#define TFT_CS         7
#define TFT_RST       10 // Or set to -1 and connect to Arduino RESET pin
#define TFT_DC         1
#define TFT_BK         0

// Use hardware spi (for esp32-c3 super mini this is SPI0/1 at pins 4-7)
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

WiFiMulti wifiMulti;
WiFiUDP Udp;

// Filter instance
LowPass<2> lp(0.1,1e3,true);

void testdrawrects(uint16_t colors[], uint16_t num_colors) {
  tft.fillScreen(ST77XX_BLACK);
  for (int16_t x=0; x < tft.width(); x+=6) {
    tft.drawRect(tft.width()/2 -x/2, tft.height()/2 -x/2 , x, x, colors[(x/6)%num_colors]);
  }
}

void setup(void) {
#if USE_SERIAL == true || DEBUG_ERROR == true || DEBUG_WARNING == true || DEBUG_INFO == true || DEBUG_NOTICE == true
  SERIAL_PORT.begin(115200);
#endif
  //S.setDebugOutput(true);
  DEBUG_I.printf("[%s] Starting...\n", DEBUG_TAG);

  DEBUG_I.printf("[%s] Connecting to WIFI...\n", DEBUG_TAG);
  wifiMulti.addAP("::1", WIFI_PW);

  DEBUG_I.printf("[%s] Initializing display...\n", DEBUG_TAG);
  pinMode(TFT_BK, OUTPUT);
  digitalWrite(TFT_BK, HIGH);

  tft.init(170, 320);           // Init ST7789 170x320
  tft.setRotation(1);
  tft.setSPISpeed(80000000);
  
  tft.fillScreen(ST77XX_BLACK);
  uint16_t colors[] = {ST77XX_BLUE, ST77XX_CYAN, ST77XX_GREEN, ST77XX_YELLOW, ST77XX_RED, ST77XX_MAGENTA};
  testdrawrects(colors, 6);
  DEBUG_I.printf("[%s] Display initialized.\n", DEBUG_TAG);
  
  DEBUG_I.printf("[%s] Waiting for WIFI connection...\n", DEBUG_TAG);
  // wait for WiFi connection
  while((wifiMulti.run() != WL_CONNECTED)){
    delay(10);
  }
  DEBUG_I.printf("[%s] WIFI connected.\n", DEBUG_TAG);

  // Port defaults to 3232
  // ArduinoOTA.setPort(3232);

  // Hostname defaults to esp3232-[MAC]
  ArduinoOTA.setHostname("twitchdisplay");

  // No authentication by default
  // ArduinoOTA.setPassword("admin");

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH) {
        type = "sketch";
      } else {  // U_SPIFFS
        type = "filesystem";
      }

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      DEBUG_I.println("Start updating " + type);
    })
    .onEnd([]() {
      DEBUG_I.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      DEBUG_I.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      DEBUG_E.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) {
        DEBUG_E.println("Auth Failed");
      } else if (error == OTA_BEGIN_ERROR) {
        DEBUG_E.println("Begin Failed");
      } else if (error == OTA_CONNECT_ERROR) {
        DEBUG_E.println("Connect Failed");
      } else if (error == OTA_RECEIVE_ERROR) {
        DEBUG_E.println("Receive Failed");
      } else if (error == OTA_END_ERROR) {
        DEBUG_E.println("End Failed");
      }
    });

  ArduinoOTA.begin();

  DEBUG_I.printf("[%s] Setup completed...\n", DEBUG_TAG);
}

uint16_t y;
uint16_t x;

void loop(){
  
  ArduinoOTA.handle();

  x = analogRead(A3);
  y = lp.filt(x);
  /*
  Serial.print(">ldr:");
  Serial.println(analogRead(A3));
  */
  // once we know where we got the inital packet from, send data back to that IP address and port
  Udp.beginPacket("192.168.6.61", 47269);
  // Just test touch pin - Touch0 is T0 which is on GPIO 4.
  Udp.print("ldr:");
  Udp.println(x);
  Udp.print("ldr_f:");
  Udp.println(y);
  Udp.endPacket();
  analogWrite(TFT_BK, constrain(map(y, 1000, 4095, 255, 10), 10, 255));
  delay(10);
}
