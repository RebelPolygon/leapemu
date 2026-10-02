// Hello-world homebrew for the Leapster: bare-metal, no BaseROM services.
#include "leapster.h"

#define W 160
#define H 160
#define STRIDE 240           // 160 pixels * 12 bits
#define FB_OFFSET 0x1000     // framebuffer inside the 64 KiB on-chip SRAM

static void uart_puts(const char* s) { while (*s) UART_TX = (uint8_t)*s++; }

static void put_pixel(int x, int y, unsigned r, unsigned g, unsigned b) {
  volatile uint8_t* p = VRAM + FB_OFFSET + y * STRIDE + (x >> 1) * 3;
  if (!(x & 1)) {
    p[0] = (uint8_t)((g << 4) | b);
    p[1] = (uint8_t)((p[1] & 0x0f) | (r << 4));
  } else {
    p[1] = (uint8_t)((p[1] & 0xf0) | r);
    p[2] = (uint8_t)((g << 4) | b);
  }
}

static void fill(int x0, int y0, int w, int h, unsigned r, unsigned g, unsigned b) {
  for (int y = y0; y < y0 + h; y++)
    for (int x = x0; x < x0 + w; x++)
      if (x >= 0 && x < W && y >= 0 && y < H) put_pixel(x, y, r, g, b);
}

// 5x7 font: one byte per row, low 5 bits, MSB = leftmost column.
static const uint8_t kFont[][7] = {
  ['A' - 'A'] = {14, 17, 17, 31, 17, 17, 17}, ['B' - 'A'] = {30, 17, 17, 30, 17, 17, 30},
  ['C' - 'A'] = {14, 17, 16, 16, 16, 17, 14}, ['D' - 'A'] = {30, 17, 17, 17, 17, 17, 30},
  ['E' - 'A'] = {31, 16, 16, 30, 16, 16, 31}, ['F' - 'A'] = {31, 16, 16, 30, 16, 16, 16},
  ['G' - 'A'] = {14, 17, 16, 23, 17, 17, 15}, ['H' - 'A'] = {17, 17, 17, 31, 17, 17, 17},
  ['I' - 'A'] = {14, 4, 4, 4, 4, 4, 14},      ['J' - 'A'] = {7, 2, 2, 2, 2, 18, 12},
  ['K' - 'A'] = {17, 18, 20, 24, 20, 18, 17}, ['L' - 'A'] = {16, 16, 16, 16, 16, 16, 31},
  ['M' - 'A'] = {17, 27, 21, 21, 17, 17, 17}, ['N' - 'A'] = {17, 25, 21, 19, 17, 17, 17},
  ['O' - 'A'] = {14, 17, 17, 17, 17, 17, 14}, ['P' - 'A'] = {30, 17, 17, 30, 16, 16, 16},
  ['Q' - 'A'] = {14, 17, 17, 17, 21, 18, 13}, ['R' - 'A'] = {30, 17, 17, 30, 20, 18, 17},
  ['S' - 'A'] = {15, 16, 16, 14, 1, 1, 30},   ['T' - 'A'] = {31, 4, 4, 4, 4, 4, 4},
  ['U' - 'A'] = {17, 17, 17, 17, 17, 17, 14}, ['V' - 'A'] = {17, 17, 17, 17, 17, 10, 4},
  ['W' - 'A'] = {17, 17, 17, 21, 21, 21, 10}, ['X' - 'A'] = {17, 17, 10, 4, 10, 17, 17},
  ['Y' - 'A'] = {17, 17, 10, 4, 4, 4, 4},     ['Z' - 'A'] = {31, 1, 2, 4, 8, 16, 31},
};

static void text(int x, int y, const char* s, int scale) {
  for (; *s; s++, x += 6 * scale) {
    if (*s < 'A' || *s > 'Z') continue;
    const uint8_t* g = kFont[*s - 'A'];
    for (int row = 0; row < 7; row++)
      for (int col = 0; col < 5; col++)
        if (g[row] & (16 >> col)) fill(x + col * scale, y + row * scale, scale, scale, 15, 15, 15);
  }
}

static void background(void) {
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) put_pixel(x, y, (unsigned)(x * 15 / W), (unsigned)(y * 15 / H), 8);
}

static void delay(unsigned n) { for (volatile unsigned i = 0; i < n; i++) {} }

int main(void) {
  uart_puts("Hello from leapemu homebrew!\n");
  LCD_FB_BASE = FB_OFFSET;
  LCD_STRIDE = STRIDE;
  LCD_FORMAT = 0x80000004u;  // enable, 12 bpp

  background();
  text(20, 12, "HELLO", 4);
  text(14, 50, "LEAPSTER HOMEBREW", 1);

  int x = 72, y = 100, color = 0;
  static const uint8_t colors[][3] = {{15, 15, 0}, {15, 0, 15}, {0, 15, 15}, {15, 15, 15}};
  uint32_t prev = 0;
  fill(x, y, 16, 16, colors[color][0], colors[color][1], colors[color][2]);
  for (;;) {
    const uint32_t b = buttons();
    int nx = x, ny = y, ncolor = color;
    if (b & BTN_LEFT) nx -= 2;
    if (b & BTN_RIGHT) nx += 2;
    if (b & BTN_UP) ny -= 2;
    if (b & BTN_DOWN) ny += 2;
    if ((b & BTN_A) && !(prev & BTN_A)) { ncolor = (color + 1) & 3; uart_puts("A pressed\n"); }
    if (nx < 0) nx = 0;
    if (nx > W - 16) nx = W - 16;
    if (ny < 64) ny = 64;
    if (ny > H - 16) ny = H - 16;
    if (nx != x || ny != y || ncolor != color) {  // redraw only on change
      fill(x, y, 16, 16, (unsigned)(x * 15 / W), (unsigned)(y * 15 / H), 8);
      x = nx; y = ny; color = ncolor;
      fill(x, y, 16, 16, colors[color][0], colors[color][1], colors[color][2]);
    }
    prev = b;
    delay(20000);
  }
}
