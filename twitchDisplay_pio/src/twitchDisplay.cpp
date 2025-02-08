// TODO: add filters for json deserialization

#include <Arduino.h>
#include <Adafruit_GFX.h>    // Core graphics library
#include <Adafruit_ST7789.h> // Hardware-specific library for ST7789
#include <SPI.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <set>

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
WebSocketsClient webSocket;

void webSocketEvent(uint8_t index, WStype_t type, uint8_t * payload, size_t length);
bool twitchEventSub();
void updateLiveChannels();

#define WS_RECONNECT_TIMEOUT 1000

// pietsmiet, bonjwa, gronkh
//std::array<const char*, 3> channel_ids = {"21991090", "73437396", "12875057"};
// pietsmiet, bonjwa, gronkhtv
std::array<std::string, 3> channel_ids = {"21991090", "73437396", "106159308"};
const uint16_t* channel_pics[3] = { epd_bitmap_pietsmiet, epd_bitmap_bonjwa, epd_bitmap_gronkh};
//bool is_live[channel_ids.size()] = {false};
std::vector<std::string> live_channel_vec;

bool update_subbed[std::tuple_size<decltype(channel_ids)>::value] = {false};
bool online_subbed[std::tuple_size<decltype(channel_ids)>::value] = {false};
bool offline_subbed[std::tuple_size<decltype(channel_ids)>::value] = {false};

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

  DEBUG_I.printf("[%s] Configuring WebSocket...\n", DEBUG_TAG);
  webSocket.onEvent(
    std::bind(webSocketEvent, 0,
              std::placeholders::_1,
              std::placeholders::_2,
              std::placeholders::_3)
  );
  webSocket.setReconnectInterval(WS_RECONNECT_TIMEOUT);

  DEBUG_I.printf("[%s] Setup completed...\n", DEBUG_TAG);
}

GFXcanvas16 pic_canvas(64, 64); // 16-bit, 320x170 pixels
GFXcanvas16 text_canvas(298+(6*8), 64); 

enum State {
  Start,
  WS_Init,
  WS_WaitForReconnect,
  Twitch_EventSub_Init,
  Idle,
  Error
} state = Start;

#define MAX_RETRIES 10
uint8_t retries = 0;

unsigned long ws_reconnect_start = 0;
#define WS_INIT_RETRY_TIMEOUT 5000
unsigned long ws_init_start = 0;
#define TW_EVENTSUB_RETRY_TIMEOUT 1000
unsigned long tw_eventsub_start = 0;

#define TW_UPDATE_INTERVAL (10*60*1000)
unsigned long tw_last_update_start = 0; 
bool tw_update_now = true;

String twitch_session_id = String();

#define KEEPALIVE_TIMEOUT_S 30
#define KEEPALIVE_TIMEOUT_MS (KEEPALIVE_TIMEOUT_S*1000)
unsigned long last_session_keepalive = 0;

