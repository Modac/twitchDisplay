// TODO: full unicode support (e.g. with https://github.com/takkaO/OpenFontRender/tree/master and Noto Sans + Noto Emoji, but together at least 1,5MB).
//       For now titles are converted to the CP437 charset of the built-in font (umlauts and other Latin-1 letters work, emoji are dropped).

#include <Arduino.h>
#include <Adafruit_GFX.h>    // Core graphics library
#include <Adafruit_ST7789.h> // Hardware-specific library for ST7789
#include <SPI.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
// Twitch and GitHub answers need neither 64-bit integers nor double
// precision; leaving them out makes the firmware smaller
#define ARDUINOJSON_USE_LONG_LONG 0
#define ARDUINOJSON_USE_DOUBLE    0
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <esp_rom_crc.h>
#include <spi_fast.h>
#include <esp_partition.h>
#include <TitleFont.h>

#include <deque>
#include <set>
#include <map>
#include <functional>
#include <algorithm>
#include <string>
#include <vector>

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

// Title scrolling: one frame per panel refresh, position from the elapsed
// time, so the speed stays even when a frame is late (the next one catches up)
#define TITLE_EM            54.0f  // Noto Sans size: accents and descenders fit the 64 px row
#define TITLE_BASELINE      51     // baseline within the row
#define TITLE_LEAD          192    // empty space before the text when a title starts
#define TITLE_GAP           96     // space between the end of the text and its repetition
#define TITLE_MARGIN        96     // canvas columns right of the visible title, where new characters are drawn
#define TITLE_SHIFT         8      // move the canvas content left once this much has scrolled out
#define BUILTIN_ADVANCE     (6*8)  // built-in font (no font.bin): text size 8, 6 px per character * 8
#define TITLE_COLOR         ST77XX_CYAN
#define TITLE_RENDER_STACK  6144   // bytes, stb_truetype rasterizer
#define GLYPH_MIN_FREE_BLOCK 12288 // render a character only with this much RAM in one block, else a box
// Panel refresh (ST7789V data sheet, FRCTRL2 C6h): 10 MHz / ((320 + FPA + BPA) * (250 + 16 * RTNA))
// with the default porches FPA = BPA = 0x0C (B2h, not changed by the library).
// RTNA 0x04 = 92.6 Hz, i.e. one refresh every 10801 us. A title frame takes
// ~6 ms (at most ~9.3 ms measured) over SPI, so every refresh gets its frame
// with some margin; with less margin (97.6 Hz, 0x03) a slow frame would miss
// a refresh and the title would jump by two steps. The module has no TE pin
// and does not answer reads, so frames cannot be synchronised exactly.
#define PANEL_FRAME_RATE    0x04
#define FRAME_INTERVAL_US   ((320 + 0x0C + 0x0C) * (250 + 16 * PANEL_FRAME_RATE) / 10)
#define SCROLL_STEP         3      // px per frame (= 278 px/s)

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
// Draw the scrolling title column by column (1) instead of row by row (0), see Display::drawColumnwise()
#define TITLE_COLUMNWISE 1

// ST7789 with a column-wise bitmap transfer for the scrolling title.
//
// The 170x320 panel refreshes its native lines, which are columns in
// landscape (rotation 1). Writing the title row by row crosses every refresh
// line, so moving text tears diagonally. drawColumnwise() switches the
// address order for the transfer (MADCTL 0 instead of MY|MV: no row/column
// exchange, no mirroring), so the pixels arrive column by column, in the
// panel's line order and refresh direction (MADCTL ML = 0: the panel refreshes
// memory rows top to bottom, data sheet 9.1.28).
//
// Mapping (rotation 1, MADCTL MY|MV, offsets _xstart/_ystart): landscape (x, y)
// is memory column X = y + _ystart and memory row Y = 319 - (x + _xstart).
// With MADCTL 0 the column address (CASET) runs along X and the row address
// (RASET) along Y, so CASET = y + _ystart, RASET = 319 - (x + _xstart) for the
// rightmost column up to the leftmost one: each landscape column is one memory
// row, sent from right to left = ascending memory rows.
class Display : public Adafruit_ST7789 {
public:
  using Adafruit_ST7789::Adafruit_ST7789;

  // Draws a w x h section of bitmap (row-major, bitmapWidth wide, starting at
  // column offsetX) at (x, y)
  void drawColumnwise(int16_t x, int16_t y, const uint16_t* bitmap, int16_t offsetX, int16_t w, int16_t h, int16_t bitmapWidth){
    startWrite();
    if(!TITLE_COLUMNWISE || rotation != 1 || h > PIC_SIZE){
      // row by row (the display's normal order)
      setAddrWindow(x, y, w, h);
      for(int16_t j = 0; j < h; j++) writePixels((uint16_t*)&bitmap[j * bitmapWidth + offsetX], w);
      endWrite();
      return;
    }
    writeCommand(ST77XX_MADCTL);
    spiWrite(ST77XX_MADCTL_RGB);
    writeCommand(ST77XX_CASET);
    SPI_WRITE32(((uint32_t)(y + _ystart) << 16) | (uint32_t)(y + _ystart + h - 1));
    writeCommand(ST77XX_RASET);
    SPI_WRITE32(((uint32_t)(319 - (x + _xstart + w - 1)) << 16) | (uint32_t)(319 - (x + _xstart)));
    writeCommand(ST77XX_RAMWR);
    // all columns as one stream in pieces of 32 pixels (the SPI's 64 byte
    // buffer); the next piece is gathered while the previous one is sent
    uint32_t piece[16];
    uint32_t left = (uint32_t)w * h, n = 0;
    int16_t i = w - 1, j = 0;
    const uint16_t* col = bitmap + offsetX + i;
    while(left){
      uint16_t p = col[j * bitmapWidth];
      p = (uint16_t)(p >> 8 | p << 8);                // high byte first on the wire
      if(n & 1) piece[n >> 1] |= (uint32_t)p << 16; else piece[n >> 1] = p;
      n++; left--;
      if(++j == h){ j = 0; col--; }
      if(n == 32 || !left){ spiFastSend(piece, n * 2); n = 0; }
    }
    spiFastWait();
    // back to the landscape order of rotation 1
    writeCommand(ST77XX_MADCTL);
    spiWrite(ST77XX_MADCTL_MY | ST77XX_MADCTL_MV | ST77XX_MADCTL_RGB);
    endWrite();
  }
};

Display tft(TFT_CS, TFT_DC, TFT_RST);

// Settings, loaded once at boot; saving them restarts the display
struct Config {
  String wifiSsid;
  String wifiPassword;
  String clientId;
  String clientSecret;
  String token;
  String webPassword;
  String channelList;  // comma separated logins (or numeric ids), empty = default channels
  uint16_t titleRepeatMinutes;  // show a live channel's title again after this many minutes without a title, 0 = off
  bool requireLogin;   // web interface needs the password (off: anyone on the network may use it)
  bool colourEmoji;    // emoji in titles: colour images downloaded from Twemoji (false: monochrome only)
  bool cjk;            // Chinese/Japanese/Korean characters: rendered from downloaded font pieces (false: boxes)

  const char* effectiveClientId() const { return clientId.length() ? clientId.c_str() : DEFAULT_CLIENT_ID; }
  bool hasTwitchCredentials() const { return clientSecret.length() || token.length(); }
} config;

#define DEFAULT_TITLE_REPEAT_MIN 5
#define MAX_TITLE_REPEAT_MIN     1440

void loadConfig(){
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true); // fails harmlessly if nothing was saved yet: defaults are returned
  config.wifiSsid     = prefs.getString("ssid", WIFI_SSID);
  config.wifiPassword = prefs.getString("wifipw", WIFI_PW);
  config.clientId     = prefs.getString("clientid", TWITCH_CLIENT_ID);
  config.clientSecret = prefs.getString("secret", TWITCH_CLIENT_SECRET);
  config.token        = prefs.getString("token", TWITCH_TOKEN);
  config.webPassword  = prefs.getString("webpw", WEB_PASSWORD);
  config.channelList  = prefs.getString("channels", "");
  config.titleRepeatMinutes = (uint16_t)prefs.getString("repeatmin", String(DEFAULT_TITLE_REPEAT_MIN)).toInt();
  config.requireLogin = prefs.getString("login", "1") != "0";
  config.colourEmoji = prefs.getString("emoji", "colour") != "mono";
  config.cjk = prefs.getString("cjk", "on") != "off";
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
void openFont();
extern SemaphoreHandle_t titleLock;
extern SemaphoreHandle_t emojiLock;
String downloadToMemory(const std::string& url, std::vector<uint8_t>& data, size_t maxLen);
void titleRenderTask(void*);
std::string toDisplayText(const char* utf8);
void assignLogoSlots();
void buildChannels();
void checkLogoPartition();
void networkTask(void*);

struct channelInfo {
  std::string id;           // Twitch user id, empty until the name is resolved
  bool isLive = false;
  std::string streamTitle;  // "<title> | <game>" (UTF-8), the scrolling title
  int8_t slotNum = -1;
  std::string name;         // display name (UTF-8)
  std::string webTitle;     // "<title> | <game>" (UTF-8) for the web interface
  int8_t logoSlot = -1;     // slot of the downloaded logo in flash, -1 = none
  std::string login;        // Twitch login (channel name in the URL)
  std::string logoStatus;   // for the status page: "stored", "downloading", error text, ...
  uint32_t titleShownSeq = 0; // when its title was shown last (titleShowCount then), 0 = never
};

#define MAX_CHANNELS 32

// Channels until a list is saved on the Settings page (Twitch user ids):
// lidi, pietsmiet, bonjwa, bonjwachill, gronkh, dhalucard, trilluxe, dracon, maxim, finanzfluss
static const char* const DEFAULT_CHANNEL_IDS[] = {
  "761017145", "21991090", "73437396", "1024088182", "12875057",
  "16064695", "55898523", "38770961", "172376071", "549536744"
};

// Built once at boot from the settings and never resized afterwards
// (titleChangeQueue keeps pointers into it)
std::vector<channelInfo> channels;

// A configured channel as the network task sees it
struct ChannelRef {
  std::string id;        // empty until resolved
  std::string login;     // empty if configured by id and not resolved yet
  std::string name;      // display name
  std::string avatarUrl; // profile_image_url from Twitch
  bool found = false;    // Twitch knows this channel (set by fetchUsers())
};

#define MAX_LOGO_SLOTS  32   // one per channel
#define LOGO_SLOT_BYTES (LOGO_SIZE*LOGO_SIZE*2)
int logoSlotCount();       // as many as the logo partition holds (16 with the old partition layout)
#define LOGO_NAMESPACE  "logos"

struct LogoSlot {
  std::string channelId; // empty: free
  uint32_t urlHash = 0;  // CRC32 of the avatar URL the logo was made from
};

bool readLogo(int slot, uint16_t* pixels);
bool writeLogo(int slot, const uint16_t* pixels);
void loadLogoIndex(std::array<LogoSlot, MAX_LOGO_SLOTS>& index);
void saveLogoIndexEntry(int slot, const LogoSlot& entry);

std::vector<ChannelRef> netChannels;      // network task's copy of the channel list
std::array<LogoSlot, MAX_LOGO_SLOTS> netLogoIndex; // network task's copy of the logo index
std::vector<std::string> unknownChannels; // loop(): configured names Twitch does not know

uint16_t live_num = 0;

std::deque<channelInfo*> titleChangeQueue;

enum class FetchResult { Ok, Unauthorized, Failed, NoCredentials };

struct StreamInfo {
  std::string id;
  std::string name;
  std::string title;  // "<title> | <game>" (UTF-8)
  std::string webTitle; // "<title> | <game>" in UTF-8
};

// Result of one Twitch update, handed from the network task to loop()
struct LiveUpdate {
  FetchResult result;
  std::vector<StreamInfo> streams;
};

#define LOGO_SLOT_UNCHANGED -2

// Message from the network task to loop() (through updateQueue)
struct NetMessage {
  enum class Type { Live, Channels, Logo } type = Type::Live;
  LiveUpdate live;                  // Live: result of a stream update
  std::vector<ChannelRef> channels; // Channels: resolved list, same order as configured
  std::vector<std::string> unknown; // Channels: configured names Twitch does not know
  std::string channelId;            // Logo: channel the logo belongs to
  int logoSlot = LOGO_SLOT_UNCHANGED; // Logo: new slot, -1 = none (being rewritten)
  std::string logoStatus;           // Logo: status text for the status page
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
// The upload page sends the file in pieces (one request each, ?offset=&total=)
// so its progress bar only advances with what the display has written; a
// request without offset carries the whole file.
#define UPLOAD_PIECE_TIMEOUT 30000  // ms without the next piece: upload abandoned
struct {
  bool authorized;
  bool headerChecked;
  String error;
  int lastPercent;
  bool pieces;                 // current request is one piece of a file
  size_t offset;               // where the current piece starts
  size_t total;                // file size (pieces only)
  size_t written;              // bytes written so far (pieces only)
  unsigned long lastPieceAt;   // millis() of the last piece, 0 = no upload in pieces running
} upload;
unsigned long rebootAt = 0;        // millis() of a scheduled restart, 0 = none
void checkUploadTimeout();

// true if the request may use the web interface: the login is switched off
// on the Settings page, or the request carries the right password
bool loginOk(){
  return !config.requireLogin || server.authenticate(WEB_USER, config.webPassword.c_str());
}

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
// The visible title plus a margin on the right, where the render task draws
// the next characters before they scroll into view
#define CANVAS_W (TITLE_WIDTH + TITLE_MARGIN)
GFXcanvas16 text_canvas(CANVAS_W, PIC_SIZE);
GFXcanvas1 builtinGlyph(BUILTIN_ADVANCE, PIC_SIZE);  // one character of the built-in font (render task)

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
  {
    uint8_t frameRate = PANEL_FRAME_RATE; // FRCTRL2
    tft.sendCommand(0xC6, &frameRate, 1);
  }
  
