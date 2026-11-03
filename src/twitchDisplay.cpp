// TODO: full unicode support (e.g. with https://github.com/takkaO/OpenFontRender/tree/master and Noto Sans + Noto Emoji, but together at least 1,5MB).
//       For now titles are converted to the CP437 charset of the built-in font (umlauts and other Latin-1 letters work, emoji are dropped).
// TODO: reduce scrolling text flicker by flipping the MV flag such that the pixels can be transfered column by column and not row by row

#include <Arduino.h>
#include <Adafruit_GFX.h>    // Core graphics library
#include <Adafruit_ST7789.h> // Hardware-specific library for ST7789
#include <SPI.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiUdp.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>

#include <deque>
#include <string>
#include <vector>

#include "pics.h"
#include "secrets.h"

#include "MovingAverage.h"
#include "web_pages.h"

static const char DEBUG_TAG[] = "TwitchDisplay";

// inspiration from https://forum.arduino.cc/t/single-line-define-to-disable-code/636044/5
#define SERIAL_PORT Serial
#define DEBUG_ERROR true
#define DEBUG_WARNING true
#define DEBUG_INFO true
#define DEBUG_E if(DEBUG_ERROR)SERIAL_PORT
#define DEBUG_W if(DEBUG_WARNING)SERIAL_PORT
#define DEBUG_I if(DEBUG_INFO)SERIAL_PORT

// Twitch credentials (see include/secrets.h.example)
#ifndef TWITCH_CLIENT_ID
#define TWITCH_CLIENT_ID "gp762nuuoqcoxypju8c569th9wz7q5"
#endif
#if !defined(TWITCH_TOKEN) && !defined(TWITCH_CLIENT_SECRET)
#error "include/secrets.h must define TWITCH_TOKEN and/or TWITCH_CLIENT_SECRET"
#endif

// Web interface (status page, firmware upload) and ArduinoOTA
#ifndef WEB_PASSWORD
#error "include/secrets.h must define WEB_PASSWORD (login for the web interface and ArduinoOTA)"
#endif
#define WEB_USER          "admin"
#define WEB_REALM         "twitchDisplay"
#define HOSTNAME          "twitchdisplay"
#define APP_CONFIRM_DELAY (60*1000) // new firmware must be online this long before it is marked as good
#define LOOP_WDT_TIMEOUT  30000     // ms; restarts the display if loop() hangs (longer than the web server timeouts)

#ifndef FW_VERSION
#define FW_VERSION "unknown"        // set by tools/version.py
#endif
#ifndef ESP_APP_DESC_MAGIC_WORD
#define ESP_APP_DESC_MAGIC_WORD 0xABCD5432
#endif

// Send the LDR values via UDP (e.g. for plotting). Off by default.
//#define DEBUG_UDP_LDR
#define DEBUG_UDP_HOST     "192.168.6.61"
#define DEBUG_UDP_PORT     47269
#define DEBUG_UDP_INTERVAL 100   // ms

// Twitch update interval and the background task that does the requests
#define TW_UPDATE_INTERVAL (30*1000)
#define NET_TASK_STACK     12288 // bytes, TLS needs a few KB
#define NET_TASK_PRIORITY  0     // below loop() (1): drawing always wins, requests run while loop() sleeps

// Light sensor: sampled every LDR_INTERVAL ms, averaged over 100 samples (2 s)
#define LDR_PIN       A3
#define LDR_INTERVAL  20
#define LDR_BRIGHTEST 1000 // ADC value in bright light
#define BK_MIN        10   // minimum backlight PWM value in the dark

// Ask the WiFi driver to reconnect if the connection is lost for this long
#define WIFI_RECONNECT_INTERVAL (30*1000)

// Status dot in the bottom right corner below the grid
#define STATUS_SIZE     5
#define STATUS_X        (320-11-STATUS_SIZE)
#define STATUS_Y        (170-STATUS_SIZE-1)
#define STATUS_OK       ST77XX_BLACK    // everything fine: dot invisible
#define STATUS_NO_WIFI  ST77XX_YELLOW   // WiFi disconnected
#define STATUS_AUTH     ST77XX_MAGENTA  // Twitch token rejected (expired or invalid)
#define STATUS_API      ST77XX_RED      // Twitch request failed for another reason

#define TFT_CS         7
#define TFT_RST       10 // Or set to -1 and connect to Arduino RESET pin
#define TFT_DC         1
#define TFT_BK         0

#define MAX_NUM_PICS 8

// Logo grid layout: 4 columns x 2 rows of 64x64 pics
#define PIC_SIZE    64
#define PIC_GAP     14
#define PIC_BORDER  7                               // thickness of the (green/black) frame around a pic
#define GRID_X0     11                              // left edge of the first column
#define GRID_Y0     14                              // top edge of the first row
#define GRID_COLS   4
#define SLOT_PITCH  (PIC_SIZE+PIC_GAP)
#define TITLE_WIDTH (GRID_COLS*PIC_SIZE+(GRID_COLS-1)*PIC_GAP) // scrolling title spans the full grid width

// Row (0 or 1) of a slot. Slots >= MAX_NUM_PICS share the last slot (the "+N" tile).
uint8_t slotRow(int8_t slot){
  return (slot >= GRID_COLS) ? 1 : 0;
}

// Top edge of a row
uint16_t rowY(uint8_t row){
  return GRID_Y0 + SLOT_PITCH*row;
}

