// TODO: add filters for json deserialization
// TODO: add title display
// TODO: add unicode support (e.g. with https://github.com/takkaO/OpenFontRender/tree/master and Noto Sans + Noto Emoji, but together at least 1,5MB)
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
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <set>
#include <ArduinoOTA.h>

#include "pics.h"
#include "secrets.h"

#include "LowPass.h"
#include "MovingAverage.h"
#include <deque>

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
WiFiUDP Udp;

// Filter instance
LowPass<2> lp(0.1,1e3,true);
MovingAverage<100> ma;
uint16_t ldr;
uint16_t ldr_f;
uint16_t ldr_f2;

void redrawLiveChannelPics(int8_t row = -1);
void updateLiveChannels();
void setupOTA();

struct channelInfo {
  std::string id;
  bool isLive;
  std::string streamTitle;
  int8_t slotNum;
  const uint16_t* pic;
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

struct titleQueueItem {
  channelInfo& channel;
  std::string newTitle;
};

std::deque<channelInfo*> titleChangeQueue;

// pietsmiet, bonjwa, gronkhtv
//std::array<std::string, 3> channel_ids = {"21991090", "73437396", "106159308"};

GFXcanvas16 pic_canvas(PIC_SIZE, PIC_SIZE);
// one character (6*8 px at text size 8) wider than the visible title so it can scroll smoothly
GFXcanvas16 text_canvas(TITLE_WIDTH+(6*8), PIC_SIZE);

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

  text_canvas.setCursor(0,0);
  text_canvas.setTextWrap(false);
  text_canvas.setTextColor(ST77XX_CYAN);
  text_canvas.setTextSize(8);

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

enum State {
  Idle,
  Error
} state = Idle;

//#define MAX_RETRIES 10
//uint8_t retries = 0;

#define TW_UPDATE_INTERVAL (30*1000)
unsigned long tw_last_update_start = 0; 
bool tw_update_now = true;

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
      //enterNormalMode();
      o_X = (o_X+6)%(6*8);

      if (!o_X && channelTitleInfo.textOffset==0) {
        if(++channelTitleInfo.repeats==MAX_TITLE_REPEAT){
          stopTitle();
        }
      }

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

// Returns nullptr if id is null or not in the channel list.
channelInfo* setIDLiveStatus(const char* id, bool now_live, bool draw_immediat=true){
  if(id == nullptr) {
    DEBUG_W.printf("[%s] Cannot set live status of channel without id.\n", DEBUG_TAG);
    return nullptr;
  }
  // custom find_if to search by id inside of struct
  channelInfo* channel = std::find_if(channels.begin(), channels.end(), [&](const channelInfo& x){return x.id == id;});
  if(channel == channels.end()) {
    DEBUG_W.printf("[%s] Cannot set live status of unknown channel id %s.\n", DEBUG_TAG, id);
    return nullptr;
  }
  if(now_live){
    if (channel->isLive) {
      DEBUG_I.printf("[%s] Channel %s already live.\n", DEBUG_TAG, id);
      return channel;
    }
    DEBUG_I.printf("[%s] Setting channel %s to live...\n", DEBUG_TAG, id);
    channel->isLive = true;
    live_num++;
    DEBUG_I.printf("[%s] There are now %d live channels.\n", DEBUG_TAG, live_num);
    if(draw_immediat) drawLiveChannelPic(*channel, live_num-1);
  } else {
    DEBUG_I.printf("[%s] Unsetting channel %s live status\n", DEBUG_TAG, id);
    channel->isLive = false;
    channel->slotNum = -1;
    assignSlots();
    DEBUG_I.printf("[%s] There are now %d live channels.\n", DEBUG_TAG, live_num);
    if(draw_immediat) redrawLiveChannelPics();
  }
  return channel;
}

void updateLiveChannels(){
  HTTPClient http;
  // Could maybe be done as constexpr??
  String url = String("https://api.twitch.tv/helix/streams?");
  for (auto& channel : channels) {
    url += "user_id=";
    url += channel.id.c_str();
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

  DEBUG_I.print("[JSON] Deserializing response...\n");

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  if (err) {
    DEBUG_W.print("[JSON] deserializeJson() failed with code ");
    DEBUG_W.println(err.f_str());
    return;
  }

  // Reset is_live array
  //memset(is_live, false, sizeof is_live);
  DEBUG_I.printf("[%s] Resetting all channels live status...\n", DEBUG_TAG);
  // slotNum is kept so assignSlots() can detect layout changes
  for (auto &&c : channels) {
    c.isLive = false;
  }
  live_num = 0;
  
  //tft.fillScreen(ST77XX_BLACK);

  
  for (JsonObject channel : doc["data"].as<JsonArray>()) {
    const char* name = channel["user_name"];
    const char* id = channel["user_id"];
    std::string title = "    ";
    title += channel["title"].as<std::string>();
    title += " | ";
    title += channel["game_name"].as<std::string>();
    title += "   ";
    // TODO: Error checking?
    DEBUG_I.printf("[%s] Live channel: %s\n", DEBUG_TAG, name);
    channelInfo* ch = setIDLiveStatus(id, true, false);
    if(ch == nullptr) continue;
    if(ch->streamTitle != title) {
      ch->streamTitle = title;
      // queue channel for title display if not already in queue
      if (std::find_if(titleChangeQueue.begin(), titleChangeQueue.end(), [ch](const channelInfo* x){return x == ch;}) == titleChangeQueue.end()) {
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