  tft.fillScreen(ST77XX_BLACK);

  builtinGlyph.setTextWrap(false);
  builtinGlyph.cp437(true); // without font.bin titles are converted to CP437, see toDisplayText()

  DEBUG_I.printf("[%s] Display initialized.\n", DEBUG_TAG);
  titleLock = xSemaphoreCreateMutex();
  emojiLock = xSemaphoreCreateMutex();
  openFont();

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
  buildChannels();
  checkLogoPartition();
  assignLogoSlots();

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
  updateQueue = xQueueCreate(8, sizeof(NetMessage*));
  loadLogoIndex(netLogoIndex);
  // titles: characters are rendered ahead in their own task, below loop() like the network task
  xTaskCreate(titleRenderTask, "render", TITLE_RENDER_STACK, nullptr, NET_TASK_PRIORITY, nullptr);
  xTaskCreate(networkTask, "network", NET_TASK_STACK, nullptr, NET_TASK_PRIORITY, &netTask);

  DEBUG_I.printf("[%s] Setup completed...\n", DEBUG_TAG);
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

#define MAX_TITLE_REPEAT 2   // the text scrolls by this many times

struct {
  channelInfo* channel;
  int8_t slot;              // slot of the channel when the title was (re)drawn
} channelTitleInfo;

volatile bool isTitleDisplaying = false;

// The title being shown, laid out by loop() when it starts. The render task
// reads it (under titleLock) while isTitleDisplaying.
struct TitleText {
  std::vector<uint32_t> chars;     // code points (font.bin) or CP437 codes (built-in font)
  std::vector<uint16_t> advance;   // pixels
  std::vector<uint32_t> cacheKey;  // cached colour emoji or CJK character to draw (0: draw chars[i] from the fonts)
  uint32_t width = 0;              // of the text, without the gap
  bool builtin = true;             // drawn with the built-in font
} titleText;

// Canvas state shared by loop() and the render task, under titleLock:
// text_canvas columns [o_X, o_X + TITLE_WIDTH) are visible, the next
// character goes to titleNextX.
SemaphoreHandle_t titleLock = nullptr;
uint32_t titleGeneration = 0;        // changes with every start/stop: stale renders are dropped
size_t titleNextItem = 0;            // next character (chars.size() = the gap)
int titleNextX = 0;
uint16_t o_X = 0;                    // first visible canvas column
uint32_t titleScrollTotal = 0;       // pixels until the title ends

unsigned long titleStartedUs = 0;    // micros() of the first frame (moved on while waiting for characters)
uint32_t titlePixels = 0;            // pixels scrolled since the start
unsigned long nextFrameUs = 0;       // micros() of the next frame

// statistics of the current / last title for the status page
uint32_t titleFrames = 0;
unsigned long titleLastFrameUs = 0, titleMaxGapUs = 0;
uint64_t titleDrawUsSum = 0, titleXferUsSum = 0; unsigned long titleDrawUsMax = 0;  // time per frame for rendering + transfer, transfer alone
uint32_t titleWaits = 0;             // frames that waited for the render task

// Fonts from the font partition (see openFont())
TitleFont titleFont;

// ---------- Font partition ("font", data subtype 0x40): font.bin from
// tools/make_fonts.py. Without it (old partition layout, or font.bin missing
// or damaged) titles use the built-in font.
#define FONT_PARTITION_SUBTYPE ((esp_partition_subtype_t)0x40)
#define FONT_ASSET             "font.bin"
#define FONT_RETRY_INTERVAL    (60UL * 60 * 1000)   // ms between download attempts after a failure
#ifndef FONT_BIN_CRC
#define FONT_BIN_CRC 0                              // set by tools/version.py
#endif

enum class FontState { NoPartition, Missing, Ready, Failed };
FontState fontState = FontState::NoPartition;
String fontProblem;                       // why font.bin is missing / the download failed
uint32_t fontCrc = 0;                     // CRC of the installed font.bin
const esp_partition_t* fontPartition = nullptr;
const void* fontMap = nullptr;
esp_partition_mmap_handle_t fontMapHandle;
bool fontDownloadWanted = false;          // loop() downloads font.bin when nothing else runs
unsigned long fontRetryAt = 0;            // millis() of the next attempt

void resetEmojiCache();
void initEmojiCache(size_t fontLen);

void closeFont(){
  resetEmojiCache();
  titleFont.end();
  if(fontMap){ esp_partition_munmap(fontMapHandle); fontMap = nullptr; }
}

// Maps the font partition and reads font.bin from it
void openFont(){
  closeFont();
  fontPartition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, FONT_PARTITION_SUBTYPE, "font");
  if(!fontPartition){ fontState = FontState::NoPartition; return; }
  FontBin bin;
  const char* err = "cannot read the font partition";
  if(esp_partition_mmap(fontPartition, 0, fontPartition->size, ESP_PARTITION_MMAP_DATA, &fontMap, &fontMapHandle) == ESP_OK){
    err = parseFontBin((const uint8_t*)fontMap, fontPartition->size, bin);
    if(!err && !titleFont.begin(bin)) err = "font.bin has an unreadable font";
  } else fontMap = nullptr;
  if(err){
    closeFont();
    fontState = FontState::Missing;
    fontProblem = err;
    fontCrc = 0;
    fontDownloadWanted = true;
    DEBUG_W.printf("[Font] %s, using the built-in font.\n", err);
    return;
  }
  fontState = FontState::Ready;
  fontCrc = bin.crc;
  fontProblem = "";
  initEmojiCache(bin.totalLen);
  DEBUG_I.printf("[Font] font.bin %08lx loaded.\n", (unsigned long)fontCrc);
  // an older/newer font.bin than this firmware's: fetch the right one once
  if(FONT_BIN_CRC && fontCrc != FONT_BIN_CRC) fontDownloadWanted = true;
}

// font.bin of this firmware's release; development builds take the newest release's
String fontUrl(){
  String v = FW_VERSION;
  bool release = v.startsWith("v") && v.indexOf('-') < 0 && v.length() > 1 && isdigit((unsigned char)v[1]);
  if(release) return "https://github.com/" GITHUB_REPO "/releases/download/" + v + "/" FONT_ASSET;
  return "https://github.com/" GITHUB_REPO "/releases/latest/download/" FONT_ASSET;
}

// ---------- Cache in the font partition after font.bin, filled by the
// network task before it hands a Twitch result to loop():
//  - colour emoji: Twemoji images (CC-BY 4.0) as 54x54 RGB565 (about 2.4 KB)
//  - CJK characters rendered from downloaded font pieces (about 1 KB)
// Entries are run-length coded and written one after another; one never
// crosses into the next 4 KB sector unless it is larger than a sector (then
// it starts at a sector). When the cache is full, the oldest sector is
// erased (with all its entries). The index is rebuilt from the sectors at boot.
#define EMOJI_SIZE           54
#define EMOJI_ADVANCE        60     // pixels in the title
#define EMOJI_TOP            (TITLE_BASELINE - 46)   // like the monochrome emoji
#define CACHE_MAGIC          0x32454D43UL            // "CME2"
#define CACHE_SECTOR         4096
#define CACHE_MAX_ENTRY      (2 * CACHE_SECTOR)
#define CACHE_COLOUR         1      // RGB565 runs: (count, colour lo, colour hi)
#define CACHE_GLYPH          2      // coverage runs: (count, alpha)
#define EMOJI_MAX_PER_UPDATE 8      // downloads before a Twitch result goes to loop(); the rest next time
#define EMOJI_MAX_DOWNLOAD   16384
#define TWEMOJI_URL          "https://cdn.jsdelivr.net/gh/jdecked/twemoji@17.0.3/assets/72x72/"

struct CacheEntryHeader {
  uint32_t magic, key, seq;  // key: CRC-32 of a name; seq: increases with every entry
  uint16_t len;              // bytes of run-length data after the header
  uint8_t type, w, h;
  int8_t xoff, yoff;         // glyphs: top-left pixel relative to the pen position on the baseline
  uint8_t advance;
  uint32_t crc;              // of the run-length data
};
static_assert(sizeof(CacheEntryHeader) == 24, "cache entry header");
struct CacheIndexEntry {
  uint32_t key, seq, offset;  // offset from the cache start
  uint16_t size;              // header + data, rounded up to 4 bytes
  uint8_t type, w, h, advance;
  int8_t xoff, yoff;
};

SemaphoreHandle_t emojiLock = nullptr;     // index and cache sectors (network task writes, loop/render read)
std::vector<CacheIndexEntry> cacheIndex;
size_t emojiCacheStart = 0;                // offset in the font partition
uint32_t cacheBytes = 0, cacheWriteOffset = 0;
uint32_t cacheSeq = 1;
bool emojiCacheReady = false;
String emojiError;                         // last download problem, for the status page
uint32_t emojiDownloads = 0;

uint32_t emojiKey(const std::string& name){
  return fontCrc32((const uint8_t*)name.data(), name.size());
}
uint32_t glyphKey(uint32_t cp){
  char name[16];
  snprintf(name, sizeof(name), "u+%lx", (unsigned long)cp);
  return emojiKey(name);
}

void resetEmojiCache(){
  if(emojiLock) xSemaphoreTake(emojiLock, portMAX_DELAY);
  emojiCacheReady = false;
  cacheIndex.clear();
  if(emojiLock) xSemaphoreGive(emojiLock);
}

// Reads the cache index from the sectors after font.bin (fontLen bytes)
void initEmojiCache(size_t fontLen){
  xSemaphoreTake(emojiLock, portMAX_DELAY);
  cacheIndex.clear();
  emojiCacheStart = (fontLen + CACHE_SECTOR - 1) / CACHE_SECTOR * CACHE_SECTOR;
  cacheBytes = fontPartition->size > emojiCacheStart ? (fontPartition->size - emojiCacheStart) / CACHE_SECTOR * CACHE_SECTOR : 0;
  const uint8_t* base = (const uint8_t*)fontMap + emojiCacheStart;
  const CacheIndexEntry* newest = nullptr;
  for(uint32_t off = 0; off + sizeof(CacheEntryHeader) <= cacheBytes;){
    CacheEntryHeader h;
    memcpy(&h, base + off, sizeof(h));
    uint32_t size = (sizeof(h) + h.len + 3) & ~3u;
    bool fits = size <= CACHE_MAX_ENTRY && off + size <= cacheBytes
             && (size > CACHE_SECTOR ? off % CACHE_SECTOR == 0 : off % CACHE_SECTOR + size <= CACHE_SECTOR);
    if(h.magic == CACHE_MAGIC && h.len && fits && (h.type == CACHE_COLOUR || h.type == CACHE_GLYPH)
       && fontCrc32(base + off + sizeof(h), h.len) == h.crc){
      cacheIndex.push_back({h.key, h.seq, off, (uint16_t)size, h.type, h.w, h.h, h.advance, h.xoff, h.yoff});
      off += size;
      if(size > CACHE_SECTOR) off = (off + CACHE_SECTOR - 1) / CACHE_SECTOR * CACHE_SECTOR;   // nothing follows a large entry in its sector
    } else off = (off / CACHE_SECTOR + 1) * CACHE_SECTOR;   // the rest of this sector is empty or stale
  }
  for(const auto& e : cacheIndex) if(!newest || e.seq > newest->seq) newest = &e;
  cacheSeq = newest ? newest->seq + 1 : 1;
  // continue in a fresh sector after the newest entry (the rest of its sector may hold a broken write)
  cacheWriteOffset = newest ? ((newest->offset + newest->size + CACHE_SECTOR - 1) / CACHE_SECTOR * CACHE_SECTOR) : 0;
  if(cacheWriteOffset >= cacheBytes) cacheWriteOffset = 0;
  emojiCacheReady = cacheBytes >= 4 * CACHE_SECTOR;
  xSemaphoreGive(emojiLock);
  DEBUG_I.printf("[Cache] %u entries in %lu KB.\n", (unsigned)cacheIndex.size(), (unsigned long)(cacheBytes / 1024));
}

const CacheIndexEntry* findCached(uint32_t key){   // under emojiLock
  const CacheIndexEntry* found = nullptr;
  for(const auto& e : cacheIndex) if(e.key == key && (!found || e.seq > found->seq)) found = &e;
  return found;
}

// Copies the index entry of a cached item (false: not cached)
bool cacheLookup(uint32_t key, CacheIndexEntry* out = nullptr){
  if(!emojiLock) return false;
  xSemaphoreTake(emojiLock, portMAX_DELAY);
  const CacheIndexEntry* e = emojiCacheReady ? findCached(key) : nullptr;
  if(e && out) *out = *e;
  xSemaphoreGive(emojiLock);
  return e != nullptr;
}
bool emojiCached(uint32_t key){ return cacheLookup(key); }

