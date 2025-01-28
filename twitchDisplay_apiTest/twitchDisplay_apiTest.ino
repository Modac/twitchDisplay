/**
 *  TODO:
 *  - Handle Twitch Websocket msgs (https://dev.twitch.tv/docs/eventsub/handling-websocket-events/)
 *    - Welcome message
 *    - Keepalive message
 *    - Ping message (handled by websockets lib)
 *    - Reconnect message
 *    - Revocation message
 *    - Close message (handled by websockets lib)
 */
 
#include "secrets.h"

#include <Arduino.h>

#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>

#include <HTTPClient.h>
#include <WebSocketsClient.h>

#include <ArduinoJson.h>

#define USE_SERIAL Serial

WiFiMulti wifiMulti;
WebSocketsClient webSocket;

void hexdump(const void *mem, uint32_t len, uint8_t cols = 16) {
  const uint8_t* src = (const uint8_t*) mem;
  USE_SERIAL.printf("\n[HEXDUMP] Address: 0x%08X len: 0x%X (%d)", (ptrdiff_t)src, len, len);
  for(uint32_t i = 0; i < len; i++) {
    if(i % cols == 0) {
      USE_SERIAL.printf("\n[0x%08X] 0x%08X: ", (ptrdiff_t)src, i);
    }
    USE_SERIAL.printf("%02X ", *src);
    src++;
  }
  USE_SERIAL.printf("\n");
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {

  switch(type) {
    case WStype_DISCONNECTED:
      USE_SERIAL.printf("[WSc] Disconnected! (length: %u)\n", length);
      break;
    case WStype_CONNECTED:
      USE_SERIAL.printf("[WSc] Connected to url: %s\n", payload);

      // send message to server when Connected
      //webSocket.sendTXT("Connected");
      break;
    case WStype_TEXT:
      {
      USE_SERIAL.printf("[WSc] get text: %s\n", payload);
      
      // Deserialize the JSON document
      DynamicJsonDocument doc(2048);
      DeserializationError error = deserializeJson(doc, payload);

      // Test if parsing succeeds.
      if (error) {
        USE_SERIAL.print(F("deserializeJson() failed: "));
        USE_SERIAL.println(error.f_str());
        return;
      }

      const char* message_type = doc["metadata"]["message_type"];

      USE_SERIAL.printf("[TwitchApi] got message: %s\n", message_type);

      if(strcmp_P(message_type,(PGM_P) F("session_welcome"))==0){
        const char* session_id = doc["payload"]["session"]["id"];
        USE_SERIAL.printf("[TwitchApi] session id: %s\n", session_id);
      }
      
      // send message to server
      // webSocket.sendTXT("message here");
      }
      break;
    case WStype_BIN:
      USE_SERIAL.printf("[WSc] get binary length: %u\n", length);
      hexdump(payload, length);

      // send data to server
      // webSocket.sendBIN(payload, length);
      break;
    case WStype_ERROR:      
    case WStype_FRAGMENT_TEXT_START:
    case WStype_FRAGMENT_BIN_START:
    case WStype_FRAGMENT:
    case WStype_FRAGMENT_FIN:
      break;
  }

}


void setup() {

  USE_SERIAL.begin(115200);

  USE_SERIAL.setDebugOutput(true);
  
  USE_SERIAL.println();
  USE_SERIAL.println();
  USE_SERIAL.println();

  for (uint8_t t = 4; t > 0; t--) {
    USE_SERIAL.printf("[SETUP] WAIT %d...\n", t);
    USE_SERIAL.flush();
    delay(1000);
  }

  wifiMulti.addAP("::1", WIFI_PW);
  
  // wait for WiFi connection
  while((wifiMulti.run() != WL_CONNECTED)){
    delay(10);
  }
  twitchCheckOnlineChannels();

  delay(1000);
  
  webSocket.beginSSL("eventsub.wss.twitch.tv", 443, "/ws");
  // webSocket.begin("192.168.6.61", 1234, "/");
  
  // event handler
  webSocket.onEvent(webSocketEvent);

  // try ever 5000 again if connection has failed
  webSocket.setReconnectInterval(10000);
}

void loop() {
  webSocket.loop();
}

void twitchCheckOnlineChannels(){
  HTTPClient http;

    USE_SERIAL.print("[HTTP] begin...\n");
    http.useHTTP10(true);
    http.begin("https://api.twitch.tv/helix/streams?user_login=gronkhtv&user_login=gronkh&user_login=pietsmiet");  //HTTP

    USE_SERIAL.print("[HTTP] adding Headers...\n");
    // add headers
    http.setAuthorizationType("Bearer");
    http.setAuthorization(TWITCH_TOKEN);
    http.addHeader("Client-Id", "gp762nuuoqcoxypju8c569th9wz7q5");
    
    USE_SERIAL.print("[HTTP] GET...\n");
    // start connection and send HTTP header
    int httpCode = http.GET();

    // httpCode will be negative on error
    if (httpCode > 0) {
      // HTTP header has been send and Server response header has been handled
      USE_SERIAL.printf("[HTTP] GET... code: %d\n", httpCode);

      // file found at server
      if (httpCode == HTTP_CODE_OK) {
        //String payload = http.getString();
        //USE_SERIAL.println(payload);
        
        DynamicJsonDocument doc(2048);
        DeserializationError err = deserializeJson(doc, http.getStream());
        if (err) {
          Serial.print(F("deserializeJson() failed with code "));
          Serial.println(err.f_str());
        } else {
          for (JsonObject channel : doc["data"].as<JsonArray>()) {
            const char* name = channel["user_name"];
            USE_SERIAL.print("Live channel: ");
            USE_SERIAL.println(name);
            // etc.
          }
        }
        
      }
    } else {
      USE_SERIAL.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
    }

    http.end();
}