// Top-left pixel of a slot. Slots >= MAX_NUM_PICS share the last slot (the "+N" tile).
void slotXY(int8_t slot, uint16_t& x, uint16_t& y){
  if(slot >= MAX_NUM_PICS) slot = MAX_NUM_PICS-1;
  if(slot < 0) slot = 0; // should not happen, keeps the coordinates on screen
  x = GRID_X0 + SLOT_PITCH*(slot%GRID_COLS);
  y = rowY(slotRow(slot));
}

// Use hardware spi (for esp32-c3 super mini this is SPI0/1 at pins 4-7)
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

WiFiMulti wifiMulti;
#ifdef DEBUG_UDP_LDR
WiFiUDP Udp;
#endif

// TLS client for the Twitch API; verifies the server certificate against the
// root certificates built into the ESP32 core
WiFiClientSecure tlsClient;

#ifdef TWITCH_TOKEN
String twitchToken = TWITCH_TOKEN;
#else
String twitchToken; // fetched with the client secret on the first update
#endif

MovingAverage<100> ldrAverage;
uint16_t ldr;     // last raw LDR reading
uint16_t ldr_avg; // averaged LDR reading used for the backlight

void redrawLiveChannelPics(int8_t row = -1);
void setupOTA();
void setupWeb();
void networkTask(void*);

struct channelInfo {
  std::string id;
  bool isLive;
  std::string streamTitle;  // display charset, used for the scrolling title
  int8_t slotNum;
  const uint16_t* pic;
  std::string name;         // display name (UTF-8) for the web interface
  std::string webTitle;     // "<title> | <game>" (UTF-8) for the web interface
};

// lidi, pietsmiet, bonjwa, bonjwachill, gronkh, dhalucard, trilluxe, dracon, maxim, finanzfluss
std::array<channelInfo, 10> channels = {{
  {"761017145", false, "", -1, epd_bitmap_lidi},
  {"21991090", false, "", -1, epd_bitmap_pietsmiet},
  {"73437396", false, "", -1, epd_bitmap_bonjwa},
  {"1024088182", false, "", -1, epd_bitmap_bonjwachill},
  {"12875057", false, "", -1, epd_bitmap_gronkh},
//  {"106159308", false, "", -1, epd_bitmap_gronkh}, gronkhtv
  {"16064695", false, "", -1, epd_bitmap_dhalucard},
  {"55898523", false, "", -1, epd_bitmap_trilluxe},
  {"38770961", false, "", -1, epd_bitmap_dracon},
  {"172376071", false, "", -1, epd_bitmap_maxim},
  {"549536744", false, "", -1, epd_bitmap_finanzfluss}
}};

uint16_t live_num = 0;

std::deque<channelInfo*> titleChangeQueue;

enum class FetchResult { Ok, Unauthorized, Failed };

struct StreamInfo {
  std::string id;
  std::string name;
  std::string title;  // "    <title> | <game>   " in the display charset
  std::string webTitle; // "<title> | <game>" in UTF-8
};

// Result of one Twitch update, handed from the network task to loop()
struct LiveUpdate {
  FetchResult result;
  std::vector<StreamInfo> streams;
};

TaskHandle_t netTask = nullptr;
QueueHandle_t updateQueue = nullptr;

// Wakes the network task for an update now instead of after TW_UPDATE_INTERVAL
void requestUpdate(){
  if(netTask) xTaskNotifyGive(netTask);
}

WebServer server(80);

// Firmware upload state (the upload runs inside server.handleClient() in loop())
volatile bool otaActive = false;   // also pauses the network task
struct {
  bool authorized;
  bool headerChecked;
  String error;
  int lastPercent;
} upload;
unsigned long rebootAt = 0;        // millis() of a scheduled restart, 0 = none
unsigned long restoreDisplayAt = 0; // millis() to leave the update screen after a failed upload

// Firmware state for the status page and the rollback protection
bool appPendingVerify = false;     // running a new OTA image that is not confirmed yet
FetchResult lastFetchResult = FetchResult::Ok;
unsigned long lastFetchMillis = 0; // 0 = no update yet

GFXcanvas16 pic_canvas(PIC_SIZE, PIC_SIZE);
// one character (6*8 px at text size 8) wider than the visible title so it can scroll smoothly
GFXcanvas16 text_canvas(TITLE_WIDTH+(6*8), PIC_SIZE);

