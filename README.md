# twitchDisplay – PlatformIO

ESP32-C3 firmware that shows which of your Twitch channels are live on a 1.9"
170×320 ST7789 display. Live channels appear with their Twitch avatar as 64×64
logos in a 4×2 grid; title/game changes scroll across the display. The backlight is
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
| `include/secrets.h.example` | Template for the optional `include/secrets.h` (default settings) |
| `include/web_pages.h` | HTML of the web interface |
| `tools/version.py` | Build script that puts the git version into the firmware |
| `lib/LogoImage/` | Converts a PNG avatar into a 64×64 logo (small PNG reader using the ROM inflater, box filter) |
| `.github/workflows/build.yml` | GitHub Action: builds the firmware, publishes releases for version tags |
| `lib/Filters/` | `LowPass.h` and `MovingAverage.h` filters for the LDR reading |
| `examples/apiTest/` | Twitch Helix / EventSub websocket test |
| `examples/displayTest/` | Display and smooth text-scrolling test (with its own `pics.h`) |
| `examples/ldrTest/` | LDR backlight dimming test |
| `test/host/` | Simulation of the firmware on the PC (`test/host/run.sh`, see its README) |
| `C3_partition_minimalSPIFFS.csv` | Partition table with two OTA slots; the `spiffs` partition stores the channel logos |

Each program has its own environment in `platformio.ini`:
`twitchDisplay` builds `src/`, and `twitchDisplay_apiTest`,
`twitchDisplay_displayTest` and `twitchDisplay_ldrTest` build the matching
folder in `examples/` (via `build_src_filter`).

## Settings and setup mode

The display stores its settings (WiFi, Twitch credentials, web password) on
the device. No secrets are compiled into the firmware, so the same
`firmware.bin` works for everyone.

**First start:** without settings the display opens its own WiFi hotspot and
shows its name and a random password:

1. Connect your phone to `TwitchDisplay-Setup` with the password shown on the
   display. The settings page usually opens by itself; otherwise open
   `http://192.168.4.1/`.
2. Choose your WiFi network, enter its password and a password for the web
   interface (at least 8 characters). Twitch credentials can be entered now or
   later.
3. Save. The display restarts and connects to your WiFi.

**Changing settings later:** web interface -> **Settings**. Stored passwords
and secrets are never shown; leave a field empty to keep the stored value.

**Setup mode again:**
- hold the **BOOT** button for 5 seconds while the display is running (not
  while plugging it in: that starts the chip's USB download mode), or
- the display opens the setup hotspot by itself if the WiFi is not reachable
  for 2 minutes after starting. If nobody uses the hotspot, it restarts after
  10 minutes and tries the WiFi again (e.g. after a power cut, when the router
  takes longer to start).

**Twitch credentials:** create an application at
[dev.twitch.tv/console](https://dev.twitch.tv/console) and enter its client ID
and client secret. The display gets and renews its own app access token. A
fixed access token also works but expires and has to be replaced by hand.

### Optional: defaults in the firmware

`include/secrets.h` (copy `include/secrets.h.example`, gitignored) can provide
default values that are used for settings never saved on the device. Only
needed if you want a firmware that starts already configured; never publish a
firmware built with it.

## Channels and logos

Enter the channels on the **Settings** page, one name per line (as in
`twitch.tv/name`; pasted links work too), at most 16. They are shown in this
order; up to 8 live channels get a tile, more are counted on a "+N" tile.
Without a saved list the display uses the ten channels it always had.

A channel's title scrolls when it goes live or changes its title or game. In
addition, after some minutes without a title (Settings, default 5, 0 = off)
a live channel's title is shown again. The live channels take turns: the one
whose title was shown longest ago comes next, chance decides among equals.

The display looks the channels up on Twitch at the start and once a day and
downloads each channel's avatar (the 70×70 version, about 10 KB) only when it
is new or has changed. It is converted to a 64×64 logo and kept in flash, so
logos appear instantly after a restart. Until a logo is there, the tile shows
the channel's initials on a coloured background.

The **Status** page lists every channel with its live state and logo state
(stored, downloading, or the error) and has a **Reload logos** button. Names
Twitch does not know are shown on the Settings and Status pages.

## Status dot

A small dot in the bottom right corner below the grid shows problems. The
display keeps showing the last known live channels while it is visible.

| Colour  | Meaning |
|---------|---------|
| none    | Everything works |
| yellow  | WiFi disconnected |
| magenta | Twitch rejected the token (expired or invalid) |
| red     | Twitch request failed for another reason |

## Web interface

Open `http://twitchdisplay.fritz.box/` (or `http://twitchdisplay.local/`) and
log in as `admin` with your web password.

- **Status**: live channels, last Twitch update, firmware version, uptime,
  WiFi, memory, restart button
- **Update**: *Online update* checks the newest GitHub release and installs
  it with one click; or upload a `firmware.bin` from the browser (phone or PC). The
  file is checked before anything is written (ESP32-C3 application image, not
  the bootloader or a merged image), the display shows the progress and
  restarts afterwards.
- **Settings**: WiFi, Twitch credentials, channel list, web password, reset
  to defaults

**Rollback protection**: a firmware installed by an update starts "on
probation" and only confirms itself after it has been online for 60 s. If it
crashes, hangs (30 s watchdog) or restarts before that, the bootloader switches
back to the previous firmware. The status page shows the state of the running
firmware. This needs a bootloader with rollback support; updates never replace
the bootloader, so if the status page shows "new (bootloader without rollback
support?)" after an update, flash once over USB.

## Releases and online update

GitHub builds the firmware on every push to this branch (download it from the
run's *Artifacts*). Pushing a version tag publishes a release:

```bash
git tag v1.0.0
```

```bash
git push origin v1.0.0
```

The release contains

- `firmware.bin`: what the display's **Online update** installs (and what the
  Update page's upload expects)
- `firmware-merged.bin`: everything for the first flash over USB, e.g. with
  [esptool-js](https://espressif.github.io/esptool-js/) in Chrome or Edge at
  address `0x0`, no installation needed

The firmware contains no secrets (settings are entered on the device), so the
releases can be public. Tag versions as `vX.Y.Z` on this branch; the display
compares the release tag with its own version (`git describe`).

The online update downloads the release over HTTPS (certificate checked),
checks that it is an ESP32-C3 application image before writing, and uses the
same rollback protection as an upload.

## Build and upload

```bash
pio run -e twitchDisplay
```

The firmware file is `.pio/build/twitchDisplay/firmware.bin`; upload it on the
web interface's Update page.

Alternatively upload directly from PlatformIO over WiFi (ArduinoOTA). It uses
the web password, taken from the environment variable
`TWITCHDISPLAY_PASSWORD` so it never ends up in the repository:

```bash
export TWITCHDISPLAY_PASSWORD='your-web-password'
```

(PowerShell: `$env:TWITCHDISPLAY_PASSWORD='your-web-password'`)

```bash
pio run -e twitchDisplay -t upload
```

For the first flash over USB, set `upload_protocol = esptool` and pass the
port, e.g. `--upload-port /dev/ttyACM0`. Serial log:

```bash
pio device monitor
```

To build a test sketch, use its environment, e.g. `pio run -e twitchDisplay_ldrTest`.
