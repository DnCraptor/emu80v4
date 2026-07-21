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

// Цикл кодирования строк. Занимает ядро целиком и не возвращается.
void hdmi_dvi_core_loop(void);

#endif // HDMI_DVI_H
