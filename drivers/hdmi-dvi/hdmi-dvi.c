// ---------------------------------------------------------------------------
//  HDMI/DVI для Вектор-06Ц на основе libdvi (PicoDVI).
//
//  Режим VESA DMT 800x600 @ 72 Гц: пиксельная 50.0 МГц ровно, битовая 500 МГц,
//  делитель PIO единичный. Кадровый буфер эмулятора 626 x 288 ложится так:
//
//    по вертикали  288 x 2 = 576 в 600, поля по 12 строк
//    по горизонтали  626 в 800 один к одному, поля по 87 точек
//
//  Интерполяции нет ни по одной оси. Удвоение строк делает сама libdvi
//  (DVI_VERTICAL_REPEAT = 2), поэтому кодируется 300 строк на кадр, а не 600.
//
//  Цвет: байты кадрового буфера — это упаковка RGB222 из graphics.h,
//  то есть индексы 0..63. Таблица символов TMDS строится под них один раз.
// ---------------------------------------------------------------------------

#include <string.h>
#include <stdlib.h>

#include "graphics.h"
#include "font8x8.h"

#include "dvi.h"
#include "dvi_timing.h"
#include "tmds_encode.h"
#include "common_dvi_pin_configs.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/structs/bus_ctrl.h"
#include "pico/stdlib.h"

#define PICTURE_W 626
#define PICTURE_H 288
#define BORDER_X ((DVI_FRAME_WIDTH - PICTURE_W) / 2)    // 87
#define BORDER_Y ((DVI_FRAME_HEIGHT - PICTURE_H) / 2)   // 6

#define DWORDS_PER_PLANE (DVI_FRAME_WIDTH / DVI_SYMBOLS_PER_WORD)
#define TMDS_WORDS (DWORDS_PER_PLANE * 3)

extern uint8_t DVI_VERTICAL_REPEAT;

static struct dvi_inst dvi0;

// Кадровый буфер эмулятора. Ставится из graphics_set_buffer().
static uint8_t *fb_data = NULL;
static uint16_t fb_w = PICTURE_W;
static uint16_t fb_h = PICTURE_H;

// Сдвиг картинки. Знак нормализован так же, как у остальных драйверов:
// положительное значение означает смещение вправо и вниз по экрану.
static int pic_shift_x = 0;
static int pic_shift_y = 0;

// Строка индексов палитры, которую получает кодировщик: вся ширина кадра,
// поля заполнены цветом рамки, в середину копируется строка буфера.
static uint8_t __attribute__((aligned(4))) line_buf[DVI_FRAME_WIDTH];

// Полностью пустая строка в уже закодированном виде — для полей сверху и снизу
static uint32_t __attribute__((aligned(4))) blank_tmds[TMDS_WORDS];

// Символы TMDS для 64 цветов: шесть слов на цвет (две полярности x три канала)
static uint32_t __attribute__((aligned(4))) tmds_palette[64 * 6];

static uint8_t border_color = 0;

// ---------------------------------------------------------------------------
//  Палитра
// ---------------------------------------------------------------------------

static void build_palette(void) {
    // Два бита на канал: 0, 85, 170, 255
    static const uint8_t lvl[4] = {0, 85, 170, 255};
    uint32_t pal24[64];
    for (unsigned c = 0; c < 64; ++c) {
        const uint8_t r = (c >> 4) & 0x03;
        const uint8_t g = (c >> 2) & 0x03;
        const uint8_t b = (c >> 0) & 0x03;
        pal24[c] = ((uint32_t)lvl[r] << 16) | ((uint32_t)lvl[g] << 8) | lvl[b];
    }
    tmds_setup_palette24_symbols(pal24, tmds_palette, 64);
}

static void build_blank_line(void) {
    memset(line_buf, border_color, sizeof(line_buf));
    tmds_encode_palette_data((const uint32_t *)line_buf, tmds_palette,
                             blank_tmds, DVI_FRAME_WIDTH, 6);
}

// ---------------------------------------------------------------------------
//  Цикл кодирования. Занимает ядро целиком.
// ---------------------------------------------------------------------------

static inline void copy_words(uint32_t *dst, const uint32_t *src, size_t n) {
    for (size_t i = 0; i < n; ++i)
        *dst++ = *src++;
}

