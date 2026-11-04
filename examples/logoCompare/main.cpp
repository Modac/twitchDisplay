/**
 * Test firmware: shows the built-in pictures (pics.h) next to the logos the
 * main firmware downloaded and stored in flash.
 *
 * Install it through the web interface's Update page. It never confirms
 * itself, so pressing RESET (or the automatic restart after 10 minutes)
 * makes the bootloader return to the main firmware (rollback). If it was
 * flashed over USB instead, flash the main firmware again afterwards.
 *
 * Pages: overview (4 channels old | new), then each channel zoomed 2x.
 * A short press on BOOT shows the next page.
 */
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <Preferences.h>
#include <esp_partition.h>

#include "../displayTest/pics.h"

#define TFT_CS   7
#define TFT_RST  10
#define TFT_DC   1
#define TFT_BK   0
#define BUTTON_PIN 9

#define LOGO_SIZE       64
#define LOGO_SLOTS      16
#define LOGO_SLOT_BYTES (LOGO_SIZE * LOGO_SIZE * 2)

#define OVERVIEW_TIME   6000    // ms per overview page
#define ZOOM_TIME       4000    // ms per zoom page
#define RESTART_AFTER   (10UL * 60 * 1000) // restart (and roll back) after 10 minutes

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

struct Channel {
  const char* id;
  const char* name;
  const uint16_t* pic;
  int slot;            // logo slot in flash, -1 = no downloaded logo
};

Channel channels[] = {
  {"761017145", "lidi", epd_bitmap_lidi, -1},
  {"21991090", "pietsmiet", epd_bitmap_pietsmiet, -1},
  {"73437396", "bonjwa", epd_bitmap_bonjwa, -1},
  {"1024088182", "bonjwachill", epd_bitmap_bonjwachill, -1},
  {"12875057", "gronkh", epd_bitmap_gronkh, -1},
  {"16064695", "dhalucard", epd_bitmap_dhalucard, -1},
  {"55898523", "trilluxe", epd_bitmap_trilluxe, -1},
  {"38770961", "dracon", epd_bitmap_dracon, -1},
  {"172376071", "maxim", epd_bitmap_maxim, -1},
  {"549536744", "finanzfluss", epd_bitmap_finanzfluss, -1},
};
const int CHANNELS = sizeof(channels) / sizeof(channels[0]);
const int OVERVIEW_PAGES = (CHANNELS + 3) / 4;

const esp_partition_t* logoPartition = nullptr;
uint16_t logoBuf[LOGO_SIZE * LOGO_SIZE];
uint16_t zoomLine[LOGO_SIZE * 2];

// Never confirm this firmware: the next restart returns to the main firmware
extern "C" bool verifyRollbackLater(){
  return true;
}

// Reads which channel's logo is in which slot (index written by the main firmware)
void loadLogoIndex(){
  Preferences prefs;
  prefs.begin("logos", true);
  for(int i = 0; i < LOGO_SLOTS; i++){
    String v = prefs.getString(("s" + String(i)).c_str(), "");   // "<channel id>:<url hash>"
    int colon = v.indexOf(':');
    if(colon <= 0) continue;
    String id = v.substring(0, colon);
    for(auto& ch : channels) if(id == ch.id) ch.slot = i;
  }
  prefs.end();
}

// The downloaded logo of a channel, or nullptr
const uint16_t* newLogo(const Channel& ch){
  if(ch.slot < 0 || !logoPartition) return nullptr;
  if(esp_partition_read(logoPartition, ch.slot * LOGO_SLOT_BYTES, logoBuf, LOGO_SLOT_BYTES) != ESP_OK) return nullptr;
  return logoBuf;
}

void drawMissing(int x, int y, int size){
  tft.drawRect(x, y, size, size, ST77XX_RED);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_RED);
  tft.setCursor(x + size / 2 - 21, y + size / 2 - 4);
  tft.print("missing");
}

