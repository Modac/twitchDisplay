// TODO: add filters for json deserialization
// TODO: add title display

#include <Arduino.h>
#include <Adafruit_GFX.h>    // Core graphics library
#include <Adafruit_ST7789.h> // Hardware-specific library for ST7789
#include <SPI.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiUdp.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <set>
#include <ArduinoOTA.h>

#include "pics.h"
#include "secrets.h"

#include "LowPass.h"
#include "MovingAverage.h"

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

#define MAX_NUM_PICS 8

// Use hardware spi (for esp32-c3 super mini this is SPI0/1 at pins 4-7)
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

WiFiMulti wifiMulti;
WiFiUDP Udp;

// Filter instance
LowPass<2> lp(0.1,1e3,true);
MovingAverage<100> ma;
uint16_t ldr;
uint16_t ldr_f;
uint16_t ldr_f2;

void updateLiveChannels();
void setupOTA();

// lidi, pietsmiet, bonjwa, bonjwachill, gronkh, dhalucard, trilluxe, dracon, maxim, finanzfluss
std::array<std::string, 10> channel_ids = {"761017145", "21991090", "73437396", "1024088182", "12875057", "16064695", "55898523", "38770961", "172376071", "549536744"};
// pietsmiet, bonjwa, gronkhtv
//std::array<std::string, 3> channel_ids = {"21991090", "73437396", "106159308"};
const uint16_t* channel_pics[10] = { epd_bitmap_lidi,
  epd_bitmap_pietsmiet, epd_bitmap_bonjwa, epd_bitmap_bonjwachill,
  epd_bitmap_gronkh, epd_bitmap_dhalucard, epd_bitmap_trilluxe,
  epd_bitmap_dracon, epd_bitmap_maxim, epd_bitmap_finanzfluss};
//bool is_live[channel_ids.size()] = {false};
// todo: maybe an std::array is enough (maybe have bool)
std::vector<std::string> live_channel_vec;

void setup(void) {
  live_channel_vec.reserve(channel_ids.size());

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
  DEBUG_I.printf("[%s] Display initialized.\n", DEBUG_TAG);

  DEBUG_I.printf("[%s] Waiting for WIFI connection...\n", DEBUG_TAG);
  // wait for WiFi connection
  while((wifiMulti.run() != WL_CONNECTED)){
    delay(10);
  }

  DEBUG_I.printf("[%s] WIFI connected.\n", DEBUG_TAG);

  setupOTA();

  DEBUG_I.printf("[%s] Setup completed...\n", DEBUG_TAG);
}

GFXcanvas16 pic_canvas(64, 64); // 16-bit, 320x170 pixels
GFXcanvas16 text_canvas(298+(6*8), 64); 

enum State {
  Idle,
  Error
} state = Idle;

//#define MAX_RETRIES 10
//uint8_t retries = 0;

#define TW_UPDATE_INTERVAL (30*1000)
unsigned long tw_last_update_start = 0; 
bool tw_update_now = true;

void loop(){
  
  ArduinoOTA.handle();
  //S.printf("[%s] Looping...\n", DEBUG_TAG);
  
  ldr = analogRead(A3);
  ldr_f = lp.filt(ldr);
  ldr_f2 = ma.compute(ldr);
  // 1000 = minimum adc value when brightest
  // 10 = minimum of backligh pwm value when darkest
  analogWrite(TFT_BK, constrain(map(ldr_f2, 1000, 4095, 255, 10), 10, 255));

  if(state == Idle){
    if (tw_update_now || millis() - tw_last_update_start >= TW_UPDATE_INTERVAL){
      DEBUG_I.printf("[%s] Idle: Updating live channels...\n", DEBUG_TAG);
#ifndef TEST_SERVER
      updateLiveChannels();
#endif
      tw_last_update_start = millis();
      tw_update_now = false;
    }
  }

  if(state == Error){
    DEBUG_E.println("Unrecoverable error... restarting...");
    delay(1000);
    ESP.restart();
  }
   
  Udp.beginPacket("192.168.6.61", 47269);
  // Just test touch pin - Touch0 is T0 which is on GPIO 4.
  Udp.print("ldr:");
  Udp.println(ldr);
  Udp.print("ldr_f:");
  Udp.println(ldr_f);
  Udp.print("ldr_f2:");
  Udp.println(ldr_f2);
  Udp.endPacket();
  delay(10);
  
}

void commonHttpInit(HTTPClient& http_client){
  DEBUG_I.print("[HTTP] Common init...\n");
  http_client.useHTTP10(true);
  
  // add headers
  http_client.setAuthorizationType("Bearer");
  http_client.setAuthorization(TWITCH_TOKEN);
  http_client.addHeader("Client-Id", "gp762nuuoqcoxypju8c569th9wz7q5");
}

