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
| `partitions.csv` | Partition table, see [Flash layout](#flash-layout) |
| `lib/TitleFont/` | Reads `font.bin` and renders the title characters (stb_truetype) |
| `fonts/font.bin` | Fonts for the titles (Noto Sans, Noto Emoji; SIL Open Font License, `fonts/OFL.txt`), built by `tools/make_fonts.py` |

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
`twitch.tv/name`; pasted links work too), at most 32. They are shown in this
order; up to 8 live channels get a tile, more are counted on a "+N" tile.
Without a saved list the display uses the ten channels it always had.

A channel's title scrolls when it goes live or changes its title or game. In
addition, after some minutes without a title (Settings, default 5, 0 = off)
a live channel's title is shown again. The live channels take turns: the one
whose title was shown longest ago comes next, chance decides among equals.

Titles are drawn in Noto Sans SemiBold: Latin with all its extensions, Greek,
Cyrillic, typographic quotes and dashes. Emoji appear in colour: the display
downloads each one once as a [Twemoji](https://github.com/jdecked/twemoji)
image (CC-BY 4.0) from cdn.jsdelivr.net before the title starts, and keeps
about 60 of them in flash (the oldest are replaced), from where they are drawn
directly (up to 255 colours each). Flags, skin tones and
combined emoji work too. Emoji without a colour image, or while a download
fails, are drawn from a monochrome emoji font in the text colour (about 400
common ones). Settings > **Emoji in titles** switches to monochrome only (no
downloads).

Chinese, Japanese and Korean characters come from Noto Sans JP, KR and SC
(SIL Open Font License), which [fontsource](https://fontsource.org) splits
into about 100 pieces per language. For characters of a new title that are
not cached yet, the display looks up the pieces in fontsource's CSS,
downloads them (17-100 KB each) into the firmware slot that is not running
(only after the running firmware has been confirmed; updates wait for such a
download), renders the characters and caches them (about 1 KB each), so a
title with many new ones can take half a minute to start. Hangul comes from
the Korean font; other characters from the Japanese font first, then the
Chinese one. Settings > **Chinese, Japanese, Korean** switches this off
(boxes). Characters no font has show as a box. Without `font.bin` (see
below) titles use the built-in 5×7 font, which knows umlauts but no emoji.

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
log in as `admin` with your web password. The login can be switched off on the
Settings page; then anyone on your WiFi can use the web interface (stored
passwords stay hidden, uploads from PlatformIO still need the password).

- **Status**: live channels (with a button that scrolls all their titles once
  more), last Twitch update, firmware version, uptime, WiFi, memory, restart
  button
- **Update**: *Online update* checks the newest GitHub release and installs
  it with one click; or upload a `firmware.bin` from the browser (phone or PC). The
  file is checked before anything is written (ESP32-C3 application image, not
  the bootloader or a merged image), the display shows the progress and
  restarts afterwards.
- **Settings**: WiFi, Twitch credentials, channel list, title repeat, emoji,
  CJK characters, web password and login, reset to defaults

**Rollback protection**: a firmware installed by an update starts "on
probation" and only confirms itself after it has been online for 60 s. If it
crashes, hangs (30 s watchdog) or restarts before that, the bootloader switches
back to the previous firmware. The status page shows the state of the running
firmware. This needs a bootloader with rollback support; updates never replace
the bootloader, so if the status page shows "new (bootloader without rollback
support?)" after an update, flash once over USB.

## Flash layout

| Offset | Size | Partition | Contents |
|---|---|---|---|
| 0x009000 | 20 KB | `nvs` | Settings, channel cache, logo index |
| 0x00E000 | 8 KB | `otadata` | Which firmware slot boots, rollback state |
| 0x010000 | 1600 KB | `app0` | Firmware slot 1 |
| 0x1A0000 | 1600 KB | `app1` | Firmware slot 2 (updates go to the slot that is not running; meanwhile it holds downloaded CJK font pieces) |
| 0x330000 | 256 KB | `logos` | 32 channel logos, 64×64 pixels each |
| 0x370000 | 512 KB | `font` | `font.bin` (about 260 KB) and the cache for colour emoji and CJK characters |
| 0x3F0000 | 64 KB | `coredump` | Crash dump |

The firmware knows which `font.bin` it was built with. If the `font`
partition is empty, damaged or holds another version, the display downloads
`font.bin` from the GitHub release (development builds: from the newest
release) and shows the progress; until then titles use the built-in font.
The Status page shows the font state.

**Updating from v0.4.x or older:** that firmware used a different layout, and
the partition table can only be changed over USB. Either

- with PlatformIO (keeps the settings; the display downloads `font.bin` from
  the release, so publish the release first):

  ```bash
  pio run -e twitchDisplay_usb -t upload --upload-port COM5
  ```

- or flash `firmware-merged.bin` at `0x0` (e.g. esptool-js in the browser).
  It contains everything including `font.bin`, but also erases the settings:
  the display starts the setup hotspot and they have to be entered again.

The logos are downloaded again in both cases. A new firmware installed by
online update on the old layout keeps working with the built-in font and 16
logos, and its Status page says that the USB flash is needed.

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
- `firmware-merged.bin`: everything for the first flash over USB (bootloader,
  partition table, firmware and `font.bin`), e.g. with
  [esptool-js](https://espressif.github.io/esptool-js/) in Chrome or Edge at
  address `0x0`, no installation needed
- `font.bin`: the fonts, which the display downloads when its `font` partition
  lacks them

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
