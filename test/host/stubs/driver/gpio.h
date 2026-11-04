#pragma once
// GPIO stand-ins plus the read side of the emulated ST7789's serial
// interface: on SCK rising edges with CS low it shifts in a command; for GSCAN
// (45h) and RDDID (04h) it then returns dummy clocks and the scanline / ID on
// SDA, for RDID1..3 (DAh..DCh) one ID byte without dummy clock.
#include <cstdint>
typedef int gpio_num_t;
typedef enum { GPIO_MODE_INPUT = 1, GPIO_MODE_OUTPUT = 2 } gpio_mode_t;
typedef enum { GPIO_PULLUP_ONLY, GPIO_PULLDOWN_ONLY, GPIO_PULLUP_PULLDOWN, GPIO_FLOATING } gpio_pull_mode_t;
extern int g_pinLevel[32];        // last level written (GPIO and digitalWrite)
extern int g_pinSignal[32];       // GPIO matrix output signal per pin (128 = plain GPIO)
extern bool g_pinInput[32];
extern int g_scanlineDummyBits;   // emulated display: 1 or 8 dummy clocks, 0 = SDA not driven (no answer)
extern unsigned long micros();
struct SerialReadEmu {
  int cmdBits = 0; uint8_t cmd = 0; int readIndex = 0; uint32_t reply = 0; int replyBits = 0, dataBits = 16;
  void reset(){ cmdBits = 0; cmd = 0; readIndex = 0; replyBits = 0; }
  void risingEdge(){
    if(g_pinLevel[7]) return;                        // CS high
    if(!g_pinInput[6]){                              // MCU drives SDA: command / data bits
      if(cmdBits < 8){ cmd = (uint8_t)(cmd << 1 | (g_pinLevel[6] & 1)); if(++cmdBits == 8) start(); }
    } else readIndex++;
  }
  void start(){
    readIndex = 0; replyBits = 0;
    if(g_scanlineDummyBits == 0) return;
    if(cmd == 0x45){
      unsigned long line = (micros() % 10251UL) * 344UL / 10251UL;  // 97.6 Hz, 344 lines per frame
      dataBits = 16; reply = (uint32_t)line; replyBits = g_scanlineDummyBits + 16;
    } else if(cmd == 0x04){ dataBits = 24; reply = 0x858552; replyBits = g_scanlineDummyBits + 24; }
    else if(cmd >= 0xDA && cmd <= 0xDC){ dataBits = 8; reply = (0x858552 >> (8 * (0xDC - cmd))) & 0xFF; replyBits = 8; } // single byte: no dummy
  }
  int bit(){                                         // level on SDA during read clock readIndex
    if(readIndex < 1 || readIndex > replyBits) return 0; // pull-down when not driven
    int pos = replyBits - readIndex;                 // MSB first after the dummy bits
    return pos < dataBits ? (int)((reply >> pos) & 1) : 0;
  }
};
extern SerialReadEmu g_serialRead;
inline void gpio_set_level(gpio_num_t pin, uint32_t level){
  bool rising = pin == 4 && !g_pinLevel[4] && level;
  g_pinLevel[pin] = level ? 1 : 0;
  if(rising && g_pinSignal[4] == 128) g_serialRead.risingEdge();
}
inline int gpio_get_level(gpio_num_t pin){ return pin == 6 && g_pinInput[6] ? g_serialRead.bit() : g_pinLevel[pin]; }
inline int gpio_set_direction(gpio_num_t pin, gpio_mode_t mode){ g_pinInput[pin] = (mode == GPIO_MODE_INPUT); return 0; }
inline int gpio_set_pull_mode(gpio_num_t, gpio_pull_mode_t){ return 0; }
inline void esp_rom_delay_us(uint32_t){}
inline void pinMatrixOutAttach(uint8_t pin, uint8_t function, bool, bool){ g_pinSignal[pin] = function; }
inline void pinMatrixOutDetach(uint8_t pin, bool, bool){ g_pinSignal[pin] = 128; }
