// Minimal Leapster hardware definitions for homebrew (see docs/hardware.md).
#pragma once
#include <stdint.h>

#define REG(a) (*(volatile uint32_t*)(a))
#define LCD_FB_BASE   REG(0x01808088)  // offset into on-chip SRAM at 0x03000000
#define LCD_STRIDE    REG(0x0180808c)
#define LCD_FORMAT    REG(0x01808090)  // bit 31 enable; mode 4 = 12 bpp packed
#define GIO_IN        REG(0x0180004c)  // buttons, active low
#define TIMER0_COUNT  REG(0x0180d084)  // 16 MHz
#define UART_TX       REG(0x0180d510)
#define VRAM          ((volatile uint8_t*)0x03000000)

enum {
  BTN_RIGHT = 1u << 7, BTN_DOWN = 1u << 8, BTN_LEFT = 1u << 9, BTN_B = 1u << 13,
  BTN_A = 1u << 14, BTN_PAUSE = 1u << 26, BTN_HINT = 1u << 28, BTN_HOME = 1u << 29,
  BTN_UP = 1u << 31,
};
static inline uint32_t buttons(void) { return ~GIO_IN & 0xb703e380u; }
