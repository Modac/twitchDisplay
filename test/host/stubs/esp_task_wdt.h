#pragma once
#include <cstdint>
typedef struct { uint32_t timeout_ms; uint32_t idle_core_mask; bool trigger_panic; } esp_task_wdt_config_t;
extern uint32_t g_wdtTimeout;
inline int esp_task_wdt_reconfigure(const esp_task_wdt_config_t* c){ g_wdtTimeout=c->timeout_ms; return 0; }
