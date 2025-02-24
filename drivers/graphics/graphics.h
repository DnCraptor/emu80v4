#ifndef GRAPHICS
#define GRAPHICS
#ifdef __cplusplus
extern "C" {
#endif

#define DISP_WIDTH 521
#define DISP_HEIGHT 288
#define RGB888(r, g, b) ((((r) >> 6) << 4) | (((g) >> 6) << 2) | ((b) >> 6))
#define RGB(rgb) (RGB888(rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF))

#include "stdbool.h"
#include "stdio.h"
#include "stdint.h"

#ifdef TFT
#include "st7789.h"
#endif
#ifdef HDMI
#include "hdmi.h"
#endif
#ifdef VGA_DRV
#include "vga.h"
#endif
#ifdef TV
#include "tv.h"
#endif
#ifdef SOFTTV
#include "tv-software.h"
#endif
//#include "font6x8.h"
#include "font8x8.h"
//#include "font8x16.h"
enum graphics_mode_t {
    GRAPHICSMODE_DEFAULT = 0,
    GMODE_640_480 = 0,
    GMODE_800_600 = 1,
    GMODE_1024_768 = 2,
    UNSUPPORTED_MODE
};

void graphics_init();

enum graphics_mode_t graphics_get_mode();
void graphics_set_mode(enum graphics_mode_t mode);

void graphics_set_buffer(uint8_t* buffer, uint16_t width, uint16_t height);
void graphics_set_1bit_buffer(uint8_t* buffer, const uint16_t width, const uint16_t height);
void graphics_set_1bit_buffer2(
    uint8_t* buffer1,
    uint8_t* buffer2,
    const uint16_t width,
    const uint16_t height
);
void graphics_set_1bit_buffer3(
    uint8_t* buffer1,
    uint8_t* buffer2,
    uint8_t* buffer3,
    const uint16_t width,
    const uint16_t height
);
void graphics_set_skip_x_pixels(int);

void graphics_inc_x(void);
void graphics_dec_x(void);
void graphics_inc_y(void);
void graphics_dec_y(void);
void graphics_set_offset(int x, int y);

void graphics_set_palette(uint8_t i, uint32_t color);

void graphics_set_textbuffer(uint8_t* buffer);

void graphics_set_bgcolor(uint32_t color888);

void graphics_set_flashmode(bool flash_line, bool flash_frame);

void draw_text(const char string[TEXTMODE_COLS + 1], uint32_t x, uint32_t y, uint8_t color, uint8_t bgcolor);
void draw_window(const char title[TEXTMODE_COLS + 1], uint32_t x, uint32_t y, uint32_t width, uint32_t height);

void clrScr(uint8_t color);

uint32_t graphics_get_width();
uint32_t graphics_get_height();
uint32_t graphics_get_font_width();
uint32_t graphics_get_font_height();
uint8_t* graphics_get_frame();

void graphics_type(int x, int y, uint8_t color, const char* msg, size_t msg_len);
void plot(int x, int y, uint8_t color);
void line(int x0, int y0, int x1, int y1, uint8_t color);
void graphics_rect(int32_t x, int32_t y, uint32_t width, uint32_t height, uint8_t color);
void graphics_fill(int32_t x, int32_t y, uint32_t width, uint32_t height, uint8_t bgcolor);

#ifdef __cplusplus
}
#endif
#endif // GRAPHICS