// Decodes a cached item: colour emoji into colour (EMOJI_SIZE x EMOJI_SIZE),
// glyphs into glyph. False if it is not (any more) in the cache.
bool readCached(uint32_t key, std::vector<uint16_t>* colour, GlyphBitmap* glyph){
  xSemaphoreTake(emojiLock, portMAX_DELAY);
  const CacheIndexEntry* e = emojiCacheReady ? findCached(key) : nullptr;
  bool ok = false;
  if(e){
    const uint8_t* p = (const uint8_t*)fontMap + emojiCacheStart + e->offset;
    CacheEntryHeader h;
    memcpy(&h, p, sizeof(h));
    p += sizeof(h);
    size_t out = 0, total = (size_t)h.w * h.h;
    if(h.type == CACHE_COLOUR && colour){
      colour->assign(total, 0);
      for(size_t i = 0; i + 3 <= h.len && out < total; i += 3){
        uint16_t c = p[i + 1] | (p[i + 2] << 8);
        for(int k = 0; k < p[i] && out < total; k++) (*colour)[out++] = c;
      }
      ok = out == total;
    } else if(h.type == CACHE_GLYPH && glyph){
      glyph->w = h.w; glyph->h = h.h; glyph->xoff = h.xoff; glyph->yoff = h.yoff;
      glyph->alpha.assign(total, 0);
      for(size_t i = 0; i + 2 <= h.len && out < total; i += 2)
        for(int k = 0; k < p[i] && out < total; k++) glyph->alpha[out++] = p[i + 1];
      ok = out == total;
    }
  }
  xSemaphoreGive(emojiLock);
  return ok;
}
bool readEmoji(uint32_t key, std::vector<uint16_t>& px){ return readCached(key, &px, nullptr); }

// Run-length coding of RGB565 pixels: (count 1..255, colour little endian)
std::vector<uint8_t> rleEncode(const uint16_t* px, size_t n){
  std::vector<uint8_t> out;
  for(size_t i = 0; i < n;){
    size_t run = 1;
    while(i + run < n && run < 255 && px[i + run] == px[i]) run++;
    out.push_back((uint8_t)run);
    out.push_back(px[i] & 0xFF);
    out.push_back(px[i] >> 8);
    i += run;
  }
  return out;
}

// Run-length coding of coverage: (count 1..255, alpha)
std::vector<uint8_t> rleEncodeAlpha(const uint8_t* a, size_t n){
  std::vector<uint8_t> out;
  for(size_t i = 0; i < n;){
    size_t run = 1;
    while(i + run < n && run < 255 && a[i + run] == a[i]) run++;
    out.push_back((uint8_t)run);
    out.push_back(a[i]);
    i += run;
  }
  return out;
}

// Appends an entry, erasing the next sector(s) (and the entries in them) first
bool storeCached(uint32_t key, uint8_t type, uint8_t w, uint8_t h, int8_t xoff, int8_t yoff, uint8_t advance, const std::vector<uint8_t>& data){
  uint32_t size = (sizeof(CacheEntryHeader) + data.size() + 3) & ~3u;
  if(data.empty() || data.size() > 0xFFFF || size > CACHE_MAX_ENTRY) return false;
  xSemaphoreTake(emojiLock, portMAX_DELAY);
  bool ok = false;
  if(emojiCacheReady){
    uint32_t off = cacheWriteOffset;
    if(size > CACHE_SECTOR ? off % CACHE_SECTOR != 0 : off % CACHE_SECTOR + size > CACHE_SECTOR)
      off = (off / CACHE_SECTOR + 1) * CACHE_SECTOR;   // to the next sector
    if(off + size > cacheBytes) off = 0;                // wrap around: the oldest sectors go
    ok = true;
    if(off % CACHE_SECTOR == 0){
      uint32_t erase = (size + CACHE_SECTOR - 1) / CACHE_SECTOR * CACHE_SECTOR;
      cacheIndex.erase(std::remove_if(cacheIndex.begin(), cacheIndex.end(), [&](const CacheIndexEntry& e){
        return e.offset < off + erase && off < e.offset + e.size; }), cacheIndex.end());
      ok = esp_partition_erase_range(fontPartition, emojiCacheStart + off, erase) == ESP_OK;
    }
    CacheEntryHeader hd{CACHE_MAGIC, key, cacheSeq, (uint16_t)data.size(), type, w, h, xoff, yoff, advance, fontCrc32(data.data(), data.size())};
    ok = ok && esp_partition_write(fontPartition, emojiCacheStart + off + sizeof(hd), data.data(), data.size()) == ESP_OK
            && esp_partition_write(fontPartition, emojiCacheStart + off, &hd, sizeof(hd)) == ESP_OK;
    if(ok){
      cacheIndex.push_back({key, cacheSeq++, off, (uint16_t)size, type, w, h, advance, xoff, yoff});
      cacheWriteOffset = off + size;
      // after an entry larger than a sector, continue in a fresh sector: otherwise replacing its
      // first sector would leave its tail in front of the next entries, and the scan at boot would miss them
      if(size > CACHE_SECTOR) cacheWriteOffset = (cacheWriteOffset + CACHE_SECTOR - 1) / CACHE_SECTOR * CACHE_SECTOR;
    }
  }
  xSemaphoreGive(emojiLock);
  return ok;
}

bool storeEmoji(uint32_t key, const std::vector<uint8_t>& rle){
  return storeCached(key, CACHE_COLOUR, EMOJI_SIZE, EMOJI_SIZE, 0, 0, EMOJI_ADVANCE, rle);
}

// Green frame around the pic of the channel whose title is scrolling
void drawTitleIndicator(){
  uint16_t x, y;
  slotXY(channelTitleInfo.slot, x, y);
  drawThickRect(x, y, PIC_SIZE, PIC_SIZE, -PIC_BORDER, ST77XX_GREEN);
}

uint32_t titleShowCount = 0;          // titles shown since boot
unsigned long lastTitleEndAt = 0;     // millis() when the last title ended

// true if a stream title has something to show (not just the " | " separator)
bool hasTitleText(const std::string& title){
  return title.find_first_not_of(" |") != std::string::npos;
}

static inline uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t a){
  if(a == 255) return fg;
  if(a == 0) return bg;
  uint32_t r = (((fg >> 11) & 31) * a + ((bg >> 11) & 31) * (255 - a) + 127) / 255;
  uint32_t g = (((fg >> 5) & 63) * a + ((bg >> 5) & 63) * (255 - a) + 127) / 255;
  uint32_t b = ((fg & 31) * a + (bg & 31) * (255 - a) + 127) / 255;
  return (uint16_t)(r << 11 | g << 5 | b);
}

// Draws a rendered character into a canvas with its pen position at (x, baseline)
void blitGlyph(GFXcanvas16& canvas, const GlyphBitmap& g, int x, int baseline, uint16_t color){
  uint16_t* buf = canvas.getBuffer();
  int cw = canvas.width(), ch = canvas.height();
  for(int j = 0; j < g.h; j++){
    int py = baseline + g.yoff + j;
    if(py < 0 || py >= ch) continue;
    const uint8_t* src = &g.alpha[(size_t)j * g.w];
    uint16_t* dst = &buf[py * cw];
    for(int i = 0; i < g.w; i++){
      int px = x + g.xoff + i;
      if(px >= 0 && px < cw && src[i]) dst[px] = blend565(dst[px], color, src[i]);
    }
  }
}

// Outline box for a character no font has
void drawGlyphBox(GFXcanvas16& canvas, int x, int advance, uint16_t color){
  int h = (int)(TITLE_EM * 0.7f);
  canvas.drawRect(x + 3, TITLE_BASELINE - h, advance - 6, h, color);
}

// A character of the built-in 5x7 font at text size 8 (48x64, top at row 0)
void renderBuiltinGlyph(uint8_t c, GlyphBitmap& g){
  builtinGlyph.fillScreen(0);
  builtinGlyph.drawChar(0, 0, c, 1, 0, 8);
  g.w = BUILTIN_ADVANCE; g.h = PIC_SIZE; g.xoff = 0; g.yoff = -TITLE_BASELINE;
  g.alpha.assign((size_t)g.w * g.h, 0);
  for(int y = 0; y < g.h; y++) for(int x = 0; x < g.w; x++) if(builtinGlyph.getPixel(x, y)) g.alpha[y * g.w + x] = 255;
}

// Lays out the title text: with font.bin as code points, else in CP437
void layoutTitle(const std::string& utf8){
  titleText = TitleText();
  if(titleFont.ready()){
    titleText.builtin = false;
    for(const TitleChar& c : titleChars(utf8.c_str())){
      uint32_t key = 0;
      if(!c.emoji.empty() && config.colourEmoji) key = emojiKey(c.emoji);
      else if(c.emoji.empty() && config.cjk && titleFont.sourceFor(c.cp) == TitleFont::NONE) key = glyphKey(c.cp);   // a downloaded CJK character?
      CacheIndexEntry cached;
      if(key && !cacheLookup(key, &cached)) key = 0;
      if(!key && !c.cp) continue;        // a flag without its colour image: nothing to draw
      titleText.chars.push_back(c.cp);
      titleText.cacheKey.push_back(key);
      titleText.advance.push_back((uint16_t)(key ? cached.advance : titleFont.advance(c.cp, TITLE_EM)));
    }
  } else {
    for(char c : toDisplayText(utf8.c_str())){
      titleText.chars.push_back((uint8_t)c);
      titleText.cacheKey.push_back(0);
      titleText.advance.push_back(BUILTIN_ADVANCE);
    }
  }
  for(uint16_t a : titleText.advance) titleText.width += a;
}

// Draws the next character into the canvas margin, if there is room. Runs in
// the render task (and in startTitle() for the first characters); rendering
// happens without the lock, so loop() keeps drawing frames meanwhile.
// Returns false when there is nothing to do.
bool renderAheadStep(){
  if(!isTitleDisplaying || !titleLock) return false;
  xSemaphoreTake(titleLock, portMAX_DELAY);
  uint32_t generation = titleGeneration;
  size_t count = titleText.chars.size(), item = titleNextItem;
  bool gap = item >= count;
  uint32_t c = gap ? 0 : titleText.chars[item];
  uint32_t key = gap ? 0 : titleText.cacheKey[item];
  bool builtin = titleText.builtin;
  int advance = gap ? TITLE_GAP : titleText.advance[item];
  // characters may reach a little past their advance; the gap has no pixels and only needs its start on the canvas
  bool room = isTitleDisplaying && (gap ? titleNextX <= CANVAS_W : titleNextX + advance + 8 <= CANVAS_W);
  xSemaphoreGive(titleLock);
  if(!room) return false;

  GlyphBitmap glyph;
  std::vector<uint16_t> colour;     // a colour emoji, if it is (still) in the cache
  bool box = false;
  // a cached colour emoji (5.8 KB buffer) or CJK character; without the RAM the fonts' version
  bool cachedGlyph = false;
  if(key && heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= GLYPH_MIN_FREE_BLOCK){
    if(!readCached(key, &colour, &glyph)) colour.clear();
    else cachedGlyph = colour.empty();
  }
  if(!gap && colour.empty() && !cachedGlyph && (c || !key)){
    if(builtin) renderBuiltinGlyph((uint8_t)c, glyph);
    else if(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < GLYPH_MIN_FREE_BLOCK) box = c != ' ';  // no RAM: a box instead of failing
    else box = !titleFont.render(c, TITLE_EM, glyph);
  }

  xSemaphoreTake(titleLock, portMAX_DELAY);
  if(generation == titleGeneration && isTitleDisplaying){
    if(!colour.empty()){
      // centred in its advance; black is the transparent background
      int x0 = titleNextX + (advance - EMOJI_SIZE) / 2;
      uint16_t* buf = text_canvas.getBuffer();
      for(int y = 0; y < EMOJI_SIZE; y++) for(int x = 0; x < EMOJI_SIZE; x++){
        uint16_t px = colour[y * EMOJI_SIZE + x];
        if(px && x0 + x >= 0 && x0 + x < CANVAS_W) buf[(EMOJI_TOP + y) * CANVAS_W + x0 + x] = px;
      }
    }
    else if(box) drawGlyphBox(text_canvas, titleNextX, advance, TITLE_COLOR);
    else if(!gap) blitGlyph(text_canvas, glyph, titleNextX, TITLE_BASELINE, TITLE_COLOR);
    titleNextX += advance;
    titleNextItem = (item + 1) % (count + 1);
  }
  xSemaphoreGive(titleLock);
  return true;
}

