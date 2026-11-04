// TODO: full unicode support (e.g. with https://github.com/takkaO/OpenFontRender/tree/master and Noto Sans + Noto Emoji, but together at least 1,5MB).
//       For now titles are converted to the CP437 charset of the built-in font (umlauts and other Latin-1 letters work, emoji are dropped).
// TODO: reduce scrolling text flicker by flipping the MV flag such that the pixels can be transfered column by column and not row by row

#include <Arduino.h>
#include <Adafruit_GFX.h>    // Core graphics library
#include <Adafruit_ST7789.h> // Hardware-specific library for ST7789
#include <SPI.h>
#include <WiFi.h>
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
#include <Preferences.h>
#include <DNSServer.h>

#include <deque>
#include <string>
#include <vector>

#include "pics.h"
// optional: default settings for the first boot (see include/secrets.h.example)
#if __has_include("secrets.h")
#include "secrets.h"
#endif

#include "MovingAverage.h"
#include "LogoImage.h"
#include "web_pages.h"

static const char DEBUG_TAG[] = "TwitchDisplay";

// inspiration from https://forum.arduino.cc/t/single-line-define-to-disable-code/636044/5
// Serial output with "\r\n" line ends. The firmware writes "\n"; terminals
// that don't add the carriage return themselves would show a staircase.
class CrlfSerial : public Print {
  bool lastWasCR = false;
public:
  void begin(unsigned long baud){ Serial.begin(baud); }
  size_t write(uint8_t c) override {
    if(c == '\n' && !lastWasCR) Serial.write('\r');
    lastWasCR = (c == '\r');
    return Serial.write(c);
  }
  size_t write(const uint8_t* buf, size_t len) override {
    size_t start = 0;
    for(size_t i = 0; i < len; i++){
      bool prevCR = i ? buf[i-1] == '\r' : lastWasCR;
      if(buf[i] == '\n' && !prevCR){
        Serial.write(buf + start, i - start);
        Serial.write('\r');
        start = i; // the '\n' is written with the next chunk
      }
    }
    Serial.write(buf + start, len - start);
    if(len) lastWasCR = (buf[len-1] == '\r');
    return len;
  }
};
CrlfSerial debugSerial;
#define SERIAL_PORT debugSerial
#define DEBUG_ERROR true
#define DEBUG_WARNING true
#define DEBUG_INFO true
#define DEBUG_E if(DEBUG_ERROR)SERIAL_PORT
#define DEBUG_W if(DEBUG_WARNING)SERIAL_PORT
#define DEBUG_I if(DEBUG_INFO)SERIAL_PORT

// Settings are stored on the device (Settings page / setup mode). Values from
// include/secrets.h are only used for settings that were never saved.
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PW
#define WIFI_PW ""
#endif
#ifndef TWITCH_CLIENT_ID
#define TWITCH_CLIENT_ID ""
#endif
#ifndef TWITCH_CLIENT_SECRET
#define TWITCH_CLIENT_SECRET ""
#endif
#ifndef TWITCH_TOKEN
#define TWITCH_TOKEN ""
#endif
#ifndef WEB_PASSWORD
#define WEB_PASSWORD ""
#endif
// Client ID sent with a token when no client ID is set
#define DEFAULT_CLIENT_ID "gp762nuuoqcoxypju8c569th9wz7q5"
#define PREFS_NAMESPACE   "config"

// Setup mode: own WiFi hotspot with the Settings page
#define SETUP_AP_SSID        "TwitchDisplay-Setup"
#define WIFI_CONNECT_TIMEOUT (2*60*1000)  // at boot: open the setup hotspot if WiFi is not reachable for this long
#define SETUP_RETRY_TIMEOUT  (10*60*1000) // ...and restart to try WiFi again after this long without a hotspot client
#define SETUP_BUTTON_PIN     9            // BOOT button (GPIO9, low when pressed)
#define BOOT_HOLD_TIME       5000         // hold BOOT this long while running to restart into setup mode
#define SETUP_REQUEST_MAGIC  0x53455455   // "SETU" in RTC memory: survives the restart into setup mode

// Web interface (status page, firmware upload, settings) and ArduinoOTA
#define WEB_USER          "admin"
#define WEB_REALM         "twitchDisplay"
#define HOSTNAME          "twitchdisplay"
#define APP_CONFIRM_DELAY (60*1000) // new firmware must be online this long before it is marked as good
#define LOOP_WDT_TIMEOUT  30000     // ms; restarts the display if loop() hangs (longer than the web server timeouts)

#ifndef FW_VERSION
#define FW_VERSION "unknown"        // set by tools/version.py
#endif