void __not_in_flash_func(hdmi_dvi_core_loop)(void) {
    // DMA_IRQ_0 берётся именно здесь, на ядре 1: обработчики прерываний
    // в RP2xxx свои у каждого ядра. Драйвер VGA в этой сборке не линкуется,
    // а звук на ядре 0 занимает DMA_IRQ_1 — пересечения нет.
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);

    uint32_t *tmdsbuf = NULL;
    while (true) {
        for (int y = 0; y < DVI_FRAME_HEIGHT; ++y) {
            // Строка кадрового буфера, попадающая в эту строку кадра.
            // Вертикальный сдвиг смещает картинку вниз при росте pic_shift_y.
            const int src = y - BORDER_Y - pic_shift_y;

            queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);

            if (!fb_data || src < 0 || src >= (int)fb_h) {
                copy_words(tmdsbuf, blank_tmds, TMDS_WORDS);
            } else {
                // Поля заполняются каждый раз: горизонтальный сдвиг может
                // измениться между строками, а отдельно отслеживать это дороже,
                // чем просто записать 800 байт.
                memset(line_buf, border_color, sizeof(line_buf));

                int at = BORDER_X + pic_shift_x;
                int len = fb_w < PICTURE_W ? fb_w : PICTURE_W;
                const uint8_t *from = fb_data + (size_t)src * fb_w;

                // Обрезка по краям строки, чтобы сдвиг не вышел за буфер
                if (at < 0) {
                    from -= at;
                    len += at;
                    at = 0;
                }
                if (at + len > DVI_FRAME_WIDTH)
                    len = DVI_FRAME_WIDTH - at;
                if (len > 0)
                    memcpy(line_buf + at, from, (size_t)len);

                tmds_encode_palette_data((const uint32_t *)line_buf, tmds_palette,
                                         tmdsbuf, DVI_FRAME_WIDTH, 6);
            }

            queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);
        }
    }
}

// ---------------------------------------------------------------------------
//  Интерфейс graphics_*
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  Диагностика запуска.
//
//  Внутри dvi_init() три места с panic(): нехватка каналов DMA, нехватка
//  автоматов состояний PIO и неудачное выделение буферов TMDS. Все три
//  выглядят снаружи одинаково — обработчик отказа мигает светодиодом, и
//  различить их нельзя. Поэтому опасные ресурсы проверяются заранее, и при
//  нехватке прошивка встаёт с распознаваемым числом миганий:
//
//      2 мигания — не хватает каналов DMA (нужно 6)
//      3 мигания — не хватает кучи под буферы TMDS (нужно 14400 байт)
//
//  Пауза между группами — секунда, так что счёт легко снять на глаз.
// ---------------------------------------------------------------------------
static void __attribute__((noreturn)) hdmi_dvi_halt(int blinks) {
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    while (true) {
        for (int i = 0; i < blinks; ++i) {
            gpio_put(PICO_DEFAULT_LED_PIN, 1);
            sleep_ms(150);
            gpio_put(PICO_DEFAULT_LED_PIN, 0);
            sleep_ms(150);
        }
        sleep_ms(1000);
    }
#else
    (void)blinks;
    while (true) tight_loop_contents();
#endif
}

void graphics_init(void) {
    DVI_VERTICAL_REPEAT = 2;

    build_palette();
    build_blank_line();

    // Шесть каналов DMA: по два на каждую из трёх линий TMDS
    int probe[6];
    int got = 0;
    for (; got < 6; ++got) {
        probe[got] = dma_claim_unused_channel(false);
        if (probe[got] < 0)
            break;
    }
    for (int i = 0; i < got; ++i)
        dma_channel_unclaim(probe[i]);
    if (got < 6)
        hdmi_dvi_halt(2);

    // Буферы TMDS выделяются из кучи: три штуки по 3 канала на 800 точек
    const size_t tmds_bytes = (size_t)TMDS_WORDS * sizeof(uint32_t);
    void *probe_mem[DVI_N_TMDS_BUFFERS];
    int mem_got = 0;
    for (; mem_got < DVI_N_TMDS_BUFFERS; ++mem_got) {
        probe_mem[mem_got] = malloc(tmds_bytes);
        if (!probe_mem[mem_got])
            break;
    }
    for (int i = 0; i < mem_got; ++i)
        free(probe_mem[i]);
    if (mem_got < DVI_N_TMDS_BUFFERS)
        hdmi_dvi_halt(3);

    dvi0.timing = &dvi_timing_800x600p_60hz;
    dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    // Как в PICO-BK: приоритет шины ядру 1, иначе обращения ядра 0 к памяти
    // подтормаживают выдачу строк. И явное обнуление полей кадра — dvi_init
    // их не трогает.
    hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
    dvi_get_blank_settings(&dvi0)->top = 0;
    dvi_get_blank_settings(&dvi0)->bottom = 0;
}

