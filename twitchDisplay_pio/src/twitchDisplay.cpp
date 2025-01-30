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
#define USE_SERIAL true
#define S if(USE_SERIAL)Serial

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
std::array<const char*, 3> channel_ids = {"21991090", "73437396", "106159308"};
bool is_live[channel_ids.size()] = {false};
std::vector<const char*> live_channel_vec;

bool update_subbed[channel_ids.size()] = {false};
bool online_subbed[channel_ids.size()] = {false};
bool offline_subbed[channel_ids.size()] = {false};

void setup(void) {
  live_channel_vec.reserve(channel_ids.size());

  S.begin(115200);
  //S.setDebugOutput(true);
  S.printf_P(PSTR("[%s] Starting...\n"), DEBUG_TAG);

  wifiMulti.addAP("::1", WIFI_PW);

  pinMode(TFT_BK, OUTPUT);
  digitalWrite(TFT_BK, HIGH);

  tft.init(170, 320);           // Init ST7789 170x320
  tft.setRotation(3);
  tft.setSPISpeed(80000000);
  
  tft.fillScreen(ST77XX_BLACK);

  // wait for WiFi connection
  while((wifiMulti.run() != WL_CONNECTED)){
    delay(10);
  }

  webSocket.onEvent(
    std::bind(webSocketEvent, 0,
              std::placeholders::_1,
              std::placeholders::_2,
              std::placeholders::_3)
  );
  webSocket.setReconnectInterval(WS_RECONNECT_TIMEOUT);
}

GFXcanvas16 canvas(64, 64); // 16-bit, 320x170 pixels
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
    state = WS_Init;
    webSocket.beginSSL("eventsub.wss.twitch.tv", 443, "/ws?keepalive_timeout_seconds=" STR(KEEPALIVE_TIMEOUT_S));
    ws_init_start = millis();
    retries++;
  }

  if(state == WS_Init){
    if (millis() - ws_init_start < WS_INIT_RETRY_TIMEOUT){
      return;
    }
    if(retries >= MAX_RETRIES) {
      state = Error;
      S.printf("[%s] Max retries while initializing websocket connection\n", DEBUG_TAG);
      return;
    }
    webSocket.disconnect();
    // TODO: Think about how to best handle the automatic reconnect in ws lib
    ws_init_start = millis();
    retries++;
  }
  
  if(state == WS_WaitForReconnect){
    if (millis() - ws_reconnect_start < WS_RECONNECT_TIMEOUT){
      return;
    }
    state = WS_Init;
    retries = 0;
    ws_init_start = millis();
  }

  if(state == Twitch_EventSub_Init){
    if (millis() - tw_eventsub_start < TW_EVENTSUB_RETRY_TIMEOUT){
      return;
    }
    if(retries >= MAX_RETRIES) {
      state = Error;
      S.printf("[%s] Max retries while subscribing to EventSubs\n", DEBUG_TAG);
      return;
    }
    retries++;
    if(!twitchEventSub()){
      tw_eventsub_start = millis();
      return;
    }
    state = Idle;
    retries = 0;
  }

  if(state == Idle){
    if (tw_update_now || millis() - tw_last_update_start >= TW_UPDATE_INTERVAL){
      updateLiveChannels();
      tw_last_update_start = millis();
      tw_update_now = false;
    }
    if (millis() - last_session_keepalive >= (KEEPALIVE_TIMEOUT_MS+5000)){
      S.printf("[%s] Session keepalive timeout\n", DEBUG_TAG);
      state = WS_Init;
      ws_init_start = 0;
      retries = 0;
    }
  }

  if(state == Error){
    S.println("Unrecoverable error... restarting...");
    delay(1000);
    ESP.restart();
  }
}

void commonHttpInit(HTTPClient& http_client){
  S.print("[HTTP] init...\n");
  http_client.useHTTP10(true);
  
  // add headers
  http_client.setAuthorizationType("Bearer");
  http_client.setAuthorization(TWITCH_TOKEN);
  http_client.addHeader("Client-Id", "gp762nuuoqcoxypju8c569th9wz7q5");
}

void setIDLiveStatus(const char* id, bool now_live){
  auto it = std::find(channel_ids.begin(), channel_ids.end(), id);
  auto i = std::distance(channel_ids.begin(), it);
  is_live[i] = now_live;
}