// Online update: newest GitHub release of this repository (built by .github/workflows/build.yml)
#ifndef GITHUB_REPO
#define GITHUB_REPO "Modac/twitchDisplay"
#endif
#define FIRMWARE_ASSET   "firmware.bin"
#define DOWNLOAD_TIMEOUT 15000      // ms without data before a download is aborted
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

// Settings, loaded once at boot; saving them restarts the display
struct Config {
  String wifiSsid;
  String wifiPassword;
  String clientId;
  String clientSecret;
  String token;
  String webPassword;

  const char* effectiveClientId() const { return clientId.length() ? clientId.c_str() : DEFAULT_CLIENT_ID; }
  bool hasTwitchCredentials() const { return clientSecret.length() || token.length(); }
} config;

void loadConfig(){
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true); // fails harmlessly if nothing was saved yet: defaults are returned
  config.wifiSsid     = prefs.getString("ssid", WIFI_SSID);
  config.wifiPassword = prefs.getString("wifipw", WIFI_PW);
  config.clientId     = prefs.getString("clientid", TWITCH_CLIENT_ID);
  config.clientSecret = prefs.getString("secret", TWITCH_CLIENT_SECRET);
  config.token        = prefs.getString("token", TWITCH_TOKEN);
  config.webPassword  = prefs.getString("webpw", WEB_PASSWORD);
  prefs.end();
}

// Set before ESP.restart() to come back in setup mode
RTC_NOINIT_ATTR uint32_t setupRequested;

bool setupMode = false;
#ifdef DEBUG_UDP_LDR
WiFiUDP Udp;
#endif

// TLS client for the Twitch API; verifies the server certificate against the
// root certificates built into the ESP32 core
WiFiClientSecure tlsClient;

String twitchToken; // config.token, or fetched with the client secret

MovingAverage<100> ldrAverage;
uint16_t ldr;     // last raw LDR reading
uint16_t ldr_avg; // averaged LDR reading used for the backlight

void redrawLiveChannelPics(int8_t row = -1);
void setupOTA();
void setupWeb();
void startSetupMode(const char* reason, bool retryLater);
bool connectWiFi();
void drawMessageScreen(const char* title, uint16_t color, const char* message);
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

enum class FetchResult { Ok, Unauthorized, Failed, NoCredentials };

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

// Online update requested on the web page; runs in loop() (blocking, ~20 s)
bool installRequested = false;
String installUrl;
String installVersion;
enum class InstallState { Idle, Failed } installState = InstallState::Idle;
String installError;
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

  pinMode(SETUP_BUTTON_PIN, INPUT_PULLUP);

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

  // New firmware from an OTA update starts "pending verify" and must confirm
  // itself (confirmFirmwareWhenStable()), otherwise the bootloader rolls back
  // on the next restart (needs a bootloader with rollback support). Checked
  // first: setup mode needs to know it too.
  esp_ota_img_states_t otaState;
  if(esp_ota_get_state_partition(esp_ota_get_running_partition(), &otaState) == ESP_OK
     && otaState == ESP_OTA_IMG_PENDING_VERIFY){
    appPendingVerify = true;
    DEBUG_I.printf("[%s] New firmware, waiting %ds before confirming it.\n", DEBUG_TAG, APP_CONFIRM_DELAY/1000);
  }

  loadConfig();
  twitchToken = config.token;

  if(setupRequested == SETUP_REQUEST_MAGIC){
    setupRequested = 0;
    startSetupMode("Requested with the BOOT button.", false);
    return;
  }
  if(config.wifiSsid.isEmpty()){
    startSetupMode("No WiFi network configured.", false);
    return;
  }
  if(config.webPassword.isEmpty()){
    startSetupMode("No web password configured.", false);
    return;
  }
  if(!connectWiFi()){
    startSetupMode("WiFi network not reachable.", true);
    return;
  }
  tft.fillScreen(ST77XX_BLACK);

  tlsClient.useBuiltinCACertBundle();

  setupOTA();
  setupWeb();

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

void setupModeLoop();
void checkBootButton();
void runInstall();

