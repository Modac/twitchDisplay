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
| `include/pics.h` | 64×64 RGB565 channel logos (made with [image2cpp](https://javl.github.io/image2cpp/)) |
| `include/secrets.h.example` | Template for `include/secrets.h` |
| `lib/Filters/` | `LowPass.h` and `MovingAverage.h` filters for the LDR reading |
| `examples/apiTest/` | Twitch Helix / EventSub websocket test |
| `examples/displayTest/` | Display and smooth text-scrolling test |
| `examples/ldrTest/` | LDR backlight dimming test |
| `test/` | Reserved for unit tests (`pio test`) |
| `C3_partition_minimalSPIFFS.csv` | Partition table with two OTA slots |

Each program has its own environment in `platformio.ini`:
`twitchDisplay` builds `src/`, and `twitchDisplay_apiTest`,
`twitchDisplay_displayTest` and `twitchDisplay_ldrTest` build the matching
folder in `examples/` (via `build_src_filter`).

## Secrets

Copy `include/secrets.h.example` to `include/secrets.h` (it is gitignored)
and fill in your values:

```cpp
#define WIFI_PW      "your-wifi-password"

// either a fixed access token ...
#define TWITCH_TOKEN "your-twitch-access-token"

// ... or (recommended) your Twitch application's client ID and secret; the
// display then gets and renews its own app access token
#define TWITCH_CLIENT_ID     "your-client-id"
#define TWITCH_CLIENT_SECRET "your-client-secret"
```

## Status dot

A small dot in the bottom right corner below the grid shows problems. The
display keeps showing the last known live channels while it is visible.

| Colour  | Meaning |
|---------|---------|
| none    | Everything works |
| yellow  | WiFi disconnected |
| magenta | Twitch rejected the token (expired or invalid) |
| red     | Twitch request failed for another reason |

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

To build a test sketch, use its environment, e.g. `pio run -e twitchDisplay_ldrTest`.
