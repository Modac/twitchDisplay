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

unsigned long g_millis = 1; unsigned long g_usFrac = 0; bool g_quiet = true;
SerialStub Serial; EspStub ESP; ArduinoOTAClass ArduinoOTA;
uint16_t g_fb[170][320]; int g_bitmapDraws = 0;
std::string g_response; int g_httpCode = 200;
std::deque<FakeResponse> g_responses; std::vector<FakeRequest> g_requests;
int g_analogReads = 0; int g_notifies = 0; int g_tasksCreated = 0;
bool g_wifiUp = true;
std::map<std::string,std::string> g_args, g_respHeaders; std::map<std::string, std::map<std::string, std::string>> g_nvs;
std::string g_serialOut;
esp_partition_t g_spiffs{"spiffs", 0x20000}; std::vector<uint8_t> g_flashData(0x20000, 0xFF); int g_flashErases = 0, g_flashWrites = 0, g_flashBadWrites = 0; bool g_flashFail = false;
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
esp_partition_t g_running{"app0", 0x1e0000}, g_next{"app1", 0x1e0000}; esp_ota_img_states_t g_otaState = ESP_OTA_IMG_UNDEFINED; int g_markedValid = 0; int g_reconnects = 0; bool g_bundleUsed = false; int g_tlsStops = 0; WiFiClass WiFi;

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
bool titleInRow(int row){ return g_fb[rowY(row)+32][GRID_X0+150]==0x1234; }

uint16_t statusDot(){ return g_fb[STATUS_Y+2][STATUS_X+2]; }
std::string helix(std::string body){ return body; }

void spiFastPixels(const uint16_t* p, uint32_t n){ tft.writePixels((uint16_t*)p, n); }