void graphics_set_buffer(uint8_t *buffer, const uint16_t width, const uint16_t height) {
    fb_data = buffer;
    fb_w = width;
    fb_h = height;
}

uint32_t graphics_get_width(void) {
    return fb_w;
}

uint32_t graphics_get_height(void) {
    return fb_h;
}

// На HDMI видно весь буфер: 288 строк укладываются в кадр целиком
uint32_t graphics_get_visible_height(void) {
    return fb_h;
}

uint8_t *graphics_get_frame(void) {
    return fb_data;
}

uint32_t graphics_get_font_width(void) {
    return 8;
}

uint32_t graphics_get_font_height(void) {
    return 8;
}

int graphics_get_picture_shift_x(void) {
    return pic_shift_x;
}

int graphics_get_picture_shift_y(void) {
    return pic_shift_y;
}

void graphics_inc_x(void) { ++pic_shift_x; }
void graphics_dec_x(void) { --pic_shift_x; }
void graphics_inc_y(void) { ++pic_shift_y; }
void graphics_dec_y(void) { --pic_shift_y; }

void graphics_set_offset(const int x, const int y) {
    pic_shift_x = x;
    pic_shift_y = y;
}

void graphics_set_mode(const enum graphics_mode_t mode) {
    (void)mode;
}

void graphics_set_duplicateLines(bool v) {
    (void)v;
}

void graphics_set_palette(uint8_t i, uint32_t color888) {
    // Палитра драйвера строится из кода цвета напрямую и внешних правок
    // не требует; функция оставлена ради единообразия интерфейса.
    (void)i; (void)color888;
}

// ---------------------------------------------------------------------------
//  Примитивы рисования. Как и у остальных драйверов, границы у rect и fill
//  включительные: на это рассчитан код меню и файлового диалога.
// ---------------------------------------------------------------------------

static inline void _plot(int32_t x, int32_t y, uint8_t color) {
    if (!fb_data) return;
    if (x < 0 || x >= (int32_t)fb_w) return;
    if (y < 0 || y >= (int32_t)fb_h) return;
    fb_data[(size_t)fb_w * y + x] = color;
}

static void hdmi_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color) {
    if (x0 == x1) {
        if (y1 < y0) { int32_t t = y0; y0 = y1; y1 = t; }
        for (int32_t yi = y0; yi <= y1; ++yi) _plot(x0, yi, color);
        return;
    }
    if (y0 == y1) {
        if (x1 < x0) { int32_t t = x0; x0 = x1; x1 = t; }
        for (int32_t xi = x0; xi <= x1; ++xi) _plot(xi, y0, color);
        return;
    }
    const int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1;
    const int32_t dy = y1 > y0 ? y1 - y0 : y0 - y1;
    if (dx > dy) {
        for (int32_t xi = 0; xi <= dx; ++xi)
            _plot(x0 + (x1 > x0 ? xi : -xi),
                  y0 + (y1 > y0 ? xi * dy / dx : -(xi * dy / dx)), color);
    } else {
        for (int32_t yi = 0; yi <= dy; ++yi)
            _plot(x0 + (x1 > x0 ? yi * dx / dy : -(yi * dx / dy)),
                  y0 + (y1 > y0 ? yi : -yi), color);
    }
}

void graphics_rect(int32_t x0, int32_t y0, uint32_t width, uint32_t height, uint8_t color) {
    const int32_t x1 = x0 + (int32_t)width;
    const int32_t y1 = y0 + (int32_t)height;
    hdmi_line(x0, y0, x1, y0, color);
    hdmi_line(x1, y0, x1, y1, color);
    hdmi_line(x1, y1, x0, y1, color);
    hdmi_line(x0, y1, x0, y0, color);
}

void graphics_fill(int32_t x0, int32_t y0, uint32_t width, uint32_t height, uint8_t bgcolor) {
    const int32_t x1 = x0 + (int32_t)width;
    const int32_t y1 = y0 + (int32_t)height;
    for (int32_t xi = x0; xi <= x1; ++xi)
        hdmi_line(xi, y0, xi, y1, bgcolor);
}

void graphics_type(int x, int y, uint8_t color, const char *msg, size_t msg_len) {
    for (size_t i = 0; i < msg_len; ++i) {
        const uint8_t ch = (uint8_t)msg[i];
        const uint8_t *glyph = font_8x8 + ch * 8;
        const uint32_t xt = x + i * 8;
        for (uint32_t j = 0; j < 8; ++j) {
            const uint8_t row = glyph[j];
            for (uint32_t k = 0; k < 8; ++k)
                if (row & (1u << k))
                    _plot(xt + k, y + j, color);
        }
    }
}