void loop(){
  if(setupMode){
    setupModeLoop();
    return;
  }
  ArduinoOTA.handle();
  server.handleClient();
  checkBootButton();

  if(rebootAt && millis() >= rebootAt){
    DEBUG_I.printf("[%s] Restarting...\n", DEBUG_TAG);
    delay(100);
    ESP.restart();
  }
  if(restoreDisplayAt && millis() >= restoreDisplayAt){
    restoreDisplayAt = 0;
    restoreDisplay();
  }
  if(installRequested){
    installRequested = false;
    runInstall();
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
  http_client.addHeader("Client-Id", config.effectiveClientId());
}

String urlEncode(const String& in){
  String out;
  const char* hex = "0123456789ABCDEF";
  for(char c : in){
    if(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') out += c;
    else { out += '%'; out += hex[(c >> 4) & 0xF]; out += hex[c & 0xF]; }
  }
  return out;
}

// Gets a new app access token (client credentials flow) into twitchToken
bool requestAppToken(){
  DEBUG_I.printf("[%s] Requesting new app access token...\n", DEBUG_TAG);
  HTTPClient http;
  http.useHTTP10(true);
  http.begin(tlsClient, "https://id.twitch.tv/oauth2/token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "client_id=" + urlEncode(config.effectiveClientId()) + "&client_secret=" + urlEncode(config.clientSecret) + "&grant_type=client_credentials";
  int httpCode = http.POST(body.c_str());
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
  bool canRenewToken = config.clientSecret.length() > 0;
  if (!config.hasTwitchCredentials()) {
    update->result = FetchResult::NoCredentials;
  } else {
    if (canRenewToken && twitchToken.isEmpty()) requestAppToken();
    update->result = fetchLiveChannels(*update);
    if (canRenewToken && update->result == FetchResult::Unauthorized && requestAppToken()) {
      // token expired or revoked: retry once with the new one
      update->streams.clear();
      update->result = fetchLiveChannels(*update);
    }
  }
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
    setStatus(update.result == FetchResult::Failed ? STATUS_API : STATUS_AUTH);
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
  ArduinoOTA.setPassword(config.webPassword.c_str());

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
void confirmFirmwareNow();

void confirmFirmwareWhenStable(bool wifiConnected){
  static unsigned long onlineSince = 0;
  if(!appPendingVerify){
    onlineSince = 0;
    return;
  }
  if(!wifiConnected){
    onlineSince = 0;
    return;
  }
  if(!onlineSince){
    onlineSince = millis();
    if(!onlineSince) onlineSince = 1;
  } else if(millis() - onlineSince >= APP_CONFIRM_DELAY){
    onlineSince = 0;
    confirmFirmwareNow();
  }
}

// Confirms a pending firmware right away (it just proved to work, e.g. the
// settings were saved in setup mode and the display is about to restart)
void confirmFirmwareNow(){
  if(!appPendingVerify) return;
  esp_ota_mark_app_valid_cancel_rollback();
  appPendingVerify = false;
  DEBUG_I.printf("[%s] New firmware confirmed.\n", DEBUG_TAG);
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

// Full screen message (firmware update, WiFi connection, setup mode)
// Layout of the message screen: title (text size 3, 24 px high), message
// (text size 2, 16 px per line) and progress bar. A one-line message ends at
// y=76, well above the bar; longer messages (errors) are shown without a bar.
#define MSG_X        20
#define MSG_TITLE_Y  20
#define MSG_TEXT_Y   60
#define PROGRESS_Y   120
#define PROGRESS_H   24

void drawMessageScreen(const char* title, uint16_t color, const char* message){
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextWrap(true);
  tft.setTextSize(3);
  tft.setTextColor(color);
  tft.setCursor(MSG_X, MSG_TITLE_Y);
  tft.print(title);
  if(message){
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(MSG_X, MSG_TEXT_Y);
    tft.print(message);
  }
}

void drawUpdateProgress(int percent){
  const int x = MSG_X, y = PROGRESS_Y, w = 320 - 2*MSG_X, h = PROGRESS_H;
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
      upload.authorized = setupMode || server.authenticate(WEB_USER, config.webPassword.c_str()); // setup mode: hotspot password protects
      upload.headerChecked = false;
      upload.error = "";
      upload.lastPercent = -1;
      if(!upload.authorized) return;
      DEBUG_I.printf("[OTA] Upload of %s started.\n", up.filename.c_str());
      otaActive = true;
      restoreDisplayAt = 0;
      drawMessageScreen("Firmware update", ST77XX_CYAN, nullptr);
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
  if(!upload.authorized || !(setupMode || server.authenticate(WEB_USER, config.webPassword.c_str()))){
    return server.requestAuthentication(DIGEST_AUTH, WEB_REALM);
  }
  if(upload.error.length() == 0 && !upload.headerChecked) failUpload("No file received.");
  if(upload.error.length()){
    otaActive = false;
    drawMessageScreen("Update failed", ST77XX_RED, upload.error.c_str());
    restoreDisplayAt = millis() + 5000;
    sendJson(400, false, "Update failed: " + upload.error);
    return;
  }
  drawUpdateProgress(100);
  drawMessageScreen("Update done", ST77XX_GREEN, "Restarting...");
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
  page.replace("%NAV%", setupMode ? "<a href=\"/settings\">Settings</a><a href=\"/update\">Update</a>"
                                  : "<a href=\"/\">Status</a><a href=\"/update\">Update</a><a href=\"/settings\">Settings</a>");
  page += body;
  page += WEB_PAGE_FOOT;
  server.send(200, "text/html; charset=utf-8", page);
}

bool checkAuth(){
  if(server.authenticate(WEB_USER, config.webPassword.c_str())) return true;
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
    case FetchResult::NoCredentials: b += "<span class=\"err\">No Twitch credentials, see <a href=\"/settings\">Settings</a></span>"; break;
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
  if(!setupMode && !checkAuth()) return;
  String body = WEB_UPDATE_BODY;
  String online;
  if(!setupMode){ // the setup hotspot has no internet connection
    online = WEB_ONLINE_UPDATE_CARD;
    online.replace("%VERSION%", FW_VERSION);
  }
  body.replace("%ONLINE%", online);
  body.replace("%MAXSIZE%", String((unsigned)maxFirmwareSize()));
  sendPage("Update", body);
}

void handleReboot(){
  if(!checkAuth()) return;
  sendPage("Restart", "<div class=\"card\"><p>Restarting... <a href=\"/\">Reload</a> in a few seconds.</p></div>");
  rebootAt = millis() + 1000;
}

void handleSettingsPage();
void handleSettingsSave();
void handleReset();
void handleUpdateCheck();
void handleUpdateInstall();
void handleUpdateState();

void setupWeb(){
  server.on("/", HTTP_GET, handleStatus);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUploadDone, handleUploadChunk);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.on("/settings", HTTP_GET, handleSettingsPage);
  server.on("/settings", HTTP_POST, handleSettingsSave);
  server.on("/reset", HTTP_POST, handleReset);
  server.on("/update/check", HTTP_GET, handleUpdateCheck);
  server.on("/update/install", HTTP_POST, handleUpdateInstall);
  server.on("/update/state", HTTP_GET, handleUpdateState);
  // answered without a login challenge: every challenge creates a new digest
  // nonce, which would invalidate the browser's current login
  server.on("/favicon.ico", HTTP_GET, [](){ server.send(204); });
  server.onNotFound([](){ server.send(404, "text/plain", "Not found"); });
  server.begin();
  MDNS.addService("http", "tcp", 80);
}


// ---------- Setup mode state

DNSServer dnsServer;
const char* setupModeReason = "";
bool setupRetryLater = false;        // restart to retry WiFi after SETUP_RETRY_TIMEOUT
unsigned long setupModeSince = 0;
String setupApPassword;
std::vector<String> scannedNetworks; // networks found when setup mode started

// ---------- Settings

bool isTwitchValue(const String& v){
  for(char c : v) if(!isalnum((unsigned char)c)) return false;
  return v.length() <= 128;
}

// Checks the submitted settings. Returns nullptr if they can be saved.
const char* validateSettings(const String& ssid, const String& wifiPassword, const String& clientId,
                             const String& clientSecret, const String& token,
                             const String& webPassword, const String& webPassword2, bool webPasswordSet){
  if(ssid.isEmpty()) return "Enter the name of your WiFi network.";
  if(ssid.length() > 32) return "The WiFi network name is too long (max. 32 bytes).";
  if(wifiPassword.length() && (wifiPassword.length() < 8 || wifiPassword.length() > 63)) return "The WiFi password must have 8 to 63 characters.";
  if(!isTwitchValue(clientId) || !isTwitchValue(clientSecret) || !isTwitchValue(token)) return "Twitch client ID, secret and token may only contain letters and digits.";
  if(webPassword != webPassword2) return "The two web passwords do not match.";
  if(webPassword.length() && webPassword.length() < 8) return "The web password must have at least 8 characters.";
  if(!webPasswordSet && webPassword.isEmpty()) return "Choose a password for the web interface.";
  return nullptr;
}

String escapeString(const String& s){
  return htmlEscape(std::string(s.c_str()));
}

// After a failed save show what was entered, otherwise the stored value
String inputValue(const char* name, const String& current){
  return escapeString(server.hasArg(name) ? server.arg(name) : current);
}

// Password field that never shows the stored value
String secretField(const char* label, const char* name, bool isSet, const char* hint, bool clearable){
  String f = String("<label>") + label + "<input type=\"password\" name=\"" + name + "\" autocomplete=\"new-password\" placeholder=\""
           + (isSet ? "stored - leave empty to keep" : "not set") + "\"></label>";
  if(hint) f += String("<p class=\"hint\">") + hint + "</p>";
  if(isSet && clearable) f += String("<label class=\"check\"><input type=\"checkbox\" name=\"") + name + "_clear\">Remove stored value</label>";
  return f;
}

void sendSettingsPage(const String& message, bool isError){
  String b;
  b.reserve(4096);
  if(setupMode){
    b += "<div class=\"card\"><h2>Setup mode</h2><p>" + escapeString(setupModeReason) + "</p>"
         "<p class=\"muted\">Enter your WiFi network and a password for the web interface. "
         "The display restarts and connects to your WiFi.</p></div>";
  }
  b += "<div class=\"card\"><h2>Settings</h2>";
  if(message.length()) b += String("<p class=\"") + (isError ? "err" : "ok") + "\">" + escapeString(message) + "</p>";
  b += "<form method=\"post\" action=\"/settings\">";

  b += "<h3>WiFi</h3>";
  b += "<label>Network<input type=\"text\" name=\"ssid\" list=\"nets\" maxlength=\"32\" required value=\"" + inputValue("ssid", config.wifiSsid) + "\"></label>";
  if(scannedNetworks.size()){
    b += "<datalist id=\"nets\">";
    for(const String& n : scannedNetworks) b += "<option value=\"" + escapeString(n) + "\">";
    b += "</datalist>";
  }
  b += secretField("Password", "wifipw", config.wifiPassword.length() > 0, nullptr, true);

  b += "<h3>Twitch</h3>";
  b += "<p class=\"hint\">Create an application at <a href=\"https://dev.twitch.tv/console\" target=\"_blank\">dev.twitch.tv/console</a> "
       "and enter its client ID and secret. The display then gets and renews its access token itself.</p>";
  b += "<label>Client ID<input type=\"text\" name=\"clientid\" maxlength=\"128\" autocomplete=\"off\" value=\"" + inputValue("clientid", config.clientId) + "\"></label>";
  b += secretField("Client secret", "secret", config.clientSecret.length() > 0, nullptr, true);
  b += secretField("Access token (optional)", "token", config.token.length() > 0, "Only needed without a client secret. It expires and is not renewed.", true);

  b += "<h3>Web interface</h3>";
  b += "<p class=\"hint\">Login user is <b>admin</b>. The password is also used for uploads from PlatformIO.</p>";
  b += secretField("New password", "webpw", config.webPassword.length() > 0, nullptr, false);
  b += "<label>Repeat new password<input type=\"password\" name=\"webpw2\" autocomplete=\"new-password\"></label>";

  b += "<p><button>Save and restart</button></p></form></div>";

  b += "<div class=\"card\"><h2>Reset</h2><p class=\"muted\">Removes all settings stored on the display. "
       "Values built into the firmware (include/secrets.h) are used again; without them the display starts in setup mode.</p>"
       "<form method=\"post\" action=\"/reset\" onsubmit=\"return confirm('Remove all settings?')\">"
       "<p><button class=\"danger\">Reset settings</button></p></form></div>";
  sendPage("Settings", b);
}

void handleSettingsPage(){
  if(!setupMode && !checkAuth()) return;
  sendSettingsPage("", false);
}

void handleSettingsSave(){
  if(!setupMode && !checkAuth()) return;
  String ssid = server.arg("ssid");           ssid.trim();
  String wifiPassword = server.arg("wifipw");
  String clientId = server.arg("clientid");   clientId.trim();
  String clientSecret = server.arg("secret"); clientSecret.trim();
  String token = server.arg("token");         token.trim();
  if(token.startsWith("oauth:")) token = token.substring(6);
  String webPassword = server.arg("webpw");
  String webPassword2 = server.arg("webpw2");

  const char* err = validateSettings(ssid, wifiPassword, clientId, clientSecret, token, webPassword, webPassword2, config.webPassword.length() > 0);
  if(err){
    sendSettingsPage(err, true);
    return;
  }

  // empty secret fields keep the stored value, "Remove" stores an empty one
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putString("ssid", ssid);
  if(wifiPassword.length()) prefs.putString("wifipw", wifiPassword);
  else if(server.hasArg("wifipw_clear")) prefs.putString("wifipw", "");
  prefs.putString("clientid", clientId);
  if(clientSecret.length()) prefs.putString("secret", clientSecret);
  else if(server.hasArg("secret_clear")) prefs.putString("secret", "");
  if(token.length()) prefs.putString("token", token);
  else if(server.hasArg("token_clear")) prefs.putString("token", "");
  if(webPassword.length()) prefs.putString("webpw", webPassword);
  prefs.end();
  DEBUG_I.printf("[%s] Settings saved, restarting.\n", DEBUG_TAG);
  // a new firmware that got this far works; the restart must not roll it back
  confirmFirmwareNow();

  String b = "<div class=\"card\"><h2>Saved</h2><p class=\"ok\">The settings were saved. The display restarts now.</p>";
  if(setupMode) b += "<p>Connect this device back to your normal WiFi and open <a href=\"http://" HOSTNAME ".fritz.box/\">http://" HOSTNAME ".fritz.box/</a> "
                     "or <a href=\"http://" HOSTNAME ".local/\">http://" HOSTNAME ".local/</a> in about 30 seconds.</p>";
  else b += "<p>Reload the <a href=\"/\">status page</a> in about 30 seconds.</p>";
  b += "</div>";
  sendPage("Saved", b);
  drawMessageScreen("Settings saved", ST77XX_GREEN, "Restarting...");
  rebootAt = millis() + 1500;
}

void handleReset(){
  if(!setupMode && !checkAuth()) return;
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.clear();
  prefs.end();
  DEBUG_I.printf("[%s] Settings reset, restarting.\n", DEBUG_TAG);
  confirmFirmwareNow();
  sendPage("Reset", "<div class=\"card\"><h2>Reset</h2><p class=\"ok\">All settings were removed. The display restarts now.</p></div>");
  drawMessageScreen("Settings reset", ST77XX_YELLOW, "Restarting...");
  rebootAt = millis() + 1500;
}

// ---------- WiFi connection and setup mode

// Connects to the configured WiFi; false if it is not reachable within WIFI_CONNECT_TIMEOUT
bool connectWiFi(){
  DEBUG_I.printf("[%s] Connecting to WIFI %s...\n", DEBUG_TAG, config.wifiSsid.c_str());
  drawMessageScreen("Connecting...", ST77XX_CYAN, config.wifiSsid.c_str());
  WiFi.setHostname(HOSTNAME); // name in the router (twitchdisplay.fritz.box)
  WiFi.begin(config.wifiSsid.c_str(), config.wifiPassword.c_str());
  unsigned long start = millis();
  while(WiFi.status() != WL_CONNECTED){
    if(millis() - start >= WIFI_CONNECT_TIMEOUT){
      DEBUG_W.printf("[%s] WIFI not reachable.\n", DEBUG_TAG);
      return false;
    }
    delay(100);
  }
  DEBUG_I.printf("[%s] WIFI connected.\n", DEBUG_TAG);
  return true;
}

String randomPassword(int len){
  const char* chars = "abcdefghjkmnpqrstuvwxyz23456789"; // no 0/o, 1/l/i
  String pw;
  for(int i = 0; i < len; i++) pw += chars[esp_random() % strlen(chars)];
  return pw;
}

void drawSetupScreen(){
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextWrap(false);
  tft.setTextSize(3);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(10, 8);
  tft.print("Setup mode");
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(10, 38);
  tft.print(setupModeReason);
  tft.setTextSize(2);
  tft.setCursor(10, 58);
  tft.print("1. Connect to WiFi");
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(34, 80);
  tft.print(SETUP_AP_SSID);
  tft.setCursor(34, 102);
  tft.print(("Password: " + setupApPassword).c_str());
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(10, 130);
  tft.print(("2. Open " + WiFi.softAPIP().toString()).c_str());
}

// Own WiFi hotspot with the Settings page; DNS answers every name with the
// display's address so phones open the page by themselves (captive portal)
void startSetupMode(const char* reason, bool retryLater){
  setupMode = true;
  setupModeReason = reason;
  setupRetryLater = retryLater;
  DEBUG_I.printf("[%s] Starting setup mode: %s\n", DEBUG_TAG, reason);
  drawMessageScreen("Setup mode", ST77XX_CYAN, "Starting...");

  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);
  // networks for the list on the Settings page
  int n = WiFi.scanNetworks();
  for(int i = 0; i < n && scannedNetworks.size() < 20; i++){
    String ssid = WiFi.SSID(i);
    if(ssid.length() && std::find(scannedNetworks.begin(), scannedNetworks.end(), ssid) == scannedNetworks.end()){
      scannedNetworks.push_back(ssid);
    }
  }
  WiFi.scanDelete();

  setupApPassword = randomPassword(8);
  WiFi.softAP(SETUP_AP_SSID, setupApPassword.c_str());
  dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/settings", HTTP_GET, handleSettingsPage);
  server.on("/settings", HTTP_POST, handleSettingsSave);
  server.on("/reset", HTTP_POST, handleReset);
  // firmware upload also works here, so a firmware that can't connect to the
  // WiFi can still be replaced through the hotspot
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUploadDone, handleUploadChunk);
  server.onNotFound([](){
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/settings", true);
    server.send(302, "text/plain", "");
  });
  server.begin();

  drawSetupScreen();
  setupModeSince = millis();

  esp_task_wdt_config_t wdtConfig = { .timeout_ms = LOOP_WDT_TIMEOUT, .idle_core_mask = 0, .trigger_panic = true };
  esp_task_wdt_reconfigure(&wdtConfig);
  enableLoopWDT();
}

void setupModeLoop(){
  dnsServer.processNextRequest();
  server.handleClient();
  if(rebootAt && millis() >= rebootAt){
    delay(100);
    ESP.restart();
  }
  if(restoreDisplayAt && millis() >= restoreDisplayAt){
    // failed firmware upload: back to the setup instructions
    restoreDisplayAt = 0;
    drawSetupScreen();
  }
  // running stable with the hotspot counts as working: a firmware update that
  // ends up in setup mode (e.g. new settings needed) must not be rolled back
  confirmFirmwareWhenStable(true);
  // WiFi was only unreachable (e.g. router still starting): try again later,
  // unless someone is using the hotspot
  if(setupRetryLater && !rebootAt && millis() - setupModeSince >= SETUP_RETRY_TIMEOUT){
    if(WiFi.softAPgetStationNum() == 0){
      DEBUG_I.printf("[%s] Setup mode unused, restarting to try WIFI again.\n", DEBUG_TAG);
      ESP.restart();
    }
    setupModeSince = millis() - SETUP_RETRY_TIMEOUT + 60000; // check again in a minute
  }
  delay(10);
}

// Holding BOOT for BOOT_HOLD_TIME restarts into setup mode
void checkBootButton(){
  static unsigned long pressedSince = 0;
  if(digitalRead(SETUP_BUTTON_PIN) != LOW){
    pressedSince = 0;
    return;
  }
  if(!pressedSince){
    pressedSince = millis();
    if(!pressedSince) pressedSince = 1;
  } else if(millis() - pressedSince >= BOOT_HOLD_TIME){
    DEBUG_I.printf("[%s] BOOT button held, restarting into setup mode.\n", DEBUG_TAG);
    drawMessageScreen("Setup mode", ST77XX_CYAN, "Restarting...");
    setupRequested = SETUP_REQUEST_MAGIC;
    delay(500);
    ESP.restart();
  }
}


// ---------- Online update from GitHub releases

struct ReleaseInfo {
  String tag;
  String published;
  String pageUrl;
  String assetUrl;
  size_t assetSize = 0;
  String error;       // empty if the release has a firmware to install
};

String httpError(HTTPClient& http, int code){
  return code <= 0 ? http.errorToString(code) : String(code);
}

// Asks GitHub for the newest release and its firmware.bin
ReleaseInfo checkLatestRelease(){
  ReleaseInfo release;
  WiFiClientSecure client;
  client.useBuiltinCACertBundle();
  HTTPClient http;
  http.useHTTP10(true);
  http.begin(client, "https://api.github.com/repos/" GITHUB_REPO "/releases/latest");
  http.setUserAgent("twitchDisplay/" FW_VERSION); // required by the GitHub API
  http.addHeader("Accept", "application/vnd.github+json");
  int code = http.GET();
  if(code == 404){
    http.end();
    release.error = "No release has been published yet.";
    return release;
  }
  if(code != HTTP_CODE_OK){
    release.error = "GitHub request failed (" + httpError(http, code) + ").";
    http.end();
    return release;
  }
  JsonDocument filter;
  filter["tag_name"] = true;
  filter["published_at"] = true;
  filter["html_url"] = true;
  JsonObject asset = filter["assets"].add<JsonObject>();
  asset["name"] = true;
  asset["size"] = true;
  asset["browser_download_url"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if(err || !doc["tag_name"].is<const char*>()){
    release.error = "Unexpected answer from GitHub.";
    return release;
  }
  release.tag = doc["tag_name"].as<const char*>();
  release.published = doc["published_at"] | "";
  release.pageUrl = doc["html_url"] | "";
  for(JsonObject a : doc["assets"].as<JsonArray>()){
    if(strcmp(a["name"] | "", FIRMWARE_ASSET) == 0){
      release.assetUrl = a["browser_download_url"] | "";
      release.assetSize = a["size"] | 0;
    }
  }
  if(release.assetUrl.isEmpty()) release.error = "Release " + release.tag + " has no " FIRMWARE_ASSET ".";
  else if(release.assetSize > maxFirmwareSize()) release.error = "The firmware of release " + release.tag + " is too large for this display.";
  return release;
}

// Downloads a firmware and writes it to the update partition. The first block
// is checked like a browser upload before anything is written.
// Returns an empty string on success, otherwise the error.
String downloadAndInstall(const String& url){
  WiFiClientSecure client;
  client.useBuiltinCACertBundle();
  HTTPClient http;
  http.begin(client, url);
  http.setUserAgent("twitchDisplay/" FW_VERSION);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // GitHub redirects to its file server
  http.setTimeout(DOWNLOAD_TIMEOUT);
  int code = http.GET();
  feedLoopWDT();
  if(code != HTTP_CODE_OK){
    String e = "Download failed (" + httpError(http, code) + ").";
    http.end();
    return e;
  }
  int size = http.getSize();
  if(size <= 0){ http.end(); return "Download failed: unknown file size."; }
  if((size_t)size > maxFirmwareSize()){ http.end(); return "The firmware is too large for this display."; }

  auto* stream = http.getStreamPtr();
  uint8_t buf[1024];
  size_t total = size, written = 0;
  bool started = false;
  int lastPercent = -1;
  unsigned long lastData = millis();
  String error;
  while(written < total){
    feedLoopWDT();
    size_t avail = stream->available();
    size_t remaining = total - written;
    // wait for data; the first read needs the whole image header for the check
    if(avail == 0 || (!started && avail < 64 && remaining >= 64)){
      if(millis() - lastData > DOWNLOAD_TIMEOUT){ error = "Download timed out."; break; }
      delay(1);
      continue;
    }
    size_t n = stream->readBytes(buf, std::min(std::min(avail, sizeof(buf)), remaining));
    if(n == 0) continue;
    lastData = millis();
    if(!started){
      const char* e = validateFirmwareHeader(buf, n);
      if(e){ error = e; break; }
      if(!Update.begin(total, U_FLASH)){ error = Update.errorString(); break; }
      started = true;
    }
    if(Update.write(buf, n) != n){ error = Update.errorString(); break; }
    written += n;
    int percent = (int)((uint64_t)written * 100 / total);
    if(percent != lastPercent){
      lastPercent = percent;
      drawUpdateProgress(percent);
    }
  }
  http.end();
  if(error.isEmpty() && written != total) error = "Download incomplete.";
  if(error.length()){
    if(Update.isRunning()) Update.abort();
    return error;
  }
  // end() verifies the checksum and the image before switching the boot partition
  if(!Update.end(true)) return Update.errorString();
  return "";
}

// Runs a requested online update (called from loop())
void runInstall(){
  DEBUG_I.printf("[OTA] Installing %s from %s\n", installVersion.c_str(), installUrl.c_str());
  otaActive = true; // pauses the network task
  restoreDisplayAt = 0;
  String title = "Downloading " + installVersion;
  drawMessageScreen("Firmware update", ST77XX_CYAN, title.c_str());
  drawUpdateProgress(0);
  String error = downloadAndInstall(installUrl);
  if(error.length()){
    DEBUG_W.printf("[OTA] Online update failed: %s\n", error.c_str());
    otaActive = false;
    installState = InstallState::Failed;
    installError = error;
    drawMessageScreen("Update failed", ST77XX_RED, error.c_str());
    restoreDisplayAt = millis() + 5000;
    return;
  }
  DEBUG_I.printf("[OTA] Online update installed, restarting.\n");
  drawMessageScreen("Update done", ST77XX_GREEN, "Restarting...");
  rebootAt = millis() + 1000;
}

void sendJsonDoc(int code, JsonDocument& doc){
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

void handleUpdateCheck(){
  if(!checkAuth()) return;
  ReleaseInfo release = checkLatestRelease();
  JsonDocument doc;
  doc["ok"] = release.error.isEmpty();
  doc["current"] = FW_VERSION;
  if(release.error.length()) doc["message"] = release.error;
  if(release.tag.length()){
    doc["latest"] = release.tag;
    doc["published"] = release.published;
    doc["notes"] = release.pageUrl;
    doc["upToDate"] = release.tag == FW_VERSION;
  }
  sendJsonDoc(200, doc);
}

void handleUpdateInstall(){
  if(!checkAuth()) return;
  // ask GitHub again, so only the current release's firmware is installed
  ReleaseInfo release = checkLatestRelease();
  JsonDocument doc;
  if(release.error.length()){
    doc["ok"] = false;
    doc["message"] = release.error;
    sendJsonDoc(200, doc);
    return;
  }
  installUrl = release.assetUrl;
  installVersion = release.tag;
  installState = InstallState::Idle;
  installRequested = true;
  doc["ok"] = true;
  doc["message"] = "Installing " + release.tag + ".";
  sendJsonDoc(200, doc);
}

void handleUpdateState(){
  if(!checkAuth()) return;
  JsonDocument doc;
  doc["state"] = installRequested ? "installing" : installState == InstallState::Failed ? "failed" : "idle";
  doc["version"] = FW_VERSION;
  if(installState == InstallState::Failed) doc["message"] = installError;
  sendJsonDoc(200, doc);
}