void titleRenderTask(void*){
  for(;;){
    if(!renderAheadStep()) vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// Moves the canvas content left by n columns (under titleLock)
void shiftTitleCanvas(int n){
  uint16_t* buf = text_canvas.getBuffer();
  for(int y = 0; y < PIC_SIZE; y++){
    uint16_t* row = &buf[y * CANVAS_W];
    memmove(row, row + n, (CANVAS_W - n) * sizeof(uint16_t));
    memset(row + CANVAS_W - n, 0, n * sizeof(uint16_t));
  }
  titleNextX -= n;
  o_X -= n;
}

void startTitle(channelInfo* channel){
  channel->titleShownSeq = ++titleShowCount;
  channelTitleInfo.channel = channel;
  channelTitleInfo.slot = channel->slotNum;
  xSemaphoreTake(titleLock, portMAX_DELAY);
  titleGeneration++;
  layoutTitle(channel->streamTitle);
  text_canvas.fillScreen(ST77XX_BLACK);
  titleNextItem = 0;
  titleNextX = TITLE_LEAD;
  o_X = 0;
  titleScrollTotal = MAX_TITLE_REPEAT * (titleText.width + TITLE_GAP);
  isTitleDisplaying = true;
  xSemaphoreGive(titleLock);
  // the first characters right away, so the title does not start with gaps
  while(titleNextX < TITLE_WIDTH + TITLE_SHIFT && renderAheadStep()) {}
  titlePixels = 0;
  titleStartedUs = nextFrameUs = micros();
  titleFrames = 0;
  titleMaxGapUs = 0;
  titleDrawUsSum = 0;
  titleXferUsSum = 0;
  titleDrawUsMax = 0;
  titleWaits = 0;
  drawTitleIndicator();
}

void stopTitle();

// Draws the title frame for the current time (called from loop() when a frame is due)
void drawTitleFrame(){
  unsigned long now = micros();
  bool first = titleFrames == 0;
  uint32_t target = (uint32_t)((uint64_t)(now - titleStartedUs) * SCROLL_STEP / FRAME_INTERVAL_US);
  if(target > titleScrollTotal) target = titleScrollTotal;
  // next frame on the fixed raster; after a long stall start a new one
  nextFrameUs += FRAME_INTERVAL_US;
  if((long)(now - nextFrameUs) > FRAME_INTERVAL_US) nextFrameUs = now + FRAME_INTERVAL_US;

  xSemaphoreTake(titleLock, portMAX_DELAY);
  if(o_X >= TITLE_SHIFT) shiftTitleCanvas(o_X);
  // never scroll past what the render task has drawn: wait (and keep the speed) instead
  int ready = titleNextX - o_X - TITLE_WIDTH;
  uint32_t step = target - titlePixels;
  if(titlePixels + step < titleScrollTotal && (int)step > ready){
    step = ready > 0 ? ready : 0;
    titleStartedUs = now - (unsigned long)((uint64_t)(titlePixels + step) * FRAME_INTERVAL_US / SCROLL_STEP);
    titleWaits++;
  }
  titlePixels += step;
  o_X += step;
  if(titlePixels >= titleScrollTotal){
    xSemaphoreGive(titleLock);
    stopTitle();
    return;
  }
  if(!first && step == 0){ xSemaphoreGive(titleLock); return; }  // nothing moved
  unsigned long xferStart = micros();
  // the title scrolls over the row that does not contain the channel's pic
  tft.drawColumnwise(GRID_X0, rowY(1-slotRow(channelTitleInfo.slot)), text_canvas.getBuffer(), o_X, TITLE_WIDTH, PIC_SIZE, CANVAS_W);
  xSemaphoreGive(titleLock);
  unsigned long drawUs = micros() - now;
  titleDrawUsSum += drawUs;
  titleXferUsSum += micros() - xferStart;
  if(drawUs > titleDrawUsMax) titleDrawUsMax = drawUs;
  if(!first && now - titleLastFrameUs > titleMaxGapUs) titleMaxGapUs = now - titleLastFrameUs;
  titleLastFrameUs = now;
  titleFrames++;
}

// Ends the title display and restores all pics (the title covers one row)
void stopTitle(){
  xSemaphoreTake(titleLock, portMAX_DELAY);
  isTitleDisplaying = false;
  titleGeneration++;
  xSemaphoreGive(titleLock);
  lastTitleEndAt = millis();
  redrawLiveChannelPics();
}

// Every few minutes without a title (setting, 0 = off), a live channel's title
// is shown again. Fair: the channel whose title was shown longest ago (or
// never) comes next; among equals chance decides. A changed title counts as
// shown too, so each live channel gets its turn before one comes again.
void repeatTitleWhenDue(){
  if(!config.titleRepeatMinutes || millis() - lastTitleEndAt < config.titleRepeatMinutes * 60000UL) return;
  channelInfo* pick = nullptr;
  uint32_t equals = 0;
  for(auto& c : channels){
    if(!c.isLive || c.slotNum < 0 || !hasTitleText(c.streamTitle)) continue;
    if(!pick || c.titleShownSeq < pick->titleShownSeq){ pick = &c; equals = 1; }
    else if(c.titleShownSeq == pick->titleShownSeq && esp_random() % ++equals == 0) pick = &c;  // each equal one with the same chance
  }
  if(!pick){ lastTitleEndAt = millis(); return; }  // nobody live: check again after the next interval
  DEBUG_I.printf("[%s] Showing the title of %s again.\n", DEBUG_TAG, pick->name.c_str());
  startTitle(pick);
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
void applyChannels(const NetMessage& msg);
void applyLogo(const NetMessage& msg);

void restoreDisplay();
void confirmFirmwareWhenStable(bool wifiConnected);

void setupModeLoop();
void checkBootButton();
void runInstall();
void waitForCjkSlot();
void runFontDownload();

void loop(){
  if(setupMode){
    setupModeLoop();
    return;
  }
  ArduinoOTA.handle();
  server.handleClient();
  checkUploadTimeout();
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
  if(fontDownloadWanted && fontPartition && WiFi.status() == WL_CONNECTED && !isTitleDisplaying && !otaActive
     && !rebootAt && !restoreDisplayAt && (long)(millis() - fontRetryAt) >= 0){
    runFontDownload();
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

  NetMessage* msg;
  if(xQueueReceive(updateQueue, &msg, 0) == pdTRUE){
    if(msg->type == NetMessage::Type::Live) applyLiveUpdate(msg->live);
    else if(msg->type == NetMessage::Type::Channels) applyChannels(*msg);
    else applyLogo(*msg);
    delete msg;
  }

  while(!isTitleDisplaying && !titleChangeQueue.empty()) {
    channelInfo* next = titleChangeQueue.front();
    titleChangeQueue.pop_front();
    // channel may have gone offline while it was queued
    if(next->isLive) startTitle(next);
  }
  if(!isTitleDisplaying) repeatTitleWhenDue();
  if(isTitleDisplaying && (long)(micros() - nextFrameUs) >= 0) drawTitleFrame();

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
  if(isTitleDisplaying){
    // sleep until the next title frame
    long wait = (long)(nextFrameUs - micros());
    if(wait > 0){
      if(wait >= 1000) delay(wait / 1000);    // lets the network task run
      delayMicroseconds(wait % 1000);
    }
  } else {
    delay(10);
  }
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
  int ids = 0;
  for (auto& channel : netChannels) {
    if (channel.id.empty()) continue; // name not resolved yet
    url += "user_id=";
    url += channel.id.c_str();
    url += "&";
    ids++;
  }
  if (ids == 0) return FetchResult::Ok; // nothing to ask for

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
    info.title = std::string(stream["title"] | "") + " | " + (stream["game_name"] | "");
    info.webTitle = std::string(stream["title"] | "") + " | " + (stream["game_name"] | "");
    update.streams.push_back(std::move(info));
  }
  return FetchResult::Ok;
}

// One update: fetch (renewing the token if needed) and hand the result to loop()
// Emoji and CJK downloads need an HTTPS connection (about 45 KB: two 16 KB
// buffers plus the handshake) and then decoding/rendering. With titles being
// rendered and drawn at the same time the heap ran empty, so these downloads
// wait for a pause between titles, and only start with enough RAM in one
// block. Returns false if there is no pause within 2 minutes (next update).
#define DOWNLOAD_MIN_FREE_BLOCK 18432   // each of the two 16 KB TLS buffers needs a block of about 17 KB
#define DOWNLOAD_MIN_FREE       40960
#define DECODE_MIN_FREE_BLOCK   22528   // a 72x72 RGBA PNG is unpacked into one 21 KB buffer
#define TITLE_PAUSE_WAIT        120000
bool waitForDownloadSlot(){
  for(unsigned long t0 = millis(); isTitleDisplaying || installRequested;){
    if(millis() - t0 > TITLE_PAUSE_WAIT || otaActive) return false;
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  if(otaActive) return false;
  if(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < DOWNLOAD_MIN_FREE_BLOCK || ESP.getFreeHeap() < DOWNLOAD_MIN_FREE){
    emojiError = "not enough free memory for a download (" + String((unsigned)(ESP.getFreeHeap() / 1024)) + " KB, largest block "
                 + String((unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024)) + " KB), trying again later";
    return false;
  }
  return true;
}

// Downloads the colour emoji of the live titles that are not cached yet
// (network task), so a new title already shows them when it starts
std::set<uint32_t> emojiMissing;   // not in Twemoji (404): not asked again until a restart
extern std::set<uint32_t> cjkMissing;
extern volatile bool cacheMissingReset;
void fetchMissingEmoji(const std::vector<StreamInfo>& streams){
  if(cacheMissingReset){ cacheMissingReset = false; emojiMissing.clear(); cjkMissing.clear(); }
  if(!config.colourEmoji || !emojiCacheReady) return;
  std::vector<std::string> names;
  for(const auto& stream : streams)
    for(const TitleChar& c : titleChars(stream.title.c_str()))
      if(!c.emoji.empty() && std::find(names.begin(), names.end(), c.emoji) == names.end()) names.push_back(c.emoji);
  int downloads = 0;
  for(const auto& name : names){
    uint32_t key = emojiKey(name);
    if(emojiMissing.count(key) || emojiCached(key)) continue;
    if(downloads++ >= EMOJI_MAX_PER_UPDATE || otaActive || !waitForDownloadSlot()) break;
    std::vector<uint8_t> png;
    String error = downloadToMemory(TWEMOJI_URL + name + ".png", png, EMOJI_MAX_DOWNLOAD);
    tlsClient.stop();   // free the TLS session before decoding
    if(error.length()){
      if(error.indexOf("(404)") >= 0){ emojiMissing.insert(key); continue; }   // no colour version: monochrome
      emojiError = name.c_str() + String(": ") + error;
      DEBUG_W.printf("[Emoji] %s\n", emojiError.c_str());
      break;   // network problem: try the rest next time
    }
    if(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < DECODE_MIN_FREE_BLOCK){
      emojiError = name.c_str() + String(": not enough memory in one block to decode it, trying again later");
      break;
    }
    std::vector<uint16_t> px(EMOJI_SIZE * EMOJI_SIZE);
    const char* e = pngToRgb565(png.data(), png.size(), px.data(), EMOJI_SIZE, EMOJI_SIZE);
    png.clear(); png.shrink_to_fit();
    if(e && !strcmp(e, "Out of memory")){ emojiError = name.c_str() + String(": out of memory while decoding, trying again later"); break; }
    if(e){ emojiError = name.c_str() + String(": ") + e; emojiMissing.insert(key); continue; }   // a broken image: not asked again
    std::vector<uint8_t> rle = rleEncode(px.data(), px.size());
    px.clear(); px.shrink_to_fit();
    if(!storeEmoji(key, rle)){ emojiError = name.c_str() + String(": cannot store it"); continue; }
    emojiDownloads++;
    DEBUG_I.printf("[Emoji] %s stored (%u bytes).\n", name.c_str(), (unsigned)rle.size());
  }
}

// ---------- CJK characters (network task): looked up in fontsource's CSS of
// Noto Sans JP/KR/SC (which numbered piece has a character), the piece
// (17-100 KB TrueType) downloaded into the firmware slot that is not running,
// the characters rendered from it and kept in the cache. The slot is only
// used when the running firmware is confirmed and no update runs; updates
// wait for a running piece download (cjkSlotBusy).
#define CJK_VERSION            "5.3.1"
#define CJK_CSS_URL            "https://cdn.jsdelivr.net/npm/@fontsource/%s@" CJK_VERSION "/600.css"
#define CJK_PIECE_URL          "https://cdn.jsdelivr.net/fontsource/fonts/%s@" CJK_VERSION "/%d-600-normal.ttf"
#define CJK_MAX_PIECE          (256 * 1024)
#define CJK_MAX_CHARS          48      // per update; the rest next time
#define CJK_MAX_PIECES         8       // downloads per update
enum { CJK_JP, CJK_KR, CJK_SC, CJK_FONTS };
static const char* const CJK_FONT_NAMES[CJK_FONTS] = {"noto-sans-jp", "noto-sans-kr", "noto-sans-sc"};

volatile bool cjkSlotBusy = false;
std::set<uint32_t> cjkMissing;   // in none of the fonts: not looked up again until a restart
uint32_t cjkDownloads = 0;

bool isCjk(uint32_t cp){
  return (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xA960 && cp <= 0xA97F)
      || (cp >= 0xAC00 && cp <= 0xD7FF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F)
      || (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x3134F);
}
bool isHangul(uint32_t cp){
  return (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F) || (cp >= 0xA960 && cp <= 0xA97F) || (cp >= 0xAC00 && cp <= 0xD7FF);
}
// Which fonts to ask, in order: Hangul from the Korean font, everything else Japanese, Chinese, Korean
int cjkFontChoice(uint32_t cp, int round){
  static const int hangul[] = {CJK_KR, CJK_JP, CJK_SC}, other[] = {CJK_JP, CJK_SC, CJK_KR};
  return isHangul(cp) ? hangul[round] : other[round];
}

// GET url and hand each line (up to 4 KB) to onLine. Returns "" or the error.
String httpGetLines(const String& url, const std::function<void(const char*)>& onLine){
  HTTPClient http;
  http.useHTTP10(true);   // no chunked transfer; the server closes the connection at the end
  http.begin(tlsClient, url);
  http.setUserAgent("twitchDisplay/" FW_VERSION);
  http.setTimeout(DOWNLOAD_TIMEOUT);
  int code = http.GET();
  if(code != HTTP_CODE_OK){
    String e = "download failed (" + (code <= 0 ? http.errorToString(code) : String(code)) + ")";
    http.end();
    return e;
  }
  int size = http.getSize();
  auto* stream = http.getStreamPtr();
  std::string line;
  uint8_t buf[256];
  size_t got = 0;
  unsigned long lastData = millis();
  String error;
  while(size <= 0 || got < (size_t)size){
    size_t avail = stream->available();
    if(avail == 0){
      if(size <= 0 && !http.connected()) break;
      if(millis() - lastData > DOWNLOAD_TIMEOUT){ error = "download timed out"; break; }
      delay(1);
      continue;
    }
    size_t n = stream->readBytes(buf, std::min(avail, sizeof(buf)));
    got += n;
    lastData = millis();
    for(size_t i = 0; i < n; i++){
      if(buf[i] == '\n'){ onLine(line.c_str()); line.clear(); }
      else if(line.size() < 4096) line += (char)buf[i];
    }
  }
  if(error.isEmpty() && !line.empty()) onLine(line.c_str());
  http.end();
  tlsClient.stop();
  return error;
}

// Finds the pieces of font f that contain the characters in cps (CSS lines:
// "src: url(./files/noto-sans-jp-119-600-normal.woff2) ..." followed by
// "unicode-range: U+3041-3096,U+4e00,U+4??;").
String cjkLookup(int f, const std::vector<uint32_t>& cps, std::map<uint32_t, int>& pieceOf){
  char url[128];
  snprintf(url, sizeof(url), CJK_CSS_URL, CJK_FONT_NAMES[f]);
  int piece = -1;
  return httpGetLines(url, [&](const char* line){
    if(const char* files = strstr(line, "files/")){
      piece = -1;
      const char* m = strstr(files, "-600-normal");
      const char* d = m;
      while(m && d > files && isdigit((unsigned char)d[-1])) d--;
      if(m && d < m && d[-1] == '-') piece = atoi(d);    // numbered pieces only (not "latin", "cyrillic", ...)
      return;
    }
    const char* p = strstr(line, "unicode-range:");
    if(!p || piece < 0) return;
    p += 14;
    while(*p && *p != ';'){
      while(*p == ' ' || *p == ',') p++;
      if(*p == 'U' || *p == 'u') p += 2;
      uint32_t a = 0, b = 0;
      bool wild = false;
      for(; isxdigit((unsigned char)*p) || *p == '?'; p++){
        if(*p == '?'){ a = a << 4; b = (b << 4) | 0xF; wild = true; }
        else { uint32_t v = isdigit((unsigned char)*p) ? *p - '0' : (tolower(*p) - 'a' + 10); a = (a << 4) | v; b = (b << 4) | v; }
      }
      if(*p == '-' && !wild){
        p++; b = 0;
        for(; isxdigit((unsigned char)*p); p++) b = (b << 4) | (isdigit((unsigned char)*p) ? *p - '0' : (tolower(*p) - 'a' + 10));
      }
      for(uint32_t cp : cps) if(cp >= a && cp <= b && !pieceOf.count(cp)) pieceOf[cp] = piece;
      while(*p && *p != ',' && *p != ';') p++;
    }
  });
}

// Downloads a piece into the free firmware slot; size = its length
String cjkDownloadPiece(int f, int piece, const esp_partition_t* slot, size_t& size){
  char url[128];
  snprintf(url, sizeof(url), CJK_PIECE_URL, CJK_FONT_NAMES[f], piece);
  size = 0;
  HTTPClient http;
  http.useHTTP10(true);
  http.begin(tlsClient, url);
  http.setUserAgent("twitchDisplay/" FW_VERSION);
  http.setTimeout(DOWNLOAD_TIMEOUT);
  int code = http.GET();
  if(code != HTTP_CODE_OK){
    String e = "download failed (" + (code <= 0 ? http.errorToString(code) : String(code)) + ")";
    http.end();
    return e;
  }
  int total = http.getSize();   // the CDN does not always send it: then until the server closes
  if(total > CJK_MAX_PIECE){ http.end(); return "piece too large"; }
  auto* stream = http.getStreamPtr();
  uint8_t buf[1024];
  size_t erased = 0;
  unsigned long lastData = millis();
  String error;
  while(total <= 0 || size < (size_t)total){
    if(otaActive){ error = "an update started"; break; }
    size_t avail = stream->available();
    if(avail == 0){
      if(total <= 0 && !http.connected()) break;
      if(millis() - lastData > DOWNLOAD_TIMEOUT){ error = "download timed out"; break; }
      delay(1);
      continue;
    }
    size_t n = stream->readBytes(buf, std::min(avail, sizeof(buf)));
    lastData = millis();
    if(size + n > CJK_MAX_PIECE){ error = "piece too large"; break; }
    while(erased < size + n){
      if(esp_partition_erase_range(slot, erased, CACHE_SECTOR) != ESP_OK){ error = "cannot erase the free firmware slot"; break; }
      erased += CACHE_SECTOR;
    }
    if(error.length()) break;
    if(esp_partition_write(slot, size, buf, n) != ESP_OK){ error = "cannot write the free firmware slot"; break; }
    size += n;
  }
  http.end();
  tlsClient.stop();
  if(error.isEmpty() && (size == 0 || (total > 0 && size != (size_t)total))) error = "download incomplete";
  return error;
}

// Renders the characters cps from the piece in the slot and caches them
int cjkRenderPiece(const esp_partition_t* slot, size_t size, const std::vector<uint32_t>& cps){
  const void* map = nullptr;
  esp_partition_mmap_handle_t handle;
  if(esp_partition_mmap(slot, 0, size, ESP_PARTITION_MMAP_DATA, &map, &handle) != ESP_OK) return 0;
  int stored = 0;
  for(uint32_t cp : cps){
    if(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < GLYPH_MIN_FREE_BLOCK) break;
    GlyphBitmap g;
    int advance = 0;
    if(!TitleFont::renderFrom((const uint8_t*)map, cp, TITLE_EM, g, advance)) continue;
    std::vector<uint8_t> rle = g.alpha.empty() ? std::vector<uint8_t>{0, 0} : rleEncodeAlpha(g.alpha.data(), g.alpha.size());
    if(g.w > 255 || g.h > 255 || advance > 255 || g.xoff < -128 || g.xoff > 127 || g.yoff < -128 || g.yoff > 127) continue;
    if(storeCached(glyphKey(cp), CACHE_GLYPH, g.w, g.h, g.xoff, g.yoff, advance, rle)) stored++;
  }
  esp_partition_munmap(handle);
  return stored;
}

// The CJK characters of the live titles that are not cached yet (network task)
void fetchMissingCjk(const std::vector<StreamInfo>& streams){
  if(!config.cjk || !emojiCacheReady || !titleFont.ready() || appPendingVerify || otaActive) return;
  const esp_partition_t* slot = esp_ota_get_next_update_partition(nullptr);
  if(!slot) return;
  std::vector<uint32_t> pending;
  for(const auto& stream : streams)
    for(uint32_t cp : titleCodepoints(stream.title.c_str()))
      if(isCjk(cp) && titleFont.sourceFor(cp) == TitleFont::NONE && !cjkMissing.count(cp)
         && std::find(pending.begin(), pending.end(), cp) == pending.end() && !cacheLookup(glyphKey(cp)))
        if(pending.size() < CJK_MAX_CHARS) pending.push_back(cp);
  if(pending.empty() || !waitForDownloadSlot()) return;
  cjkSlotBusy = true;
  int pieces = 0;
  for(int round = 0; round < CJK_FONTS && !pending.empty() && !otaActive; round++){
    for(int f = 0; f < CJK_FONTS && !otaActive; f++){
      std::vector<uint32_t> ask;
      for(uint32_t cp : pending) if(cjkFontChoice(cp, round) == f) ask.push_back(cp);
      if(ask.empty()) continue;
      std::map<uint32_t, int> pieceOf;
      if(!waitForDownloadSlot()) goto done;
      String error = cjkLookup(f, ask, pieceOf);
      if(error.length()){ emojiError = String(CJK_FONT_NAMES[f]) + " CSS: " + error; goto done; }
      std::set<int> needed;
      for(auto& kv : pieceOf) needed.insert(kv.second);
      for(int piece : needed){
        if(pieces++ >= CJK_MAX_PIECES || otaActive || !waitForDownloadSlot()) goto done;
        size_t size = 0;
        error = cjkDownloadPiece(f, piece, slot, size);
        if(error.length()){ emojiError = String(CJK_FONT_NAMES[f]) + " piece " + String(piece) + ": " + error; goto done; }
        std::vector<uint32_t> chars;
        for(auto& kv : pieceOf) if(kv.second == piece) chars.push_back(kv.first);
        cjkRenderPiece(slot, size, chars);
        cjkDownloads++;
        DEBUG_I.printf("[CJK] %s piece %d: %u characters.\n", CJK_FONT_NAMES[f], piece, (unsigned)chars.size());
        for(uint32_t cp : chars) pending.erase(std::remove(pending.begin(), pending.end(), cp), pending.end());
      }
    }
  }
  for(uint32_t cp : pending) cjkMissing.insert(cp);   // no font has them: boxes
done:
  cjkSlotBusy = false;
}

void runNetworkUpdate(){
  if (WiFi.status() != WL_CONNECTED) return; // handleWiFi() requests an update when it is back
  if (otaActive) return;                      // firmware upload in progress

  NetMessage* msg = new NetMessage();
  LiveUpdate* update = &msg->live;
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
  if (update->result == FetchResult::Ok){
    fetchMissingEmoji(update->streams);
    fetchMissingCjk(update->streams);
  }
  if (xQueueSend(updateQueue, &msg, portMAX_DELAY) != pdTRUE) delete msg;
}

// Looks the channels up on Twitch (by id, or by login if the id is not known
// yet) and fills in id, login, display name and avatar URL
FetchResult fetchUsers(std::vector<ChannelRef>& refs){
  String url = "https://api.twitch.tv/helix/users?";
  int n = 0;
  for (auto& r : refs) {
    r.found = false;
    if (!r.id.empty()) { url += "id=" + String(r.id.c_str()) + "&"; n++; }
    else if (!r.login.empty()) { url += "login=" + urlEncode(r.login.c_str()) + "&"; n++; }
  }
  if (n == 0) return FetchResult::Ok;

  DEBUG_I.printf("[%s] Looking up channels: %s\n", DEBUG_TAG, url.c_str());
  HTTPClient http;
  http.begin(tlsClient, url);
  commonHttpInit(http);
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    DEBUG_W.printf("[HTTP] Channel lookup failed, error: %s\n",
      (httpCode<=0) ? http.errorToString(httpCode).c_str() : String(httpCode).c_str());
    http.end();
    return (httpCode == HTTP_CODE_UNAUTHORIZED) ? FetchResult::Unauthorized : FetchResult::Failed;
  }
  JsonDocument filter;
  JsonObject user = filter["data"].add<JsonObject>();
  user["id"] = true;
  user["login"] = true;
  user["display_name"] = true;
  user["profile_image_url"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err || !doc["data"].is<JsonArray>()) return FetchResult::Failed;
  for (JsonObject u : doc["data"].as<JsonArray>()) {
    std::string id = u["id"] | "", login = u["login"] | "";
    for (auto& r : refs) {
      if ((!r.id.empty() && r.id == id) || (r.id.empty() && !login.empty() && r.login == login)) {
        r.id = id;
        r.login = login;
        r.name = u["display_name"] | "";
        r.avatarUrl = u["profile_image_url"] | "";
        r.found = true;
      }
    }
  }
  return FetchResult::Ok;
}

void saveChannelCache(const std::vector<ChannelRef>& refs);
void syncLogos(const std::vector<ChannelRef>& refs);

// ---------- Channel logos (network task)

#define LOGO_MAX_DOWNLOAD (48*1024) // the 70x70 avatars are about 10 KB

volatile bool logoReloadRequested = false;     // "Reload logos" on the status page

void sendLogoMessage(const std::string& channelId, int slot, const std::string& status){
  NetMessage* msg = new NetMessage();
  msg->type = NetMessage::Type::Logo;
  msg->channelId = channelId;
  msg->logoSlot = slot;
  msg->logoStatus = status;
  if (xQueueSend(updateQueue, &msg, portMAX_DELAY) != pdTRUE) delete msg;
}

// Twitch serves every avatar in several sizes; the 70x70 one is closest to the
// 64x64 tiles ("...-300x300.png" -> "...-70x70.png", both URL formats)
std::string smallAvatarUrl(const std::string& url){
  size_t dot = url.find_last_of('.');
  size_t pos = url.rfind("300x300");
  if (dot != std::string::npos && pos != std::string::npos && pos + 7 == dot) return url.substr(0, pos) + "70x70" + url.substr(dot);
  return url;
}

// Downloads a file of at most maxLen bytes. Returns an empty string or the error.
String downloadToMemory(const std::string& url, std::vector<uint8_t>& data, size_t maxLen){
  data.clear();
  HTTPClient http;
  http.useHTTP10(true); // no keep-alive: the connection (and its TLS buffers) is closed afterwards
  http.begin(tlsClient, url.c_str());
  http.setUserAgent("twitchDisplay/" FW_VERSION);
  http.setTimeout(DOWNLOAD_TIMEOUT);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    String e = "download failed (" + (code <= 0 ? http.errorToString(code) : String(code)) + ")";
    http.end();
    return e;
  }
  int size = http.getSize();
  if (size > (int)maxLen) { http.end(); return "image too large"; }
  auto* stream = http.getStreamPtr();
  size_t expected = size > 0 ? (size_t)size : maxLen;
  data.reserve(expected);
  uint8_t buf[512];
  unsigned long lastData = millis();
  while (data.size() < expected) {
    size_t avail = stream->available();
    if (avail == 0) {
      if (size <= 0 && !http.connected()) break; // no length given: until the server closes
      if (millis() - lastData > DOWNLOAD_TIMEOUT) { http.end(); return "download timed out"; }
      delay(1);
      continue;
    }
    size_t n = stream->readBytes(buf, std::min(std::min(avail, sizeof(buf)), expected - data.size()));
    data.insert(data.end(), buf, buf + n);
    lastData = millis();
  }
  http.end();
  if (size > 0 && data.size() != (size_t)size) return "download incomplete";
  return "";
}

// Downloads the logos that are missing or whose avatar changed. A slot is only
// overwritten once the new logo is ready, so a failed download keeps the old one.
void syncLogos(const std::vector<ChannelRef>& refs){
  bool force = logoReloadRequested;
  logoReloadRequested = false;
  for (const auto& ref : refs) {
    if (!ref.found || ref.avatarUrl.empty()) continue;
    std::string url = smallAvatarUrl(ref.avatarUrl);
    uint32_t hash = esp_rom_crc32_le(0, (const uint8_t*)url.data(), url.size());
    int slot = -1;
    for (int i = 0; i < logoSlotCount(); i++) if (netLogoIndex[i].channelId == ref.id) slot = i;
    if (slot >= 0 && netLogoIndex[slot].urlHash == hash && !force) continue; // up to date
    if (slot < 0) {
      // a free slot, or one of a channel that is no longer in the list
      for (int i = 0; i < logoSlotCount() && slot < 0; i++) {
        const std::string& owner = netLogoIndex[i].channelId;
        bool inUse = false;
        for (const auto& r : refs) if (!owner.empty() && r.id == owner) inUse = true;
        if (!inUse) slot = i;
      }
      if (slot < 0) continue; // more channels than slots (old partition layout): this one keeps its placeholder
    }

    DEBUG_I.printf("[%s] Downloading logo of %s: %s\n", DEBUG_TAG, ref.login.c_str(), url.c_str());
    sendLogoMessage(ref.id, LOGO_SLOT_UNCHANGED, "downloading");
    std::vector<uint8_t> png;
    String error = downloadToMemory(url, png, LOGO_MAX_DOWNLOAD);
    tlsClient.stop(); // free the TLS session before decoding
    std::vector<uint16_t> logo(LOGO_SIZE * LOGO_SIZE);
    if (error.isEmpty()) {
      const char* e = pngToLogo(png.data(), png.size(), logo.data());
      if (e) error = e;
    }
    png.clear();
    png.shrink_to_fit();
    if (error.length()) {
      DEBUG_W.printf("[%s] Logo of %s: %s\n", DEBUG_TAG, ref.login.c_str(), error.c_str());
      sendLogoMessage(ref.id, LOGO_SLOT_UNCHANGED, error.c_str());
      continue;
    }

    // loop() stops drawing from the slot before it is rewritten
    sendLogoMessage(ref.id, -1, "updating");
    if (!netLogoIndex[slot].channelId.empty() && netLogoIndex[slot].channelId != ref.id) sendLogoMessage(netLogoIndex[slot].channelId, -1, "");
    vTaskDelay(pdMS_TO_TICKS(100));
    netLogoIndex[slot] = LogoSlot();
    saveLogoIndexEntry(slot, netLogoIndex[slot]);
    if (!writeLogo(slot, logo.data())) {
      sendLogoMessage(ref.id, -1, "could not write flash");
      continue;
    }
    netLogoIndex[slot].channelId = ref.id;
    netLogoIndex[slot].urlHash = hash;
    saveLogoIndexEntry(slot, netLogoIndex[slot]);
    sendLogoMessage(ref.id, slot, "stored");
  }
}

#define CHANNEL_SYNC_INTERVAL (24UL*60*60*1000) // look the channels up again once a day
volatile bool channelSyncRequested = true;      // first lookup right after the start
unsigned long lastChannelSync = 0;

// Resolves the channel list and hands it to loop(). False if it has to be retried.
bool runChannelSync(){
  if (WiFi.status() != WL_CONNECTED || otaActive || !config.hasTwitchCredentials()) return false;
  std::vector<ChannelRef> refs = netChannels;
  bool canRenewToken = config.clientSecret.length() > 0;
  if (canRenewToken && twitchToken.isEmpty()) requestAppToken();
  FetchResult result = fetchUsers(refs);
  if (canRenewToken && result == FetchResult::Unauthorized && requestAppToken()) {
    refs = netChannels;
    result = fetchUsers(refs);
  }
  if (result != FetchResult::Ok) return false;

  NetMessage* msg = new NetMessage();
  msg->type = NetMessage::Type::Channels;
  for (const auto& r : refs) {
    if (!r.found) msg->unknown.push_back(r.login.empty() ? r.id : r.login);
  }
  msg->channels = refs;
  netChannels = refs;
  saveChannelCache(refs);
  if (xQueueSend(updateQueue, &msg, portMAX_DELAY) != pdTRUE) delete msg;
  syncLogos(refs);
  return true;
}

void networkTask(void*){
  for(;;){
    if (channelSyncRequested || millis() - lastChannelSync >= CHANNEL_SYNC_INTERVAL) {
      if (runChannelSync()) {
        channelSyncRequested = false;
        lastChannelSync = millis();
      }
    }
    runNetworkUpdate();
    // sleep until the next interval or until requestUpdate()
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TW_UPDATE_INTERVAL));
  }
}

