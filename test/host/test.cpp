#include <set>
// Host simulation of the twitchDisplay firmware: compiles the real
// src/twitchDisplay.cpp against the stand-ins in stubs/ and checks its
// behaviour (see README.md). Build and run with ./run.sh
#include "../../src/twitchDisplay.cpp"
#include "test_data.h"
#include <vector>
#include <utility>
#include <fstream>
#include <iterator>
// generated test avatars (70x70 PNG), one per name
std::string avatar(const std::string& name){ return makeAvatar(name == "bonjwa" ? 1 : name == "gronkh" ? 2 : 3); }
// Feeds a file through the upload handlers like WebServer does (1436-byte chunks)
void simulateUpload(const std::vector<uint8_t>& file, bool abortHalfway=false){
  auto* r = server.find("/update", HTTP_POST);
  HTTPUpload& up = server.upload();
  g_contentLength = file.size() + 200;
  up.filename = "firmware.bin"; up.totalSize = 0; up.currentSize = 0;
  up.status = UPLOAD_FILE_START; r->ufn();
  size_t pos = 0;
  while(pos < file.size()){
    if(abortHalfway && pos > file.size()/2){ up.status = UPLOAD_FILE_ABORTED; r->ufn(); return; }
    size_t n = std::min((size_t)HTTP_UPLOAD_BUFLEN, file.size()-pos);
    memcpy(up.buf, &file[pos], n); up.currentSize = n; up.status = UPLOAD_FILE_WRITE; r->ufn();
    pos += n; up.totalSize = pos;
  }
  up.status = UPLOAD_FILE_END; up.currentSize = 0; r->ufn();
  r->fn();
}
// Sends a file in pieces like the upload page does (?offset=&total=, one request each)
void simulatePieces(const std::vector<uint8_t>& file, size_t piece, size_t stopAfter = SIZE_MAX){
  size_t sent = 0;
  for(size_t off = 0; off < file.size() && sent < stopAfter; off += piece, sent++){
    std::vector<uint8_t> part(file.begin() + off, file.begin() + std::min(file.size(), off + piece));
    g_args["offset"] = std::to_string(off); g_args["total"] = std::to_string(file.size());
    simulateUpload(part);
    if(g_lastBody.find("\"ok\":true") == std::string::npos) break;
  }
  g_args.erase("offset"); g_args.erase("total");
}

unsigned long g_millis = 1; unsigned long g_usFrac = 0; bool g_quiet = true;
SerialStub Serial; EspStub ESP; ArduinoOTAClass ArduinoOTA;
uint16_t g_fb[170][320]; int g_bitmapDraws = 0;
std::string g_response; int g_httpCode = 200;
std::deque<FakeResponse> g_responses; std::vector<FakeRequest> g_requests;
int g_analogReads = 0; int g_notifies = 0; int g_tasksCreated = 0;
bool g_wifiUp = true;
std::map<std::string,std::string> g_args, g_respHeaders; std::map<std::string, std::map<std::string, std::string>> g_nvs;
std::string g_serialOut;
esp_partition_t g_spiffs{"logos", 0x40000}; std::vector<uint8_t> g_flashData(0x40000, 0xFF);
esp_partition_t g_fontPart{"font", 0x80000}; std::vector<uint8_t> g_fontData(0x80000, 0xFF); bool g_haveFontPartition = true; int g_mmaps = 0;
size_t g_largestBlock = 61440;
void (*g_delayHook)() = nullptr; int g_flashErases = 0, g_flashWrites = 0, g_flashBadWrites = 0; bool g_flashFail = false;
std::vector<TextBox> g_textBoxes;
int g_spiFastPieces = 0;
int g_panelFrameRate = -1; uint16_t g_gram[320][240]; uint8_t g_madctl = 0; bool g_recordWrites = false; std::vector<std::pair<int,int>> g_writeOrder;
int g_dnsStarted = 0; bool g_bootPressed = false; std::vector<std::string> g_scan; std::string g_beginSsid, g_beginPw, g_apSsid, g_apPw; int g_apStations = 0;
bool contains(const std::string& h, const std::string& n){ return h.find(n) != std::string::npos; }
bool throwsRestart(std::function<void()> f){ try { f(); } catch(const char*) { return true; } return false; }
// Runs loop() for ms of simulated time; true if it restarted
bool runLoopFor(unsigned long ms){ unsigned long end = g_millis + ms; return throwsRestart([&](){ while(g_millis < end) loop(); }); }
int setupModeTests();
// Release answer as GitHub sends it (shortened, with the fields the firmware ignores)
std::string releaseJson(const std::string& tag, std::vector<std::pair<std::string,size_t>> assets){
  std::string j = "{\"url\":\"https://api.github.com/repos/Modac/twitchDisplay/releases/1\",\"html_url\":\"https://github.com/Modac/twitchDisplay/releases/tag/" + tag + "\","
    "\"id\":1,\"author\":{\"login\":\"github-actions[bot]\",\"id\":41898282},\"tag_name\":\"" + tag + "\",\"target_commitish\":\"platform/platformio\","
    "\"name\":\"" + tag + "\",\"draft\":false,\"prerelease\":false,\"created_at\":\"2026-11-03T20:00:00Z\",\"published_at\":\"2026-11-03T20:05:00Z\",\"assets\":[";
  for(size_t i = 0; i < assets.size(); i++){
    if(i) j += ",";
    j += "{\"url\":\"https://api.github.com/x\",\"id\":" + std::to_string(10+i) + ",\"name\":\"" + assets[i].first + "\",\"label\":\"\",\"uploader\":{\"login\":\"github-actions[bot]\"},"
         "\"content_type\":\"application/octet-stream\",\"state\":\"uploaded\",\"size\":" + std::to_string(assets[i].second) + ",\"download_count\":3,"
         "\"browser_download_url\":\"https://github.com/Modac/twitchDisplay/releases/download/" + tag + "/" + assets[i].first + "\"}";
  }
  return j + "],\"body\":\"## What's Changed\\n* Lots of things\"}";
}
int g_wdtFeeds = 0; uint32_t g_wdtTimeout = 0;
bool g_authOk = true; int g_lastCode = 0; std::string g_lastBody; int g_authChallenges = 0; int g_contentLength = 0;
std::vector<uint8_t> g_flashed; bool g_updateRunning = false, g_updateEndOk = true; int g_updateBegins = 0, g_updateAborts = 0, g_updateEnds = 0;
UpdateClass Update; MDNSStub MDNS;
esp_partition_t g_running{"app0", 0x190000}, g_next{"app1", 0x190000}; std::vector<uint8_t> g_nextData(0x190000, 0xFF); esp_ota_img_states_t g_otaState = ESP_OTA_IMG_UNDEFINED; int g_markedValid = 0; int g_reconnects = 0; bool g_bundleUsed = false; int g_tlsStops = 0; WiFiClass WiFi;

static int fails = 0;
#define CHECK(cond) do{ if(!(cond)){ printf("  FAIL line %d: %s\n", __LINE__, #cond); fails++; } }while(0)

void setResponse(std::vector<std::pair<int,std::string>> live){
  std::string j = "{\"data\":[";
  for(size_t k=0;k<live.size();k++){
    auto& c = channels[live[k].first];
    if(k) j += ",";
    j += "{\"user_id\":\""+c.id+"\",\"user_name\":\"n"+std::to_string(live[k].first)+"\",\"title\":\""+live[k].second+"\",\"game_name\":\"g\"}";
  }
  j += "]}";
  g_response = j;
}
void reset(){
  for(auto& c: channels){ c.isLive=false; c.slotNum=-1; c.streamTitle=""; }
  titleChangeQueue.clear(); isTitleDisplaying=false; live_num=0; g_httpCode=200;
  updateQueue->items.clear();
  g_responses.clear(); g_requests.clear(); g_wifiUp=true; statusColor=STATUS_OK;
#ifdef VARIANT_SECRET
  twitchToken = "";
  g_responses.push_back({200, "{\"access_token\":\"tok1\",\"expires_in\":5000000,\"token_type\":\"bearer\"}"});
#endif
  for(auto& r: g_fb) for(auto& p: r) p=0;
}
// one loop() iteration that performs an update first
void loopWithUpdate(){ runNetworkUpdate(); loop(); }
// loop() iterations without an update
void loopNoUpdate(int n){ for(int i=0;i<n;i++) loop(); }
void runUntilTitleDone(){ int n=0; while(isTitleDisplaying && n++<100000) loopNoUpdate(1); }

// true if the tile shows channel ch as the firmware draws it: its stored logo, else its placeholder
bool tileIsPic(int slot, int ch){
  uint16_t x,y; slotXY(slot,x,y);
  std::vector<uint16_t> expect(64*64);
  if(channels[ch].logoSlot >= 0) readLogo(channels[ch].logoSlot, expect.data());
  else { std::string keep = pic_canvas.lastText; drawPlaceholder(channels[ch]); memcpy(expect.data(), pic_canvas.getBuffer(), 64*64*2); pic_canvas.lastText = keep; }
  for(int j=0;j<64;j++) for(int i=0;i<64;i++) if(g_fb[y+j][x+i]!=expect[j*64+i]) return false;
  return true;
}
bool tileIsFill(int slot, uint16_t c){
  uint16_t x,y; slotXY(slot,x,y);
  for(int j=0;j<64;j++) for(int i=0;i<64;i++) if(g_fb[y+j][x+i]!=c) return false;
  return true;
}
uint16_t frame(int slot){ uint16_t x,y; slotXY(slot,x,y); return g_fb[y+32][x-3]; }
// true if the row shows the visible part of the title canvas (and some text)
#define ROW_HEIGHT_FOR_TEST 64
int xSemaphoreTakeCount(SemaphoreHandle_t m){ return m->taken; }
bool titleInRow(int row){
  const uint16_t* b = text_canvas.getBuffer(); int y0 = rowY(row); bool text = false;
  for(int j = 0; j < 64; j++) for(int i = 0; i < TITLE_WIDTH; i++){
    uint16_t v = b[j*CANVAS_W + o_X + i];
    if(g_fb[y0+j][GRID_X0+i] != v) return false;
    if(v) text = true;
  }
  return text;
}
// the render task's work, done while loop() sleeps
void renderHook(){ for(int n = 0; n < 64 && renderAheadStep(); n++) {} }
std::vector<uint8_t> readFile(const char* path){ std::ifstream f(path, std::ios::binary); return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {}); }
std::vector<uint8_t> g_fontBin;

uint16_t statusDot(){ return g_fb[STATUS_Y+2][STATUS_X+2]; }
std::string helix(std::string body){ return body; }

void spiFastPixels(const uint16_t* p, uint32_t n){ tft.writePixels((uint16_t*)p, n); }