void drawLiveChannelPic(const char* id, uint8_t pic_slot_index, decltype(channel_ids)::iterator it){
  if(pic_slot_index<8){
    DEBUG_I.printf("[%s] Drawing channel pic of %s in slot number %u\n", DEBUG_TAG, id, pic_slot_index);
    // Channel id not found
    if (it == std::end(channel_ids)) {
      pic_canvas.fillScreen(ST77XX_BLACK);
      pic_canvas.setCursor(0,0);
      pic_canvas.setTextWrap(true);
      pic_canvas.setTextColor(ST77XX_RED);
      pic_canvas.setTextSize(2);
      pic_canvas.println("?????");
      pic_canvas.println(id);
      pic_canvas.println("?????");
    } else {
      auto channel_index = std::distance(std::begin(channel_ids), it);
      pic_canvas.drawRGBBitmap(0, 0, channel_pics[channel_index], pic_canvas.width(), pic_canvas.height());
    }

    if(pic_slot_index<4){
      tft.drawRGBBitmap(11+((64+14)*pic_slot_index), 14, pic_canvas.getBuffer(), pic_canvas.width(), pic_canvas.height());
    } else if(pic_slot_index<8){
      tft.drawRGBBitmap(11+((64+14)*(pic_slot_index-4)), 14+64+14, pic_canvas.getBuffer(), pic_canvas.width(), pic_canvas.height());
    }
  } else {
    pic_canvas.fillScreen(ST77XX_BLACK);
    pic_canvas.setCursor(4, 14);
    pic_canvas.setTextSize(5);
    pic_canvas.printf("+%d", pic_slot_index-6);
    tft.drawRGBBitmap(11+((64+14)*(3)), 14+64+14, pic_canvas.getBuffer(), pic_canvas.width(), pic_canvas.height());
  }
}

void redrawLiveChannelPics(){
  //tft.fillScreen(ST77XX_BLACK);
  
  std::size_t i;
  for (i = 0; i < live_channel_vec.size(); i++) {
    auto it = std::find(std::begin(channel_ids), std::end(channel_ids), live_channel_vec[i]);
    drawLiveChannelPic(live_channel_vec[i].c_str(), i, it);
  }
  // block out remaining pic slots
  for (;i < MAX_NUM_PICS; i++){
    // copied from drawLiveChannelPic()
    pic_canvas.fillScreen(ST77XX_BLACK);
    if(i<4){
      tft.drawRGBBitmap(11+((64+14)*i), 14, pic_canvas.getBuffer(), pic_canvas.width(), pic_canvas.height());
    } else if(i<8){
      tft.drawRGBBitmap(11+((64+14)*(i-4)), 14+64+14, pic_canvas.getBuffer(), pic_canvas.width(), pic_canvas.height());
    }
  }
}

void setIDLiveStatus(const char* id, bool now_live){
  if(now_live){
    if (std::find(live_channel_vec.begin(), live_channel_vec.end(), id) == live_channel_vec.end()) {
      DEBUG_I.printf("[%s] Adding %s to live channels...\n", DEBUG_TAG, id);
      uint8_t pic_slot_index = live_channel_vec.size();
      live_channel_vec.push_back(id);
      DEBUG_I.printf("[%s] There are now %d live channels.\n", DEBUG_TAG, live_channel_vec.size());
      auto it = std::find(std::begin(channel_ids), std::end(channel_ids), id);
      drawLiveChannelPic(id, pic_slot_index, it);
    } else {
      DEBUG_I.printf("[%s] Not adding %s to live channels (already in vector)\n", DEBUG_TAG, id);
    }
  } else {
    DEBUG_I.printf("[%s] Removing %s from live channels\n", DEBUG_TAG, id);
    live_channel_vec.erase(
      std::remove(live_channel_vec.begin(), live_channel_vec.end(), id),
      live_channel_vec.end());
    DEBUG_I.printf("[%s] There are now %d live channels.\n", DEBUG_TAG, live_channel_vec.size());
    redrawLiveChannelPics();
  }
  //is_live[i] = now_live;
}

void updateLiveChannels(){
  HTTPClient http;
  // Could maybe be done as constexpr??
  String url = String("https://api.twitch.tv/helix/streams?");
  for (const auto& id : channel_ids) {
    url += "user_id=";
    url += id.c_str();
    url += "&"; 
  }

  DEBUG_I.printf("[%s] Update live channels with url: %s\n", DEBUG_TAG, url.c_str());  
  
  http.begin(url);  //HTTP
  commonHttpInit(http);
  // start connection and send HTTP header
  DEBUG_I.print("[HTTP] GET...\n");
  int httpCode = http.GET();
  
  
  if (httpCode != HTTP_CODE_OK) {
    DEBUG_W.printf(
      "[HTTP] GET failed, error: %s\n",
      (httpCode<=0)?
        http.errorToString(httpCode).c_str():
        String(httpCode).c_str());
    return;
  }

  DEBUG_I.printf("[JSON] Deserializing response...\n", DEBUG_TAG);

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  if (err) {
    DEBUG_W.print("[JSON] deserializeJson() failed with code ");
    DEBUG_W.println(err.f_str());
    return;
  }

  // Reset is_live array
  //memset(is_live, false, sizeof is_live);
  DEBUG_I.printf("[%s] Rebuilding live channels vector...\n", DEBUG_TAG);
  live_channel_vec.clear();
  //tft.fillScreen(ST77XX_BLACK);

  
  for (JsonObject channel : doc["data"].as<JsonArray>()) {
    const char* name = channel["user_name"];
    const char* id = channel["user_id"];
    // TODO: Error checking?
    DEBUG_I.printf("[%s] Live channel: %s\n", DEBUG_TAG, name);
    //setIDLiveStatus(id, true);
    live_channel_vec.push_back(id);
  }

  redrawLiveChannelPics();
  /*
  for (const auto& id : channel_ids) {
    setIDLiveStatus(id.c_str(), true);
  }
  */
  
}

void setupOTA(){
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
}