// Draws a 64x64 picture at 1x or 2x (pixel doubling, no smoothing)
void drawPicture(int x, int y, const uint16_t* pixels, int zoom){
  if(!pixels){ drawMissing(x, y, LOGO_SIZE * zoom); return; }
  if(zoom == 1){ tft.drawRGBBitmap(x, y, (uint16_t*)pixels, LOGO_SIZE, LOGO_SIZE); return; }
  tft.startWrite();
  tft.setAddrWindow(x, y, LOGO_SIZE * 2, LOGO_SIZE * 2);
  for(int row = 0; row < LOGO_SIZE; row++){
    for(int col = 0; col < LOGO_SIZE; col++) zoomLine[col * 2] = zoomLine[col * 2 + 1] = pixels[row * LOGO_SIZE + col];
    tft.writePixels(zoomLine, LOGO_SIZE * 2);
    tft.writePixels(zoomLine, LOGO_SIZE * 2);
  }
  tft.endWrite();
}

void label(int x, int y, const char* text, uint16_t color){
  tft.setTextSize(1);
  tft.setTextColor(color);
  tft.setCursor(x, y);
  tft.print(text);
}

// 4 channels per page: name, then old | new
void drawOverview(int page){
  tft.fillScreen(ST77XX_BLACK);
  for(int k = 0; k < 4; k++){
    int i = page * 4 + k;
    if(i >= CHANNELS) break;
    int x = 12 + (k % 2) * 160, y = 2 + (k / 2) * 84;
    label(x, y, channels[i].name, ST77XX_WHITE);
    drawPicture(x, y + 10, channels[i].pic, 1);
    drawPicture(x + 68, y + 10, newLogo(channels[i]), 1);
  }
  char footer[48];
  snprintf(footer, sizeof(footer), "old | new   page %d/%d   RESET = back", page + 1, OVERVIEW_PAGES);
  label(12, 160, footer, ST77XX_CYAN);
}

// One channel, both pictures zoomed 2x
void drawZoom(int i){
  tft.fillScreen(ST77XX_BLACK);
  label(24, 4, channels[i].name, ST77XX_WHITE);
  label(24 + 128 - 18, 4, "old", ST77XX_CYAN);
  label(24 + 128 + 16 + 128 - 18, 4, "new", ST77XX_CYAN);
  drawPicture(24, 18, channels[i].pic, 2);
  drawPicture(24 + 128 + 16, 18, newLogo(channels[i]), 2);
  label(24, 156, "2x zoom   RESET = back to normal firmware", ST77XX_CYAN);
}

int page = 0;
unsigned long pageShownAt = 0;

void showPage(){
  if(page < OVERVIEW_PAGES) drawOverview(page);
  else drawZoom(page - OVERVIEW_PAGES);
  pageShownAt = millis();
}

void setup(){
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(TFT_BK, OUTPUT);
  digitalWrite(TFT_BK, HIGH);
  tft.init(170, 320);
  tft.setRotation(1);
  tft.setSPISpeed(80000000);

  logoPartition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  loadLogoIndex();
  for(const auto& ch : channels) Serial.printf("[logoCompare] %s: slot %d\r\n", ch.name, ch.slot);
  showPage();
}

void loop(){
  static bool wasPressed = false;
  bool pressed = digitalRead(BUTTON_PIN) == LOW;
  unsigned long shown = millis() - pageShownAt;
  bool next = (pressed && !wasPressed) || shown >= (unsigned long)(page < OVERVIEW_PAGES ? OVERVIEW_TIME : ZOOM_TIME);
  wasPressed = pressed;
  if(next){
    page = (page + 1) % (OVERVIEW_PAGES + CHANNELS);
    showPage();
  }
  if(millis() >= RESTART_AFTER){
    Serial.printf("[logoCompare] Restarting, back to the main firmware.\r\n");
    delay(100);
    ESP.restart();
  }
  delay(20);
}
