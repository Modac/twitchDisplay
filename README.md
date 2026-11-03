# twitchDisplay – Arduino IDE sketches

Early test sketches for the twitchDisplay (ESP32-C3 with a 1.9" 170×320
ST7789 display), written for the Arduino IDE. The current firmware and
newer versions of these tests are on the `platform/platformio` branch.

| Sketch | Purpose |
|--------|---------|
| `twitchDisplay_displayTest/` | Display test with logos (`pics.h`) and smooth, tear-free text scrolling |
| `twitchDisplay_apiTest/` | Twitch Helix "live streams" request and EventSub websocket subscription test |

## Hardware

| Function  | GPIO |
|-----------|------|
| TFT SCK   | 4    |
| TFT MOSI  | 6    |
| TFT CS    | 7    |
| TFT DC    | 1    |
| TFT RST   | 10   |

## Setup

1. Install the ESP32 board package and select an ESP32-C3 board
   (e.g. "ESP32C3 Dev Module").
2. Install the libraries via the Library Manager:
   - Adafruit GFX Library
   - Adafruit ST7735 and ST7789 Library
   - ArduinoJson (the API test uses `DynamicJsonDocument`, so ArduinoJson 6 is
     the safest match)
   - WebSockets by Markus Sattler (links2004)
3. For `twitchDisplay_apiTest`, create `twitchDisplay_apiTest/secrets.h`
   (it is gitignored):

   ```cpp
   #define WIFI_PW      "your-wifi-password"
   #define TWITCH_TOKEN "your-twitch-access-token"
   ```
