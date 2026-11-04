#pragma once
#include <cstdint>
#define ESP_OK 0
#define CONFIG_IDF_FIRMWARE_CHIP_ID 0x0005
typedef int esp_err_t;
typedef enum { ESP_OTA_IMG_NEW=0, ESP_OTA_IMG_PENDING_VERIFY=1, ESP_OTA_IMG_VALID=2, ESP_OTA_IMG_INVALID=3, ESP_OTA_IMG_ABORTED=4, ESP_OTA_IMG_UNDEFINED=-1 } esp_ota_img_states_t;
struct esp_partition_t { const char* label; uint32_t size; };
// fake flash for the logo partition: erase sets 0xFF, write can only clear bits (like NOR flash)
#include <vector>
#include <cstring>
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_DATA_SPIFFS 0x82
extern esp_partition_t g_spiffs; extern std::vector<uint8_t> g_flashData; extern int g_flashErases, g_flashWrites, g_flashBadWrites; extern bool g_flashFail;
inline const esp_partition_t* esp_partition_find_first(int type, int subtype, const char*){ return (type==ESP_PARTITION_TYPE_DATA && subtype==ESP_PARTITION_SUBTYPE_DATA_SPIFFS) ? &g_spiffs : nullptr; }
inline esp_err_t esp_partition_read(const esp_partition_t*, size_t off, void* dst, size_t n){ if(off+n > g_flashData.size()) return -1; memcpy(dst, &g_flashData[off], n); return ESP_OK; }
inline esp_err_t esp_partition_erase_range(const esp_partition_t*, size_t off, size_t n){ if(g_flashFail || off%4096 || n%4096 || off+n > g_flashData.size()) return -1; memset(&g_flashData[off], 0xFF, n); g_flashErases++; return ESP_OK; }
inline esp_err_t esp_partition_write(const esp_partition_t*, size_t off, const void* src, size_t n){ if(g_flashFail || off+n > g_flashData.size()) return -1; const uint8_t* b=(const uint8_t*)src; for(size_t i=0;i<n;i++){ if((g_flashData[off+i] & b[i]) != b[i]) g_flashBadWrites++; g_flashData[off+i] &= b[i]; } g_flashWrites++; return ESP_OK; }
extern esp_partition_t g_running, g_next; extern esp_ota_img_states_t g_otaState; extern int g_markedValid;
inline const esp_partition_t* esp_ota_get_running_partition(){ return &g_running; }
inline const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*){ return &g_next; }
inline esp_err_t esp_ota_get_state_partition(const esp_partition_t*, esp_ota_img_states_t* s){ *s=g_otaState; return ESP_OK; }
inline esp_err_t esp_ota_mark_app_valid_cancel_rollback(){ g_markedValid++; g_otaState=ESP_OTA_IMG_VALID; return ESP_OK; }