void loop(){
  //S.printf("[%s] Looping...\n", DEBUG_TAG);
  
  webSocket.loop();

  if(state == Start){
    DEBUG_I.printf("[%s] Start: WebSocket begin SSL...\n", DEBUG_TAG);
#ifdef TEST_SERVER
    webSocket.begin("192.168.6.61", 8080, "/ws?keepalive_timeout_seconds=" STR(KEEPALIVE_TIMEOUT_S));
#else
    webSocket.beginSSL("eventsub.wss.twitch.tv", 443, "/ws?keepalive_timeout_seconds=" STR(KEEPALIVE_TIMEOUT_S));
#endif
    DEBUG_I.printf("[%s] Start: Transitioning to WS_Init state...\n", DEBUG_TAG);
    state = WS_Init;
    ws_init_start = millis();
    retries++;
  }

  if(state == WS_Init){
    if (millis() - ws_init_start < WS_INIT_RETRY_TIMEOUT){
      return;
    }
    if(retries >= MAX_RETRIES) {
      DEBUG_E.printf("[%s] Max retries while initializing websocket connection\n", DEBUG_TAG);
      state = Error;
      return;
    }
    DEBUG_W.printf("[%s] WS_Init: Disconnect WebSocket... (Try: %u)\n", DEBUG_TAG, retries);
    webSocket.disconnect();
    // TODO: Think about how to best handle the automatic reconnect in ws lib
    ws_init_start = millis();
    retries++;
  }
  
  if(state == WS_WaitForReconnect){
    if (millis() - ws_reconnect_start < WS_RECONNECT_TIMEOUT){
      return;
    }
    DEBUG_W.printf("[%s] WS_WaitForReconnect: Automatic reconnect failed.\n", DEBUG_TAG);
    DEBUG_I.printf("[%s] WS_WaitForReconnect: Transition to WS_Init.\n", DEBUG_TAG);
    state = WS_Init;
    retries = 0;
    ws_init_start = millis();
  }

  if(state == Twitch_EventSub_Init){
    if (millis() - tw_eventsub_start < TW_EVENTSUB_RETRY_TIMEOUT){
      return;
    }
    if(retries >= MAX_RETRIES) {
      DEBUG_E.printf("[%s] Max retries while subscribing to EventSubs\n", DEBUG_TAG);
      state = Error;
      return;
    }
    retries++;
    DEBUG_I.printf("[%s] Twitch_EventSub_Init: Trying to sub events...\n", DEBUG_TAG);
    if(!twitchEventSub()){
      tw_eventsub_start = millis();
      return;
    }
    
    DEBUG_I.printf("[%s] Twitch_EventSub_Init: Transitioning to Idle state...\n", DEBUG_TAG);
    state = Idle;
    retries = 0;
  }

  if(state == Idle){
    if (tw_update_now || millis() - tw_last_update_start >= TW_UPDATE_INTERVAL){
      DEBUG_I.printf("[%s] Idle: Updating live channels...\n", DEBUG_TAG);
#ifndef TEST_SERVER
      updateLiveChannels();
#endif
      tw_last_update_start = millis();
      tw_update_now = false;
    }
    if (millis() - last_session_keepalive >= (KEEPALIVE_TIMEOUT_MS+5000)){
      DEBUG_W.printf("[%s] Session keepalive timeout\n", DEBUG_TAG);
      state = WS_Init;
      ws_init_start = 0;
      retries = 0;
    }
  }

  if(state == Error){
    DEBUG_E.println("Unrecoverable error... restarting...");
    delay(1000);
    ESP.restart();
  }
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

  // TODO: also poulate second pic line
  tft.drawRGBBitmap(11+((64+14)*pic_slot_index), 14, pic_canvas.getBuffer(), pic_canvas.width(), pic_canvas.height());
  
}