int main(){
  g_fontBin = readFile("../../fonts/font.bin");
  std::copy(g_fontBin.begin(), g_fontBin.end(), g_fontData.begin());
  g_delayHook = renderHook;
#ifdef VARIANT_NOSECRETS
  return setupModeTests();
#endif
  setup();
  CHECK(g_bundleUsed); CHECK(g_tasksCreated==2 && netTask!=nullptr && updateQueue!=nullptr);   // network and render task
  CHECK(fontState == FontState::Ready && titleFont.ready() && fontCrc == FONT_BIN_CRC);
  printf("T1 boot with 3 live channels\n");
  reset(); setResponse({{0,"A"},{1,"B"},{2,"C"}}); loopWithUpdate();
  CHECK(tileIsPic(0,0)); CHECK(tileIsPic(1,1)); CHECK(tileIsPic(2,2)); CHECK(tileIsFill(3,0));
  CHECK(isTitleDisplaying && channelTitleInfo.channel==&channels[0]);
  CHECK(frame(0)==ST77XX_GREEN); CHECK(frame(1)==ST77XX_BLACK); CHECK(titleInRow(1));
  CHECK(titleChangeQueue.size()==2);

  printf("T2 update without changes during a title: no pic redraw (A4)\n");
  g_bitmapDraws=0; loopWithUpdate();
  CHECK(g_bitmapDraws==0); CHECK(titleInRow(1)); CHECK(frame(0)==ST77XX_GREEN); CHECK(isTitleDisplaying);

  printf("T3 titled channel goes offline: title stops, next live title starts (A3)\n");
  setResponse({{1,"B"},{2,"C"}}); loopWithUpdate();
  CHECK(tileIsPic(0,1)); CHECK(tileIsPic(1,2)); CHECK(tileIsFill(2,0));
  CHECK(isTitleDisplaying && channelTitleInfo.channel==&channels[1]); CHECK(frame(0)==ST77XX_GREEN);

  printf("T4 queued channel goes offline, layout changes in the pic row during a title\n");
  reset(); setResponse({{0,"A"},{1,"B"},{2,"C"}}); loopWithUpdate();
  setResponse({{0,"A"},{2,"C"}}); g_bitmapDraws=0; loopWithUpdate();
  CHECK(isTitleDisplaying && channelTitleInfo.channel==&channels[0]);
  CHECK(tileIsPic(0,0)); CHECK(tileIsPic(1,2)); CHECK(tileIsFill(2,0));
  CHECK(frame(0)==ST77XX_GREEN); CHECK(titleInRow(1)); CHECK(g_bitmapDraws==2);
  runUntilTitleDone(); loopNoUpdate(1);
  CHECK(isTitleDisplaying && channelTitleInfo.channel==&channels[2]); // ch1 skipped
  CHECK(frame(1)==ST77XX_GREEN); CHECK(frame(0)==ST77XX_BLACK);

  printf("T5 titled channel moves to the other row: title restarts\n");
  reset(); setResponse({{0,"A"},{1,"B"},{2,"C"},{3,"D"},{4,"E"}}); loopWithUpdate();
  while(isTitleDisplaying || !titleChangeQueue.empty()){ runUntilTitleDone(); loopNoUpdate(1); }
  setResponse({{0,"A"},{1,"B"},{2,"C"},{3,"D"},{4,"E2"}}); loopWithUpdate();
  CHECK(isTitleDisplaying && channelTitleInfo.channel==&channels[4] && channelTitleInfo.slot==4);
  CHECK(frame(4)==ST77XX_GREEN); CHECK(titleInRow(0));
  setResponse({{1,"B"},{2,"C"},{3,"D"},{4,"E2"}}); loopWithUpdate();
  CHECK(isTitleDisplaying && channelTitleInfo.channel==&channels[4] && channelTitleInfo.slot==3);
  CHECK(titlePixels==0 && titleFrames==1); // restarted from the first character
  CHECK(tileIsPic(0,1)); CHECK(tileIsPic(1,2)); CHECK(tileIsPic(2,3)); CHECK(tileIsPic(3,4));
  CHECK(frame(3)==ST77XX_GREEN); CHECK(frame(4)==ST77XX_BLACK); CHECK(titleInRow(1));

  printf("T6 10 live channels: '+N' tile in slot 7, title end restores all tiles\n");
  reset(); { std::vector<std::pair<int,std::string>> v; for(int k=0;k<10;k++) v.push_back({k,"T"}); setResponse(v); }
  loopWithUpdate(); runUntilTitleDone();
  for(int k=0;k<7;k++) CHECK(tileIsPic(k,k));
  { uint16_t x,y; slotXY(7,x,y); CHECK(g_fb[y+32][x+32]==0x4321 && g_fb[y+2][x+2]==0); }  // "+N" text on black
  CHECK(live_num==10);
  for(int k=0;k<8;k++) CHECK(frame(k)==ST77XX_BLACK);

  printf("T7 HTTP error: nothing changes\n");
  g_httpCode=401; g_bitmapDraws=0; loopWithUpdate(); CHECK(g_bitmapDraws==0); CHECK(live_num==10);

  printf("T8 all channels offline: everything blank after the title\n");
  reset(); setResponse({{0,"A"}}); loopWithUpdate();
  setResponse({}); loopWithUpdate();
  CHECK(!isTitleDisplaying); for(int k=0;k<8;k++) CHECK(tileIsFill(k,0));


  printf("T9 realistic Helix response with extra fields (filter)\n");
  reset();
  g_response = "{\"data\":[{\"id\":\"1\",\"user_id\":\"21991090\",\"user_login\":\"pietsmiet\",\"user_name\":\"PietSmiet\",\"game_id\":\"27471\",\"game_name\":\"Minecraft\",\"type\":\"live\",\"title\":\"Hallo Welt\",\"viewer_count\":1234,\"started_at\":\"2026-11-03T10:00:00Z\",\"language\":\"de\",\"thumbnail_url\":\"https://x/{width}x{height}.jpg\",\"tag_ids\":[],\"tags\":[\"Deutsch\",\"Gaming\"],\"is_mature\":false}],\"pagination\":{\"cursor\":\"abc\"}}";
  loopWithUpdate();
  CHECK(channels[1].isLive && channels[1].slotNum==0);
  CHECK(channels[1].streamTitle=="Hallo Welt | Minecraft");
  CHECK(statusDot()==STATUS_OK);
  { auto& r = g_requests.back(); CHECK(r.method=="GET" && r.tls); CHECK(r.url.find("https://api.twitch.tv/helix/streams?user_id=761017145&")==0); }

  printf("T10 HTTP 200 without data array: keep state, red dot\n");
  g_response = "{\"error\":\"oops\"}"; g_bitmapDraws=0; loopWithUpdate();
  CHECK(channels[1].isLive && live_num==1); CHECK(statusDot()==STATUS_API);
  printf("    and recovery clears the dot\n");
  setResponse({{1,"Hallo Welt"}}); g_response.replace(g_response.find("\"game_name\":\"g\""), 15, "\"game_name\":\"Minecraft\""); loopWithUpdate();
  CHECK(statusDot()==STATUS_OK); CHECK(channels[1].isLive);

  printf("T11 WiFi lost: no requests, yellow dot, reconnect nudges, update requested when back\n");
  reset(); setResponse({{0,"A"}}); loopWithUpdate();
  size_t nReq = g_requests.size(); int nRec = g_reconnects;
  g_wifiUp = false; loopWithUpdate();
  CHECK(g_requests.size()==nReq); CHECK(statusDot()==STATUS_NO_WIFI); CHECK(g_reconnects==nRec);
  CHECK(updateQueue->items.empty());
  for(int i=0;i<3100;i++) loop();          // > 30 s
  CHECK(g_reconnects==nRec+1);
  int nNotify = g_notifies;
  g_wifiUp = true; loop();
  CHECK(g_notifies==nNotify+1);             // network task woken
  loopWithUpdate();
  CHECK(g_requests.size()==nReq+1); CHECK(statusDot()==STATUS_OK);
  CHECK(channels[0].isLive);

#ifndef VARIANT_SECRET
  printf("T12 static token rejected: magenta dot, state kept, no token request\n");
  reset(); setResponse({{0,"A"}}); loopWithUpdate();
  CHECK(g_requests.back().auth=="statictoken"); CHECK(g_requests.back().clientId=="gp762nuuoqcoxypju8c569th9wz7q5");
  g_responses.push_back({401, "{\"error\":\"Unauthorized\"}"}); size_t n0=g_requests.size(); loopWithUpdate();
  CHECK(g_requests.size()==n0+1); CHECK(statusDot()==STATUS_AUTH); CHECK(channels[0].isLive && live_num==1);
#else
  printf("T12 client secret: token fetched first, renewed and retried on 401\n");
  reset(); setResponse({{0,"A"}}); loopWithUpdate();
  CHECK(g_requests.size()==2);
  CHECK(g_requests[0].method=="POST" && g_requests[0].tls && g_requests[0].url=="https://id.twitch.tv/oauth2/token");
  CHECK(g_requests[0].body=="client_id=myid&client_secret=mysecret&grant_type=client_credentials");
  CHECK(g_requests[1].method=="GET" && g_requests[1].auth=="tok1" && g_requests[1].clientId=="myid");
  CHECK(channels[0].isLive && statusDot()==STATUS_OK);
  printf("    update with valid token: no token request\n");
  loopWithUpdate(); CHECK(g_requests.size()==3 && g_requests[2].method=="GET");
  printf("    401 -> new token -> retry succeeds\n");
  g_responses.push_back({401, "{}"});
  g_responses.push_back({200, "{\"access_token\":\"tok2\"}"});
  loopWithUpdate();
  CHECK(g_requests.size()==6); CHECK(g_requests[4].method=="POST"); CHECK(g_requests[5].auth=="tok2");
  CHECK(statusDot()==STATUS_OK); CHECK(twitchToken=="tok2");
  printf("    token request fails: magenta dot, state kept\n");
  g_responses.push_back({401, "{}"}); g_responses.push_back({400, "{\"message\":\"invalid client secret\"}"});
  loopWithUpdate();
  CHECK(statusDot()==STATUS_AUTH); CHECK(channels[0].isLive && live_num==1);
#endif

  printf("T13 UTF-8 -> CP437 conversion\n");
  CHECK(toDisplayText("Gr\xc3\xbc\xc3\x9f" "e aus K\xc3\xb6ln") == "Gr\x81\xe1" "e aus K\x94ln");
  CHECK(toDisplayText("\xc3\x84\xc3\x96\xc3\x9c \xc3\xa4\xc3\xb6\xc3\xbc \xc3\xa9\xc3\xa8 \xc3\xb1") == "\x8e\x99\x9a \x84\x94\x81 \x82\x8a \xa4");
  CHECK(toDisplayText("Spa\xc3\x9f \xf0\x9f\x8e\xae!") == "Spa\xe1 !");                 // emoji dropped
  CHECK(toDisplayText("\xe2\x80\x9eneu\xe2\x80\x9c \xe2\x80\x93 100\xe2\x82\xac\xe2\x80\xa6") == "\"neu\" - 100EUR...");
  CHECK(toDisplayText("a\nb\tc") == "a b c");
  CHECK(toDisplayText("bad \xc3 \xff end") == "bad   end");                                    // invalid bytes skipped
  CHECK(toDisplayText("cut \xe2\x80") == "cut ");                                               // truncated sequence
  CHECK(toDisplayText(nullptr) == "");
  CHECK(toDisplayText("\xc3\x80\xc3\x89\xc3\xa3") == "A\x90" "a");                             // no glyph -> base letter

  printf("T14 titles from the API are kept as UTF-8\n");
  reset();
  g_response = "{\"data\":[{\"user_id\":\"12875057\",\"user_name\":\"Gronkh\",\"title\":\"Gr\\u00fc\\u00dfe \xf0\x9f\x8e\xae\",\"game_name\":\"Just Chatting\"}]}";
  loopWithUpdate();
  CHECK(channels[4].isLive); CHECK(channels[4].streamTitle == "Gr\xc3\xbc\xc3\x9f" "e \xf0\x9f\x8e\xae | Just Chatting");   // UTF-8, as Twitch sends it

  printf("T15 network task result is only applied by loop()\n");
  reset(); setResponse({{0,"A"}});
  runNetworkUpdate();
  CHECK(!channels[0].isLive && updateQueue->items.size()==1);
  loop();
  CHECK(channels[0].isLive && updateQueue->items.empty());

  printf("T16 LDR sampled every 20 ms\n");
  g_analogReads = 0; loopNoUpdate(100);       // each loop() sleeps 10 ms
  CHECK(g_analogReads >= 48 && g_analogReads <= 52);

  printf("T17 firmware file check (app, bootloader, merged image, partition table, other chip, junk)\n");
  auto fw = makeAppImage(1100000), bl = makeBootloaderImage(20000), merged = makeMergedImage(1600000), pt = makePartitionTable();
  CHECK(fw.size() > 1000000 && bl.size() > 10000 && merged.size() > 1000000);
  CHECK(validateFirmwareHeader(fw.data(), fw.size()) == nullptr);
  CHECK(validateFirmwareHeader(bl.data(), bl.size()) != nullptr);
  CHECK(validateFirmwareHeader(merged.data(), merged.size()) != nullptr);
  CHECK(validateFirmwareHeader(pt.data(), pt.size()) != nullptr);
  CHECK(validateFirmwareHeader(fw.data(), 20) != nullptr);
  { auto other = fw; other[12] = 0x09; CHECK(validateFirmwareHeader(other.data(), other.size()) != nullptr); } // ESP32-S3 chip id
  { std::vector<uint8_t> junk(4096, 0x42); CHECK(validateFirmwareHeader(junk.data(), junk.size()) != nullptr); }

  printf("T18 valid upload: written completely, network paused, restart scheduled\n");
  reset(); setResponse({{0,"A"}}); loopWithUpdate();
  g_authOk = true; g_updateEndOk = true; g_updateBegins = g_updateAborts = g_updateEnds = 0; g_wdtFeeds = 0; rebootAt = 0;
  {
    // network task must not run while the upload is in progress
    auto* r = server.find("/update", HTTP_POST); HTTPUpload& up = server.upload();
    up.status = UPLOAD_FILE_START; up.filename="firmware.bin"; r->ufn();
    CHECK(otaActive); size_t n0 = g_requests.size(); runNetworkUpdate(); CHECK(g_requests.size() == n0);
    up.status = UPLOAD_FILE_ABORTED; r->ufn(); otaActive = false;
  }
  simulateUpload(fw);
  CHECK(g_flashed == fw); CHECK(g_updateEnds == 1); CHECK(g_updateAborts <= 1);
  CHECK(g_lastCode == 200 && g_lastBody.find("\"ok\":true") != std::string::npos);
  CHECK(rebootAt != 0); CHECK(g_wdtFeeds >= (int)(fw.size() / HTTP_UPLOAD_BUFLEN)); // watchdog fed for every chunk
  { bool restarted = false; g_millis += 1500; try { loop(); } catch(const char*) { restarted = true; } CHECK(restarted); }
  rebootAt = 0; otaActive = false;

  printf("T19 merged image rejected before anything is written, display restored\n");
  reset(); setResponse({{0,"A"}}); loopWithUpdate(); runUntilTitleDone();
  g_updateEnds = 0; g_updateAborts = 0;
  simulateUpload(merged);
  CHECK(g_flashed.empty()); CHECK(g_updateEnds == 0); CHECK(g_updateAborts == 1);
  CHECK(g_lastCode == 400 && g_lastBody.find("bootloader or merged") != std::string::npos);
  CHECK(!otaActive && rebootAt == 0 && restoreDisplayAt != 0);
  CHECK(!tileIsPic(0,0));                    // update screen visible
  g_millis += 5100; loop();
  CHECK(restoreDisplayAt == 0 && tileIsPic(0,0) && tileIsFill(1,0));
  printf("    scrolling and updates work again afterwards\n");
  setResponse({{0,"A2"}}); loopWithUpdate(); CHECK(isTitleDisplaying && channelTitleInfo.channel == &channels[0]);

  printf("T20 image verification fails at the end\n");
  g_updateEndOk = false; simulateUpload(fw);
  CHECK(g_lastCode == 400 && g_lastBody.find("Image verification failed") != std::string::npos); CHECK(rebootAt == 0);
  g_updateEndOk = true; g_millis += 5100; loop();

  printf("T21 upload without login: nothing written, login requested\n");
  g_authOk = false; g_updateBegins = 0; g_authChallenges = 0; g_flashed.clear(); simulateUpload(fw);
  CHECK(g_updateBegins == 0 && g_flashed.empty() && g_lastCode == 401 && g_authChallenges == 1 && !otaActive);
  g_authOk = true;

  printf("T22 aborted upload\n");
  g_updateAborts = 0; simulateUpload(fw, true); server.find("/update", HTTP_POST)->fn();
  CHECK(g_updateAborts == 1 && g_lastCode == 400 && rebootAt == 0);
  g_millis += 5100; loop();

  printf("T45 upload in pieces: display progress follows the written bytes, one restart at the end\n");
  { reset(); setResponse({{0,"A"}}); loopWithUpdate();
    g_updateBegins = g_updateEnds = 0; g_flashed.clear(); rebootAt = 0;
    simulatePieces(fw, 65536, 1);
    CHECK(otaActive && g_updateBegins == 1 && g_flashed.size() == 65536 && rebootAt == 0);
    CHECK(contains(g_lastBody, "\"ok\":true") && contains(g_lastBody, "Received 65536 bytes."));
    CHECK(upload.lastPercent == (int)(65536ULL * 100 / fw.size()));            // display bar: written / file size
    loop(); loop(); CHECK(otaActive && !tileIsPic(0,0));                         // update screen stays between pieces
    g_flashed.clear(); g_updateBegins = 0; otaActive = false; Update.abort();
    simulatePieces(fw, 65536);
    CHECK(g_flashed == fw && g_updateBegins == 1 && g_updateEnds == 1 && rebootAt != 0);
    CHECK(contains(g_lastBody, "Update successful") && upload.lastPieceAt == 0);
    rebootAt = 0; otaActive = false;

    printf("    a piece out of order is rejected\n");
    reset(); setResponse({{0,"A"}}); loopWithUpdate();
    simulatePieces(fw, 65536, 1);
    std::vector<uint8_t> part(fw.begin() + 131072, fw.begin() + 196608);
    g_args["offset"] = "131072"; g_args["total"] = std::to_string(fw.size());
    g_updateEnds = 0; simulateUpload(part); g_args.clear();
    CHECK(g_lastCode == 400 && contains(g_lastBody, "out of order") && !otaActive && g_updateEnds == 0 && rebootAt == 0);
    g_millis += 5100; loop(); CHECK(tileIsPic(0,0));

    printf("    a merged image is rejected with the first piece\n");
    g_updateBegins = 0; simulatePieces(merged, 65536);
    CHECK(g_lastCode == 400 && contains(g_lastBody, "bootloader or merged") && !otaActive && g_flashed.empty());
    g_millis += 5100; loop();

    printf("    the page stops sending: given up after 30 s, display back\n");
    g_updateAborts = 0; simulatePieces(fw, 65536, 2);
    CHECK(otaActive);
    g_millis += 29000; loop(); CHECK(otaActive);
    g_millis += 1500; loop(); CHECK(!otaActive && g_updateAborts == 1 && restoreDisplayAt != 0);
    g_millis += 5100; loop(); CHECK(tileIsPic(0,0));
    setResponse({{0,"A2"}}); loopWithUpdate(); CHECK(isTitleDisplaying);     // Twitch updates run again
    reset(); }

  printf("T46 a live channel's title comes again after the set minutes, channels take turns\n");
  { reset(); for(auto& c : channels) c.titleShownSeq = 0;
    CHECK(config.titleRepeatMinutes == 5);                                     // default without a saved value
    setResponse({{0,"Title zero"},{2,"Title two"},{5,"Title five"}}); loopWithUpdate();
    for(int k = 0; k < 3; k++){ CHECK(isTitleDisplaying); runUntilTitleDone(); loopNoUpdate(1); }  // the three new titles
    CHECK(!isTitleDisplaying);
    std::vector<channelInfo*> shown;
    auto waitForRepeat = [&](){
      g_millis += 4 * 60000; loopNoUpdate(1); CHECK(!isTitleDisplaying);       // not before 5 minutes
      g_millis += 60000 + 10; loopNoUpdate(1); CHECK(isTitleDisplaying);
      shown.push_back(channelTitleInfo.channel); runUntilTitleDone(); };
    for(int k = 0; k < 6; k++) waitForRepeat();
    CHECK(shown[0] == &channels[0] && shown[1] == &channels[2] && shown[2] == &channels[5]);   // longest ago first
    CHECK(shown[3] == shown[0] && shown[4] == shown[1] && shown[5] == shown[2]);                 // then the same turns again

    printf("    a changed title counts as shown, and restarts the waiting time\n");
    g_millis += 3 * 60000;
    setResponse({{0,"Title zero"},{2,"Title two"},{5,"New five"}}); loopWithUpdate();
    CHECK(isTitleDisplaying && channelTitleInfo.channel == &channels[5]); runUntilTitleDone();
    g_millis += 4 * 60000; loopNoUpdate(1); CHECK(!isTitleDisplaying);
    g_millis += 60000 + 10; loopNoUpdate(1); CHECK(isTitleDisplaying && channelTitleInfo.channel == &channels[0]); runUntilTitleDone();

    printf("    never shown channels in equal turns: chance decides, each comes once\n");
    int first[3] = {0, 0, 0};
    for(int round = 0; round < 60; round++){
      for(int ch : {0, 2, 5}) channels[ch].titleShownSeq = 0;
      std::set<channelInfo*> once;
      for(int k = 0; k < 3; k++){
        g_millis += 5 * 60000 + 10; loopNoUpdate(1); CHECK(isTitleDisplaying);
        if(k == 0) first[channelTitleInfo.channel == &channels[0] ? 0 : channelTitleInfo.channel == &channels[2] ? 1 : 2]++;
        once.insert(channelTitleInfo.channel); runUntilTitleDone();
      }
      CHECK(once.size() == 3);
    }
    CHECK(first[0] >= 8 && first[1] >= 8 && first[2] >= 8);                  // each starts sometimes (60 rounds)

    printf("    0 = off; nobody live = nothing\n");
    config.titleRepeatMinutes = 0; g_millis += 60 * 60000; loopNoUpdate(1); CHECK(!isTitleDisplaying);
    config.titleRepeatMinutes = 5; setResponse({}); loopWithUpdate();
    g_millis += 10 * 60000; loopNoUpdate(1); CHECK(!isTitleDisplaying);

    printf("    Settings page: minutes validated and saved\n");
    auto post = [](std::map<std::string,std::string> a){ a["ssid"] = "homewifi"; g_args = a; server.find("/settings", HTTP_POST)->fn(); g_args.clear(); };
    rebootAt = 0;
    for(const char* bad : {"-1", "1441", "abc", ""}){ post({{"repeatmin", bad}}); CHECK(contains(g_lastBody, "Title repeat: enter minutes from 0 to 1440.") && rebootAt == 0); }
    CHECK(!g_nvs["config"].count("repeatmin"));
    post({{"repeatmin", " 12 "}}); CHECK(rebootAt != 0 && g_nvs["config"]["repeatmin"] == "12"); rebootAt = 0;
    loadConfig(); CHECK(config.titleRepeatMinutes == 12);
    post({{"repeatmin", "0"}}); CHECK(g_nvs["config"]["repeatmin"] == "0"); rebootAt = 0;
    loadConfig(); CHECK(config.titleRepeatMinutes == 0);
    server.find("/settings", HTTP_GET)->fn();
    CHECK(contains(g_lastBody, "name=\"repeatmin\" min=\"0\" max=\"1440\" value=\"0\"") && contains(g_lastBody, "take turns"));
    g_nvs["config"].erase("repeatmin"); loadConfig(); CHECK(config.titleRepeatMinutes == 5);
    reset(); }

  printf("T47 web login can be switched off on the Settings page\n");
  { CHECK(config.requireLogin);                                                // default: password needed
    g_authOk = false; g_authChallenges = 0;
    server.find("/", HTTP_GET)->fn(); CHECK(g_lastCode == 401 && g_authChallenges == 1);
    server.find("/settings", HTTP_GET)->fn();
    CHECK(g_lastCode == 401);
    g_authOk = true;
    server.find("/settings", HTTP_GET)->fn();
    CHECK(contains(g_lastBody, "name=\"login\" value=\"1\" checked>Require the password") && contains(g_lastBody, "name=\"login_form\""));
    server.find("/", HTTP_GET)->fn(); CHECK(!contains(g_lastBody, "Web login"));
    printf("    switched off: saved, pages and uploads without password\n");
    auto post = [](std::map<std::string,std::string> a){ a["ssid"] = "homewifi"; g_args = a; server.find("/settings", HTTP_POST)->fn(); g_args.clear(); };
    rebootAt = 0;
    post({{"login_form", "1"}}); CHECK(rebootAt != 0 && g_nvs["config"]["login"] == "0"); rebootAt = 0;
    loadConfig(); CHECK(!config.requireLogin);
    g_authOk = false; g_authChallenges = 0;
    server.find("/", HTTP_GET)->fn();
    CHECK(g_lastCode == 200 && g_authChallenges == 0 && contains(g_lastBody, "Web login</td><td class=\"err\">off: anyone on the network"));
    server.find("/settings", HTTP_GET)->fn(); CHECK(g_lastCode == 200 && contains(g_lastBody, "name=\"login\" value=\"1\">Require"));
    server.find("/update/check", HTTP_GET)->fn(); CHECK(g_authChallenges == 0);
    g_updateBegins = 0; g_updateEnds = 0; g_flashed.clear(); rebootAt = 0; otaActive = false;
    simulateUpload(fw);
    CHECK(g_authChallenges == 0 && g_flashed == fw && g_updateEnds == 1 && rebootAt != 0);
    rebootAt = 0; otaActive = false;
    printf("    the settings form keeps the posted choice after an error\n");
    post({{"login_form", "1"}, {"login", "1"}, {"repeatmin", "abc"}});
    CHECK(contains(g_lastBody, "Title repeat") && contains(g_lastBody, "value=\"1\" checked>Require") && g_nvs["config"]["login"] == "0");
    printf("    switched on again\n");
    post({{"login_form", "1"}, {"login", "1"}}); CHECK(g_nvs["config"]["login"] == "1"); rebootAt = 0;
    loadConfig(); CHECK(config.requireLogin);
    server.find("/", HTTP_GET)->fn(); CHECK(g_lastCode == 401 && g_authChallenges == 1);
    g_authOk = true; g_nvs["config"].erase("login"); loadConfig(); CHECK(config.requireLogin); }

  printf("T48 status page button shows all live titles once more\n");
  { reset();
    server.find("/", HTTP_GET)->fn(); CHECK(!contains(g_lastBody, "/titles/show"));   // nobody live: no button
    setResponse({{0,"Title zero"},{2,"Title two"},{5,"Title five"}}); loopWithUpdate();
    for(int k = 0; k < 3; k++){ runUntilTitleDone(); loopNoUpdate(1); }
    CHECK(!isTitleDisplaying && titleChangeQueue.empty());
    server.find("/", HTTP_GET)->fn(); CHECK(contains(g_lastBody, "action=\"/titles/show\"><p><button>Show all titles on the display"));
    g_authOk = false; g_authChallenges = 0; server.find("/titles/show", HTTP_POST)->fn();
    CHECK(g_authChallenges == 1 && titleChangeQueue.empty()); g_authOk = true;
    server.find("/titles/show", HTTP_POST)->fn();
    CHECK(g_lastCode == 303 && g_respHeaders["Location"] == "/");
    std::vector<channelInfo*> shown;
    for(int k = 0; k < 3; k++){ loopNoUpdate(1); CHECK(isTitleDisplaying); shown.push_back(channelTitleInfo.channel); runUntilTitleDone(); }
    CHECK(shown[0] == &channels[0] && shown[1] == &channels[2] && shown[2] == &channels[5]);
    loopNoUpdate(1); CHECK(!isTitleDisplaying);                               // each once
    printf("    pressed while a title scrolls: that one is not repeated, the others follow\n");
    server.find("/titles/show", HTTP_POST)->fn(); loopNoUpdate(1); CHECK(isTitleDisplaying && channelTitleInfo.channel == &channels[0]);
    server.find("/titles/show", HTTP_POST)->fn();                             // pressed twice: no duplicates
    CHECK(titleChangeQueue.size() == 2 && titleChangeQueue[0] == &channels[2] && titleChangeQueue[1] == &channels[5]);
    reset(); }

  printf("T49 fonts: code points, font.bin, glyphs, boxes\n");
  { auto cps = titleCodepoints("A\xc3\xa9\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd\xe2\x9d\xa4\xef\xb8\x8f\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x92\xbb\n\xff");
    CHECK(cps == std::vector<uint32_t>({'A', 0xE9, 0x1F44D, 0x2764, 0x1F468, ' '}));   // emoji as their first code point, the flag left out
    FontBin bin;
    CHECK(parseFontBin(g_fontBin.data(), g_fontBin.size(), bin) == nullptr && bin.crc == FONT_BIN_CRC && bin.textLen > 50000 && bin.emojiLen > 150000);
    auto bad = g_fontBin; bad[1000] ^= 1;
    CHECK(contains(parseFontBin(bad.data(), bad.size(), bin), "checksum"));
    CHECK(contains(parseFontBin((const uint8_t*)"nothing here, really......", 26, bin), "no font.bin"));
    CHECK(parseFontBin(g_fontBin.data(), g_fontBin.size() - 1, bin) != nullptr);              // cut off
    CHECK(titleFont.sourceFor('A') == TitleFont::TEXT && titleFont.sourceFor(0x416) == TitleFont::TEXT   // Latin, Cyrillic
          && titleFont.sourceFor(0x1F525) == TitleFont::EMOJI && titleFont.sourceFor(0x65E5) == TitleFont::NONE);   // emoji, CJK (not yet)
    GlyphBitmap g;
    CHECK(titleFont.render('W', TITLE_EM, g) && g.w > 30 && g.h > 30 && g.yoff < -30);
    CHECK(titleFont.render(0xC4, TITLE_EM, g) && TITLE_BASELINE + g.yoff >= 0);                // "Ä" fits the row
    CHECK(titleFont.render('g', TITLE_EM, g) && TITLE_BASELINE + g.yoff + g.h <= ROW_HEIGHT_FOR_TEST);   // descender fits
    CHECK(titleFont.render(0x1F525, TITLE_EM, g) && g.w > 30);
    CHECK(!titleFont.render(0x65E5, TITLE_EM, g) && titleFont.advance(0x65E5, TITLE_EM) == (int)(TITLE_EM * 0.6f));

    printf("    a title with emoji and a character no font has (box)\n");
    reset(); setResponse({{0,"Stream \xf0\x9f\x94\xa5 \xe6\x97\xa5 \xd0\x96"}}); loopWithUpdate();
    CHECK(isTitleDisplaying && !titleText.builtin && titleText.chars.size() == 16);   // "Stream 🔥 日 Ж | g"
    CHECK(titleText.chars[7] == 0x1F525 && titleText.chars[9] == 0x65E5 && titleText.advance[9] == 32);
    { // the box: a vertical line 37 px high at its left edge, somewhere on the canvas while it passes
      bool box = false;
      for(int k = 0; k < 400 && isTitleDisplaying && !box; k++){
        loopNoUpdate(1);
        const uint16_t* b = text_canvas.getBuffer();
        for(int x = 0; x < CANVAS_W && !box; x++){
          int n = 0; for(int y = TITLE_BASELINE - 37; y < TITLE_BASELINE; y++) if(b[y*CANVAS_W + x] == TITLE_COLOR) n++;
          if(n == 37) box = true;
        }
      }
      CHECK(box); }
    runUntilTitleDone(); CHECK(titleWaits == 0 && xSemaphoreTakeCount(titleLock) == 0);   // render task always ahead, lock released
    printf("    no RAM for a character: boxes instead\n");
    g_largestBlock = 4096;
    reset(); setResponse({{0,"Hallo"}}); loopWithUpdate();
    { const uint16_t* b = text_canvas.getBuffer();
      auto column = [&](int x){ int n = 0; for(int y = 0; y < 64; y++) if(b[y*CANVAS_W + x] == TITLE_COLOR) n++; return n; };
      CHECK(column(TITLE_LEAD + 3) == 37 && column(TITLE_LEAD + 10) == 2); }  // "H" as a box outline: left edge, top and bottom
    runUntilTitleDone(); g_largestBlock = 61440;
    server.find("/", HTTP_GET)->fn();
    CHECK(contains(g_lastBody, "<tr><td>Font</td><td>Noto Sans (font.bin ") && !contains(g_lastBody, "built-in font"));
    CHECK(contains(g_lastBody, "<tr><td>Firmware size</td><td>1413 KB of 1600 KB (88 %)</td></tr>") && contains(g_lastBody, "<tr><td>Flash</td><td>4 MB</td></tr>")
          && contains(g_lastBody, "<tr><td>RAM</td><td>283 KB heap</td></tr>"));
    reset(); }

  printf("T50 font.bin missing or damaged: built-in font, then downloaded from the release\n");
  { config.titleRepeatMinutes = 0;                                            // no repeated titles in between (downloads wait for titles)
    g_fontData[1000] ^= 1; openFont();
    CHECK(fontState == FontState::Missing && !titleFont.ready() && fontDownloadWanted && contains(fontProblem.c_str(), "checksum") && g_mmaps == 0);
    server.find("/", HTTP_GET)->fn(); CHECK(contains(g_lastBody, "built-in font: font.bin damaged (checksum), downloading font.bin"));
    reset(); fontRetryAt = millis() + 3600000;                                 // not yet: titles in the built-in font meanwhile
    setResponse({{0,"Gr\xc3\xbc\xc3\x9f" "e"}}); loopWithUpdate();
    CHECK(isTitleDisplaying && titleText.builtin && titleText.chars.size() == 9 && titleText.chars[2] == 0x81 && titleText.advance[0] == BUILTIN_ADVANCE);
    CHECK(titleInRow(1));
    runUntilTitleDone(); CHECK(!isTitleDisplaying);
    printf("    download (the newest release's font.bin for a development build)\n");
    fontRetryAt = 0; g_requests.clear();
    g_responses.push_back({200, std::string(g_fontBin.begin(), g_fontBin.end())});
    loop();
    CHECK(!g_requests.empty() && g_requests.back().url == "https://github.com/Modac/twitchDisplay/releases/latest/download/font.bin"
          && g_requests.back().follow == HTTPC_STRICT_FOLLOW_REDIRECTS && g_requests.back().tls);
    CHECK(fontState == FontState::Ready && titleFont.ready() && fontCrc == FONT_BIN_CRC && !fontDownloadWanted && !otaActive);
    CHECK(std::equal(g_fontBin.begin(), g_fontBin.end(), g_fontData.begin()) && g_mmaps == 1);
    reset(); setResponse({{0,"A"}}); loopWithUpdate(); CHECK(isTitleDisplaying && !titleText.builtin); runUntilTitleDone();

    printf("    download fails: built-in font, next try in an hour\n");
    g_fontData[1000] ^= 1; openFont();
    g_responses.push_back({404, "Not Found"}); loop();
    CHECK(fontState == FontState::Failed && contains(fontProblem.c_str(), "404") && fontDownloadWanted && !otaActive);
    server.find("/", HTTP_GET)->fn(); CHECK(contains(g_lastBody, "built-in font: font.bin download failed (404) (trying again later)"));
    size_t n = g_requests.size(); loop(); g_millis += 30 * 60000; loop(); CHECK(g_requests.size() == n);   // waits
    g_responses.push_back({200, std::string(g_fontBin.begin(), g_fontBin.end() - 100), (int)g_fontBin.size()});   // breaks off
    g_millis += 31 * 60000; loop();
    CHECK(g_requests.size() == n + 1 && fontState == FontState::Failed && contains(fontProblem.c_str(), "timed out"));
    g_millis += 61 * 60000; g_responses.push_back({200, std::string(g_fontBin.begin(), g_fontBin.end())}); loop();
    CHECK(fontState == FontState::Ready && !fontDownloadWanted);

    printf("    a valid font.bin of another version: used, the right one fetched once\n");
    { auto other = g_fontBin; other.insert(other.end(), 4, 0);
      uint32_t total = other.size(); memcpy(&other[8], &total, 4);
      uint32_t crc = fontCrc32(other.data() + 16, other.size() - 16); memcpy(&other[12], &crc, 4);
      std::fill(g_fontData.begin(), g_fontData.end(), 0xFF); std::copy(other.begin(), other.end(), g_fontData.begin());
      openFont(); fontRetryAt = 0;
      CHECK(fontState == FontState::Ready && titleFont.ready() && fontCrc == crc && fontDownloadWanted);
      server.find("/", HTTP_GET)->fn(); CHECK(contains(g_lastBody, "not this firmware's version, update pending"));
      g_responses.push_back({200, std::string(g_fontBin.begin(), g_fontBin.end())}); loop();
      CHECK(fontState == FontState::Ready && fontCrc == FONT_BIN_CRC && !fontDownloadWanted); }

    printf("    old partition layout: built-in font, no downloads, 16 logo slots\n");
    g_haveFontPartition = false; g_spiffs.size = 0x20000; openFont();
    CHECK(fontState == FontState::NoPartition && !titleFont.ready() && logoSlotCount() == 16);
    size_t before = g_requests.size(); loop(); CHECK(g_requests.size() == before);
    server.find("/", HTTP_GET)->fn(); CHECK(contains(g_lastBody, "old partition layout, flash firmware-merged.bin over USB once"));
    reset(); setResponse({{0,"A"}}); loopWithUpdate(); CHECK(isTitleDisplaying && titleText.builtin); runUntilTitleDone();
    g_haveFontPartition = true; g_spiffs.size = 0x40000;
    std::fill(g_fontData.begin(), g_fontData.end(), 0xFF); std::copy(g_fontBin.begin(), g_fontBin.end(), g_fontData.begin()); openFont();
    CHECK(fontState == FontState::Ready && logoSlotCount() == 32 && !fontDownloadWanted);
    config.titleRepeatMinutes = 5;
    printf("    the logo partition moved (new layout): stored logos are forgotten\n");
    saveLogoIndexEntry(3, LogoSlot{"12875057", 0x1234});
    checkLogoPartition(); CHECK(g_nvs["logos"].count("s3"));                   // same place: kept
    g_spiffs.address = 0x330000; checkLogoPartition();
    CHECK(!g_nvs["logos"].count("s3") && g_nvs["logos"]["part"] == "330000:40000");
    assignLogoSlots(); for(auto& c : channels) CHECK(c.logoSlot == -1);
    g_spiffs.address = 0;
    reset(); }

  printf("T51 colour emoji: sequences, download, cache, drawing\n");
  { auto names = [](const char* t){ std::vector<std::string> v; for(auto& c : titleChars(t)) v.push_back(c.emoji.empty() ? "U+" + std::to_string(c.cp) : c.emoji); return v; };
    CHECK(names("\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd\xe2\x9d\xa4\xef\xb8\x8f\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x92\xbb")
          == std::vector<std::string>({"1f44d-1f3fd", "2764", "1f1e9-1f1ea", "1f468-200d-1f4bb"}));            // skin tone, VS16 dropped without joiner, flag, joiner
    CHECK(names("#\xef\xb8\x8f\xe2\x83\xa3\xc2\xa9\xc2\xa9\xef\xb8\x8f\xe2\x9c\x85\xef\xb8\x8e") == std::vector<std::string>({"23-20e3", "U+169", "a9", "U+9989"}));   // keycap, © text/emoji, ✅ as text
    CHECK(names("\xf0\x9f\x91\xa9\xe2\x80\x8d\xe2\x9d\xa4\xef\xb8\x8f\xe2\x80\x8d\xf0\x9f\x91\xa8") == std::vector<std::string>({"1f469-200d-2764-fe0f-200d-1f468"}));   // joiner sequence keeps VS16
    CHECK(names("\xf0\x9f\x8f\xb4\xf3\xa0\x81\xa7\xf3\xa0\x81\xa2\xf3\xa0\x81\xa5\xf3\xa0\x81\xae\xf3\xa0\x81\xa7\xf3\xa0\x81\xbf") == std::vector<std::string>({"1f3f4-e0067-e0062-e0065-e006e-e0067-e007f"}));   // England
    emojiMissing.clear(); emojiError = "";                                    // earlier tests' titles had emoji too (with no images to get)
    CHECK(emojiCacheReady && cacheIndex.empty() && cacheBytes == 62 * 4096);
    std::string fire = avatar("bonjwa"), flag = avatar("gronkh"), heart = avatar("x");
    auto twitchAnswer = [](){ g_responses.push_back({200, g_response}); };
    reset(); g_requests.clear();
    setResponse({{0, "Stream \xf0\x9f\x94\xa5 \xf0\x9f\x87\xa9\xf0\x9f\x87\xaa \xe2\x9d\xa4\xef\xb8\x8f \xf0\x9f\x94\xa5"}}); twitchAnswer();
    g_responses.push_back({200, fire}); g_responses.push_back({200, flag}); g_responses.push_back({200, heart});
    loopWithUpdate();
    std::vector<std::string> urls; for(auto& r : g_requests) if(contains(r.url, "twemoji")) urls.push_back(r.url);
    CHECK(urls == std::vector<std::string>({TWEMOJI_URL "1f525.png", TWEMOJI_URL "1f1e9-1f1ea.png", TWEMOJI_URL "2764.png"}));   // each once, before the title
    CHECK(cacheIndex.size() == 3 && emojiDownloads >= 3 && emojiError.isEmpty());
    CHECK(isTitleDisplaying && titleText.cacheKey[7] == emojiKey("1f525") && titleText.advance[7] == EMOJI_ADVANCE && titleText.cacheKey[9] == emojiKey("1f1e9-1f1ea"));
    { std::vector<uint16_t> px; CHECK(readEmoji(emojiKey("1f525"), px));
      std::vector<uint16_t> expect(EMOJI_SIZE * EMOJI_SIZE); pngToRgb565((const uint8_t*)fire.data(), fire.size(), expect.data(), EMOJI_SIZE, EMOJI_SIZE);
      CHECK(px == expect);                                                       // stored losslessly
      bool drawn = false;                                                        // and drawn into the title
      for(int k = 0; k < 300 && isTitleDisplaying && !drawn; k++){
        loopNoUpdate(1);
        const uint16_t* b = text_canvas.getBuffer();
        for(int x = 0; x + EMOJI_SIZE <= CANVAS_W && !drawn; x++){
          bool same = true;
          for(int y = 0; y < EMOJI_SIZE && same; y += 7) for(int i = 0; i < EMOJI_SIZE && same; i += 5) if(expect[y*EMOJI_SIZE + i] && b[(EMOJI_TOP + y)*CANVAS_W + x + i] != expect[y*EMOJI_SIZE + i]) same = false;
          if(same) drawn = true;
        }
      }
      CHECK(drawn); }
    runUntilTitleDone(); CHECK(xSemaphoreTakeCount(emojiLock) == 0);

    printf("    cached: not downloaded again, also after a restart\n");
    g_requests.clear(); setResponse({{0, "Neu \xf0\x9f\x94\xa5\xe2\x9d\xa4"}}); twitchAnswer(); loopWithUpdate();
    CHECK(std::none_of(g_requests.begin(), g_requests.end(), [](const FakeRequest& r){ return contains(r.url, "twemoji"); }));
    runUntilTitleDone();
    openFont(); CHECK(cacheIndex.size() == 3 && emojiCached(emojiKey("2764")) && cacheWriteOffset % CACHE_SECTOR == 0 && cacheWriteOffset > 0);   // next entry in a fresh sector

    printf("    not in Twemoji (404): monochrome, not asked again; network error: tried again next time\n");
    g_requests.clear(); setResponse({{0, "A \xf0\x9f\x8e\xae \xf0\x9f\x91\xbe"}}); twitchAnswer();
    g_responses.push_back({404, "Not Found"}); g_responses.push_back({500, "oops"});
    loopWithUpdate();
    CHECK(isTitleDisplaying && titleText.cacheKey[2] == 0 && titleText.chars[2] == 0x1F3AE && titleText.advance[2] == titleFont.advance(0x1F3AE, TITLE_EM));   // monochrome 🎮
    CHECK(contains(emojiError.c_str(), "1f47e") && contains(emojiError.c_str(), "500"));
    runUntilTitleDone();
    g_requests.clear(); setResponse({{0, "A \xf0\x9f\x8e\xae \xf0\x9f\x91\xbe!"}}); twitchAnswer(); g_responses.push_back({200, heart}); loopWithUpdate();
    { std::vector<std::string> u; for(auto& r : g_requests) if(contains(r.url, "twemoji")) u.push_back(r.url);
      CHECK(u == std::vector<std::string>({TWEMOJI_URL "1f47e.png"})); }      // 🎮 not again, 👾 now
    CHECK(emojiCached(emojiKey("1f47e")) && titleText.cacheKey[4] == emojiKey("1f47e"));
    server.find("/", HTTP_GET)->fn(); CHECK(contains(g_lastBody, "<tr><td>Emoji, CJK</td><td>colour emoji (Twemoji, CC-BY 4.0), CJK downloaded<br>cache: 4 emoji, 0 CJK characters, ") && contains(g_lastBody, " of 248 KB"));
    runUntilTitleDone();

    printf("    monochrome only: no downloads, flags left out\n");
    config.colourEmoji = false; g_requests.clear();
    setResponse({{0, "B \xf0\x9f\x8d\x95 \xf0\x9f\x87\xa9\xf0\x9f\x87\xaa \xf0\x9f\x94\xa5"}}); twitchAnswer(); loopWithUpdate();
    CHECK(std::none_of(g_requests.begin(), g_requests.end(), [](const FakeRequest& r){ return contains(r.url, "twemoji"); }));
    CHECK(isTitleDisplaying && titleText.chars.size() == 10 && titleText.cacheKey[5] == 0 && titleText.chars[5] == 0x1F525);   // "B 🍕  🔥 | g" without the flag
    server.find("/", HTTP_GET)->fn(); CHECK(contains(g_lastBody, "<tr><td>Emoji, CJK</td><td>monochrome emoji, CJK downloaded"));
    runUntilTitleDone(); config.colourEmoji = true;

    printf("    full cache: the oldest sectors are replaced, the index survives a restart\n");
    std::vector<uint8_t> typical(2400, 1), big(5000, 1), tiny(30, 1);
    for(int i = 0; i < 70; i++) CHECK(storeCached(emojiKey("e" + std::to_string(i)), CACHE_COLOUR, EMOJI_SIZE, EMOJI_SIZE, 0, 0, EMOJI_ADVANCE, i % 10 == 9 ? big : typical));
    CHECK(!emojiCached(emojiKey("1f525")) && !emojiCached(emojiKey("e0")) && emojiCached(emojiKey("e69")) && emojiCached(emojiKey("e60")));
    for(int i = 0; i < 100; i++) CHECK(storeCached(emojiKey("t" + std::to_string(i)), CACHE_GLYPH, 2, 2, 0, -2, 20, tiny));   // small ones share sectors
    { std::set<uint32_t> sectors; for(auto& e : cacheIndex) if(e.size < 100) sectors.insert(e.offset / CACHE_SECTOR);
      CHECK(sectors.size() == 2); }                                          // 100 x 56 bytes in two sectors
    for(auto& e : cacheIndex) CHECK(e.size > CACHE_SECTOR ? e.offset % CACHE_SECTOR == 0 : e.offset % CACHE_SECTOR + e.size <= CACHE_SECTOR);
    size_t count = cacheIndex.size();
    openFont(); CHECK(cacheIndex.size() == count && emojiCached(emojiKey("e69")) && emojiCached(emojiKey("t99")));
    CHECK(!storeCached(1, CACHE_COLOUR, EMOJI_SIZE, EMOJI_SIZE, 0, 0, EMOJI_ADVANCE, std::vector<uint8_t>(9000, 1)));   // more than two sectors
    { const CacheIndexEntry* newest = nullptr; for(auto& e : cacheIndex) if(!newest || e.seq > newest->seq) newest = &e;
      g_fontData[emojiCacheStart + newest->offset + 30] ^= 0xFF; }          // damaged entry: dropped at the next start
    openFont(); CHECK(cacheIndex.size() == count - 1);

    printf("    setting\n");
    auto post = [](std::map<std::string,std::string> a){ a["ssid"] = "homewifi"; g_args = a; server.find("/settings", HTTP_POST)->fn(); g_args.clear(); };
    server.find("/settings", HTTP_GET)->fn();
    CHECK(contains(g_lastBody, "<select name=\"emoji\"><option value=\"colour\" selected>colour (downloaded, monochrome when that fails)"));
    rebootAt = 0; post({{"emoji", "mono"}}); CHECK(g_nvs["config"]["emoji"] == "mono" && rebootAt != 0); rebootAt = 0;
    loadConfig(); CHECK(!config.colourEmoji);
    server.find("/settings", HTTP_GET)->fn(); CHECK(contains(g_lastBody, "<option value=\"mono\" selected>"));
    g_nvs["config"].erase("emoji"); loadConfig(); CHECK(config.colourEmoji);
    reset(); }

  printf("T52 CJK characters: looked up in the CSS, piece into the free slot, rendered, cached\n");
  { auto css = [](std::vector<std::pair<int, std::string>> pieces){   // fontsource-like CSS: piece number -> unicode-range
      std::string c = "/* noto-sans-xx-latin-600-normal */\n@font-face {\n  src: url(./files/noto-sans-xx-latin-600-normal.woff2) format('woff2');\n  unicode-range: U+0000-00FF,U+4e00-9fff;\n}\n";
      for(auto& p : pieces) c += "@font-face {\n  font-family: 'Noto Sans XX';\n  src: url(./files/noto-sans-xx-" + std::to_string(p.first) + "-600-normal.woff2) format('woff2'), url(./files/noto-sans-xx-"
                                + std::to_string(p.first) + "-600-normal.woff) format('woff');\n  unicode-range: " + p.second + ";\n}\n\n";
      return c; };
    auto piece = readFile("data/noto-sans-kr-118-600-normal.ttf");
    CHECK(piece.size() > 10000);
    auto twitchAnswer = [](){ g_responses.push_back({200, g_response}); };
    auto cjkUrls = [](){ std::vector<std::string> v; for(auto& r : g_requests) if(contains(r.url, "fontsource")) v.push_back(r.url); return v; };
    reset(); g_requests.clear(); emojiError = ""; cjkMissing.clear(); appPendingVerify = false;
    size_t glyphsBefore = 0; for(auto& e : cacheIndex) if(e.type == CACHE_GLYPH) glyphsBefore++;
    std::fill(g_nextData.begin(), g_nextData.end(), 0xAB);                   // the free slot holds an old firmware
    setResponse({{0, "\xec\x95\x88\xec\x84\xb8 \xe4\xbd\xa0"}}); twitchAnswer();   // "안세 你"
    g_responses.push_back({200, css({{3, "U+3041-3096"}})});                 // Japanese: not there (你 asked here first)
    g_responses.push_back({200, css({{117, "U+c544-c547"}, {118, "U+c138,U+c548,U+4??"}})});   // Korean: piece 118
    g_responses.push_back({200, std::string(piece.begin(), piece.end()), -1});   // no Content-Length: until the server closes
    g_responses.push_back({200, css({{5, "U+4f5f-4f61"}})});                 // Chinese: piece 5
    g_responses.push_back({404, "Not Found"});
    loopWithUpdate();
    CHECK(cjkUrls() == std::vector<std::string>({
      "https://cdn.jsdelivr.net/npm/@fontsource/noto-sans-jp@5.3.1/600.css",
      "https://cdn.jsdelivr.net/npm/@fontsource/noto-sans-kr@5.3.1/600.css",
      "https://cdn.jsdelivr.net/fontsource/fonts/noto-sans-kr@5.3.1/118-600-normal.ttf",
      "https://cdn.jsdelivr.net/npm/@fontsource/noto-sans-sc@5.3.1/600.css",
      "https://cdn.jsdelivr.net/fontsource/fonts/noto-sans-sc@5.3.1/5-600-normal.ttf"}));
    CHECK(std::equal(piece.begin(), piece.end(), g_nextData.begin()) && !cjkSlotBusy);   // the piece went into the free slot
    CHECK(contains(emojiError.c_str(), "noto-sans-sc piece 5: download failed (404)") && !cjkMissing.count(0x4F60));   // tried again later
    { CacheIndexEntry e; CHECK(cacheLookup(glyphKey(0xC548), &e) && e.type == CACHE_GLYPH && cacheLookup(glyphKey(0xC138)) && !cacheLookup(glyphKey(0x4F60)));
      GlyphBitmap cached, direct; int adv = 0;
      CHECK(readCached(glyphKey(0xC548), nullptr, &cached) && TitleFont::renderFrom(piece.data(), 0xC548, TITLE_EM, direct, adv));
      CHECK(cached.alpha == direct.alpha && cached.w == direct.w && cached.yoff == direct.yoff && e.advance == adv); }   // stored losslessly
    CHECK(isTitleDisplaying && titleText.cacheKey[0] == glyphKey(0xC548) && titleText.cacheKey[1] == glyphKey(0xC138) && titleText.cacheKey[3] == 0);
    CHECK(titleText.advance[3] == (int)(TITLE_EM * 0.6f));                     // 你: a box for now
    { bool drawn = false;                                                      // 안 drawn in the title colour
      GlyphBitmap g; readCached(glyphKey(0xC548), nullptr, &g);
      for(int k = 0; k < 200 && isTitleDisplaying && !drawn; k++){
        loopNoUpdate(1);
        const uint16_t* b = text_canvas.getBuffer();
        for(int x = 0; x + g.w < CANVAS_W && !drawn; x++){
          int same = 0, ink = 0;
          for(int j = 0; j < g.h; j++) for(int i = 0; i < g.w; i++) if(g.alpha[j*g.w + i] == 255){ ink++; if(b[(TITLE_BASELINE + g.yoff + j)*CANVAS_W + x + g.xoff + i] == TITLE_COLOR) same++; }
          if(ink > 50 && same == ink) drawn = true;
        }
      }
      CHECK(drawn); }
    runUntilTitleDone();
    { size_t glyphs = 0; for(auto& e : cacheIndex) if(e.type == CACHE_GLYPH) glyphs++;
      server.find("/", HTTP_GET)->fn(); CHECK(contains(g_lastBody, "emoji, " + std::to_string(glyphs) + " CJK characters,") && glyphs == glyphsBefore + 2); }

    printf("    a character no font has: looked up in all three, then not again\n");
    g_requests.clear(); setResponse({{0, "\xe4\xb8\x81"}}); twitchAnswer();      // 丁
    for(int k = 0; k < 3; k++) g_responses.push_back({200, css({{1, "U+3041"}})});
    loopWithUpdate(); runUntilTitleDone();
    CHECK(cjkUrls().size() == 3 && cjkMissing.count(0x4E01));
    g_requests.clear(); setResponse({{0, "\xe4\xb8\x81!"}}); twitchAnswer(); loopWithUpdate(); runUntilTitleDone();
    CHECK(cjkUrls().empty());

    printf("    not while the firmware is on probation, not while an update runs, not when switched off\n");
    for(int mode = 0; mode < 3; mode++){
      appPendingVerify = mode == 0; otaActive = false; config.cjk = mode != 2;
      g_requests.clear(); setResponse({{0, "\xe4\xbd\xa0" + std::to_string(mode)}}); twitchAnswer();
      if(mode == 1){ otaActive = true; fetchMissingCjk(std::vector<StreamInfo>{StreamInfo{"1", "x", "\xe4\xbd\xa0"}}); otaActive = false; }
      else { loopWithUpdate(); runUntilTitleDone(); }
      CHECK(cjkUrls().empty());
    }
    appPendingVerify = false; config.cjk = true;
    printf("    updates wait for a piece download that uses the slot\n");
    cjkSlotBusy = true; unsigned long t0 = g_millis; waitForCjkSlot(); CHECK(g_millis - t0 >= 20000);   // gives up after 20 s
    cjkSlotBusy = false; t0 = g_millis; waitForCjkSlot(); CHECK(g_millis - t0 < 10);

    printf("    clear the cache: erased, failed ones asked again\n");
    { server.find("/", HTTP_GET)->fn(); CHECK(contains(g_lastBody, "action=\"/cache/clear\"") && contains(g_lastBody, "Clear emoji/CJK cache"));
      CHECK(!cacheIndex.empty() && !cjkMissing.empty());
      g_authOk = false; server.find("/cache/clear", HTTP_POST)->fn(); CHECK(g_lastCode == 401 && !cacheIndex.empty()); g_authOk = true;
      server.find("/cache/clear", HTTP_POST)->fn();
      CHECK(g_lastCode == 303 && cacheIndex.empty() && cacheWriteOffset == 0 && emojiError.isEmpty());
      CHECK(std::all_of(g_fontData.begin() + emojiCacheStart, g_fontData.begin() + emojiCacheStart + cacheBytes, [](uint8_t b){ return b == 0xFF; }));
      CHECK(std::equal(g_fontBin.begin(), g_fontBin.end(), g_fontData.begin()));          // font.bin untouched
      openFont(); CHECK(fontState == FontState::Ready && cacheIndex.empty());
      g_requests.clear(); setResponse({{0, "\xe4\xb8\x81"}}); twitchAnswer();            // 丁 was "in no font": looked up again
      for(int k = 0; k < 3; k++) g_responses.push_back({200, css({{1, "U+3041"}})});
      loopWithUpdate(); CHECK(cjkUrls().size() == 3 && !cacheMissingReset); runUntilTitleDone(); }

    printf("    downloads wait for a pause between titles and for enough RAM\n");
    { reset(); setResponse({{0, "Lang genug"}}); loopWithUpdate(); CHECK(isTitleDisplaying);   // a title scrolls
      g_requests.clear(); setResponse({{0, "Lang genug"}, {2, "Neu \xf0\x9f\xa6\x84"}}); twitchAnswer();   // 🦄 new
      unsigned long t0 = g_millis; runNetworkUpdate();
      CHECK(cjkUrls().empty() && std::none_of(g_requests.begin(), g_requests.end(), [](const FakeRequest& r){ return contains(r.url, "twemoji"); }));
      CHECK(g_millis - t0 >= TITLE_PAUSE_WAIT && !updateQueue->items.empty());   // waited 2 minutes, then handed the result over anyway
      loop(); runUntilTitleDone(); while(isTitleDisplaying || !titleChangeQueue.empty()){ runUntilTitleDone(); loopNoUpdate(1); }
      g_requests.clear(); setResponse({{0, "Lang genug"}, {2, "Neu \xf0\x9f\xa6\x84!"}}); twitchAnswer(); g_responses.push_back({200, avatar("x")});
      runNetworkUpdate(); CHECK(emojiCached(emojiKey("1f984")));          // in the pause: downloaded
      loop(); runUntilTitleDone(); while(isTitleDisplaying || !titleChangeQueue.empty()){ runUntilTitleDone(); loopNoUpdate(1); }
      g_largestBlock = 16000; emojiError = "";
      g_requests.clear(); setResponse({{0, "Wenig \xf0\x9f\xa6\x8a"}}); twitchAnswer(); runNetworkUpdate();
      CHECK(!emojiCached(emojiKey("1f98a")) && contains(emojiError.c_str(), "not enough free memory") && std::none_of(g_requests.begin(), g_requests.end(), [](const FakeRequest& r){ return contains(r.url, "twemoji"); }));
      g_largestBlock = 61440; loop(); runUntilTitleDone();
      printf("    enough for the download but not for decoding: not marked as missing, tried again\n");
      reset(); emojiError = ""; g_largestBlock = 20000;
      g_requests.clear(); setResponse({{0, "Wenig \xf0\x9f\xa6\x8a"}}); twitchAnswer(); g_responses.push_back({200, avatar("x")}); runNetworkUpdate();
      CHECK(!emojiCached(emojiKey("1f98a")) && !emojiMissing.count(emojiKey("1f98a")) && contains(emojiError.c_str(), "decode it, trying again later"));
      g_largestBlock = 61440; loop(); runUntilTitleDone();
      g_requests.clear(); setResponse({{0, "Wenig \xf0\x9f\xa6\x8a!"}}); twitchAnswer(); g_responses.push_back({200, avatar("x")}); runNetworkUpdate();
      CHECK(emojiCached(emojiKey("1f98a")));
      loop(); runUntilTitleDone(); reset(); }

    printf("    setting\n");
    auto post = [](std::map<std::string,std::string> a){ a["ssid"] = "homewifi"; g_args = a; server.find("/settings", HTTP_POST)->fn(); g_args.clear(); };
    server.find("/settings", HTTP_GET)->fn(); CHECK(contains(g_lastBody, "<select name=\"cjk\"><option value=\"on\" selected>downloaded when a title needs them"));
    rebootAt = 0; post({{"cjk", "off"}}); CHECK(g_nvs["config"]["cjk"] == "off"); rebootAt = 0;
    loadConfig(); CHECK(!config.cjk);
    g_nvs["config"].erase("cjk"); loadConfig(); CHECK(config.cjk);
    reset(); }

  printf("T23 new firmware confirmed only after 60 s online\n");
  appPendingVerify = true; g_otaState = ESP_OTA_IMG_PENDING_VERIFY; g_markedValid = 0; g_wifiUp = true;
  for(int i=0;i<3000;i++) loop();              // ~30 s
  g_wifiUp = false; loop(); g_wifiUp = true;   // connection lost: timer restarts
  for(int i=0;i<4000;i++) loop();              // ~40 s
  CHECK(g_markedValid == 0 && appPendingVerify);
  for(int i=0;i<2200;i++) loop();              // > 60 s since reconnect
  CHECK(g_markedValid == 1 && !appPendingVerify && g_otaState == ESP_OTA_IMG_VALID);
  CHECK(verifyRollbackLater() == true);
  CHECK(g_wdtTimeout == 30000);

  printf("T24 web pages: login, escaping, no login challenge for favicon/404\n");
  reset();
  g_response = "{\"data\":[{\"user_id\":\"21991090\",\"user_name\":\"PietSmiet\",\"title\":\"<script>alert(1)</script> & Gr\\u00fc\\u00dfe\",\"game_name\":\"Minecraft\"}]}";
  loopWithUpdate();
  g_authOk = true; server.find("/", HTTP_GET)->fn();
  CHECK(g_lastCode == 200);
  CHECK(g_lastBody.find("<b>PietSmiet</b>") != std::string::npos);
  CHECK(g_lastBody.find("&lt;script&gt;alert(1)&lt;/script&gt; &amp; Gr\xc3\xbc\xc3\x9f" "e | Minecraft") != std::string::npos);
  CHECK(g_lastBody.find("<script>alert") == std::string::npos);
  if(getenv("DUMP")){ std::ofstream("status.html") << g_lastBody; }
  CHECK(g_lastBody.find("flashed via USB") != std::string::npos || g_lastBody.find("confirmed") != std::string::npos);
  server.find("/update", HTTP_GET)->fn();
  CHECK(g_lastCode == 200 && g_lastBody.find("const maxSize = 1638400;") != std::string::npos);
  if(getenv("DUMP")){ std::ofstream("update.html") << g_lastBody; }
  g_authOk = false; g_authChallenges = 0;
  server.find("/", HTTP_GET)->fn(); CHECK(g_lastCode == 401);
  server.find("/reboot", HTTP_POST)->fn(); CHECK(g_lastCode == 401 && rebootAt == 0);
  server.find("/favicon.ico", HTTP_GET)->fn(); CHECK(g_lastCode == 204);
  server.notFound(); CHECK(g_lastCode == 404);
  CHECK(g_authChallenges == 2);
  g_authOk = true; server.find("/reboot", HTTP_POST)->fn(); CHECK(rebootAt != 0); rebootAt = 0;

  printf("T25 settings: NVS values override secrets.h defaults, an empty stored value beats the default\n");
  CHECK(!setupMode && config.wifiSsid == "homewifi" && config.webPassword == "pw" && g_beginSsid == "homewifi" && g_beginPw == "x");
  g_nvs.clear(); g_nvs["config"]["ssid"] = "other"; g_nvs["config"]["token"] = ""; g_nvs["config"]["secret"] = "";
  loadConfig();
  CHECK(config.wifiSsid == "other" && config.token == "" && config.clientSecret == "" && config.wifiPassword == "x");
  g_nvs.clear(); loadConfig();
  CHECK(config.wifiSsid == "homewifi");
#ifdef VARIANT_SECRET
  CHECK(config.clientSecret == "mysecret" && std::string(config.effectiveClientId()) == "myid");
#else
  CHECK(config.token == "statictoken" && std::string(config.effectiveClientId()) == DEFAULT_CLIENT_ID);
#endif

  printf("T26 settings page: login required, stored secrets never shown\n");
  g_args.clear(); g_authOk = false; server.find("/settings", HTTP_GET)->fn(); CHECK(g_lastCode == 401);
  g_authOk = true; server.find("/settings", HTTP_GET)->fn();
  CHECK(g_lastCode == 200 && contains(g_lastBody, "value=\"homewifi\"") && contains(g_lastBody, "stored - leave empty to keep"));
  CHECK(!contains(g_lastBody, "statictoken") && !contains(g_lastBody, "mysecret") && !contains(g_lastBody, "value=\"x\""));
  CHECK(contains(g_lastBody, "href=\"/settings\">Settings") && contains(g_lastBody, "href=\"/update\""));
  CHECK(!contains(g_lastBody, "Setup mode"));
  if(getenv("DUMP")) std::ofstream("settings.html") << g_lastBody;

  printf("T27 settings validation: nothing saved on errors\n");
  rebootAt = 0;
  auto post = [](std::map<std::string,std::string> a){ g_args = a; server.find("/settings", HTTP_POST)->fn(); g_args.clear(); };
  post({{"ssid","homewifi"},{"webpw","short"},{"webpw2","short"}});           CHECK(contains(g_lastBody, "at least 8 characters"));
  post({{"ssid","homewifi"},{"webpw","password1"},{"webpw2","password2"}});   CHECK(contains(g_lastBody, "do not match"));
  post({{"ssid","homewifi"},{"token","abc\r\nX-Evil: 1"}});                   CHECK(contains(g_lastBody, "letters and digits"));
  post({{"ssid","homewifi"},{"wifipw","1234"}});                              CHECK(contains(g_lastBody, "8 to 63 characters"));
  post({{"ssid",""}});                                                        CHECK(contains(g_lastBody, "name of your WiFi"));
  post({{"ssid","homewifi"},{"clientid","bad id<script>"}});                  CHECK(contains(g_lastBody, "letters and digits") && !contains(g_lastBody, "<script>"));
  CHECK(contains(g_lastBody, "value=\"bad id&lt;script&gt;\""));               // entered value shown again, escaped
  CHECK(g_nvs.empty() && rebootAt == 0);

  printf("T28 settings save: empty secret fields keep values, 'Remove' clears, oauth: prefix stripped\n");
  post({{"ssid"," newnet "},{"wifipw",""},{"clientid","cid123"},{"secret",""},{"token","oauth:abc123"},{"webpw",""},{"webpw2",""}});
  CHECK(rebootAt != 0 && contains(g_lastBody, "were saved"));
  CHECK(g_nvs["config"]["ssid"] == "newnet" && g_nvs["config"]["clientid"] == "cid123" && g_nvs["config"]["token"] == "abc123");
  CHECK(!g_nvs["config"].count("wifipw") && !g_nvs["config"].count("secret") && !g_nvs["config"].count("webpw"));
  CHECK(!contains(g_lastBody, "abc123"));
  loadConfig(); CHECK(config.wifiPassword == "x" && config.token == "abc123" && config.webPassword == "pw");
  rebootAt = 0;
  post({{"ssid","newnet"},{"secret_clear","on"},{"token_clear","on"},{"webpw","newpassword1"},{"webpw2","newpassword1"}});
  CHECK(g_nvs["config"]["secret"] == "" && g_nvs["config"]["token"] == "" && g_nvs["config"]["webpw"] == "newpassword1");
  rebootAt = 0;

  printf("T29 reset removes all stored settings\n");
  g_authOk = false; server.find("/reset", HTTP_POST)->fn(); CHECK(g_lastCode == 401 && !g_nvs["config"].empty());
  g_authOk = true; server.find("/reset", HTTP_POST)->fn(); CHECK(g_nvs["config"].empty() && rebootAt != 0);
  rebootAt = 0; g_millis += 5100; loop(); loadConfig();

  printf("T30 no Twitch credentials: no request, magenta dot, hint on status page\n");
  { Config saved = config; String savedToken = twitchToken;
    config.token = ""; config.clientSecret = ""; twitchToken = "";
    size_t n0 = g_requests.size(); runNetworkUpdate(); loop();
    CHECK(g_requests.size() == n0 && statusDot() == STATUS_AUTH);
    server.find("/", HTTP_GET)->fn(); CHECK(contains(g_lastBody, "No Twitch credentials"));
    config = saved; twitchToken = savedToken; }

  printf("T31 BOOT held 5 s restarts into setup mode, a short press does nothing\n");
  setupRequested = 0;
  g_bootPressed = true; CHECK(!runLoopFor(2000)); g_bootPressed = false; loop();
  g_bootPressed = true; CHECK(!runLoopFor(4900)); CHECK(runLoopFor(300)); g_bootPressed = false;
  CHECK(setupRequested == SETUP_REQUEST_MAGIC); setupRequested = 0;

  printf("T32 online update: check the newest GitHub release\n");
  { auto fw = makeAppImage(1100000);
    auto check = [](){ server.find("/update/check", HTTP_GET)->fn(); };
    g_authOk = false; check(); CHECK(g_lastCode == 401); g_authOk = true;
    g_requests.clear();
    g_responses.push_back({404, "{\"message\":\"Not Found\"}"}); check();
    CHECK(contains(g_lastBody, "\"ok\":false") && contains(g_lastBody, "No release has been published yet."));
    CHECK(g_requests.back().url == "https://api.github.com/repos/Modac/twitchDisplay/releases/latest" && g_requests.back().tls && g_requests.back().userAgent == "twitchDisplay/unknown");
    g_responses.push_back({200, releaseJson("v1.2.0", {{"firmware-merged.bin", 1800000}, {"firmware.bin", fw.size()}})}); check();
    CHECK(contains(g_lastBody, "\"ok\":true") && contains(g_lastBody, "\"latest\":\"v1.2.0\"") && contains(g_lastBody, "\"upToDate\":false"));
    CHECK(contains(g_lastBody, "\"notes\":\"https://github.com/Modac/twitchDisplay/releases/tag/v1.2.0\"") && contains(g_lastBody, "\"published\":\"2026-11-03T20:05:00Z\""));
    g_responses.push_back({200, releaseJson("unknown", {{"firmware.bin", fw.size()}})}); check();
    CHECK(contains(g_lastBody, "\"upToDate\":true"));
    g_responses.push_back({200, releaseJson("v1.3.0", {{"source.zip", 1000}})}); check();
    CHECK(contains(g_lastBody, "\"ok\":false") && contains(g_lastBody, "has no firmware.bin"));
    g_responses.push_back({200, releaseJson("v1.4.0", {{"firmware.bin", 3000000}})}); check();
    CHECK(contains(g_lastBody, "too large"));
    g_responses.push_back({500, "oops"}); check(); CHECK(contains(g_lastBody, "GitHub request failed (500)"));
    g_responses.push_back({200, "{\"unexpected\":1}"}); check(); CHECK(contains(g_lastBody, "Unexpected answer"));

    printf("T33 online update: install downloads, checks and writes the firmware, then restarts\n");
    reset(); setResponse({{0,"A"}}); loopWithUpdate();
    g_responses.push_back({200, releaseJson("v1.2.0", {{"firmware.bin", fw.size()}})});
    g_responses.push_back({200, std::string(fw.begin(), fw.end())});
    g_updateBegins = g_updateEnds = 0; g_flashed.clear(); g_updateEndOk = true;
    server.find("/update/install", HTTP_POST)->fn();
    CHECK(contains(g_lastBody, "\"ok\":true") && contains(g_lastBody, "Installing v1.2.0") && installRequested);
    server.find("/update/state", HTTP_GET)->fn(); CHECK(contains(g_lastBody, "\"state\":\"installing\""));
    size_t nReq = g_requests.size();
    loop();
    CHECK(g_requests.size() == nReq + 1 && g_requests.back().url == "https://github.com/Modac/twitchDisplay/releases/download/v1.2.0/firmware.bin");
    CHECK(g_requests.back().follow == HTTPC_STRICT_FOLLOW_REDIRECTS && g_requests.back().tls);
    CHECK(g_flashed == fw && g_updateBegins == 1 && g_updateEnds == 1 && rebootAt != 0 && otaActive);
    runNetworkUpdate(); CHECK(g_requests.size() == nReq + 1);   // Twitch paused
    CHECK(runLoopFor(1500)); rebootAt = 0; otaActive = false;

    printf("T34 online update: a merged image is rejected before anything is written\n");
    reset(); setResponse({{0,"A"}}); loopWithUpdate(); runUntilTitleDone();
    auto merged = makeMergedImage(1600000);
    g_responses.push_back({200, releaseJson("v1.2.1", {{"firmware.bin", merged.size()}})});
    g_responses.push_back({200, std::string(merged.begin(), merged.end())});
    g_updateBegins = 0; g_flashed.clear();
    server.find("/update/install", HTTP_POST)->fn(); loop();
    CHECK(g_updateBegins == 0 && g_flashed.empty() && rebootAt == 0 && !otaActive);
    server.find("/update/state", HTTP_GET)->fn();
    CHECK(contains(g_lastBody, "\"state\":\"failed\"") && contains(g_lastBody, "bootloader or merged"));
    CHECK(!tileIsPic(0,0)); g_millis += 5100; loop(); CHECK(tileIsPic(0,0));   // error shown, then back to the grid
    setResponse({{0,"A2"}}); loopWithUpdate(); CHECK(isTitleDisplaying);        // Twitch works again

    printf("T35 online update: download errors\n");
    g_responses.push_back({200, releaseJson("v1.2.2", {{"firmware.bin", fw.size()}})});
    g_responses.push_back({500, "error"});
    server.find("/update/install", HTTP_POST)->fn(); loop();
    server.find("/update/state", HTTP_GET)->fn(); CHECK(contains(g_lastBody, "Download failed (500)"));
    g_millis += 5100; loop();
    g_responses.push_back({200, releaseJson("v1.2.3", {{"firmware.bin", fw.size()}})});
    g_responses.push_back({200, std::string(fw.begin(), fw.begin() + 100000), (int)fw.size()}); // connection breaks off
    g_updateAborts = 0; g_wdtFeeds = 0; unsigned long t0 = g_millis;
    server.find("/update/install", HTTP_POST)->fn(); loop();
    server.find("/update/state", HTTP_GET)->fn(); CHECK(contains(g_lastBody, "Download timed out."));
    CHECK(g_updateAborts == 1 && rebootAt == 0 && g_millis - t0 >= 15000 && g_wdtFeeds > 1000);
    g_millis += 5100; loop();
    g_responses.push_back({200, releaseJson("v1.2.4", {{"firmware.bin", fw.size()}})});
    g_responses.push_back({200, std::string(fw.begin(), fw.end())}); g_updateEndOk = false;
    server.find("/update/install", HTTP_POST)->fn(); loop();
    server.find("/update/state", HTTP_GET)->fn(); CHECK(contains(g_lastBody, "Image verification failed") && rebootAt == 0);
    g_updateEndOk = true; g_millis += 5100; loop();
    g_responses.push_back({404, "{}"});
    server.find("/update/install", HTTP_POST)->fn(); CHECK(contains(g_lastBody, "\"ok\":false") && !installRequested);
  }

  printf("T36 update page shows the online update with the installed version\n");
  server.find("/update", HTTP_GET)->fn();
  CHECK(contains(g_lastBody, "Online update") && contains(g_lastBody, "Installed version: <b>unknown</b>") && contains(g_lastBody, "Upload firmware"));
  CHECK(!contains(g_lastBody, "%ONLINE%") && !contains(g_lastBody, "%VERSION%"));
  if(getenv("DUMP")) std::ofstream("update_online.html") << g_lastBody;

  printf("T38 message screen: text never overlaps the progress bar and stays on screen\n");
  { auto barRows = [](){ int top=-1, bottom=-1; for(int y=0;y<170;y++) if(g_fb[y][20]==0xFFFF){ if(top<0) top=y; bottom=y; } return std::make_pair(top, bottom); };
    auto overlapsBar = [&](){ auto b = barRows(); for(auto& t: g_textBoxes) if(t.y <= b.second && t.y + t.h - 1 >= b.first) { printf("    '%s' y=%d..%d overlaps bar y=%d..%d\n", t.text.c_str(), t.y, t.y+t.h-1, b.first, b.second); return true; } return false; };
    drawMessageScreen("Firmware update", ST77XX_CYAN, "Downloading v0.2.10"); drawUpdateProgress(50);
    CHECK(barRows().first > 0 && !overlapsBar());
    drawMessageScreen("Firmware update", ST77XX_CYAN, nullptr); drawUpdateProgress(0);
    CHECK(!overlapsBar());
    drawMessageScreen("Update failed", ST77XX_RED, "Not an application image. Upload firmware.bin, not the bootloader or merged image.");
    for(auto& t: g_textBoxes) CHECK(t.y + t.h <= 170 && t.x + t.w <= 320);
    restoreDisplay(); }

  printf("T39 logo storage: slots in the spiffs partition, index in NVS, drawing order logo > picture > placeholder\n");
  { reset();
    std::vector<uint16_t> logoA(64*64), logoB(64*64), back(64*64);
    for(int i=0;i<64*64;i++){ logoA[i] = 0x1000 + i; logoB[i] = 0xF000 - i; }
    g_flashBadWrites = 0;
    CHECK(writeLogo(0, logoA.data()) && writeLogo(15, logoB.data()));
    CHECK(writeLogo(0, logoB.data()));                                   // rewrite: erased before writing
    CHECK(readLogo(0, back.data()) && back == logoB && readLogo(15, back.data()) && back == logoB);
    CHECK(g_flashBadWrites == 0);
    CHECK(logoSlotCount() == 32 && writeLogo(31, logoA.data()) && readLogo(31, back.data()) && back == logoA);   // 32 slots in the new layout
    CHECK(!writeLogo(32, logoA.data()) && !readLogo(-1, back.data()) && !readLogo(32, back.data()));
    g_flashFail = true; CHECK(!writeLogo(1, logoA.data())); g_flashFail = false;

    CHECK(writeLogo(3, logoA.data()));
    saveLogoIndexEntry(3, LogoSlot{"12875057", 0xdeadbeef});
    saveLogoIndexEntry(5, LogoSlot{"99999999", 0x00000001});
    std::array<LogoSlot, MAX_LOGO_SLOTS> idx; loadLogoIndex(idx);
    CHECK(idx[3].channelId == "12875057" && idx[3].urlHash == 0xdeadbeef && idx[5].channelId == "99999999" && idx[5].urlHash == 1 && idx[0].channelId.empty());
    saveLogoIndexEntry(5, LogoSlot{}); loadLogoIndex(idx); CHECK(idx[5].channelId.empty() && g_nvs["logos"].size() == 1);

    assignLogoSlots();
    CHECK(channels[4].logoSlot == 3 && channels[0].logoSlot == -1);
    setResponse({{0,"A"},{4,"B"}}); loopWithUpdate(); runUntilTitleDone();
    { uint16_t x,y; slotXY(1,x,y); bool same = true;                    // gronkh (slot 1) shows the stored logo
      for(int j=0;j<64;j++) for(int i=0;i<64;i++) if(g_fb[y+j][x+i] != logoA[j*64+i]) same = false;
      CHECK(same); }
    CHECK(tileIsPic(0,0));                                               // lidi: no logo, placeholder
    setResponse({{0,"A"},{2,"C"},{4,"B"}}); loopWithUpdate(); runUntilTitleDone();
    { uint16_t x0,y0,x1,y1; slotXY(0,x0,y0); slotXY(1,x1,y1);
      uint16_t bg0 = g_fb[y0+2][x0+2], bg1 = g_fb[y1+2][x1+2];
      CHECK(bg0 != 0 && bg1 != 0 && bg0 != bg1);                         // coloured, differs per channel
      auto white = [&](int x, int y){ int n = 0; for(int j = 20; j < 46; j++) for(int i = 6; i < 58; i++) if(g_fb[y+j][x+i] == ST77XX_WHITE) n++; return n; };
      CHECK(white(x0, y0) > 60 && g_fb[y0+10][x0+32] == bg0 && g_fb[y0+50][x0+32] == bg0); }   // initials (Noto Sans) in the middle rows
    CHECK(placeholderInitials("n2") == std::vector<uint32_t>({'N', '2'}) && placeholderInitials("N2xyz") == std::vector<uint32_t>({'N', '2'}));
    CHECK(placeholderInitials("\xc3\xa4x") == std::vector<uint32_t>({0xC4, 'X'}));        // "äx" -> "ÄX"
    CHECK(placeholderInitials("_\xf0\x9f\x8e\xae z") == std::vector<uint32_t>({'Z'}) && placeholderInitials("") == std::vector<uint32_t>({'?'}));
    { channelInfo anon; anon.id = "123"; drawPlaceholder(anon); int n = 0;  // no name: "?"
      for(int i = 0; i < 64*64; i++) if(pic_canvas.getBuffer()[i] == ST77XX_WHITE) n++;
      CHECK(n > 20); }
    channels[0].name = channels[2].name = "";
    g_nvs.erase("logos"); for(auto& c : channels) c.logoSlot = -1;
    std::fill(g_flashData.begin(), g_flashData.end(), 0xFF);
    reset(); }

  printf("T40 channel list: defaults, lookup via Twitch, cache, Settings page\n");
  { auto usersJson = [](std::vector<std::array<std::string,3>> users){
      std::string j = "{\"data\":[";
      for(size_t i=0;i<users.size();i++){ if(i) j += ",";
        j += "{\"id\":\"" + users[i][0] + "\",\"login\":\"" + users[i][1] + "\",\"display_name\":\"" + users[i][2] + "\",\"type\":\"\",\"broadcaster_type\":\"partner\","
             "\"description\":\"x\",\"profile_image_url\":\"\",\"offline_image_url\":\"\",\"view_count\":0,\"created_at\":\"2020-01-01T00:00:00Z\"}"; }
      return j + "]}"; };
    reset(); g_nvs["config"].erase("channels"); g_nvs["config"].erase("chcache"); buildChannels(); assignLogoSlots();
    CHECK(channels.size() == 10 && channels[1].id == "21991090" && channels[1].login.empty());

    printf("    lookup of the default channels by id\n");
    std::vector<std::array<std::string,3>> all = {{"761017145","lidi","Lidi"},{"21991090","pietsmiet","PietSmiet"},{"73437396","bonjwa","Bonjwa"},
      {"1024088182","bonjwachill","BonjwaChill"},{"12875057","gronkh","Gronkh"},{"16064695","dhalucard","Dhalucard"},{"55898523","trilluxe","Trilluxe"},
      {"38770961","dracon","Dracon"},{"172376071","maxim","Maxim"}};   // finanzfluss missing: account gone
    g_responses.push_back({200, usersJson(all)});
    size_t n0 = g_requests.size();
    CHECK(runChannelSync());
    CHECK(g_requests.size() >= n0 + 1 && contains(g_requests.back().url, "https://api.twitch.tv/helix/users?id=761017145&id=21991090&")); // (+ token request with a client secret)
    CHECK(channels[1].login.empty());                         // loop() has not applied it yet
    loop();
    CHECK(channels[1].login == "pietsmiet" && channels[1].name == "PietSmiet" && channels[9].login.empty());
    CHECK(unknownChannels.size() == 1 && unknownChannels[0] == "549536744");
    CHECK(contains(g_nvs["config"]["chcache"], "21991090\tpietsmiet\tPietSmiet\n") && !contains(g_nvs["config"]["chcache"], "549536744"));
    server.find("/", HTTP_GET)->fn();
    CHECK(contains(g_lastBody, "<td>PietSmiet</td><td>offline") && contains(g_lastBody, "unknown to Twitch"));
    server.find("/settings", HTTP_GET)->fn();
    CHECK(contains(g_lastBody, "lidi\npietsmiet\nbonjwa") && contains(g_lastBody, "Twitch does not know: 549536744"));

    printf("    failed lookup: retried later, nothing changes\n");
    g_responses.push_back({500, "x"}); CHECK(!runChannelSync()); CHECK(updateQueue->items.empty());

    printf("    Settings page: channel names are validated and saved\n");
    auto post = [](std::map<std::string,std::string> a){ a["ssid"] = "homewifi"; g_args = a; server.find("/settings", HTTP_POST)->fn(); g_args.clear(); };
    rebootAt = 0;
    post({{"channels", "bad-name!"}});                        CHECK(contains(g_lastBody, "Invalid channel name: bad-name!") && rebootAt == 0);
    post({{"channels", "  \n "}});                            CHECK(contains(g_lastBody, "at least one channel"));
    { std::string many; for(int i=0;i<33;i++) many += "ch" + std::to_string(i) + "\n";
      post({{"channels", many}});                             CHECK(contains(g_lastBody, "At most 32 channels")); }
    CHECK(!g_nvs["config"].count("channels"));
    post({{"channels", "PietSmiet\nhttps://www.twitch.tv/Bonjwa, gronkh\ngronkh\nnewguy;12875057"}});
    CHECK(rebootAt != 0 && g_nvs["config"]["channels"] == "pietsmiet,bonjwa,gronkh,newguy,12875057");
    rebootAt = 0;

    printf("    after the restart: names from the cache, unknown ones looked up by login\n");
    loadConfig(); buildChannels(); assignLogoSlots();
    CHECK(channels.size() == 5 && channels[0].id == "21991090" && channels[0].name == "PietSmiet" && channels[1].id == "73437396");
    CHECK(channels[3].login == "newguy" && channels[3].id.empty());
    CHECK(channels[4].id == "12875057" && channels[4].login == "gronkh");      // numeric entry = user id
    setResponse({{1,"B"}});                                                    // streams query: only known ids
    loopWithUpdate();
    CHECK(contains(g_requests.back().url, "user_id=21991090&user_id=73437396&user_id=12875057&user_id=12875057&") && !contains(g_requests.back().url, "user_id=&"));
    g_responses.push_back({200, usersJson({{"21991090","pietsmiet","PietSmiet"},{"73437396","bonjwa","Bonjwa"},{"12875057","gronkh","Gronkh"},{"555","newguy","NewGuy"}})});
    CHECK(runChannelSync()); loop();
    CHECK(contains(g_requests.back().url, "login=newguy&") && channels[3].id == "555" && channels[3].name == "NewGuy" && unknownChannels.empty());

    printf("    a name Twitch does not know\n");
    g_nvs["config"]["channels"] = "pietsmiet,nosuchchannel"; loadConfig(); buildChannels();
    g_responses.push_back({200, usersJson({{"21991090","pietsmiet","PietSmiet"}})});
    CHECK(runChannelSync()); loop();
    CHECK(channels[1].id.empty() && unknownChannels.size() == 1 && unknownChannels[0] == "nosuchchannel");
    server.find("/", HTTP_GET)->fn(); CHECK(contains(g_lastBody, "<td>nosuchchannel</td><td><span class=\"err\">not found"));

    g_nvs["config"].erase("channels"); g_nvs["config"].erase("chcache"); loadConfig(); buildChannels(); assignLogoSlots(); reset(); }

  printf("T41 logos: download the 70x70 avatar, convert, store, draw; errors keep the old logo\n");
  { auto usersJson = [](std::vector<std::array<std::string,3>> users, std::string suffix){
      std::string j = "{\"data\":[";
      for(size_t i=0;i<users.size();i++){ if(i) j += ",";
        j += "{\"id\":\"" + users[i][0] + "\",\"login\":\"" + users[i][1] + "\",\"display_name\":\"" + users[i][2] + "\","
             "\"profile_image_url\":\"https://static-cdn.jtvnw.net/jtv_user_pictures/" + users[i][1] + suffix + "-profile_image-300x300.png\"}"; }
      return j + "]}"; };
    auto file = [](const char* name){ return avatar(name); };
    auto expectLogo = [](const char* name){ std::string v = avatar(name); std::vector<uint16_t> out(64*64); CHECK(pngToLogo((const uint8_t*)v.data(), v.size(), out.data()) == nullptr); return out; };
    auto slotData = [](int slot){ std::vector<uint16_t> d(64*64); readLogo(slot, d.data()); return d; };
    auto drain = [](){ for(int i=0;i<30;i++) loop(); };
    auto slotOf = [](const char* id){ for(auto& c : channels) if(c.id == id) return (int)c.logoSlot; return -9; };
    auto statusOf = [](const char* id){ for(auto& c : channels) if(c.id == id) return c.logoStatus; return std::string("?"); };

    CHECK(smallAvatarUrl("https://static-cdn.jtvnw.net/jtv_user_pictures/a4bec7db-47b2-44f7-bb79-6de32e7bbca4-profile_image-300x300.png") == "https://static-cdn.jtvnw.net/jtv_user_pictures/a4bec7db-47b2-44f7-bb79-6de32e7bbca4-profile_image-70x70.png");
    CHECK(smallAvatarUrl("https://static-cdn.jtvnw.net/jtv_user_pictures/gronkh-profile_image-76b34139eaa46bb5-300x300.png") == "https://static-cdn.jtvnw.net/jtv_user_pictures/gronkh-profile_image-76b34139eaa46bb5-70x70.png");
    CHECK(smallAvatarUrl("https://example.com/other.png") == "https://example.com/other.png");

    reset(); g_nvs.erase("logos"); std::fill(g_flashData.begin(), g_flashData.end(), 0xFF);
    g_nvs["config"].erase("channels"); g_nvs["config"].erase("chcache"); loadConfig(); buildChannels(); assignLogoSlots(); loadLogoIndex(netLogoIndex);
    std::vector<std::array<std::string,3>> users = {{"761017145","lidi","Lidi"},{"21991090","pietsmiet","PietSmiet"},{"73437396","bonjwa","Bonjwa"},{"12875057","gronkh","Gronkh"}};
    g_responses.push_back({200, usersJson(users, "")});
    g_responses.push_back({404, "not found"});                                        // lidi
    g_responses.push_back({200, "<html>not an image</html>"});                        // pietsmiet
    g_responses.push_back({200, file("bonjwa")});              // bonjwa
    g_responses.push_back({200, file("gronkh")});              // gronkh
    size_t n0 = g_requests.size(); int stops0 = g_tlsStops;
    CHECK(runChannelSync());
    CHECK(g_requests.size() >= n0 + 5);
    CHECK(g_tlsStops - stops0 == 4);                                   // TLS closed after every download, before decoding
    CHECK(g_requests.back().url == "https://static-cdn.jtvnw.net/jtv_user_pictures/gronkh-profile_image-70x70.png" && g_requests.back().tls);
    drain();
    CHECK(slotOf("73437396") >= 0 && slotOf("12875057") >= 0 && slotOf("73437396") != slotOf("12875057"));
    CHECK(statusOf("73437396") == "stored" && statusOf("761017145") == "download failed (404)" && statusOf("21991090") == "Unsupported image format (not PNG)");
    CHECK(slotOf("761017145") == -1 && slotOf("21991090") == -1);
    CHECK(slotData(slotOf("12875057")) == expectLogo("gronkh"));
    CHECK(slotData(slotOf("73437396")) == expectLogo("bonjwa"));
    { std::array<LogoSlot, MAX_LOGO_SLOTS> idx; loadLogoIndex(idx); int n = 0; for(auto& e : idx) if(!e.channelId.empty()) n++; CHECK(n == 2); }
    CHECK(g_flashBadWrites == 0);

    printf("    the tile shows the downloaded logo\n");
    setResponse({{4,"G"}}); loopWithUpdate(); runUntilTitleDone();
    { uint16_t x,y; slotXY(0,x,y); auto e = expectLogo("gronkh"); bool same = true;
      for(int j=0;j<64;j++) for(int i=0;i<64;i++) if(g_fb[y+j][x+i] != e[j*64+i]) same = false;
      CHECK(same); }
    server.find("/", HTTP_GET)->fn();
    CHECK(contains(g_lastBody, "logo: <span class=\"muted\">stored") && contains(g_lastBody, "logo: <span class=\"err\">download failed (404)") && contains(g_lastBody, "Reload logos"));
    if(getenv("DUMP")){ std::ofstream("status_logos.html") << g_lastBody; server.find("/settings", HTTP_GET)->fn(); std::ofstream("settings_channels.html") << g_lastBody; }

    printf("    unchanged avatars are not downloaded again\n");
    g_responses.push_back({200, usersJson({{"73437396","bonjwa","Bonjwa"},{"12875057","gronkh","Gronkh"}}, "")});
    n0 = g_requests.size(); CHECK(runChannelSync()); drain();
    CHECK(g_requests.size() <= n0 + 2 && contains(g_requests.back().url, "/helix/users?"));

    printf("    avatar changed but the download fails: the old logo stays\n");
    int gslot = slotOf("12875057"); auto before = slotData(gslot);
    g_responses.push_back({200, usersJson({{"12875057","gronkh","Gronkh"}}, "-new")});
    g_responses.push_back({500, "error"});
    CHECK(runChannelSync()); drain();
    CHECK(slotOf("12875057") == gslot && slotData(gslot) == before && statusOf("12875057") == "download failed (500)");
    printf("    ...and when it works, the new logo goes into the same slot\n");
    g_responses.push_back({200, usersJson({{"12875057","gronkh","Gronkh"}}, "-new")});
    g_responses.push_back({200, file("maxim")});
    CHECK(runChannelSync()); drain();
    CHECK(slotOf("12875057") == gslot && slotData(gslot) == expectLogo("maxim") && statusOf("12875057") == "stored");
    CHECK(g_requests.back().url == "https://static-cdn.jtvnw.net/jtv_user_pictures/gronkh-new-profile_image-70x70.png");

    printf("    oversized download, flash write error\n");
    g_responses.push_back({200, usersJson({{"21991090","pietsmiet","PietSmiet"}}, "")});
    g_responses.push_back({200, std::string(60000, 'x')});
    CHECK(runChannelSync()); drain(); CHECK(statusOf("21991090") == "image too large");
    g_responses.push_back({200, usersJson({{"21991090","pietsmiet","PietSmiet"}}, "")});
    g_responses.push_back({200, file("bonjwa")});
    g_flashFail = true; CHECK(runChannelSync()); drain(); g_flashFail = false;
    CHECK(statusOf("21991090") == "could not write flash" && slotOf("21991090") == -1);
    { std::array<LogoSlot, MAX_LOGO_SLOTS> idx; loadLogoIndex(idx); for(auto& e : idx) CHECK(e.channelId != "21991090"); }

    printf("    'Reload logos' downloads all again\n");
    g_authOk = false; server.find("/logos/reload", HTTP_POST)->fn(); CHECK(g_lastCode == 401 && !logoReloadRequested); g_authOk = true;
    channelSyncRequested = false;
    server.find("/logos/reload", HTTP_POST)->fn(); CHECK(g_lastCode == 303 && logoReloadRequested && channelSyncRequested);
    g_responses.push_back({200, usersJson({{"73437396","bonjwa","Bonjwa"}}, "")});
    g_responses.push_back({200, file("bonjwa")});
    n0 = g_requests.size(); CHECK(runChannelSync()); drain();
    CHECK(g_requests.size() >= n0 + 2 && contains(g_requests.back().url, "bonjwa-profile_image-70x70.png") && !logoReloadRequested);

    printf("    a new channel reuses the slot of a removed one\n");
    for(int i = 0; i < logoSlotCount(); i++) if(netLogoIndex[i].channelId.empty()){ netLogoIndex[i].channelId = "gone" + std::to_string(i); netLogoIndex[i].urlHash = 1; saveLogoIndexEntry(i, netLogoIndex[i]); }
    g_responses.push_back({200, usersJson({{"21991090","pietsmiet","PietSmiet"}}, "")});
    g_responses.push_back({200, file("maxim")});
    CHECK(runChannelSync()); drain();
    CHECK(slotOf("21991090") >= 0 && contains(netLogoIndex[slotOf("21991090")].channelId, "21991090"));

    g_nvs.erase("logos"); std::fill(g_flashData.begin(), g_flashData.end(), 0xFF); loadLogoIndex(netLogoIndex);
    g_nvs["config"].erase("chcache"); loadConfig(); buildChannels(); assignLogoSlots(); reset(); }

  printf("T42 display: rotation model, column-wise title transfer\n");
  { // the emulator's MADCTL model must reproduce Adafruit's rotation table:
    // a point in rotation m is the same memory cell as in rotation 0 under GFXcanvas' rotation formula
    auto cellOf = [](int rot, int x, int y){
      tft.setRotation(rot); uint16_t marker = 0xBEEF; tft.setAddrWindow(x, y, 1, 1);
      for(auto& row : g_gram) for(auto& v : row) if(v == marker) v = 0;
      tft.writePixels(&marker, 1);
      for(int Y = 0; Y < 320; Y++) for(int X = 0; X < 240; X++) if(g_gram[Y][X] == marker) return std::make_pair(X, Y);
      return std::make_pair(-1, -1); };
    bool consistent = true;
    for(auto pt : std::vector<std::pair<int,int>>{{0,0},{319,0},{0,169},{319,169},{100,37},{11,92}}){
      int x = pt.first, y = pt.second;                                        // landscape point (rotation 1 / 3)
      if(cellOf(1, x, y) != cellOf(0, 169 - y, x)) consistent = false;
      if(cellOf(3, x, y) != cellOf(0, y, 319 - x)) consistent = false;
      int px = std::min(x, 169), py = std::min(y * 2, 319);                  // portrait point (rotation 2)
      if(cellOf(2, px, py) != cellOf(0, 169 - px, 319 - py)) consistent = false;
    }
    CHECK(consistent);
    tft.setRotation(1); CHECK(g_madctl == (ST77XX_MADCTL_MY | ST77XX_MADCTL_MV));

    // column-wise transfer of a test image with a unique value per pixel
    std::vector<uint16_t> bm(346 * 64);
    for(size_t i = 0; i < bm.size(); i++) bm[i] = (uint16_t)(i * 7 + 1);
    for(int rowTop : {14, 92}){
      g_writeOrder.clear(); g_recordWrites = true; g_spiFastPieces = 0;
      tft.drawColumnwise(GRID_X0, rowTop, bm.data(), 6, TITLE_WIDTH, 64, 346);
      g_recordWrites = false;
      bool same = true;
      for(int j = 0; j < 64; j++) for(int i = 0; i < TITLE_WIDTH; i++) if(g_fb[rowTop + j][GRID_X0 + i] != bm[j * 346 + 6 + i]) same = false;
      CHECK(same);                                                          // right place, not mirrored or shifted
      CHECK(g_writeOrder.size() == (size_t)TITLE_WIDTH * 64);
      CHECK(g_spiFastPieces == (TITLE_WIDTH * 64 + 31) / 32);              // whole title in 32 pixel pieces
      bool columnwise = true;                                               // top to bottom, then the next column to the left
      for(size_t k = 0; k < g_writeOrder.size(); k++)
        if(g_writeOrder[k] != std::make_pair(GRID_X0 + TITLE_WIDTH - 1 - (int)(k / 64), rowTop + (int)(k % 64))) { columnwise = false; break; }
      CHECK(columnwise);
      CHECK(319 - g_writeOrder.front().first < 319 - g_writeOrder.back().first);  // ascending memory rows = panel refresh direction (ML = 0)
      CHECK(g_madctl == (ST77XX_MADCTL_MY | ST77XX_MADCTL_MV));            // back to rotation 1
    }
    { // odd sizes: a piece spans columns, the last piece is short
      std::vector<uint16_t> small(9 * 7);
      for(size_t i = 0; i < small.size(); i++) small[i] = (uint16_t)(0x1234 + i * 13);
      g_spiFastPieces = 0;
      tft.drawColumnwise(40, 30, small.data(), 2, 5, 7, 9);
      bool same = true;
      for(int j = 0; j < 7; j++) for(int i = 0; i < 5; i++) if(g_fb[30 + j][40 + i] != small[j * 9 + 2 + i]) same = false;
      CHECK(same && g_spiFastPieces == 2 && g_fb[30][45] != small[7]);
    }

    // the scrolling title in loop() uses it, and the tiles are still drawn correctly afterwards
    reset(); setResponse({{0,"A"},{4,"B"}}); loopWithUpdate();
    CHECK(isTitleDisplaying && titleInRow(1));
    g_writeOrder.clear(); g_recordWrites = true; loopNoUpdate(1); g_recordWrites = false;
    CHECK(g_writeOrder.size() >= (size_t)TITLE_WIDTH * 64 && g_writeOrder[0] == std::make_pair(GRID_X0 + TITLE_WIDTH - 1, (int)rowY(1)) && g_writeOrder[1] == std::make_pair(GRID_X0 + TITLE_WIDTH - 1, (int)rowY(1) + 1));
    runUntilTitleDone();
    CHECK(tileIsPic(0,0) && tileIsPic(1,4) && tileIsFill(2,0));
    reset(); }

  printf("T43 title scrolling: fixed frame rate, even steps, constant speed\n");
  { CHECK(g_panelFrameRate == PANEL_FRAME_RATE);                        // panel refresh set at boot
    reset(); setResponse({{0,"A long title to scroll over the display"}}); loopWithUpdate();
    CHECK(isTitleDisplaying && titleFrames == 1);
    CHECK(FRAME_INTERVAL_US == 10801);                                     // 92.6 Hz panel refresh (data sheet formula)
    // one second: ~92.6 frames of 3 px each
    std::vector<uint32_t> steps; uint32_t lastPixels = titlePixels, framesBefore = titleFrames;
    unsigned long t0 = g_millis;
    while(g_millis - t0 < 1000){ loopNoUpdate(1); if(titleFrames != framesBefore + steps.size()){ steps.push_back(titlePixels - lastPixels); lastPixels = titlePixels; } }
    CHECK(steps.size() >= 91 && steps.size() <= 94);
    bool even = true; for(auto st : steps) if(st != 3) even = false;
    CHECK(even);                                                           // 3 px per frame, no 6 px jumps
    CHECK(titlePixels >= 274 && titlePixels <= 281);                       // 3 px per panel refresh = 278 px/s
    printf("    a late frame catches up, the speed stays the same\n");
    uint32_t p0 = titlePixels; unsigned long tStall = g_millis;
    g_millis += 47;                                                        // e.g. a slow web request
    loopNoUpdate(1);
    CHECK(titlePixels - p0 >= 13 && titlePixels - p0 <= 17);              // (0..11 ms since the last frame + 47 ms stall) * 278 px/s
    t0 = g_millis; p0 = titlePixels;
    while(g_millis - t0 < 500) loopNoUpdate(1);
    CHECK(titlePixels - p0 >= 136 && titlePixels - p0 <= 142);
    (void)tStall;
    printf("    the title takes as long as the speed says\n");
    reset(); setResponse({{0,"Exactly twenty chars"}}); loopWithUpdate();
    t0 = g_millis; runUntilTitleDone();
    unsigned long expectMs = (unsigned long)((uint64_t)MAX_TITLE_REPEAT * (titleText.width + TITLE_GAP) * FRAME_INTERVAL_US / SCROLL_STEP / 1000);
    CHECK(!isTitleDisplaying && g_millis - t0 >= expectMs - 20 && g_millis - t0 <= expectMs + 30);
    CHECK(titleFrames >= expectMs * 1000 / FRAME_INTERVAL_US - 5 && titleMaxGapUs <= 12000);
    server.find("/", HTTP_GET)->fn();
    CHECK(contains(g_lastBody, "Title scrolling</td><td>") && contains(g_lastBody, " fps, longest frame gap ") && contains(g_lastBody, "drawing a frame takes ") && contains(g_lastBody, "panel refresh 10.8 ms, SPI clock 80 MHz") && contains(g_lastBody, ", transfer "));
    reset(); }

  printf("T37 serial output uses CRLF line ends\n");
  g_serialOut.clear();
  debugSerial.printf("a\nb\r\nc\n"); debugSerial.println("x"); debugSerial.print("y\r"); debugSerial.print("\nz\n\n");
  CHECK(g_serialOut == "a\r\nb\r\nc\r\nx\r\ny\r\nz\r\n\r\n");
  g_serialOut.clear(); DEBUG_I.printf("[%s] Live channel: %s\n", DEBUG_TAG, "Bonjwa");
  CHECK(g_serialOut == "[TwitchDisplay] Live channel: Bonjwa\r\n");
  printf(fails? "\n%d check(s) FAILED\n" : "\nall checks passed\n", fails);
  return fails?1:0;
}


