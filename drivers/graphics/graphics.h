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

// Режим вывода. Перечисление используют драйверы st7789, hdmi, tv и
// tv-software, а также Main.cpp, но объявлено оно не было нигде — сборка
// с любым из них, кроме VGA, не проходила.
enum graphics_mode_t {
    GRAPHICSMODE_DEFAULT = 0,
    TEXTMODE_DEFAULT,
};

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
#ifdef HDMI_DVI
#include "hdmi-dvi.h"
#endif

#ifdef SOFTTV
#include "tv-software.h"
#endif

void graphics_init();

// Частоты системного тактирования, допустимые для текущего видеодрайвера.
const uint32_t* graphics_get_supported_system_clocks(uint32_t* count);
bool graphics_system_clock_can_change();
void graphics_system_clock_changed();

void graphics_set_duplicateLines(bool v);
void graphics_set_buffer(uint8_t* buffer, uint16_t width, uint16_t height);

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

// Сколько строк кадрового буфера реально видно. Для VGA совпадает с высотой,
// для композита меньше — растр короче буфера, особенно у NTSC.
uint32_t graphics_get_visible_height();

// На сколько точек картинка смещена по экрану вправо и вниз относительно
// нулевого положения. Знак нормализован: у драйверов внутренние shift_x/shift_y
// заданы в противоположных направлениях (у VGA строка экрана n показывает
// строку буфера n + shift_y, у композита наоборот), и наружу этой разницы
// быть не должно.
//
// Нужно для меню: оно рисуется в тот же кадровый буфер, поэтому сдвиг картинки
// уносит его вместе с ней. Зная смещение, меню держится на своём месте экрана.
int graphics_get_picture_shift_x();
int graphics_get_picture_shift_y();
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