void setup(void) {

  SERIAL_PORT.begin(115200);
  DEBUG_I.printf("[%s] Starting...\n", DEBUG_TAG);

  DEBUG_I.printf("[%s] Connecting to WIFI...\n", DEBUG_TAG);
  WiFi.setHostname(HOSTNAME); // name in the router (twitchdisplay.fritz.box)
  wifiMulti.addAP("::1", WIFI_PW);


  DEBUG_I.printf("[%s] Initializing display...\n", DEBUG_TAG);
  pinMode(TFT_BK, OUTPUT);
  digitalWrite(TFT_BK, HIGH);

  tft.init(170, 320);           // Init ST7789 170x320
  tft.setRotation(1);
  tft.setSPISpeed(80000000);
  
  tft.fillScreen(ST77XX_BLACK);

  text_canvas.setCursor(0,0);
  text_canvas.setTextWrap(false);
  text_canvas.setTextColor(ST77XX_CYAN);
  text_canvas.setTextSize(8);
  text_canvas.cp437(true); // titles are converted to CP437, see toDisplayText()

  DEBUG_I.printf("[%s] Display initialized.\n", DEBUG_TAG);

  DEBUG_I.printf("[%s] Waiting for WIFI connection...\n", DEBUG_TAG);
  // wait for WiFi connection
  while((wifiMulti.run() != WL_CONNECTED)){
    delay(10);
  }

  DEBUG_I.printf("[%s] WIFI connected.\n", DEBUG_TAG);

  tlsClient.useBuiltinCACertBundle();

  setupOTA();
  setupWeb();

  // New firmware from an OTA update starts "pending verify": loop() confirms it
  // after APP_CONFIRM_DELAY online, otherwise the bootloader rolls back on the
  // next restart (needs a bootloader with rollback support)
  esp_ota_img_states_t otaState;
  if(esp_ota_get_state_partition(esp_ota_get_running_partition(), &otaState) == ESP_OK
     && otaState == ESP_OTA_IMG_PENDING_VERIFY){
    appPendingVerify = true;
    DEBUG_I.printf("[%s] New firmware, waiting %ds online before confirming it.\n", DEBUG_TAG, APP_CONFIRM_DELAY/1000);
  }

  // Restart if loop() hangs; with a pending firmware this triggers the rollback
  esp_task_wdt_config_t wdtConfig = { .timeout_ms = LOOP_WDT_TIMEOUT, .idle_core_mask = 0, .trigger_panic = true };
  esp_task_wdt_reconfigure(&wdtConfig);
  enableLoopWDT();

  // Twitch requests run in their own task so they don't block scrolling;
  // results come back to loop() through updateQueue
  updateQueue = xQueueCreate(2, sizeof(LiveUpdate*));
  xTaskCreate(networkTask, "network", NET_TASK_STACK, nullptr, NET_TASK_PRIORITY, &netTask);

  DEBUG_I.printf("[%s] Setup completed...\n", DEBUG_TAG);
}

void drawRGBBitmapSectionFast(int16_t x, int16_t y, uint16_t *bitmap,
                                int16_t o_x, int16_t o_y, int16_t w, int16_t h, int16_t b_w) {
  tft.startWrite();
  tft.setAddrWindow(x, y, w, h);
  for (int16_t j = o_y; j < (o_y+h); j++) {
    tft.writePixels(&bitmap[j * b_w + o_x], w);
  }
  tft.endWrite();
}

/**************************************************************************/
/*!
   @brief   Draw a rectangle with no fill color
    @param    x   Top left corner x coordinate
    @param    y   Top left corner y coordinate
    @param    w   Width in pixels
    @param    h   Height in pixels
    @param    t   Thickness of outer line
    @param    color 16-bit 5-6-5 Color to draw with
*/
/**************************************************************************/
void drawThickRect(int16_t x, int16_t y, int16_t w, int16_t h,
  int16_t t, uint16_t color){
  if(t==0) return;
  if(t<0) {
    t=-t;
    x-=t; y-=t;
    w=w+t+t; h=h+t+t;
  }
  tft.fillRect(x, y, w, t, color);
  tft.fillRect(x, y, t, h, color);
  tft.fillRect(x+w-1, y, -t, h, color);
  tft.fillRect(x+w-1, y+h-1, -w, -t, color);
}

#define MAX_TITLE_REPEAT 2

struct {
  channelInfo* channel;
  int8_t slot;              // slot of the channel when the title was (re)drawn
  std::string displayTitle;
  uint16_t textOffset;
  uint8_t repeats;
} channelTitleInfo;

bool isTitleDisplaying = false;

uint8_t o_X = 0;

// Green frame around the pic of the channel whose title is scrolling
void drawTitleIndicator(){
  uint16_t x, y;
  slotXY(channelTitleInfo.slot, x, y);
  drawThickRect(x, y, PIC_SIZE, PIC_SIZE, -PIC_BORDER, ST77XX_GREEN);
}

void startTitle(channelInfo* channel){
  channelTitleInfo.channel = channel;
  channelTitleInfo.slot = channel->slotNum;
  channelTitleInfo.displayTitle = channel->streamTitle;
  channelTitleInfo.textOffset = 0;
  channelTitleInfo.repeats = 0;
  o_X = 0;
  isTitleDisplaying = true;
  drawTitleIndicator();
}

// Ends the title display and restores all pics (the title covers one row)
void stopTitle(){
  isTitleDisplaying = false;
  redrawLiveChannelPics();
}

uint16_t statusColor = STATUS_OK;

void setStatus(uint16_t color){
  if(color == statusColor) return;
  statusColor = color;
  tft.fillRect(STATUS_X, STATUS_Y, STATUS_SIZE, STATUS_SIZE, color);
}

// Returns true while WiFi is connected. The ESP32 reconnects on its own;
// this only shows the state, nudges the driver now and then and triggers
// an update as soon as the connection is back.
bool handleWiFi(){
  static unsigned long lostSince = 0;
  static unsigned long lastReconnect = 0;
  if(WiFi.status() == WL_CONNECTED){
    if(lostSince){
      DEBUG_I.printf("[%s] WIFI reconnected after %lus.\n", DEBUG_TAG, (millis()-lostSince)/1000);
      lostSince = 0;
      requestUpdate();
    }
    return true;
  }
  if(!lostSince){
    DEBUG_W.printf("[%s] WIFI connection lost.\n", DEBUG_TAG);
    lostSince = lastReconnect = millis();
    if(!lostSince) lostSince = lastReconnect = 1; // 0 means "connected"
    setStatus(STATUS_NO_WIFI);
  } else if(millis() - lastReconnect >= WIFI_RECONNECT_INTERVAL){
    DEBUG_I.printf("[%s] WIFI still disconnected, reconnecting...\n", DEBUG_TAG);
    WiFi.reconnect();
    lastReconnect = millis();
  }
  return false;
}

void applyLiveUpdate(const LiveUpdate& update);

void restoreDisplay();
void confirmFirmwareWhenStable(bool wifiConnected);