// ---------- Drawing and channel state (loop() only)


// ---------- Logo storage
// Logos are kept ready to draw (64x64 RGB565 = 8 KB, two flash sectors) in
// fixed slots of the otherwise unused "spiffs" partition (128 KB = 16 slots).
// An index in NVS says which channel and which avatar URL each slot holds; a
// slot is only listed there after it has been written completely.

const esp_partition_t* logoPartition(){
  // "logos" (old partition layout: "spiffs"), written without a file system
  static const esp_partition_t* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  return partition;
}

int logoSlotCount(){
  const esp_partition_t* p = logoPartition();
  return p ? (int)std::min<size_t>(MAX_LOGO_SLOTS, p->size / LOGO_SLOT_BYTES) : 0;
}

bool readLogo(int slot, uint16_t* pixels){
  const esp_partition_t* p = logoPartition();
  return p && slot >= 0 && slot < logoSlotCount()
      && esp_partition_read(p, slot * LOGO_SLOT_BYTES, pixels, LOGO_SLOT_BYTES) == ESP_OK;
}

bool writeLogo(int slot, const uint16_t* pixels){
  const esp_partition_t* p = logoPartition();
  return p && slot >= 0 && slot < logoSlotCount()
      && esp_partition_erase_range(p, slot * LOGO_SLOT_BYTES, LOGO_SLOT_BYTES) == ESP_OK
      && esp_partition_write(p, slot * LOGO_SLOT_BYTES, pixels, LOGO_SLOT_BYTES) == ESP_OK;
}

