#pragma once
#include <cstdint>
#define ESP_OK 0
#define CONFIG_IDF_FIRMWARE_CHIP_ID 0x0005
typedef int esp_err_t;
typedef enum { ESP_OTA_IMG_NEW=0, ESP_OTA_IMG_PENDING_VERIFY=1, ESP_OTA_IMG_VALID=2, ESP_OTA_IMG_INVALID=3, ESP_OTA_IMG_ABORTED=4, ESP_OTA_IMG_UNDEFINED=-1 } esp_ota_img_states_t;
struct esp_partition_t { const char* label; uint32_t size; uint32_t address = 0; };
// fake flash for the logo partition: erase sets 0xFF, write can only clear bits (like NOR flash)
#include <vector>
#include <cstring>
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_DATA_SPIFFS 0x82
extern esp_partition_t g_spiffs; extern std::vector<uint8_t> g_flashData; extern int g_flashErases, g_flashWrites, g_flashBadWrites; extern bool g_flashFail;
// the font partition (data subtype 0x40); g_haveFontPartition = false emulates the old partition layout
extern esp_partition_t g_fontPart; extern std::vector<uint8_t> g_fontData; extern bool g_haveFontPartition; extern int g_mmaps;
typedef int esp_partition_subtype_t;
inline const esp_partition_t* esp_partition_find_first(int type, int subtype, const char* label){
  if(type != ESP_PARTITION_TYPE_DATA) return nullptr;
  if(subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS) return &g_spiffs;
  if(subtype == 0x40 && g_haveFontPartition && (!label || !strcmp(label, "font"))) return &g_fontPart;
  return nullptr;
}
inline std::vector<uint8_t>& partData(const esp_partition_t* p){ return p == &g_fontPart ? g_fontData : g_flashData; }
inline esp_err_t esp_partition_read(const esp_partition_t* p, size_t off, void* dst, size_t n){ auto& d = partData(p); if(off+n > d.size()) return -1; memcpy(dst, &d[off], n); return ESP_OK; }
inline esp_err_t esp_partition_erase_range(const esp_partition_t* p, size_t off, size_t n){ auto& d = partData(p); if(g_flashFail || off%4096 || n%4096 || off+n > d.size()) return -1; memset(&d[off], 0xFF, n); g_flashErases++; return ESP_OK; }
inline esp_err_t esp_partition_write(const esp_partition_t* p, size_t off, const void* src, size_t n){ auto& d = partData(p); if(g_flashFail || off+n > d.size()) return -1; const uint8_t* b=(const uint8_t*)src; for(size_t i=0;i<n;i++){ if((d[off+i] & b[i]) != b[i]) g_flashBadWrites++; d[off+i] &= b[i]; } g_flashWrites++; return ESP_OK; }
typedef uint32_t esp_partition_mmap_handle_t;
#define ESP_PARTITION_MMAP_DATA 0
inline esp_err_t esp_partition_mmap(const esp_partition_t* p, size_t off, size_t n, int, const void** out, esp_partition_mmap_handle_t* h){ auto& d = partData(p); if(off+n > d.size()) return -1; *out = d.data() + off; *h = 1; g_mmaps++; return ESP_OK; }
inline void esp_partition_munmap(esp_partition_mmap_handle_t){ g_mmaps--; }
extern esp_partition_t g_running, g_next; extern esp_ota_img_states_t g_otaState; extern int g_markedValid;
inline const esp_partition_t* esp_ota_get_running_partition(){ return &g_running; }
inline const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*){ return &g_next; }
inline esp_err_t esp_ota_get_state_partition(const esp_partition_t*, esp_ota_img_states_t* s){ *s=g_otaState; return ESP_OK; }
inline esp_err_t esp_ota_mark_app_valid_cancel_rollback(){ g_markedValid++; g_otaState=ESP_OTA_IMG_VALID; return ESP_OK; }
