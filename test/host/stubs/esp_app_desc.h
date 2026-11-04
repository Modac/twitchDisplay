#pragma once
#include <cstdio>
inline const char* g_appElfSha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
inline int esp_app_get_elf_sha256(char* dst, size_t n){ snprintf(dst, n, "%s", g_appElfSha); return (int)n - 1; }