void redrawLiveChannelPics(){
  tft.fillScreen(ST77XX_BLACK);
  
  for (std::size_t i = 0; i < live_channel_vec.size(); i++) {
    auto it = std::find(std::begin(channel_ids), std::end(channel_ids), live_channel_vec[i]);
    drawLiveChannelPic(live_channel_vec[i].c_str(), i, it);
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
  tft.fillScreen(ST77XX_BLACK);

  for (JsonObject channel : doc["data"].as<JsonArray>()) {
    const char* name = channel["user_name"];
    const char* id = channel["user_id"];
    // TODO: Error checking?
    DEBUG_I.printf("[%s] Live channel: %s\n", DEBUG_TAG, name);
    setIDLiveStatus(id, true);
  }
}

void processTwitchNotification(const JsonDocument& local_doc){
  const char* subscription_type = local_doc["metadata"]["subscription_type"];
  if(!subscription_type){
    DEBUG_W.printf("[JSON] subscription_type not available\n");
    return;
  }

  DEBUG_I.printf("[%s] got notification: %s\n", DEBUG_TAG, subscription_type);

  if(strcmp(subscription_type,"stream.online")==0){
    const char* id = local_doc["payload"]["event"]["broadcaster_user_id"];
    const char* name = local_doc["payload"]["event"]["broadcaster_user_name"];
    if(!id || !name){
      DEBUG_W.printf("[JSON] id or name not available\n", DEBUG_TAG);
      return;
    }
    DEBUG_I.printf("[%s] Now live: %s\n", DEBUG_TAG, name);
    setIDLiveStatus(id, true);
  }
  else if(strcmp(subscription_type,"stream.offline")==0){
    const char* id = local_doc["payload"]["event"]["broadcaster_user_id"];
    const char* name = local_doc["payload"]["event"]["broadcaster_user_name"];
    if(!id || !name){
      DEBUG_W.printf("[JSON] id or name not available\n", DEBUG_TAG);
      return;
    }
    DEBUG_I.printf("[%s] Now offline: %s\n", DEBUG_TAG, name);
    setIDLiveStatus(id, false);
  }
  else if(strcmp(subscription_type,"channel.update")==0){
    const char* id = local_doc["payload"]["event"]["broadcaster_user_id"];
    const char* name = local_doc["payload"]["event"]["broadcaster_user_name"];
    const char* title = local_doc["payload"]["event"]["title"];
    const char* category_name = local_doc["payload"]["event"]["category_name"];
    if(!id || !name || !title || !category_name){
      DEBUG_W.printf("[JSON] id or name or title or category_name not available\n", DEBUG_TAG);
      return;
    }
    DEBUG_I.printf("[%s] Channel %s updated title or category: %s | %s\n", DEBUG_TAG, name, title, category_name);
    // TODO: add change to draw queue and draw scrolling text
  }
}

void webSocketEvent(uint8_t index, WStype_t type, uint8_t * payload, size_t length) {
  
  switch(type) {
    case WStype_DISCONNECTED:
      DEBUG_W.print("[WSc] Disconnected!\n"); 
      DEBUG_I.printf("[%s] Resetting everything and transition to WS_WaitForReconnect state...\n", DEBUG_TAG);
      twitch_session_id.clear();
      memset(update_subbed, false, sizeof update_subbed);
      memset(online_subbed, false, sizeof online_subbed);
      memset(offline_subbed, false, sizeof offline_subbed);
      retries = 0;
      state = WS_WaitForReconnect;
      ws_reconnect_start = millis();
      // TODO?
      break;
    case WStype_CONNECTED:
      DEBUG_I.printf("[WSc] Connected to url: %s\n", payload);
      break;
    case WStype_TEXT: {
      DEBUG_I.print("[WSc] got text\n");
      
      DEBUG_I.print("[JSON] Deserialize message...\n");
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload);
      if (error) {
        DEBUG_I.print("deserializeJson() failed: ");
        DEBUG_I.println(error.f_str());
        return;
      }

      const char* message_type = doc["metadata"]["message_type"];
      if(!message_type){
        DEBUG_W.printf("[%s] message_type not available\n", DEBUG_TAG);
        return;
      }

      DEBUG_I.printf("[%s] got message: %s\n", DEBUG_TAG, message_type);

      if(strcmp(message_type,"session_welcome")==0){
        last_session_keepalive = millis();
        const char* session_id = doc["payload"]["session"]["id"];
        if(!session_id){
          DEBUG_W.printf("[%s] %s: session_id not available\n", DEBUG_TAG, message_type);
          return;
        }
        DEBUG_I.printf("[%s] %s: session id: %s\n", DEBUG_TAG, message_type, session_id);
        // Copies session_id to global var
        twitch_session_id = session_id;
        DEBUG_I.printf("[%s] Transitioning to Twitch_EventSub_Init state\n", DEBUG_TAG);
        state = Twitch_EventSub_Init;
        retries = 0;
        tw_eventsub_start = 0;
      }
      else if(strcmp(message_type,"session_keepalive")==0){
        last_session_keepalive = millis();
      }
      else if(strcmp(message_type,"notification")==0){
        last_session_keepalive = millis();
        processTwitchNotification(doc);
      }
      else if(strcmp(message_type,"session_reconnect")==0){
        // TODO: https://dev.twitch.tv/docs/eventsub/handling-websocket-events/#reconnect-message
      }
      else if(strcmp(message_type,"revocation")==0){
       // TODO: https://dev.twitch.tv/docs/eventsub/handling-websocket-events/#revocation-message
      }
      
    }
    break;
    case WStype_BIN:
      DEBUG_W.printf("[WSc] get binary length: %u\n", length);
    case WStype_ERROR:      
    case WStype_FRAGMENT_TEXT_START:
    case WStype_FRAGMENT_BIN_START:
    case WStype_FRAGMENT:
    case WStype_FRAGMENT_FIN:
      DEBUG_W.printf("[WSc] recieved unsopported type: %d\n", type);
      break;
  }
}

