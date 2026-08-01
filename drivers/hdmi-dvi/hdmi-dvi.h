#ifndef HDMI_DVI_H
#define HDMI_DVI_H

#include <stdbool.h>
#include <stdint.h>

// Экран, который видит эмулятор. Совпадает с кадровым буфером Вектора:
// 626 x 288, восьмибитные индексы палитры в упаковке RGB222 из graphics.h.
#define TEXTMODE_COLS 80
#define TEXTMODE_ROWS 30

// Режим VESA DMT 800x600@72. Кадровый буфер выводится с удвоением строк
// средствами libdvi (DVI_VERTICAL_REPEAT = 2), поэтому кодируется 300 строк,
// а на экран уходит 600.
#define DVI_FRAME_WIDTH   800
#define DVI_FRAME_HEIGHT  300

static inline void graphics_set_flashmode(bool flash_line, bool flash_frame) {
    (void)flash_line; (void)flash_frame;
}
static inline void graphics_set_bgcolor(uint32_t color888) {
    (void)color888;
}

// RP2040: публикация видеопамяти и регистров Корвета для прямого
// построчного рендеринга на core1 без полноэкранного framebuffer.
void graphics_set_korvet_source(
        const uint8_t* plane0, const uint8_t* plane1,
        const uint8_t* plane2, const uint8_t* symbols,
        const uint8_t* attrs, const uint8_t* font,
        const uint8_t* palette, const uint8_t* lut,
        bool wide_char_mode, bool show_border);

// Цикл кодирования строк. Занимает ядро целиком и не возвращается.
void hdmi_dvi_core_loop(void);

// Передать один стереоотсчёт 16+16 бит в HDMI audio ring.
// До инициализации DVI отсчёт безопасно отбрасывается.
void hdmi_dvi_push_audio_sample(int16_t left, int16_t right);

#endif // HDMI_DVI_H