void loop(){
  ArduinoOTA.handle();
  server.handleClient();

  if(rebootAt && millis() >= rebootAt){
    DEBUG_I.printf("[%s] Restarting...\n", DEBUG_TAG);
    delay(100);
    ESP.restart();
  }
  if(restoreDisplayAt && millis() >= restoreDisplayAt){
    restoreDisplayAt = 0;
    restoreDisplay();
  }

  static unsigned long lastLdr = 0;
  if(millis() - lastLdr >= LDR_INTERVAL){
    lastLdr = millis();
    ldr = analogRead(LDR_PIN);
    ldr_avg = ldrAverage.compute(ldr);
    analogWrite(TFT_BK, constrain(map(ldr_avg, LDR_BRIGHTEST, 4095, 255, BK_MIN), BK_MIN, 255));
  }

  bool wifiConnected = handleWiFi();
  confirmFirmwareWhenStable(wifiConnected);

  if(otaActive || rebootAt || restoreDisplayAt){
    // the display shows the update screen
    delay(10);
    return;
  }

  LiveUpdate* update;
  if(xQueueReceive(updateQueue, &update, 0) == pdTRUE){
    applyLiveUpdate(*update);
    delete update;
  }

  while(!isTitleDisplaying && !titleChangeQueue.empty()) {
    channelInfo* next = titleChangeQueue.front();
    titleChangeQueue.pop_front();
    // channel may have gone offline while it was queued
    if(next->isLive) startTitle(next);
  }
  if(isTitleDisplaying){
    if(!o_X){
      text_canvas.fillScreen(ST77XX_BLACK);
      text_canvas.setCursor(0,0);
      text_canvas.print(&channelTitleInfo.displayTitle[channelTitleInfo.textOffset]);
      channelTitleInfo.textOffset++;
      channelTitleInfo.textOffset%=channelTitleInfo.displayTitle.length()-5;
    }
    // the title scrolls over the row that does not contain the channel's pic
    drawRGBBitmapSectionFast(GRID_X0, rowY(1-slotRow(channelTitleInfo.slot)), text_canvas.getBuffer(), o_X, 0, TITLE_WIDTH, text_canvas.height(), text_canvas.width());
    o_X = (o_X+6)%(6*8);

    if (!o_X && channelTitleInfo.textOffset==0) {
      if(++channelTitleInfo.repeats==MAX_TITLE_REPEAT){
        stopTitle();
      }
    }
  }

#ifdef DEBUG_UDP_LDR
  static unsigned long lastUdp = 0;
  if(wifiConnected && millis() - lastUdp >= DEBUG_UDP_INTERVAL){
    lastUdp = millis();
    Udp.beginPacket(DEBUG_UDP_HOST, DEBUG_UDP_PORT);
    Udp.print("ldr:");
    Udp.println(ldr);
    Udp.print("ldr_avg:");
    Udp.println(ldr_avg);
    Udp.endPacket();
  }
#else
  (void)wifiConnected;
#endif
  delay(10);
}

// ---------- Text conversion

// CP437 code (or ASCII fallback) for U+00A0..U+00FF, 0 = drop
static const uint8_t latin1ToCp437[96] = {
  ' ',  0xAD, 0x9B, 0x9C, '*',  0x9D, '|',  'S',  '"',  'c',  0xA6, 0xAE, 0xAA, 0,    'R',  '-',   // A0-AF
  0xF8, 0xF1, 0xFD, '3',  '\'', 0xE6, 'P',  0xFA, ',',  '1',  0xA7, 0xAF, 0xAC, 0xAB, '3',  0xA8,  // B0-BF
  'A',  'A',  'A',  'A',  0x8E, 0x8F, 0x92, 0x80, 'E',  0x90, 'E',  'E',  'I',  'I',  'I',  'I',   // C0-CF
  'D',  0xA5, 'O',  'O',  'O',  'O',  0x99, 'x',  'O',  'U',  'U',  'U',  0x9A, 'Y',  'P',  0xE1,  // D0-DF
  0x85, 0xA0, 0x83, 'a',  0x84, 0x86, 0x91, 0x87, 0x8A, 0x82, 0x88, 0x89, 0x8D, 0xA1, 0x8C, 0x8B,  // E0-EF
  'd',  0xA4, 0x95, 0xA2, 0x93, 'o',  0x94, 0xF6, 'o',  0x97, 0xA3, 0x96, 0x81, 'y',  'p',  0x98   // F0-FF
};