#ifdef TEST_SERVER
// No condition for test server
bool sendPOSTRequest(HTTPClient& http_client, const char* type, const char* version){
  // start connection and send HTTP header
  DEBUG_I.printf("[HTTP] POST request: %s\n", type);
  int httpCode = http_client.POST(String("{\"type\": \"") + type + "\",\"version\": \"" + version + "\",\"transport\": {\"method\": \"websocket\",\"session_id\": \"" + twitch_session_id + "\"}}");
#else
bool sendPOSTRequest(HTTPClient& http_client, const char* type, const char* version, const char* channel_id){
  // start connection and send HTTP header
  DEBUG_I.printf("[HTTP] POST request: %s, %s\n", type, channel_id);
  int httpCode = http_client.POST(String("{\"type\": \"") + type + "\",\"version\": \"" + version + "\",\"condition\": {\"broadcaster_user_id\": \"" + channel_id + "\"},\"transport\": {\"method\": \"websocket\",\"session_id\": \"" + twitch_session_id + "\"}}");
#endif

  String payload=http_client.getString();
  DEBUG_I.println("[HTTP] response: ");
  DEBUG_I.println(payload);

  if (httpCode != HTTP_CODE_ACCEPTED) {
    DEBUG_W.printf(
      "[HTTP] POST... failed, error: %s\n",
      (httpCode<=0)?
        http_client.errorToString(httpCode).c_str():
        String(httpCode).c_str());
    
    return false;
  }

  /* Could verify response but i dont know if thats really necessary
  
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http_client.getStream());
  if (err) {
    S.print("deserializeJson() failed with code ");
    S.println(err.f_str());
    return false;
  }

  const char* sub_type = doc["data"][0]["type"];
  if(!sub_type){
    S.printf("[%s] sub_type not available\n", DEBUG_TAG);
    return false;
  }

  if(strcmp(sub_type,type)!=0){
    S.printf("[%s] anwser type doesn't match request type\n", DEBUG_TAG);
    return false;
  }
  */
  return true;
}

#ifdef TEST_SERVER
bool twitchEventSub(){

  HTTPClient http;
  http.begin("http://192.168.6.61:8080/eventsub/subscriptions");
  commonHttpInit(http);
  http.addHeader("Content-Type", "application/json");
  
  if(!update_subbed[0]) update_subbed[0]  = sendPOSTRequest(http, "channel.update", "2");
  if(!online_subbed[0]) online_subbed[0]  = sendPOSTRequest(http, "stream.online", "1");
  if(!offline_subbed[0]) offline_subbed[0]= sendPOSTRequest(http, "stream.offline", "1");

  http.end();

  if (!update_subbed[0]
      ||
      !online_subbed[0]
      ||
      !offline_subbed[0]
      ) {
    DEBUG_W.printf("[%s] Failed to sub to all events.\n", DEBUG_TAG);  
    return false;
  }
  DEBUG_I.printf("[%s] Successfully subscribed to all events.\n", DEBUG_TAG);
  return true;
}
#else
bool twitchEventSub(){

  HTTPClient http;
  http.begin("https://api.twitch.tv/helix/eventsub/subscriptions");
  commonHttpInit(http);
  http.addHeader("Content-Type", "application/json");
  
  // TODO: do not block main loop for this long
  for(uint8_t i=0;i<channel_ids.size();i++){
    if(!update_subbed[i]) update_subbed[i]  = sendPOSTRequest(http, "channel.update", "2", channel_ids[i].c_str());
    if(!online_subbed[i]) online_subbed[i]  = sendPOSTRequest(http, "stream.online", "1", channel_ids[i].c_str());
    if(!offline_subbed[i]) offline_subbed[i]= sendPOSTRequest(http, "stream.offline", "1", channel_ids[i].c_str());
  }

  http.end();

  if (!std::all_of(
        std::begin(update_subbed), std::end(update_subbed), 
        [](bool i){return i;})
      ||
      !std::all_of(
        std::begin(online_subbed), std::end(online_subbed), 
        [](bool i){return i;})
      ||
      !std::all_of(
        std::begin(offline_subbed), std::end(offline_subbed), 
        [](bool i){return i;})
      ) {
    DEBUG_W.printf("[%s] Failed to sub to all events.\n", DEBUG_TAG);  
    return false;
  }
  DEBUG_I.printf("[%s] Successfully subscribed to all events.\n", DEBUG_TAG);
  return true;
}
#endif