int main(){
#ifdef VARIANT_NOSECRETS
  return setupModeTests();
#endif
  setup();
  CHECK(g_bundleUsed); CHECK(g_tasksCreated==1 && netTask!=nullptr && updateQueue!=nullptr);
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
  CHECK(channelTitleInfo.textOffset==0 && channelTitleInfo.repeats==0 && titleFrames==1); // restarted from the first character
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
  CHECK(channels[1].streamTitle=="    Hallo Welt | Minecraft   ");
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

  printf("T14 titles from the API are converted\n");
  reset();
  g_response = "{\"data\":[{\"user_id\":\"12875057\",\"user_name\":\"Gronkh\",\"title\":\"Gr\\u00fc\\u00dfe \xf0\x9f\x8e\xae\",\"game_name\":\"Just Chatting\"}]}";
  loopWithUpdate();
  CHECK(channels[4].isLive); CHECK(channels[4].streamTitle == "    Gr\x81\xe1" "e  | Just Chatting   ");

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
  CHECK(g_lastCode == 200 && g_lastBody.find("const maxSize = 1966080;") != std::string::npos);
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
    CHECK(!writeLogo(16, logoA.data()) && !readLogo(-1, back.data()) && !readLogo(16, back.data()));
    g_flashFail = true; CHECK(!writeLogo(1, logoA.data())); g_flashFail = false;

    CHECK(writeLogo(3, logoA.data()));
    saveLogoIndexEntry(3, LogoSlot{"12875057", 0xdeadbeef});
    saveLogoIndexEntry(5, LogoSlot{"99999999", 0x00000001});
    std::array<LogoSlot, LOGO_SLOTS> idx; loadLogoIndex(idx);
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
      CHECK(g_fb[y0+32][x0+32] == 0x1234 && g_fb[y0+10][x0+32] == bg0);   // initials in the middle rows
      CHECK(pic_canvas.lastText == "N2"); }                               // initials of the name from Twitch ("n2"), last placeholder drawn
    { channelInfo anon; anon.id = "123"; drawPlaceholder(anon); CHECK(pic_canvas.lastText == "?"); }
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
    { std::string many; for(int i=0;i<17;i++) many += "ch" + std::to_string(i) + "\n";
      post({{"channels", many}});                             CHECK(contains(g_lastBody, "At most 16 channels")); }
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
    { std::array<LogoSlot, LOGO_SLOTS> idx; loadLogoIndex(idx); int n = 0; for(auto& e : idx) if(!e.channelId.empty()) n++; CHECK(n == 2); }
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
    { std::array<LogoSlot, LOGO_SLOTS> idx; loadLogoIndex(idx); for(auto& e : idx) CHECK(e.channelId != "21991090"); }

    printf("    'Reload logos' downloads all again\n");
    g_authOk = false; server.find("/logos/reload", HTTP_POST)->fn(); CHECK(g_lastCode == 401 && !logoReloadRequested); g_authOk = true;
    channelSyncRequested = false;
    server.find("/logos/reload", HTTP_POST)->fn(); CHECK(g_lastCode == 303 && logoReloadRequested && channelSyncRequested);
    g_responses.push_back({200, usersJson({{"73437396","bonjwa","Bonjwa"}}, "")});
    g_responses.push_back({200, file("bonjwa")});
    n0 = g_requests.size(); CHECK(runChannelSync()); drain();
    CHECK(g_requests.size() >= n0 + 2 && contains(g_requests.back().url, "bonjwa-profile_image-70x70.png") && !logoReloadRequested);

    printf("    a new channel reuses the slot of a removed one\n");
    for(int i = 0; i < LOGO_SLOTS; i++) if(netLogoIndex[i].channelId.empty()){ netLogoIndex[i].channelId = "gone" + std::to_string(i); netLogoIndex[i].urlHash = 1; saveLogoIndexEntry(i, netLogoIndex[i]); }
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
    CHECK(FRAME_INTERVAL_US == 19057);                                     // 52.5 Hz panel refresh (data sheet formula)
    // one second: ~52.5 frames of 6 px each
    std::vector<uint32_t> steps; uint32_t lastPixels = titlePixels, framesBefore = titleFrames;
    unsigned long t0 = g_millis;
    while(g_millis - t0 < 1000){ loopNoUpdate(1); if(titleFrames != framesBefore + steps.size()){ steps.push_back(titlePixels - lastPixels); lastPixels = titlePixels; } }
    CHECK(steps.size() >= 51 && steps.size() <= 54);
    bool even = true; for(auto st : steps) if(st != 6) even = false;
    CHECK(even);                                                           // 6 px per frame, no 12 px jumps
    CHECK(titlePixels >= 309 && titlePixels <= 320);                       // 6 px per panel refresh = 315 px/s
    printf("    a late frame catches up, the speed stays the same\n");
    uint32_t p0 = titlePixels; unsigned long tStall = g_millis;
    g_millis += 47;                                                        // e.g. a slow web request
    loopNoUpdate(1);
    CHECK(titlePixels - p0 >= 14 && titlePixels - p0 <= 21);              // (0..19 ms since the last frame + 47 ms stall) * 315 px/s
    t0 = g_millis; p0 = titlePixels;
    while(g_millis - t0 < 500) loopNoUpdate(1);
    CHECK(titlePixels - p0 >= 152 && titlePixels - p0 <= 162);
    (void)tStall;
    printf("    the title takes as long as the speed says\n");
    reset(); setResponse({{0,"Exactly twenty chars"}}); loopWithUpdate();
    t0 = g_millis; runUntilTitleDone();
    unsigned long expectMs = (unsigned long)((uint64_t)(channels[0].streamTitle.length() - 5) * CHAR_WIDTH * MAX_TITLE_REPEAT * FRAME_INTERVAL_US / SCROLL_STEP / 1000);
    CHECK(!isTitleDisplaying && g_millis - t0 >= expectMs - 20 && g_millis - t0 <= expectMs + 30);
    CHECK(titleFrames >= expectMs * 1000 / FRAME_INTERVAL_US - 5 && titleMaxGapUs <= 21000);
    server.find("/", HTTP_GET)->fn();
    CHECK(contains(g_lastBody, "Title scrolling</td><td>") && contains(g_lastBody, " fps, longest frame gap ") && contains(g_lastBody, "drawing a frame takes ") && contains(g_lastBody, "panel refresh 19.1 ms, SPI clock 80 MHz") && contains(g_lastBody, ", transfer "));
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
  CHECK(contains(g_lastBody, "Choose a password") && g_nvs.empty() && rebootAt == 0);

  printf("S4 saving restarts, secrets are not echoed\n");
  g_args = {{"ssid","homewifi"},{"wifipw","wifipassword"},{"clientid","cid"},{"secret","sec123"},{"webpw","webpassword"},{"webpw2","webpassword"}};
  server.find("/settings", HTTP_POST)->fn(); g_args.clear();
  CHECK(g_nvs["config"]["ssid"] == "homewifi" && g_nvs["config"]["wifipw"] == "wifipassword" && g_nvs["config"]["secret"] == "sec123" && g_nvs["config"]["webpw"] == "webpassword");
  CHECK(contains(g_lastBody, "twitchdisplay.fritz.box") && !contains(g_lastBody, "wifipassword") && !contains(g_lastBody, "sec123") && !contains(g_lastBody, "webpassword"));
  CHECK(runLoopFor(2000));

  printf("S5 after the restart: normal mode with the saved settings\n");
  simulateReboot(); g_wifiUp = true; g_authOk = true;
  setup();
  CHECK(!setupMode && g_beginSsid == "homewifi" && g_beginPw == "wifipassword" && g_tasksCreated == 1);
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
