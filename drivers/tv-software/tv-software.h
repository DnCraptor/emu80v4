#pragma once

#include "inttypes.h"
#include "stdbool.h"

#define PIO_VIDEO pio0

// База выхода по умолчанию. На конкретной плате переопределяется в .c по
// VGA_BASE_PIN: композит сидит на том же резисторном ЦАП, что и VGA.
#ifndef TV_BASE_PIN
#define TV_BASE_PIN (6)
#endif

#define TEXTMODE_COLS 40
#define TEXTMODE_ROWS 30

// RGB888 здесь не переопределяется: graphics.h задаёт его как упаковку
// в 6 бит (по два на канал), и именно такие байты кладёт в кадровый буфер
// эмулятор. Прежнее 24-битное определение перекрывало его и ломало цвета.

typedef enum g_out_TV_t {
    g_TV_OUT_PAL,
    g_TV_OUT_NTSC
} g_out_TV_t;


typedef enum NUM_TV_LINES_t {
    _624_lines, _625_lines, _524_lines, _525_lines,
} NUM_TV_LINES_t;

typedef enum COLOR_FREQ_t {
    _3579545, _4433619
} COLOR_FREQ_t;

// Описание режима вывода. Раньше структура жила в tv-software.c, а Main.cpp
// объявлял её копию у себя — две независимые декларации одного и того же,
// которые обязаны были совпадать.
typedef struct tv_out_mode_t {
    float color_index;
    COLOR_FREQ_t c_freq;
    enum graphics_mode_t mode_bpp;
    g_out_TV_t tv_system;
    NUM_TV_LINES_t N_lines;
    bool cb_sync_PI_shift_lines;
    bool cb_sync_PI_shift_half_frame;
} tv_out_mode_t;

extern tv_out_mode_t tv_out_mode;
void graphics_set_modeTV(tv_out_mode_t mode);

// TODO: Сделать настраиваемо
static const uint8_t textmode_palette[16] = {
    200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215
};
static void graphics_set_flashmode(bool flash_line, bool flash_frame) {
    // dummy
}

static void graphics_set_bgcolor(uint32_t color888) {
    // dummy
}
