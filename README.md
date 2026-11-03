# twitchDisplay – PlatformIO

ESP32-C3 firmware that shows which of a fixed list of Twitch channels are live
on a 1.9" 170×320 ST7789 display. Live channels appear as 64×64 logos in a
4×2 grid; title/game changes scroll across the display. The backlight is
dimmed automatically via an LDR.

## Hardware

- Generic ESP32-C3 board (black AliExpress board, similar to the ESP32-C3
  SuperMini), 4 MB flash; built with the `esp32-c3-devkitm-1` board definition
- 1.9" 170×320 ST7789 display
- LDR on A3 for automatic backlight dimming

| Function          | GPIO |
|-------------------|------|
| TFT SCK           | 4    |
| TFT MOSI          | 6    |
| TFT CS            | 7    |
| TFT DC            | 1    |
| TFT RST           | 10   |
| TFT backlight PWM | 0    |
| LDR (analog)      | 3 (A3) |

## Project layout

| Path | Purpose |
|------|---------|
| `src/twitchDisplay.cpp` | Main firmware |
| `src/twitchDisplay_apiTest.cpp` | Twitch Helix / EventSub websocket test |
| `src/twitchDisplay_displayTest.cpp` | Display and smooth text-scrolling test |
| `src/twitchDisplay_ldrTest.cpp` | LDR backlight dimming test |
| `src/pics.h` | 64×64 RGB565 channel logos (made with [image2cpp](https://javl.github.io/image2cpp/)) |
| `src/LowPass.h`, `src/MovingAverage.h` | Filters for the LDR reading |
| `C3_partition_minimalSPIFFS.csv` | Partition table with two OTA slots |

Each source file has its own environment in `platformio.ini`
(`build_src_filter`).

## Secrets

Create `src/secrets.h` (it is gitignored):

```cpp
#define WIFI_PW      "your-wifi-password"
#define TWITCH_TOKEN "your-twitch-access-token"
```

## Build and upload

Uploads go over the air (`espota`) to `twitchdisplay.fritz.box` by default.
For the first flash over USB, override the upload protocol and port, for
example with `--upload-port /dev/ttyACM0` and `upload_protocol = esptool`.

```bash
pio run -e twitchDisplay
```

```bash
pio run -e twitchDisplay -t upload
```

```bash
pio device monitor
```
