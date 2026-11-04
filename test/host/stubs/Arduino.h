#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <string>
#include <algorithm>
#include <array>
#include <functional>
#include <cstring>
extern unsigned long g_millis;
extern bool g_quiet;
inline unsigned long millis(){ return g_millis; }
inline unsigned long micros(){ return g_millis*1000UL; }
inline void delay(unsigned long ms){ g_millis += ms; }
inline void delayMicroseconds(unsigned long us){ g_millis += (us + 999) / 1000; }
extern int g_analogReads;
inline int analogRead(int){ g_analogReads++; return 2000; }
inline void analogWrite(int,int){}
inline void pinMode(int,int){}
inline void digitalWrite(int,int){}
extern bool g_bootPressed;
inline int digitalRead(int pin){ return (pin==9 && g_bootPressed) ? 0 : 1; }
inline uint32_t esp_random(){ return (uint32_t)rand(); }
#define A3 3
#define OUTPUT 1
#define HIGH 1
#define LOW 0
#define INPUT_PULLUP 2
#define RTC_NOINIT_ATTR
#define constrain(amt,low,high) ((amt)<(low)?(low):((amt)>(high)?(high):(amt)))
inline long map(long x,long a,long b,long c,long d){ return (x-a)*(d-c)/(b-a)+c; }
class String : public std::string {
public:
  String(){} String(const char* s):std::string(s){} String(const std::string& s):std::string(s){}
  String(int v):std::string(std::to_string(v)){}
  String(unsigned v):std::string(std::to_string(v)){}
  String(long v):std::string(std::to_string(v)){}
  String(unsigned long v):std::string(std::to_string(v)){}
  void replace(const char* from, const std::string& to){ size_t f=std::string::find(from); while(f!=npos){ std::string::replace(f,strlen(from),to); f=std::string::find(from,f+to.size()); } }
  String& operator+=(const char* s){ append(s); return *this; }
  String& operator+=(char c){ push_back(c); return *this; }
  String& operator+=(const std::string& s){ append(s); return *this; }
  bool isEmpty() const { return empty(); }
  size_t write(uint8_t c){ push_back((char)c); return 1; }
  size_t write(const uint8_t* b, size_t n){ append((const char*)b, n); return n; }
  void trim(){ size_t a=find_first_not_of(" \t\r\n"); if(a==npos){ clear(); return; } size_t b=find_last_not_of(" \t\r\n"); *this=String(std::string::substr(a,b-a+1)); }
  bool startsWith(const char* p) const { return std::string::compare(0, strlen(p), p)==0; }
  String substring(size_t from) const { return String(std::string::substr(from)); }
  String substring(size_t from, size_t to) const { return String(std::string::substr(from, to - from)); }
  int indexOf(char c, size_t from = 0) const { size_t p = std::string::find(c, from); return p == npos ? -1 : (int)p; }
  int indexOf(const char* t, size_t from = 0) const { size_t p = std::string::find(t, from); return p == npos ? -1 : (int)p; }
  String toLowerCase() const { String r(*this); for(auto& ch : r) ch = (char)tolower((unsigned char)ch); return r; }
  void toLowerCaseInPlace(){ for(auto& ch : *this) ch = (char)tolower((unsigned char)ch); }
  bool equals(const char* o) const { return *this == o; }
  char charAt(size_t i) const { return (*this)[i]; }
  int toInt() const { return atoi(c_str()); }
};
inline String operator+(const char* a, const String& b){ return String(std::string(a)+b); }
extern std::string g_serialOut;
class Print {
public:
  virtual ~Print(){}
  virtual size_t write(uint8_t c) = 0;
  virtual size_t write(const uint8_t* b, size_t n){ size_t r = 0; while(n--) r += write(*b++); return r; }
  int printf(const char* f, ...){ char buf[1024]; va_list ap; va_start(ap,f); int n = vsnprintf(buf, sizeof buf, f, ap); va_end(ap); write((const uint8_t*)buf, std::min(n, 1023)); return n; }
  size_t print(const char* s){ return write((const uint8_t*)s, strlen(s)); }
  size_t print(const std::string& s){ return print(s.c_str()); }
  size_t print(int v){ return print(std::to_string(v)); }
  size_t println(const char* s = ""){ size_t n = print(s); return n + print("\r\n"); }
  size_t println(const std::string& s){ return println(s.c_str()); }
  size_t println(int v){ return println(std::to_string(v).c_str()); }
};
struct SerialStub {
  void begin(unsigned long){}
  size_t write(uint8_t c){ g_serialOut += (char)c; if(!g_quiet) putchar(c); return 1; }
  size_t write(const uint8_t* b, size_t n){ for(size_t i = 0; i < n; i++) write(b[i]); return n; }
};
extern SerialStub Serial;
struct EspStub { void restart(){ throw "restart"; } uint32_t getFreeHeap(){ return 200000; } uint32_t getMinFreeHeap(){ return 150000; } };
extern EspStub ESP;
#define PROGMEM

// --- FreeRTOS stand-ins: the network task is not started, tests call runNetworkUpdate()
#include <deque>
#include <vector>
#include <cstring>
typedef void* TaskHandle_t;
struct FakeQueue { size_t itemSize; std::deque<std::vector<uint8_t>> items; };
typedef FakeQueue* QueueHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xFFFFFFFF
#define pdMS_TO_TICKS(ms) (ms)
extern int g_notifies; extern int g_tasksCreated;
inline QueueHandle_t xQueueCreate(int, size_t sz){ return new FakeQueue{sz,{}}; }
inline int xQueueSend(QueueHandle_t q, const void* item, unsigned long){ const uint8_t* b=(const uint8_t*)item; q->items.emplace_back(b,b+q->itemSize); return pdTRUE; }
inline int xQueueReceive(QueueHandle_t q, void* item, unsigned long){ if(q->items.empty()) return pdFALSE; memcpy(item,q->items.front().data(),q->itemSize); q->items.pop_front(); return pdTRUE; }
inline int xTaskCreate(void(*)(void*), const char*, int, void*, int, TaskHandle_t* h){ g_tasksCreated++; *h=(void*)1; return pdTRUE; }
inline void xTaskNotifyGive(TaskHandle_t){ g_notifies++; }
inline uint32_t ulTaskNotifyTake(int, unsigned long){ return 0; }
inline void vTaskDelay(uint32_t ms){ g_millis += ms; }

extern int g_wdtFeeds;
inline void enableLoopWDT(){} inline void feedLoopWDT(){ g_wdtFeeds++; }

#define MALLOC_CAP_8BIT 4
inline size_t heap_caps_get_largest_free_block(int){ return 61440; }