// Converts UTF-8 text to the CP437 charset of the built-in GFX font.
// Umlauts and other Latin-1 letters are kept, typographic quotes/dashes are
// replaced, everything else without a glyph (e.g. emoji) is dropped.
std::string toDisplayText(const char* utf8){
  std::string out;
  if(utf8 == nullptr) return out;
  const uint8_t* p = (const uint8_t*)utf8;
  while(*p){
    uint32_t cp;
    int len;
    if(*p < 0x80)                { cp = *p;        len = 1; }
    else if((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
    else if((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
    else if((*p & 0xF8) == 0xF0) { cp = *p & 0x07; len = 4; }
    else { p++; continue; }      // invalid lead byte
    int k = 1;
    for(; k < len && (p[k] & 0xC0) == 0x80; k++) cp = (cp << 6) | (p[k] & 0x3F);
    if(k < len){ p++; continue; } // truncated sequence
    p += len;

    if(cp < 0x20) out += ' ';                       // newlines, tabs
    else if(cp < 0x7F) out += (char)cp;
    else if(cp >= 0xA0 && cp <= 0xFF){ if(latin1ToCp437[cp-0xA0]) out += (char)latin1ToCp437[cp-0xA0]; }
    else switch(cp){
      case 0x2013: case 0x2014: out += '-';  break; // en/em dash
      case 0x2018: case 0x2019: case 0x201A: out += '\''; break;
      case 0x201C: case 0x201D: case 0x201E: out += '"';  break;
      case 0x2022: out += (char)0xFA; break;        // bullet
      case 0x2026: out += "..."; break;
      case 0x20AC: out += "EUR"; break;
      default: break;                               // no glyph: drop
    }
  }
  return out;
}

// ---------- Twitch API (runs in the network task)

void commonHttpInit(HTTPClient& http_client){
  DEBUG_I.print("[HTTP] Common init...\n");
  http_client.useHTTP10(true);
  
  // add headers
  http_client.setAuthorizationType("Bearer");
  http_client.setAuthorization(twitchToken.c_str());
  http_client.addHeader("Client-Id", TWITCH_CLIENT_ID);
}

#ifdef TWITCH_CLIENT_SECRET
// Gets a new app access token (client credentials flow) into twitchToken
bool requestAppToken(){
  DEBUG_I.printf("[%s] Requesting new app access token...\n", DEBUG_TAG);
  HTTPClient http;
  http.useHTTP10(true);
  http.begin(tlsClient, "https://id.twitch.tv/oauth2/token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int httpCode = http.POST("client_id=" TWITCH_CLIENT_ID "&client_secret=" TWITCH_CLIENT_SECRET "&grant_type=client_credentials");
  if(httpCode != HTTP_CODE_OK){
    DEBUG_W.printf("[HTTP] Token request failed, error: %s\n",
      (httpCode<=0) ? http.errorToString(httpCode).c_str() : String(httpCode).c_str());
    http.end();
    return false;
  }
  JsonDocument filter;
  filter["access_token"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  const char* token = doc["access_token"];
  if(err || token == nullptr){
    DEBUG_W.printf("[%s] Token response invalid.\n", DEBUG_TAG);
    return false;
  }
  twitchToken = token;
  DEBUG_I.printf("[%s] Got new app access token.\n", DEBUG_TAG);
  return true;
}
#endif

// Requests the streams of all channels and fills update.streams
FetchResult fetchLiveChannels(LiveUpdate& update){
  // Could maybe be done as constexpr??
  String url = String("https://api.twitch.tv/helix/streams?");
  for (auto& channel : channels) {
    url += "user_id=";
    url += channel.id.c_str();
    url += "&";
  }

  DEBUG_I.printf("[%s] Update live channels with url: %s\n", DEBUG_TAG, url.c_str());

  HTTPClient http;
  http.begin(tlsClient, url);
  commonHttpInit(http);
  DEBUG_I.print("[HTTP] GET...\n");
  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    DEBUG_W.printf(
      "[HTTP] GET failed, error: %s\n",
      (httpCode<=0)?
        http.errorToString(httpCode).c_str():
        String(httpCode).c_str());
    http.end();
    return (httpCode == HTTP_CODE_UNAUTHORIZED) ? FetchResult::Unauthorized : FetchResult::Failed;
  }

  DEBUG_I.print("[JSON] Deserializing response...\n");

  // only keep the fields that are used
  JsonDocument filter;
  JsonObject stream = filter["data"].add<JsonObject>();
  stream["user_id"] = true;
  stream["user_name"] = true;
  stream["title"] = true;
  stream["game_name"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    DEBUG_W.print("[JSON] deserializeJson() failed with code ");
    DEBUG_W.println(err.f_str());
    return FetchResult::Failed;
  }
  if (!doc["data"].is<JsonArray>()) {
    DEBUG_W.print("[JSON] Response has no data array.\n");
    return FetchResult::Failed;
  }

  for (JsonObject stream : doc["data"].as<JsonArray>()) {
    const char* id = stream["user_id"];
    if (id == nullptr) continue;
    StreamInfo info;
    info.id = id;
    info.name = stream["user_name"] | "";
    info.title = "    " + toDisplayText(stream["title"]) + " | " + toDisplayText(stream["game_name"]) + "   ";
    info.webTitle = std::string(stream["title"] | "") + " | " + (stream["game_name"] | "");
    update.streams.push_back(std::move(info));
  }
  return FetchResult::Ok;
}

// One update: fetch (renewing the token if needed) and hand the result to loop()
void runNetworkUpdate(){
  if (WiFi.status() != WL_CONNECTED) return; // handleWiFi() requests an update when it is back
  if (otaActive) return;                      // firmware upload in progress

  LiveUpdate* update = new LiveUpdate();
#ifdef TWITCH_CLIENT_SECRET
  if (twitchToken.isEmpty()) requestAppToken();
#endif
  update->result = fetchLiveChannels(*update);
#ifdef TWITCH_CLIENT_SECRET
  if (update->result == FetchResult::Unauthorized && requestAppToken()) {
    // token expired or revoked: retry once with the new one
    update->streams.clear();
    update->result = fetchLiveChannels(*update);
  }
#endif
  if (xQueueSend(updateQueue, &update, portMAX_DELAY) != pdTRUE) delete update;
}

void networkTask(void*){
  for(;;){
    runNetworkUpdate();
    // sleep until the next interval or until requestUpdate()
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TW_UPDATE_INTERVAL));
  }
}

// ---------- Drawing and channel state (loop() only)


void drawLiveChannelPic(channelInfo& channel, uint8_t pic_slot_index){
  channel.slotNum = pic_slot_index;
  if(pic_slot_index<MAX_NUM_PICS){
    DEBUG_I.printf("[%s] Drawing channel pic of %s in slot number %u\n", DEBUG_TAG, channel.id.c_str(), pic_slot_index);
    pic_canvas.drawRGBBitmap(0, 0, channel.pic, pic_canvas.width(), pic_canvas.height());
  } else {
    // more live channels than slots: the last slot shows how many are hidden
    pic_canvas.fillScreen(ST77XX_BLACK);
    pic_canvas.setCursor(4, 14);
    pic_canvas.setTextSize(5);
    pic_canvas.printf("+%d", pic_slot_index-(MAX_NUM_PICS-2));
  }
  uint16_t x, y;
  slotXY(pic_slot_index, x, y);
  tft.drawRGBBitmap(x, y, pic_canvas.getBuffer(), pic_canvas.width(), pic_canvas.height());
  drawThickRect(x, y, PIC_SIZE, PIC_SIZE, -PIC_BORDER, ST77XX_BLACK);
}

// Blanks a slot including its frame
void clearSlot(int8_t slot){
  uint16_t x, y;
  slotXY(slot, x, y);
  tft.fillRect(x-PIC_BORDER, y-PIC_BORDER, PIC_SIZE+2*PIC_BORDER, PIC_SIZE+2*PIC_BORDER, ST77XX_BLACK);
}

// Gives every live channel the next slot in list order and offline channels -1.
// Returns true if any channel's slot changed.
bool assignSlots(){
  bool changed = false;
  int8_t next = 0;
  for (channelInfo& channel : channels) {
    int8_t slot = channel.isLive ? next++ : -1;
    if(channel.slotNum != slot){
      channel.slotNum = slot;
      changed = true;
    }
  }
  live_num = next;
  return changed;
}

// Draws the pics of all live channels at their assigned slot and blanks the
// unused slots. With row >= 0 only that row is redrawn.
void redrawLiveChannelPics(int8_t row){
  for (channelInfo& channel : channels) {
    if(channel.isLive && (row < 0 || slotRow(channel.slotNum) == row)){
      drawLiveChannelPic(channel, channel.slotNum);
    }
  }
  for (int8_t slot = live_num; slot < MAX_NUM_PICS; slot++){
    if(row < 0 || slotRow(slot) == row) clearSlot(slot);
  }
}

channelInfo* findChannel(const std::string& id){
  for (channelInfo& channel : channels) {
    if (channel.id == id) return &channel;
  }
  return nullptr;
}

// Applies a result of the network task to the channels and the display
void applyLiveUpdate(const LiveUpdate& update){
  lastFetchResult = update.result;
  lastFetchMillis = millis();
  if (update.result != FetchResult::Ok) {
    // keep showing the last known state
    setStatus(update.result == FetchResult::Unauthorized ? STATUS_AUTH : STATUS_API);
    return;
  }
  setStatus(STATUS_OK);

  // slotNum is kept so assignSlots() can detect layout changes
  for (auto &&c : channels) {
    c.isLive = false;
  }

  for (const StreamInfo& stream : update.streams) {
    channelInfo* ch = findChannel(stream.id);
    if (ch == nullptr) {
      DEBUG_W.printf("[%s] Unknown channel id %s.\n", DEBUG_TAG, stream.id.c_str());
      continue;
    }
    DEBUG_I.printf("[%s] Live channel: %s\n", DEBUG_TAG, stream.name.c_str());
    ch->isLive = true;
    ch->name = stream.name;
    ch->webTitle = stream.webTitle;
    if (ch->streamTitle != stream.title) {
      ch->streamTitle = stream.title;
      // queue channel for title display if not already in queue
      if (std::find(titleChangeQueue.begin(), titleChangeQueue.end(), ch) == titleChangeQueue.end()) {
        titleChangeQueue.push_back(ch);
      }
    }
  }

  bool layoutChanged = assignSlots();

  if(isTitleDisplaying){
    channelInfo* ch = channelTitleInfo.channel;
    if(!ch->isLive){
      // channel went offline during its title
      DEBUG_I.printf("[%s] Channel %s went offline, stopping title.\n", DEBUG_TAG, ch->id.c_str());
      stopTitle();
    } else if(slotRow(ch->slotNum) != slotRow(channelTitleInfo.slot)){
      // the pic moved to the row the title scrolls over: show the title again from the start
      DEBUG_I.printf("[%s] Channel %s changed row, restarting title.\n", DEBUG_TAG, ch->id.c_str());
      stopTitle();
      if (std::find(titleChangeQueue.begin(), titleChangeQueue.end(), ch) == titleChangeQueue.end()) {
        titleChangeQueue.push_front(ch);
      }
    } else if(layoutChanged){
      // only redraw the row with the pics, the other row is covered by the title
      // and gets redrawn when the title ends
      channelTitleInfo.slot = ch->slotNum;
      redrawLiveChannelPics(slotRow(ch->slotNum));
      drawTitleIndicator();
    }
  } else if(layoutChanged){
    redrawLiveChannelPics();
  }
}

void setupOTA(){
  // Port defaults to 3232
  // ArduinoOTA.setPort(3232);

  // Hostname defaults to esp3232-[MAC]
  ArduinoOTA.setHostname(HOSTNAME); // also starts mDNS: twitchdisplay.local

  // Same password as the web interface; pio passes it via upload_flags
  ArduinoOTA.setPassword(WEB_PASSWORD);

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

// ---------- Rollback protection

// Tell the Arduino core that this firmware confirms itself (see
// confirmFirmwareWhenStable()) instead of being marked as good right at boot
extern "C" bool verifyRollbackLater(){
  return true;
}

// Marks a freshly updated firmware as good once it has been online for
// APP_CONFIRM_DELAY. Until then a crash or restart rolls back to the old one.
void confirmFirmwareWhenStable(bool wifiConnected){
  static unsigned long onlineSince = 0;
  if(!appPendingVerify) return;
  if(!wifiConnected){
    onlineSince = 0;
    return;
  }
  if(!onlineSince){
    onlineSince = millis();
    if(!onlineSince) onlineSince = 1;
  } else if(millis() - onlineSince >= APP_CONFIRM_DELAY){
    esp_ota_mark_app_valid_cancel_rollback();
    appPendingVerify = false;
    DEBUG_I.printf("[%s] New firmware confirmed.\n", DEBUG_TAG);
  }
}

const char* otaStateName(){
  esp_ota_img_states_t state;
  if(esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK) return "unknown";
  switch(state){
    case ESP_OTA_IMG_NEW:            return "new (bootloader without rollback support?)";
    case ESP_OTA_IMG_PENDING_VERIFY: return "on probation, confirmed after 60 s online";
    case ESP_OTA_IMG_VALID:          return "confirmed";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    case ESP_OTA_IMG_UNDEFINED:      return "flashed via USB";
    default:                         return "unknown";
  }
}

// ---------- Firmware upload

// Checks the first bytes of an upload before anything is written to flash.
// Returns nullptr if it looks like an application image for this chip.
const char* validateFirmwareHeader(const uint8_t* data, size_t len){
  if(len < 36) return "File is too small to be a firmware image.";
  if(data[0] != 0xE9) return "Not an ESP32 firmware image. Upload firmware.bin.";
  uint16_t chipId = data[12] | (data[13] << 8);
  if(chipId != CONFIG_IDF_FIRMWARE_CHIP_ID) return "This firmware is built for a different chip.";
  // an application image starts its first segment with the app description,
  // a bootloader or a merged image (bootloader + partitions + app) does not
  uint32_t magic = data[32] | (data[33] << 8) | (data[34] << 16) | ((uint32_t)data[35] << 24);
  if(magic != ESP_APP_DESC_MAGIC_WORD) return "Not an application image. Upload firmware.bin, not the bootloader or merged image.";
  return nullptr;
}

size_t maxFirmwareSize(){
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  return next ? next->size : 0;
}

void drawUpdateScreen(const char* title, uint16_t color, const char* message){
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextWrap(true);
  tft.setTextSize(3);
  tft.setTextColor(color);
  tft.setCursor(20, 30);
  tft.print(title);
  if(message){
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(0, 110);
    tft.print(message);
  }
}

void drawUpdateProgress(int percent){
  const int x = 20, y = 90, w = 280, h = 24;
  tft.drawRect(x, y, w, h, ST77XX_WHITE);
  tft.fillRect(x + 2, y + 2, (w - 4) * percent / 100, h - 4, ST77XX_GREEN);
}

// Back to the channel grid after a failed upload
void restoreDisplay(){
  isTitleDisplaying = false;
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(STATUS_X, STATUS_Y, STATUS_SIZE, STATUS_SIZE, statusColor);
  redrawLiveChannelPics();
}

void failUpload(const char* message){
  if(upload.error.length()) return; // keep the first error
  upload.error = message;
  DEBUG_W.printf("[OTA] Upload failed: %s\n", message);
  if(Update.isRunning()) Update.abort();
}

// Called by the web server for every chunk of the uploaded file
void handleUploadChunk(){
  HTTPUpload& up = server.upload();
  switch(up.status){
    case UPLOAD_FILE_START:
      upload.authorized = server.authenticate(WEB_USER, WEB_PASSWORD);
      upload.headerChecked = false;
      upload.error = "";
      upload.lastPercent = -1;
      if(!upload.authorized) return;
      DEBUG_I.printf("[OTA] Upload of %s started.\n", up.filename.c_str());
      otaActive = true;
      restoreDisplayAt = 0;
      drawUpdateScreen("Firmware update", ST77XX_CYAN, nullptr);
      drawUpdateProgress(0);
      if(!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) failUpload(Update.errorString());
      break;

    case UPLOAD_FILE_WRITE:
      feedLoopWDT();
      if(!upload.authorized || upload.error.length()) return;
      if(!upload.headerChecked){
        const char* err = validateFirmwareHeader(up.buf, up.currentSize);
        if(err){ failUpload(err); return; }
        upload.headerChecked = true;
      }
      if(Update.write(up.buf, up.currentSize) != up.currentSize){
        failUpload(Update.errorString());
        return;
      }
      {
        // Content-Length includes the multipart framing, close enough for a progress bar
        int total = server.clientContentLength();
        int percent = total > 0 ? (int)((uint64_t)up.totalSize * 100 / total) : 0;
        if(percent > 100) percent = 100;
        if(percent != upload.lastPercent){
          upload.lastPercent = percent;
          drawUpdateProgress(percent);
        }
      }
      break;

    case UPLOAD_FILE_END:
      feedLoopWDT();
      if(!upload.authorized || upload.error.length()) return;
      if(!upload.headerChecked){ failUpload("File is too small to be a firmware image."); return; }
      // end() verifies the checksum and the image before switching the boot partition
      if(!Update.end(true)) failUpload(Update.errorString());
      else DEBUG_I.printf("[OTA] Upload complete, %u bytes.\n", (unsigned)up.totalSize);
      break;

    case UPLOAD_FILE_ABORTED:
      if(upload.authorized) failUpload("Upload aborted.");
      break;
  }
}

void sendJson(int code, bool ok, const String& message){
  String body = String("{\"ok\":") + (ok ? "true" : "false") + ",\"message\":\"";
  for(char c : message){
    if(c == '"' || c == '\\') body += '\\';
    body += c;
  }
  body += "\"}";
  server.send(code, "application/json", body);
}

// Called by the web server after the whole upload has been received
void handleUploadDone(){
  if(!upload.authorized || !server.authenticate(WEB_USER, WEB_PASSWORD)){
    return server.requestAuthentication(DIGEST_AUTH, WEB_REALM);
  }
  if(upload.error.length() == 0 && !upload.headerChecked) failUpload("No file received.");
  if(upload.error.length()){
    otaActive = false;
    drawUpdateScreen("Update failed", ST77XX_RED, upload.error.c_str());
    restoreDisplayAt = millis() + 5000;
    sendJson(400, false, "Update failed: " + upload.error);
    return;
  }
  drawUpdateProgress(100);
  drawUpdateScreen("Update done", ST77XX_GREEN, "Restarting...");
  sendJson(200, true, "Update successful, the display restarts now.");
  rebootAt = millis() + 1000; // give the response time to reach the browser
}

// ---------- Web pages

String htmlEscape(const std::string& in){
  String out;
  for(char c : in){
    switch(c){
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

String formatDuration(unsigned long seconds){
  char buf[32];
  snprintf(buf, sizeof(buf), "%lud %02luh %02lum %02lus", seconds/86400, (seconds/3600)%24, (seconds/60)%60, seconds%60);
  return String(buf);
}

// Sends a page made of WEB_PAGE_HEAD, body and WEB_PAGE_FOOT
void sendPage(const char* title, const String& body){
  String page = WEB_PAGE_HEAD;
  page.replace("%TITLE%", title);
  page += body;
  page += WEB_PAGE_FOOT;
  server.send(200, "text/html; charset=utf-8", page);
}

bool checkAuth(){
  if(server.authenticate(WEB_USER, WEB_PASSWORD)) return true;
  server.requestAuthentication(DIGEST_AUTH, WEB_REALM);
  return false;
}

void handleStatus(){
  if(!checkAuth()) return;
  String b;
  b.reserve(2048);
  b += "<div class=\"card\"><h2>Live channels</h2>";
  int live = 0;
  for(const channelInfo& ch : channels){
    if(!ch.isLive) continue;
    live++;
    b += "<p><b>" + htmlEscape(ch.name.empty() ? ch.id : ch.name) + "</b><br><span class=\"muted\">" + htmlEscape(ch.webTitle) + "</span></p>";
  }
  if(!live) b += "<p class=\"muted\">No channel is live.</p>";
  b += "</div>";

  b += "<div class=\"card\"><h2>Twitch</h2><table>";
  b += "<tr><td>Last update</td><td>";
  if(!lastFetchMillis) b += "none yet";
  else b += formatDuration((millis() - lastFetchMillis) / 1000) + " ago";
  b += "</td></tr><tr><td>Result</td><td>";
  switch(lastFetchResult){
    case FetchResult::Ok:           b += "<span class=\"ok\">OK</span>"; break;
    case FetchResult::Unauthorized: b += "<span class=\"err\">Token rejected (expired or invalid)</span>"; break;
    case FetchResult::Failed:       b += "<span class=\"err\">Request failed</span>"; break;
  }
  b += "</td></tr></table></div>";

  const esp_partition_t* running = esp_ota_get_running_partition();
  b += "<div class=\"card\"><h2>Device</h2><table>";
  b += "<tr><td>Firmware</td><td>" FW_VERSION "</td></tr>";
  b += "<tr><td>Built</td><td>" __DATE__ " " __TIME__ "</td></tr>";
  b += String("<tr><td>Partition</td><td>") + (running ? running->label : "?") + ", " + otaStateName() + "</td></tr>";
  b += "<tr><td>Uptime</td><td>" + formatDuration(millis() / 1000) + "</td></tr>";
  b += "<tr><td>WiFi</td><td>" + WiFi.SSID() + ", " + String(WiFi.RSSI()) + " dBm</td></tr>";
  b += "<tr><td>IP address</td><td>" + WiFi.localIP().toString() + "</td></tr>";
  b += "<tr><td>Free memory</td><td>" + String(ESP.getFreeHeap() / 1024) + " KB (lowest " + String(ESP.getMinFreeHeap() / 1024) + " KB)</td></tr>";
  b += "<tr><td>Brightness sensor</td><td>" + String(ldr_avg) + "</td></tr>";
  b += "</table><form method=\"post\" action=\"/reboot\" onsubmit=\"return confirm('Restart the display?')\"><p><button>Restart</button></p></form></div>";
  sendPage("Status", b);
}

void handleUpdatePage(){
  if(!checkAuth()) return;
  String body = WEB_UPDATE_BODY;
  body.replace("%MAXSIZE%", String((unsigned)maxFirmwareSize()));
  sendPage("Update", body);
}

void handleReboot(){
  if(!checkAuth()) return;
  sendPage("Restart", "<div class=\"card\"><p>Restarting... <a href=\"/\">Reload</a> in a few seconds.</p></div>");
  rebootAt = millis() + 1000;
}

void setupWeb(){
  server.on("/", HTTP_GET, handleStatus);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUploadDone, handleUploadChunk);
  server.on("/reboot", HTTP_POST, handleReboot);
  // answered without a login challenge: every challenge creates a new digest
  // nonce, which would invalidate the browser's current login
  server.on("/favicon.ico", HTTP_GET, [](){ server.send(204); });
  server.onNotFound([](){ server.send(404, "text/plain", "Not found"); });
  server.begin();
  MDNS.addService("http", "tcp", 80);
}