String logoIndexKey(int slot){
  return "s" + String(slot);
}

void loadLogoIndex(std::array<LogoSlot, MAX_LOGO_SLOTS>& index){
  Preferences prefs;
  prefs.begin(LOGO_NAMESPACE, true);
  for(int i = 0; i < logoSlotCount(); i++){
    String v = prefs.getString(logoIndexKey(i).c_str(), "");   // "<channel id>:<url hash>"
    int colon = v.indexOf(':');
    index[i] = LogoSlot();
    if(colon > 0){
      index[i].channelId = v.substring(0, colon).c_str();
      index[i].urlHash = strtoul(v.substring(colon + 1).c_str(), nullptr, 16);
    }
  }
  prefs.end();
}

void saveLogoIndexEntry(int slot, const LogoSlot& entry){
  Preferences prefs;
  prefs.begin(LOGO_NAMESPACE, false);
  if(entry.channelId.empty()) prefs.remove(logoIndexKey(slot).c_str());
  else {
    char hash[9];
    snprintf(hash, sizeof(hash), "%08lx", (unsigned long)entry.urlHash);
    prefs.putString(logoIndexKey(slot).c_str(), String(entry.channelId.c_str()) + ":" + hash);
  }
  prefs.end();
}

// The logo index belongs to one place in flash: after a new partition layout
// (USB flash) the slots hold something else, so the index is cleared and the
// logos are downloaded again. Called at boot, before the index is read.
void checkLogoPartition(){
  const esp_partition_t* p = logoPartition();
  if(!p) return;
  char where[24];
  snprintf(where, sizeof(where), "%lx:%lx", (unsigned long)p->address, (unsigned long)p->size);
  Preferences prefs;
  prefs.begin(LOGO_NAMESPACE, false);
  if(prefs.getString("part", "") != where){
    if(prefs.getString("part", "").length() || prefs.isKey("s0")) DEBUG_I.printf("[%s] Logo partition moved, downloading the logos again.\n", DEBUG_TAG);
    prefs.clear();
    prefs.putString("part", where);
  }
  prefs.end();
}

// Gives every channel its stored logo, if there is one (at boot, before the network task runs)
void assignLogoSlots(){
  std::array<LogoSlot, MAX_LOGO_SLOTS> index;
  loadLogoIndex(index);
  for(channelInfo& ch : channels){
    ch.logoSlot = -1;
    for(int i = 0; i < logoSlotCount(); i++) if(!ch.id.empty() && index[i].channelId == ch.id) ch.logoSlot = i;
    if(ch.logoStatus.empty() || ch.logoStatus == "stored" || ch.logoStatus == "missing") ch.logoStatus = ch.logoSlot >= 0 ? "stored" : "missing";
  }
}

// Initials for a placeholder: the first two letters or digits the font has,
// upper case where it is simple (ASCII and Latin-1), "?" without any
std::vector<uint32_t> placeholderInitials(const std::string& name){
  std::vector<uint32_t> label;
  for(uint32_t cp : titleCodepoints(name.c_str())){
    bool letter = cp < 0x80 ? isalnum((int)cp) != 0 : (cp >= 0xC0 && cp != 0xD7 && cp != 0xF7);
    if(!letter || titleFont.sourceFor(cp) != TitleFont::TEXT) continue;
    if(cp < 0x80) cp = toupper((int)cp);
    else if(cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) cp -= 0x20;
    label.push_back(cp);
    if(label.size() == 2) break;
  }
  if(label.empty()) label.push_back('?');
  return label;
}

// Placeholder while a channel has no logo: coloured tile with its initials
void drawPlaceholder(const channelInfo& channel){
  static const uint16_t colors[] = {0x8000, 0x0410, 0x6010, 0x8400, 0x0013, 0x0400, 0x8200, 0x4208};
  const std::string& key = channel.name.empty() ? channel.id : channel.name;
  uint32_t hash = 0;
  for(char c : key) hash = hash * 31 + (uint8_t)c;
  pic_canvas.fillScreen(colors[hash % (sizeof(colors) / sizeof(colors[0]))]);
  if(titleFont.ready()){
    std::vector<uint32_t> label = placeholderInitials(channel.name);
    const float em = 30;
    int width = 0;
    for(uint32_t cp : label) width += titleFont.advance(cp, em);
    int x = (PIC_SIZE - width) / 2;
    for(uint32_t cp : label){
      GlyphBitmap g;
      if(titleFont.render(cp, em, g)) blitGlyph(pic_canvas, g, x, 43, ST77XX_WHITE);
      x += titleFont.advance(cp, em);
    }
    return;
  }
  std::string label;
  for(char c : channel.name){
    if(isalnum((unsigned char)c)) label += (char)toupper((unsigned char)c);
    if(label.size() == 2) break;
  }
  if(label.empty()) label = "?";
  pic_canvas.setTextSize(3);
  pic_canvas.setTextColor(ST77XX_WHITE);
  pic_canvas.setCursor((PIC_SIZE - (int)label.size() * 18 + 3) / 2, (PIC_SIZE - 21) / 2);
  pic_canvas.print(label.c_str());
}