// Restarts the firmware "from scratch" as far as the tests need it
void simulateReboot(){
  setupMode = false; server.routes.clear(); server.notFound = nullptr; rebootAt = 0; restoreDisplayAt = 0;
  scannedNetworks.clear(); g_tasksCreated = 0; netTask = nullptr; g_args.clear();
  for(auto& c: channels){ c.isLive=false; c.slotNum=-1; c.streamTitle=""; }
  titleChangeQueue.clear(); isTitleDisplaying = false;
}

int setupModeTests(){
  printf("S1 first start without settings: setup hotspot with random password\n");
  g_scan = {"homewifi", "", "neighbor", "homewifi"};
  setup();
  CHECK(setupMode && std::string(setupModeReason) == "No WiFi network configured.");
  CHECK(g_apSsid == "TwitchDisplay-Setup" && g_apPw.size() == 8 && g_apPw.find_first_not_of("abcdefghjkmnpqrstuvwxyz23456789") == std::string::npos);
  CHECK(g_dnsStarted == 1 && g_tasksCreated == 0 && !setupRetryLater);
  CHECK(scannedNetworks.size() == 2 && scannedNetworks[0] == "homewifi" && scannedNetworks[1] == "neighbor");

  printf("S2 settings page without login, captive portal redirect\n");
  g_authOk = false;
  server.find("/settings", HTTP_GET)->fn();
  CHECK(g_lastCode == 200 && contains(g_lastBody, "Setup mode") && contains(g_lastBody, "<option value=\"neighbor\">"));
  CHECK(contains(g_lastBody, "not set") && contains(g_lastBody, "href=\"/update\"") && !contains(g_lastBody, ">Status<"));
  if(getenv("DUMP")) std::ofstream("setup.html") << g_lastBody;
  CHECK(server.find("/", HTTP_GET) == nullptr && server.find("/reboot", HTTP_POST) == nullptr && server.find("/update", HTTP_POST) != nullptr);
  g_respHeaders.clear(); server.notFound();
  CHECK(g_lastCode == 302 && g_respHeaders["Location"] == "http://192.168.4.1/settings");
  CHECK(!runLoopFor(2000));

  printf("S3 web password is required in setup mode\n");
  g_args = {{"ssid","homewifi"},{"wifipw","wifipassword"}}; server.find("/settings", HTTP_POST)->fn();
  CHECK(contains(g_lastBody, "Choose a password") && g_nvs["config"].empty() && rebootAt == 0);   // nothing saved (only the logo partition marker exists)

  printf("S4 saving restarts, secrets are not echoed\n");
  g_args = {{"ssid","homewifi"},{"wifipw","wifipassword"},{"clientid","cid"},{"secret","sec123"},{"webpw","webpassword"},{"webpw2","webpassword"}};
  server.find("/settings", HTTP_POST)->fn(); g_args.clear();
  CHECK(g_nvs["config"]["ssid"] == "homewifi" && g_nvs["config"]["wifipw"] == "wifipassword" && g_nvs["config"]["secret"] == "sec123" && g_nvs["config"]["webpw"] == "webpassword");
  CHECK(contains(g_lastBody, "twitchdisplay.fritz.box") && !contains(g_lastBody, "wifipassword") && !contains(g_lastBody, "sec123") && !contains(g_lastBody, "webpassword"));
  CHECK(runLoopFor(2000));

  printf("S5 after the restart: normal mode with the saved settings\n");
  simulateReboot(); g_wifiUp = true; g_authOk = true;
  setup();
  CHECK(!setupMode && g_beginSsid == "homewifi" && g_beginPw == "wifipassword" && g_tasksCreated == 2);
  CHECK(config.clientSecret == "sec123" && std::string(config.effectiveClientId()) == "cid" && config.webPassword == "webpassword");
  CHECK(server.find("/", HTTP_GET) != nullptr && server.find("/settings", HTTP_GET) != nullptr);

  printf("S6 WiFi not reachable: setup mode after 2 min, restart after 10 min without hotspot client\n");
  simulateReboot(); g_wifiUp = false;
  unsigned long t0 = g_millis;
  setup();
  CHECK(setupMode && setupRetryLater && std::string(setupModeReason) == "WiFi network not reachable.");
  CHECK(g_millis - t0 >= 120000 && g_millis - t0 < 125000);
  g_apStations = 1; CHECK(!runLoopFor(11*60*1000));   // someone is configuring: stay
  g_apStations = 0; CHECK(runLoopFor(61*1000));       // nobody: restart and retry
  CHECK(g_nvs["config"]["ssid"] == "homewifi");      // settings untouched

  printf("S7 BOOT request: setup mode with the stored settings\n");
  simulateReboot(); g_wifiUp = true; setupRequested = SETUP_REQUEST_MAGIC;
  setup();
  CHECK(setupMode && !setupRetryLater && setupRequested == 0 && std::string(setupModeReason) == "Requested with the BOOT button.");
  server.find("/settings", HTTP_GET)->fn();
  CHECK(contains(g_lastBody, "value=\"homewifi\"") && contains(g_lastBody, "stored - leave empty to keep") && contains(g_lastBody, "value=\"cid\""));
  CHECK(!contains(g_lastBody, "wifipassword") && !contains(g_lastBody, "sec123") && !contains(g_lastBody, "webpassword"));

  printf("S9 firmware upload works in setup mode without web login; new firmware gets confirmed there\n");
  g_authOk = false;
  server.find("/update", HTTP_GET)->fn(); CHECK(g_lastCode == 200 && contains(g_lastBody, "Upload firmware") && !contains(g_lastBody, "Online update"));
  { auto fw = makeAppImage(1100000), merged = makeMergedImage(1600000);
    g_updateEndOk = true;
    simulateUpload(merged);
    CHECK(g_lastCode == 400 && restoreDisplayAt != 0);
    CHECK(!runLoopFor(6000) && restoreDisplayAt == 0);   // back to the setup screen, still in setup mode
    CHECK(setupMode);
    simulateUpload(fw);
    CHECK(g_lastCode == 200 && g_flashed == fw && rebootAt != 0);
    CHECK(runLoopFor(2000)); rebootAt = 0; otaActive = false; }
  appPendingVerify = true; g_otaState = ESP_OTA_IMG_PENDING_VERIFY; g_markedValid = 0;
  CHECK(!runLoopFor(59000) && g_markedValid == 0);
  CHECK(!runLoopFor(2000) && g_markedValid == 1 && !appPendingVerify);
  g_authOk = true;

  printf("S8 reset in setup mode, then the next start is a first start again\n");
  server.find("/reset", HTTP_POST)->fn(); CHECK(g_nvs["config"].empty());
  CHECK(runLoopFor(2000));
  simulateReboot(); setup();
  CHECK(setupMode && std::string(setupModeReason) == "No WiFi network configured.");

  printf("S10 OTA-updated firmware without settings: setup mode knows it is on probation, saving confirms it\n");
  simulateReboot(); g_nvs.clear(); g_wifiUp = true;
  g_otaState = ESP_OTA_IMG_PENDING_VERIFY; g_markedValid = 0; appPendingVerify = false;
  setup();
  CHECK(setupMode && appPendingVerify && g_markedValid == 0);
  g_args = {{"ssid","homewifi"},{"wifipw","wifipassword"},{"webpw","webpassword"},{"webpw2","webpassword"}};
  server.find("/settings", HTTP_POST)->fn(); g_args.clear();       // saved within seconds
  CHECK(g_markedValid == 1 && !appPendingVerify && g_otaState == ESP_OTA_IMG_VALID);
  CHECK(runLoopFor(2000));
  printf("    ...and without saving it confirms itself after 60 s in setup mode\n");
  simulateReboot(); g_nvs.clear();
  g_otaState = ESP_OTA_IMG_PENDING_VERIFY; g_markedValid = 0; appPendingVerify = false;
  setup();
  CHECK(!runLoopFor(59000) && g_markedValid == 0);
  CHECK(!runLoopFor(2000) && g_markedValid == 1);
  printf("    ...and a USB-flashed firmware is not touched\n");
  simulateReboot(); g_nvs.clear();
  g_otaState = ESP_OTA_IMG_UNDEFINED; g_markedValid = 0; appPendingVerify = false;
  setup(); CHECK(setupMode && !appPendingVerify);
  g_args = {{"ssid","homewifi"},{"webpw","webpassword"},{"webpw2","webpassword"}};
  server.find("/settings", HTTP_POST)->fn(); g_args.clear();
  CHECK(g_markedValid == 0);
  CHECK(runLoopFor(2000));

  printf(fails? "\n%d check(s) FAILED\n" : "\nall checks passed\n", fails);
  return fails?1:0;
}
