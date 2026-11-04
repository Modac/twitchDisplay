#pragma once
// Core dump in the "coredump" partition: g_coredump holds the image (empty: none)
#include <cstdint>
#include <cstring>
#include <vector>
#include "esp_flash.h"
#include <cstdio>
#define ESP_FAIL -1
#define APP_ELF_SHA256_SZ 65
struct esp_core_dump_bt_info_t { uint8_t stackdump[1024]; uint32_t dump_size; };
struct esp_core_dump_summary_extra_info_t { uint32_t mstatus, mtvec, mcause, mtval, ra, sp, exc_a[8]; };
struct esp_core_dump_summary_t {
  uint32_t exc_tcb; char exc_task[16]; uint32_t exc_pc; esp_core_dump_bt_info_t exc_bt_info;
  uint32_t core_dump_version; uint8_t app_elf_sha256[APP_ELF_SHA256_SZ]; esp_core_dump_summary_extra_info_t ex_info;
};
#define COREDUMP_ADDR 0x3F0000
inline std::vector<uint8_t> g_coredump;
inline esp_core_dump_summary_t g_coredumpSummary;
inline const char* g_coredumpReason = nullptr;
inline esp_err_t esp_core_dump_image_check(){ return g_coredump.empty() ? ESP_FAIL : ESP_OK; }
inline esp_err_t esp_core_dump_image_get(size_t* addr, size_t* size){
  if(g_coredump.empty()) return ESP_FAIL; *addr = COREDUMP_ADDR; *size = g_coredump.size(); return ESP_OK; }
inline esp_err_t esp_core_dump_image_erase(){ g_coredump.clear(); return ESP_OK; }
inline esp_err_t esp_core_dump_get_summary(esp_core_dump_summary_t* s){ if(g_coredump.empty()) return ESP_FAIL; *s = g_coredumpSummary; return ESP_OK; }
inline esp_err_t esp_core_dump_get_panic_reason(char* buf, size_t n){
  if(g_coredump.empty() || !g_coredumpReason) return ESP_FAIL; snprintf(buf, n, "%s", g_coredumpReason); return ESP_OK; }
inline esp_err_t esp_flash_read(void*, void* buf, uint32_t addr, uint32_t n){
  if(addr < COREDUMP_ADDR || addr - COREDUMP_ADDR + n > g_coredump.size()) return ESP_FAIL;
  memcpy(buf, g_coredump.data() + (addr - COREDUMP_ADDR), n); return ESP_OK; }