void drawLiveChannelPic(channelInfo& channel, uint8_t pic_slot_index){
  channel.slotNum = pic_slot_index;
  if(pic_slot_index<MAX_NUM_PICS){
    DEBUG_I.printf("[%s] Drawing channel pic of %s in slot number %u\n", DEBUG_TAG, channel.id.c_str(), pic_slot_index);
    // downloaded logo, else placeholder
    if(!(channel.logoSlot >= 0 && readLogo(channel.logoSlot, pic_canvas.getBuffer()))) drawPlaceholder(channel);
  } else {
    // more live channels than slots: the last slot shows how many are hidden
    pic_canvas.fillScreen(ST77XX_BLACK);
    pic_canvas.setCursor(4, 14);
    pic_canvas.setTextColor(ST77XX_WHITE);
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

// ---------- Channel list

bool isNumeric(const std::string& s){
  if (s.empty()) return false;
  for (char c : s) if (!isdigit((unsigned char)c)) return false;
  return true;
}

// The configured channels: the saved list (logins, or numeric user ids) or the defaults
std::vector<ChannelRef> configuredChannels(){
  std::vector<ChannelRef> refs;
  if (config.channelList.length()) {
    std::string list = config.channelList.c_str();
    size_t start = 0;
    while (start <= list.size()) {
      size_t end = list.find(',', start);
      if (end == std::string::npos) end = list.size();
      std::string entry = list.substr(start, end - start);
      if (!entry.empty() && refs.size() < MAX_CHANNELS) {
        ChannelRef r;
        if (isNumeric(entry)) r.id = entry; else r.login = entry;
        refs.push_back(r);
      }
      start = end + 1;
    }
  }
  if (refs.empty()) {
    for (const char* id : DEFAULT_CHANNEL_IDS) { ChannelRef r; r.id = id; refs.push_back(r); }
  }
  return refs;
}

// Fills in what the last lookup found ("id\tlogin\tname" lines), so the display
// works right away after a restart, before Twitch has been asked again
void applyChannelCache(std::vector<ChannelRef>& refs){
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true);
  std::string cache = prefs.getString("chcache", "").c_str();
  prefs.end();
  size_t start = 0;
  while (start < cache.size()) {
    size_t end = cache.find('\n', start);
    if (end == std::string::npos) end = cache.size();
    std::string line = cache.substr(start, end - start);
    size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
    if (t2 != std::string::npos) {
      std::string id = line.substr(0, t1), login = line.substr(t1 + 1, t2 - t1 - 1), name = line.substr(t2 + 1);
      for (auto& r : refs) {
        if ((!r.id.empty() && r.id == id) || (r.id.empty() && r.login == login)) { r.id = id; r.login = login; r.name = name; }
      }
    }
    start = end + 1;
  }
}

void saveChannelCache(const std::vector<ChannelRef>& refs){
  std::string cache;
  for (const auto& r : refs) {
    if (r.found) cache += r.id + "\t" + r.login + "\t" + r.name + "\n";
  }
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  if (prefs.getString("chcache", "") != String(cache.c_str())) prefs.putString("chcache", cache.c_str());
  prefs.end();
}

// Builds the channel list at boot (before the network task starts)
void buildChannels(){
  std::vector<ChannelRef> refs = configuredChannels();
  applyChannelCache(refs);
  channels.clear();
  channels.reserve(MAX_CHANNELS);
  for (const auto& r : refs) {
    channelInfo ch;
    ch.id = r.id;
    ch.login = r.login;
    ch.name = r.name;
    channels.push_back(ch);
  }
  netChannels = refs;
  unknownChannels.clear();
}

// Takes over the result of a channel lookup from the network task
void applyChannels(const NetMessage& msg){
  unknownChannels = msg.unknown;
  bool changed = false;
  for (size_t i = 0; i < channels.size() && i < msg.channels.size(); i++) {
    channelInfo& ch = channels[i];
    const ChannelRef& r = msg.channels[i];
    if (ch.id != r.id) { ch.id = r.id; changed = true; }
    if (ch.login != r.login) { ch.login = r.login; changed = true; }
    if (!r.name.empty() && ch.name != r.name) { ch.name = r.name; changed = true; }
  }
  if (!changed) return;
  assignLogoSlots();
  // placeholders show the initials of the name; the title row is redrawn when the title ends
  if (!isTitleDisplaying) redrawLiveChannelPics();
}

// Takes over a logo state change from the network task
void applyLogo(const NetMessage& msg){
  channelInfo* ch = nullptr;
  for (channelInfo& c : channels) if (!msg.channelId.empty() && c.id == msg.channelId) ch = &c;
  if (!ch) return;
  if (msg.logoStatus.length()) ch->logoStatus = msg.logoStatus;
  if (msg.logoSlot == LOGO_SLOT_UNCHANGED || msg.logoSlot == ch->logoSlot) return;
  ch->logoSlot = msg.logoSlot;
  if (ch->isLive && !isTitleDisplaying) redrawLiveChannelPics(slotRow(ch->slotNum));
  // during a title the tiles are redrawn when it ends
}

channelInfo* findChannel(const std::string& id){
  for (channelInfo& channel : channels) {
    if (!id.empty() && channel.id == id) return &channel;
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
      otaActive = true;    // pauses the network task; nothing is written before this returns
      waitForCjkSlot();
    })
    .onEnd([]() {
      DEBUG_I.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      DEBUG_I.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      otaActive = false;
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

// An upload in pieces whose next piece does not come (page closed, connection
// lost): give up, so the display and the Twitch updates continue
void checkUploadTimeout(){
  if(!upload.lastPieceAt || millis() - upload.lastPieceAt < UPLOAD_PIECE_TIMEOUT) return;
  upload.lastPieceAt = 0;
  if(!otaActive) return;
  failUpload("Upload stopped (no data for 30 s).");
  otaActive = false;
  drawMessageScreen("Update failed", ST77XX_RED, upload.error.c_str());
  restoreDisplayAt = millis() + 5000;
}

// Called by the web server for every chunk of the uploaded file
void handleUploadChunk(){
  HTTPUpload& up = server.upload();
  switch(up.status){
    case UPLOAD_FILE_START:
      upload.authorized = setupMode || loginOk(); // setup mode: hotspot password protects
      if(!upload.authorized) return;
      upload.pieces = server.hasArg("offset");
      upload.offset = upload.pieces ? (size_t)server.arg("offset").toInt() : 0;
      if(upload.pieces && upload.offset > 0){
        // next piece of the running upload
        upload.lastPieceAt = millis();
        if(upload.error.length()) return;
        if(!otaActive || !Update.isRunning() || upload.offset != upload.written) failUpload("Upload out of order, start again.");
        return;
      }
      upload.headerChecked = false;
      upload.error = "";
      upload.lastPercent = -1;
      upload.written = 0;
      upload.total = upload.pieces ? (size_t)server.arg("total").toInt() : 0;
      upload.lastPieceAt = upload.pieces ? millis() : 0;
      DEBUG_I.printf("[OTA] Upload of %s started.\n", up.filename.c_str());
      otaActive = true;
      waitForCjkSlot();
      restoreDisplayAt = 0;
      drawMessageScreen("Firmware update", ST77XX_CYAN, nullptr);
      drawUpdateProgress(0);
      if(upload.pieces && (upload.total == 0 || upload.total > maxFirmwareSize())) failUpload("File is too large.");
      else if(!Update.begin(upload.pieces ? upload.total : UPDATE_SIZE_UNKNOWN, U_FLASH)) failUpload(Update.errorString());
      break;

    case UPLOAD_FILE_WRITE:
      feedLoopWDT();
      if(!upload.authorized || upload.error.length()) return;
      if(!upload.headerChecked){
        const char* err = validateFirmwareHeader(up.buf, up.currentSize);
        if(err){ failUpload(err); return; }
        upload.headerChecked = true;
      }
      if(upload.pieces && upload.written + up.currentSize > upload.total){ failUpload("More data than announced."); return; }
      if(Update.write(up.buf, up.currentSize) != up.currentSize){
        failUpload(Update.errorString());
        return;
      }
      upload.written += up.currentSize;
      {
        // single request: Content-Length includes the multipart framing, close enough for a progress bar
        int total = upload.pieces ? (int)upload.total : server.clientContentLength();
        int done = upload.pieces ? (int)upload.written : (int)up.totalSize;
        int percent = total > 0 ? (int)((uint64_t)done * 100 / total) : 0;
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
      if(upload.pieces && upload.written < upload.total) return;   // more pieces to come
      // end() verifies the checksum and the image before switching the boot partition
      if(!Update.end(true)) failUpload(Update.errorString());
      else DEBUG_I.printf("[OTA] Upload complete, %u bytes.\n", (unsigned)(upload.pieces ? upload.written : up.totalSize));
      upload.lastPieceAt = 0;
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
  if(!upload.authorized || !(setupMode || loginOk())){
    return server.requestAuthentication(DIGEST_AUTH, WEB_REALM);
  }
  if(upload.error.length() == 0 && !upload.headerChecked) failUpload("No file received.");
  if(upload.error.length()){
    upload.lastPieceAt = 0;
    otaActive = false;
    drawMessageScreen("Update failed", ST77XX_RED, upload.error.c_str());
    restoreDisplayAt = millis() + 5000;
    sendJson(400, false, "Update failed: " + upload.error);
    return;
  }
  if(upload.pieces && upload.written < upload.total){
    sendJson(200, true, "Received " + String((unsigned)upload.written) + " bytes.");
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
  if(loginOk()) return true;
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
  else b += "<form method=\"post\" action=\"/titles/show\"><p><button>Show all titles on the display</button></p></form>";
  b += "</div>";

  b += "<div class=\"card\"><h2>Channels</h2><table>";
  for(const channelInfo& ch : channels){
    b += "<tr><td>" + htmlEscape(!ch.name.empty() ? ch.name : !ch.login.empty() ? ch.login : ch.id) + "</td><td>";
    if(ch.id.empty()) b += "<span class=\"err\">not found</span>";
    else {
      b += ch.isLive ? "<span class=\"ok\">live</span>" : "offline";
      bool logoOk = ch.logoStatus == "stored" || ch.logoStatus == "downloading" || ch.logoStatus == "updating";
      b += String(" &middot; logo: <span class=\"") + (logoOk ? "muted" : "err") + "\">" + htmlEscape(ch.logoStatus) + "</span>";
    }
    b += "</td></tr>";
  }
  b += "</table>";
  b += "<form method=\"post\" action=\"/logos/reload\"><p><button>Reload logos</button></p></form>";
  if(!unknownChannels.empty()) b += "<p class=\"err\">Some channels are unknown to Twitch, see <a href=\"/settings\">Settings</a>.</p>";
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
  if(running){
    static uint32_t app = ESP.getSketchSize();   // verifies the whole image (SHA-256 of ~1.4 MB): once
    b += "<tr><td>Firmware size</td><td>" + String((unsigned)(app / 1024)) + " KB of " + String((unsigned)(running->size / 1024)) + " KB ("
         + String((unsigned)((uint64_t)app * 100 / running->size)) + " %)</td></tr>";
  }
  b += "<tr><td>Flash</td><td>" + String((unsigned)(ESP.getFlashChipSize() / (1024 * 1024))) + " MB</td></tr>";
  b += "<tr><td>RAM</td><td>" + String((unsigned)(ESP.getHeapSize() / 1024)) + " KB heap</td></tr>";
  b += "<tr><td>Uptime</td><td>" + formatDuration(millis() / 1000) + "</td></tr>";
  b += "<tr><td>Font</td><td>";
  switch(fontState){
    case FontState::Ready: {
      char crc[9]; snprintf(crc, sizeof(crc), "%08lx", (unsigned long)fontCrc);
      b += String("Noto Sans (font.bin ") + crc + ")";
      if(FONT_BIN_CRC && fontCrc != FONT_BIN_CRC) b += " <span class=\"muted\">- not this firmware's version" + String(fontDownloadWanted ? ", update pending" : "") + "</span>";
      break;
    }
    case FontState::NoPartition:
      b += "<span class=\"err\">built-in font: old partition layout, flash firmware-merged.bin over USB once</span>"; break;
    case FontState::Missing:
      b += "<span class=\"err\">built-in font: " + htmlEscape(fontProblem.c_str()) + ", downloading font.bin</span>"; break;
    case FontState::Failed:
      b += "<span class=\"err\">built-in font: font.bin " + htmlEscape(fontProblem.c_str()) + " (trying again later)</span>"; break;
  }
  b += "</td></tr>";
  b += "<tr><td>Emoji, CJK</td><td>";
  b += String(config.colourEmoji ? "colour emoji (Twemoji, CC-BY 4.0)" : "monochrome emoji") + ", CJK " + (config.cjk ? "downloaded" : "off");
  if(emojiCacheReady){
    xSemaphoreTake(emojiLock, portMAX_DELAY);
    size_t used = 0, emoji = 0, glyphs = 0;
    for(const auto& e : cacheIndex){ used += e.size; (e.type == CACHE_COLOUR ? emoji : glyphs)++; }
    xSemaphoreGive(emojiLock);
    b += "<br>cache: " + String((unsigned)emoji) + " emoji, " + String((unsigned)glyphs) + " CJK characters, "
         + String((unsigned)(used / 1024)) + " of " + String((unsigned)(cacheBytes / 1024)) + " KB";
  } else if(config.colourEmoji || config.cjk) b += " <span class=\"muted\">(needs font.bin)</span>";
  if(emojiError.length()) b += "<br><span class=\"err\">last problem: " + htmlEscape(emojiError.c_str()) + "</span>";
  if(emojiCacheReady) b += "<form method=\"post\" action=\"/cache/clear\" onsubmit=\"return confirm('Clear the emoji/CJK cache? Everything is downloaded again when needed.')\">"
                           "<p><button>Clear emoji/CJK cache</button></p></form>";
  b += "</td></tr>";
  if(!config.requireLogin) b += "<tr><td>Web login</td><td class=\"err\">off: anyone on the network can use this page</td></tr>";
  b += "<tr><td>WiFi</td><td>" + WiFi.SSID() + ", " + String(WiFi.RSSI()) + " dBm</td></tr>";
  b += "<tr><td>IP address</td><td>" + WiFi.localIP().toString() + "</td></tr>";
  b += "<tr><td>Free memory</td><td>" + String(ESP.getFreeHeap() / 1024) + " KB (lowest " + String(ESP.getMinFreeHeap() / 1024) + " KB, largest block "
       + String((unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024)) + " KB)</td></tr>";
  b += "<tr><td>Brightness sensor</td><td>" + String(ldr_avg) + "</td></tr>";
  b += "<tr><td>Title scrolling</td><td>";
  if(titleFrames < 2) b += "no title shown yet";
  else {
    unsigned long span = titleLastFrameUs - titleStartedUs;
    char fps[16];
    snprintf(fps, sizeof(fps), "%.1f", span ? (titleFrames - 1) * 1e6 / span : 0.0);
    b += String(fps) + " fps, longest frame gap " + String((unsigned)(titleMaxGapUs / 1000)) + " ms";
    char draw[192];
    snprintf(draw, sizeof(draw), "<br>drawing a frame takes %.1f ms (max %.1f ms, transfer %.1f ms), panel refresh %.1f ms, SPI clock %lu MHz",
             titleDrawUsSum / 1000.0 / titleFrames, titleDrawUsMax / 1000.0, titleXferUsSum / 1000.0 / titleFrames,
             FRAME_INTERVAL_US / 1000.0, (unsigned long)(spiFastClockHz() / 1000000));
    b += draw;
  }
  b += "</td></tr>";
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

void handleLogoReload(){
  if(!checkAuth()) return;
  logoReloadRequested = true;
  channelSyncRequested = true;
  requestUpdate();
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

// "Clear emoji/CJK cache": erases the cache sectors; emoji and CJK characters
// are downloaded again when titles need them (also those that failed before)
volatile bool cacheMissingReset = false;   // the network task forgets what it could not get
void handleCacheClear(){
  if(!checkAuth()) return;
  if(emojiCacheReady){
    xSemaphoreTake(emojiLock, portMAX_DELAY);
    feedLoopWDT();
    esp_partition_erase_range(fontPartition, emojiCacheStart, cacheBytes);   // a few seconds
    feedLoopWDT();
    cacheIndex.clear();
    cacheWriteOffset = 0;
    emojiError = "";
    xSemaphoreGive(emojiLock);
    cacheMissingReset = true;
    DEBUG_I.printf("[Cache] Cleared.\n");
  }
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

// "Show all titles": every live channel's title scrolls once more, in the
// order of the channel list (after a title that is scrolling right now)
void handleShowTitles(){
  if(!checkAuth()) return;
  for(auto& ch : channels){
    if(!ch.isLive || ch.slotNum < 0 || !hasTitleText(ch.streamTitle)) continue;
    if(isTitleDisplaying && channelTitleInfo.channel == &ch) continue;
    if(std::find(titleChangeQueue.begin(), titleChangeQueue.end(), &ch) == titleChangeQueue.end()) titleChangeQueue.push_back(&ch);
  }
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
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
  server.on("/logos/reload", HTTP_POST, handleLogoReload);
  server.on("/titles/show", HTTP_POST, handleShowTitles);
  server.on("/cache/clear", HTTP_POST, handleCacheClear);
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

// Splits the entered channel list (names separated by new lines, commas or
// spaces; twitch.tv links are accepted) into lower-case logins.
// Returns an empty string if the list is valid, otherwise the error.
String parseChannelList(const String& text, std::vector<std::string>& logins){
  logins.clear();
  std::string current;
  std::string all = std::string(text.c_str()) + "\n";
  for (char c : all) {
    if (c == '\n' || c == '\r' || c == ',' || c == ';' || c == ' ' || c == '\t') {
      if (!current.empty()) {
        size_t slash = current.find_last_of('/');
        if (slash != std::string::npos) current = current.substr(slash + 1); // twitch.tv/name
        if (!current.empty() && std::find(logins.begin(), logins.end(), current) == logins.end()) logins.push_back(current);
        current.clear();
      }
    } else current += (char)tolower((unsigned char)c);
  }
  for (const auto& l : logins) {
    bool ok = l.size() >= 1 && l.size() <= 25;
    for (char c : l) if (!(isalnum((unsigned char)c) || c == '_')) ok = false;
    if (!ok) return String("Invalid channel name: ") + l.c_str();
  }
  if (logins.empty()) return "Enter at least one channel.";
  if (logins.size() > MAX_CHANNELS) return "At most " + String(MAX_CHANNELS) + " channels.";
  return "";
}

// The channel list for the Settings page: logins where known, else ids
String channelListText(){
  String text;
  for (const auto& ch : channels) {
    if (text.length()) text += "\n";
    text += (ch.login.empty() ? ch.id : ch.login).c_str();
  }
  return text;
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

  b += "<h3>Channels</h3>";
  b += "<label>Twitch channels<textarea name=\"channels\" rows=\"6\" autocapitalize=\"none\" spellcheck=\"false\">";
  if(server.hasArg("channels")) b += escapeString(server.arg("channels"));
  else b += escapeString(channelListText());
  b += "</textarea></label>";
  b += "<p class=\"hint\">One channel name per line (as in twitch.tv/<i>name</i>), at most " + String(MAX_CHANNELS) + ". "
       "They are shown in this order; up to 8 live channels get a tile.</p>";
  b += "<label>Show a live channel's title again after<input type=\"number\" name=\"repeatmin\" min=\"0\" max=\"" + String(MAX_TITLE_REPEAT_MIN)
       + "\" value=\"" + inputValue("repeatmin", String(config.titleRepeatMinutes)) + "\"></label>";
  b += "<p class=\"hint\">Minutes without a title, 0 = off. The live channels take turns: the one whose title was shown longest ago comes next.</p>";
  {
    bool colour = server.hasArg("emoji") ? server.arg("emoji") != "mono" : config.colourEmoji;
    b += String("<label>Emoji in titles<select name=\"emoji\"><option value=\"colour\"") + (colour ? " selected" : "") + ">colour (downloaded, monochrome when that fails)</option>"
         "<option value=\"mono\"" + (colour ? "" : " selected") + ">monochrome only (no downloads)</option></select></label>";
    b += "<p class=\"hint\">Colour emoji are Twemoji images (CC-BY 4.0), downloaded once from cdn.jsdelivr.net and kept on the display.</p>";
    bool cjk = server.hasArg("cjk") ? server.arg("cjk") != "off" : config.cjk;
    b += String("<label>Chinese, Japanese, Korean<select name=\"cjk\"><option value=\"on\"") + (cjk ? " selected" : "") + ">downloaded when a title needs them</option>"
         "<option value=\"off\"" + (cjk ? "" : " selected") + ">not shown (boxes)</option></select></label>";
    b += "<p class=\"hint\">Characters from Noto Sans JP, KR and SC (fontsource), downloaded once per character; a title with many new ones can take half a minute to start.</p>";
  }
  if(!unknownChannels.empty()){
    String names;
    for(const auto& n : unknownChannels){ if(names.length()) names += ", "; names += n.c_str(); }
    b += "<p class=\"err\">Twitch does not know: " + escapeString(names) + "</p>";
  }

  b += "<h3>Web interface</h3>";
  b += "<p class=\"hint\">Login user is <b>admin</b>. The password is also used for uploads from PlatformIO.</p>";
  b += secretField("New password", "webpw", config.webPassword.length() > 0, nullptr, false);
  b += "<label>Repeat new password<input type=\"password\" name=\"webpw2\" autocomplete=\"new-password\"></label>";
  {
    bool require = server.hasArg("login_form") ? server.hasArg("login") : config.requireLogin;
    b += String("<input type=\"hidden\" name=\"login_form\" value=\"1\"><label class=\"check\"><input type=\"checkbox\" name=\"login\" value=\"1\"")
         + (require ? " checked" : "") + ">Require the password for the web interface</label>";
    b += "<p class=\"hint\">Without it, anyone on your WiFi can open these pages, change the settings and install firmware "
         "(stored passwords stay hidden). Uploads from PlatformIO always need the password.</p>";
  }

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
  bool repeatSubmitted = server.hasArg("repeatmin");
  String repeatText = server.arg("repeatmin"); repeatText.trim();
  long repeatMinutes = repeatText.toInt();
  if(repeatSubmitted && (repeatText.length() == 0 || repeatText.length() > 4 || (repeatMinutes == 0 && repeatText != "0")
                         || repeatMinutes < 0 || repeatMinutes > MAX_TITLE_REPEAT_MIN)){
    sendSettingsPage("Title repeat: enter minutes from 0 to " + String(MAX_TITLE_REPEAT_MIN) + ".", true);
    return;
  }
  std::vector<std::string> logins;
  bool channelsSubmitted = server.hasArg("channels");
  if(channelsSubmitted){
    String channelError = parseChannelList(server.arg("channels"), logins);
    if(channelError.length()){
      sendSettingsPage(channelError, true);
      return;
    }
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
  if(channelsSubmitted){
    String list;
    for(const auto& l : logins){ if(list.length()) list += ","; list += l.c_str(); }
    prefs.putString("channels", list);
  }
  if(repeatSubmitted) prefs.putString("repeatmin", String(repeatMinutes));
  if(server.hasArg("login_form")) prefs.putString("login", server.hasArg("login") ? "1" : "0");
  if(server.hasArg("emoji")) prefs.putString("emoji", server.arg("emoji") == "mono" ? "mono" : "colour");
  if(server.hasArg("cjk")) prefs.putString("cjk", server.arg("cjk") == "off" ? "off" : "on");
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
  checkUploadTimeout();
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

// Downloads font.bin into the font partition (titles must not be showing)
String downloadFontBin(const String& url){
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
    String e = "download failed (" + httpError(http, code) + ")";
    http.end();
    return e;
  }
  int size = http.getSize();
  if(size < 20 || (size_t)size > fontPartition->size){ http.end(); return "download failed: unexpected size " + String(size); }
  closeFont();
  esp_err_t err = esp_partition_erase_range(fontPartition, 0, ((size_t)size + 4095) & ~(size_t)4095);
  feedLoopWDT();
  if(err != ESP_OK){ http.end(); return "cannot erase the font partition"; }
  auto* stream = http.getStreamPtr();
  uint8_t buf[1024];
  size_t total = size, written = 0;
  int lastPercent = -1;
  unsigned long lastData = millis();
  String error;
  while(written < total){
    feedLoopWDT();
    size_t avail = stream->available();
    if(avail == 0){
      if(millis() - lastData > DOWNLOAD_TIMEOUT){ error = "download timed out"; break; }
      delay(1);
      continue;
    }
    size_t n = stream->readBytes(buf, std::min(std::min(avail, sizeof(buf)), total - written));
    if(n == 0) continue;
    lastData = millis();
    if(esp_partition_write(fontPartition, written, buf, n) != ESP_OK){ error = "cannot write the font partition"; break; }
    written += n;
    int percent = (int)((uint64_t)written * 100 / total);
    if(percent != lastPercent){
      lastPercent = percent;
      drawUpdateProgress(percent);
    }
  }
  http.end();
  if(error.isEmpty() && written != total) error = "download incomplete";
  return error;
}

// Fetches font.bin when it is missing or not this firmware's (called from loop())
void runFontDownload(){
  fontDownloadWanted = false;
  String url = fontUrl();
  DEBUG_I.printf("[Font] Downloading %s\n", url.c_str());
  otaActive = true; // pauses the network task
  drawMessageScreen("Font update", ST77XX_CYAN, "Downloading font.bin");
  drawUpdateProgress(0);
  String error = downloadFontBin(url);
  otaActive = false;
  openFont();   // whatever is in the partition now (on failure: the old font.bin, if it is still intact)
  fontDownloadWanted = false;
  if(error.isEmpty() && fontState != FontState::Ready) error = fontProblem;
  if(error.length()){
    DEBUG_W.printf("[Font] Download failed: %s\n", error.c_str());
    if(fontState != FontState::Ready){ fontState = FontState::Failed; fontProblem = error; }
    fontRetryAt = millis() + FONT_RETRY_INTERVAL;
    fontDownloadWanted = true;
  }
  restoreDisplay();
}

// Before an update writes the free firmware slot: wait until a CJK piece
// download that uses it has stopped (it stops when it sees otaActive)
void waitForCjkSlot(){
  extern volatile bool cjkSlotBusy;
  for(unsigned long t0 = millis(); cjkSlotBusy && millis() - t0 < 20000;){
    feedLoopWDT();
    delay(10);
  }
}

// Runs a requested online update (called from loop())
void runInstall(){
  DEBUG_I.printf("[OTA] Installing %s from %s\n", installVersion.c_str(), installUrl.c_str());
  otaActive = true; // pauses the network task
  waitForCjkSlot();
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