void updateLiveChannels(){
  S.printf("[%s] Updating Live Channels\n", DEBUG_TAG);
  HTTPClient http;
  // Could maybe be done as constexpr??
  String url = String("https://api.twitch.tv/helix/streams?");
  for (const auto& id : channel_ids) {
    url += "user_id=";
    url += id;
    url += "&"; 
  }
  
  http.begin(url);  //HTTP
  commonHttpInit(http);

  S.print("[HTTP] GET...\n");
  S.printf("[HTTP] url: %s\n", url.c_str());
  // start connection and send HTTP header
  int httpCode = http.GET();
  
  
  if (httpCode != HTTP_CODE_OK) {
    S.printf(
      "[HTTP] GET... failed, error: %s\n",
      (httpCode<=0)?
        http.errorToString(httpCode).c_str():
        String(httpCode).c_str());
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  if (err) {
    S.print("deserializeJson() failed with code ");
    S.println(err.f_str());
    return;
  }

  // Reset is_live array
  memset(is_live, false, sizeof is_live);

  for (JsonObject channel : doc["data"].as<JsonArray>()) {
    const char* name = channel["user_name"];
    const char* id = channel["user_id"];
    // TODO: Error checking?
    S.printf("[%s] Live channel: %s\n", DEBUG_TAG, name);
    setIDLiveStatus(id, true);
  }
}

void processTwitchNotification(const JsonDocument& local_doc){
  const char* subscription_type = local_doc["metadata"]["subscription_type"];
  if(!subscription_type){
    S.printf("[%s] subscription_type not available\n", DEBUG_TAG);
    return;
  }

  S.printf("[%s] got notification: %s\n", DEBUG_TAG, subscription_type);

  if(strcmp(subscription_type,"stream.online")==0){
    const char* id = local_doc["payload"]["event"]["broadcaster_user_id"];
    const char* name = local_doc["payload"]["event"]["broadcaster_user_name"];
    if(!id || !name){
      //S.printf("[%s] id or name not available\n", DEBUG_TAG);
      return;
    }
    S.printf("[%s] Now live: %s", DEBUG_TAG, name);
    setIDLiveStatus(id, true);
  }
  else if(strcmp(subscription_type,"stream.offline")==0){
    const char* id = local_doc["payload"]["event"]["broadcaster_user_id"];
    const char* name = local_doc["payload"]["event"]["broadcaster_user_name"];
    if(!id || !name){
      //S.printf("[%s] id or name not available\n", DEBUG_TAG);
      return;
    }
    S.printf("[%s] Now offline: %s", DEBUG_TAG, name);
    setIDLiveStatus(id, false);
  }
  else if(strcmp(subscription_type,"channel.update")==0){
    const char* id = local_doc["payload"]["event"]["broadcaster_user_id"];
    const char* name = local_doc["payload"]["event"]["broadcaster_user_name"];
    const char* title = local_doc["payload"]["event"]["title"];
    const char* category_name = local_doc["payload"]["event"]["category_name"];
    if(!id || !name || !title || !category_name){
      //S.printf("[%s] id or name or title or category_name not available\n", DEBUG_TAG);
      return;
    }
    S.printf("[%s] Channel: %s updated title or category: %s | %s", DEBUG_TAG, name, title, category_name);
    // TODO
  }
}

void webSocketEvent(uint8_t index, WStype_t type, uint8_t * payload, size_t length) {
  
  switch(type) {
    case WStype_DISCONNECTED:
      S.print("[WSc] Disconnected!\n");
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
      S.printf("[WSc] Connected to url: %s\n", payload);
      break;
    case WStype_TEXT: {
      S.print("[WSc] got text\n");
      
      // Deserialize the JSON document
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload);

      // Test if parsing succeeds.
      if (error) {
        S.print(F("deserializeJson() failed: "));
        S.println(error.f_str());
        return;
      }

      const char* message_type = doc["metadata"]["message_type"];
      if(!message_type){
        S.printf("[%s] message_type not available\n", DEBUG_TAG);
        return;
      }

      S.printf("[%s] got message: %s\n", DEBUG_TAG, message_type);

      if(strcmp(message_type,"session_welcome")==0){
        last_session_keepalive = millis();
        const char* session_id = doc["payload"]["session"]["id"];
        if(!session_id){
          S.printf("[%s] session_id not available\n", DEBUG_TAG);
          return;
        }
        S.printf("[%s] session id: %s\n", DEBUG_TAG, session_id);
        // Copies session_id to global var
        twitch_session_id = session_id;
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
      S.printf("[WSc] get binary length: %u\n", length);
    case WStype_ERROR:      
    case WStype_FRAGMENT_TEXT_START:
    case WStype_FRAGMENT_BIN_START:
    case WStype_FRAGMENT:
    case WStype_FRAGMENT_FIN:
      S.printf("[WSc] recieved unsopported type: %d\n", type);
      break;
  }
}

bool sendPOSTRequest(HTTPClient& http_client, const char* type, const char* version, const char* channel_id){
  // start connection and send HTTP header
  S.printf("[HTTP] POST request: %s, %s\n", type, channel_id);
  int httpCode = http_client.POST(String("{\"type\": \"") + type + "\",\"version\": \"" + version + "\",\"condition\": {\"broadcaster_user_id\": \"" + channel_id + "\"},\"transport\": {\"method\": \"websocket\",\"session_id\": \"" + twitch_session_id + "\"}}");

  if (httpCode != HTTP_CODE_ACCEPTED) {
    S.printf(
      "[HTTP] POST... failed, error: %s\n",
      (httpCode<=0)?
        http_client.errorToString(httpCode).c_str():
        String(httpCode).c_str());
    
    String payload=http_client.getString();
    S.println(payload);
    
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
  String payload=http_client.getString();
  S.println(payload);
  return true;
}

bool twitchEventSub(){

  HTTPClient http;
  //commonHttpInit(http);
  //http.useHTTP10(true);
  http.begin("https://api.twitch.tv/helix/eventsub/subscriptions");
  //http.setAuthorizationType("Bearer");
  //http.setAuthorization(TWITCH_TOKEN);
  //http.addHeader("Client-Id", "gp762nuuoqcoxypju8c569th9wz7q5");
  commonHttpInit(http);
  http.addHeader("Content-Type", "application/json");
  
  for(uint8_t i=0;i<channel_ids.size();i++){
    // TODO
    if(!update_subbed[i]) update_subbed[i]  = sendPOSTRequest(http, "channel.update", "2", channel_ids[i]);
    if(!online_subbed[i]) online_subbed[i]  = sendPOSTRequest(http, "stream.online", "1", channel_ids[i]);
    if(!offline_subbed[i]) offline_subbed[i]= sendPOSTRequest(http, "stream.offline", "1", channel_ids[i]);
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
    return false;
  }
  S.println("All EventSubs successfull\n");
  return true;
}