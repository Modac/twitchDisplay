# twitchDisplay – ESPHome prototype

First prototype of the twitchDisplay, built with [ESPHome](https://esphome.io/).
It draws a static 4×2 grid of 64×64 placeholders (two of them with channel
logos) and shows the current time. It does not query Twitch; the current
firmware is on the `platform/platformio` branch.

## Hardware

- Seeed XIAO ESP32-C3
- 1.9" 170×320 ST7789V display

| Function          | GPIO |
|-------------------|------|
| TFT SCK           | 4    |
| TFT MOSI          | 6    |
| TFT MISO          | 5    |
| TFT CS            | 7    |
| TFT DC            | 1    |
| TFT RST           | 10   |
| TFT backlight PWM | 0    |

## Files

| File | Purpose |
|------|---------|
| `twitchDisplay.yaml` | ESPHome configuration |
| `gronkh.png`, `pietsmiet.jpg` | Channel logos, resized to 64×64 by ESPHome |

## Secrets

Create `secrets.yaml` next to the configuration (it is gitignored):

```yaml
home_wifi_pw: "your-wifi-password"
```

## Build and upload

```bash
esphome run twitchDisplay.yaml
```
