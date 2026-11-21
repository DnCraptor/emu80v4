// 800x600 at 60 Hz. The 384x256 Specialist image is doubled on both axes.
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

#define PICTURE_W 384
#define PICTURE_H 256

#define DWORDS_PER_PLANE (DVI_FRAME_WIDTH / DVI_SYMBOLS_PER_WORD)
#define TMDS_WORDS (DWORDS_PER_PLANE * 3)

extern uint8_t DVI_VERTICAL_REPEAT;

static struct dvi_inst dvi0;

#define HDMI_AUDIO_INPUT_RATE 50000
#define HDMI_AUDIO_RATE 48000
#define HDMI_AUDIO_BUFFER_SIZE 256

static audio_sample_t hdmi_audio_buffer[HDMI_AUDIO_BUFFER_SIZE];
static volatile bool hdmi_audio_ready = false;

/*
 * SoundMixer generates exactly 50 kHz. HDMI/libdvi consumes 48 kHz.
 *
 * The conversion is performed here, at the output boundary, so AY, Covox,
 * beeper and the rest of the emulation keep their existing 50 kHz timing.
 *
 * 48/50 = 24/25: for each 25 input samples, 24 output samples are produced.
 * The output position is tracked with an integer phase accumulator. A linear
 * interpolation is made at the exact fractional position between the previous
 * and current 50 kHz samples. No division is used in the common no-output
 * path, and there can be at most one output sample per input sample.
 */
static bool hdmi_resampler_started = false;
static uint32_t hdmi_resampler_phase = 0;
static int16_t hdmi_resampler_prev_left = 0;
static int16_t hdmi_resampler_prev_right = 0;

static inline __attribute__((always_inline))
void hdmi_audio_enqueue(int16_t left, int16_t right)
{
    if (!hdmi_audio_ready ||
        get_write_size(&dvi0.audio_ring, false) == 0)
        return;

    audio_sample_t* sample = get_write_pointer(&dvi0.audio_ring);
    sample->channels[0] = left;
    sample->channels[1] = right;
    increase_write_pointer(&dvi0.audio_ring, 1);
}

void __not_in_flash_func(hdmi_dvi_push_audio_sample)(
        int16_t left, int16_t right)
{
    if (!hdmi_resampler_started) {
        hdmi_resampler_prev_left = left;
        hdmi_resampler_prev_right = right;
        hdmi_resampler_started = true;
        return;
    }

    const uint32_t old_phase = hdmi_resampler_phase;
    uint32_t new_phase = old_phase + HDMI_AUDIO_RATE;

    if (new_phase >= HDMI_AUDIO_INPUT_RATE) {
        /*
         * The next 48 kHz sample lies inside the interval:
         *
         *   previous + fraction * (current - previous)
         *
         * fraction = (INPUT_RATE - old_phase) / OUTPUT_RATE.
         */
        const uint32_t numerator =
            HDMI_AUDIO_INPUT_RATE - old_phase;

        const int32_t left_delta =
            (int32_t)left - hdmi_resampler_prev_left;
        const int32_t right_delta =
            (int32_t)right - hdmi_resampler_prev_right;

        const int32_t out_left =
            hdmi_resampler_prev_left +
            (int32_t)(((int64_t)left_delta * numerator +
                       HDMI_AUDIO_RATE / 2) /
                      HDMI_AUDIO_RATE);
        const int32_t out_right =
            hdmi_resampler_prev_right +
            (int32_t)(((int64_t)right_delta * numerator +
                       HDMI_AUDIO_RATE / 2) /
                      HDMI_AUDIO_RATE);

        hdmi_audio_enqueue((int16_t)out_left, (int16_t)out_right);
        new_phase -= HDMI_AUDIO_INPUT_RATE;
    }

    hdmi_resampler_phase = new_phase;
    hdmi_resampler_prev_left = left;
    hdmi_resampler_prev_right = right;
}

// Кадровый буфер эмулятора. Ставится из graphics_set_buffer().
static uint8_t *fb_data = NULL;
static uint16_t fb_w = PICTURE_W;
static uint16_t fb_h = PICTURE_H;
static uint16_t fb_stride = PICTURE_W;   // физ. шаг строки; по умолчанию = fb_w

// Сдвиг картинки. Знак нормализован так же, как у остальных драйверов:
// положительное значение означает смещение вправо и вниз по экрану.
static int pic_shift_x = 0;
static int pic_shift_y = 0;

// Строка индексов палитры, которую получает кодировщик: вся ширина кадра,
// поля заполнены цветом рамки, в середину копируется строка буфера.
static uint8_t __attribute__((aligned(4))) line_buf[DVI_FRAME_WIDTH];

// Полностью пустая строка в уже закодированном виде — для полей сверху и снизу
static uint32_t __attribute__((aligned(4))) blank_tmds[TMDS_WORDS];

#ifdef PICO_RP2040
/*
 * tmds_palette_encode_loop_x/y is unrolled for 80 input pixels per
 * iteration. n_pix must be a multiple of 80.
 */
static uint32_t* blank_tmds_buffers[DVI_N_TMDS_BUFFERS];
static uint8_t blank_tmds_count = 0;
#endif

// Символы TMDS для 64 цветов: шесть слов на цвет (две полярности x три канала)
static uint32_t __attribute__((aligned(4))) tmds_palette[64 * 6];

// PicoDVI's word per doubled pixel encodes two nearby levels with zero
// aggregate running disparity. Every RGB222 level can be selected directly.
static const uint32_t doubled_symbol_table[] = {
#include "tmds_table.h"
};
static uint32_t doubled_rgb222[64][3];

#ifdef PICO_RP2040
#define MENU_TEXT_COLS 80
#else
#define MENU_TEXT_COLS 100
#endif
#define MENU_TEXT_ROWS 37
#define MENU_TEXT_CELL_W 8
#define MENU_TEXT_CELL_H 16
#define MENU_TEXT_TRANSPARENT_ATTR 0xeeu

/*
 * The OSD text surface is rendered by the DVI scanline loop.
 */
static uint16_t menu_text_cells[MENU_TEXT_COLS * MENU_TEXT_ROWS];

/*
 * For each 8-bit text attribute and each pair of glyph bits, store two
 * ready RGB222 pixels. Four uint16_t writes produce one 8-pixel cell.
 */
static uint16_t menu_text_pair_lut[256][4];

#ifdef PICO_RP2040
static uint8_t menu_font_8x8_sram[256 * 8];
#endif

static inline uint8_t menu_color_index(uint8_t color)
{
    static const uint8_t rgb16[16][3] = {
        {0, 0, 0}, {0, 0, 2}, {0, 2, 0}, {0, 2, 2},
        {2, 0, 0}, {2, 0, 2}, {2, 1, 0}, {2, 2, 2},
        {1, 1, 1}, {1, 1, 3}, {1, 3, 1}, {1, 3, 3},
        {3, 1, 1}, {3, 1, 3}, {3, 3, 1}, {3, 3, 3}
    };

    const int r = (color >> 4) & 3u;
    const int g = (color >> 2) & 3u;
    const int b = color & 3u;
    unsigned best = 0;
    unsigned best_distance = ~0u;

    for (unsigned i = 0; i < 16; ++i) {
        const int dr = r - rgb16[i][0];
        const int dg = g - rgb16[i][1];
        const int db = b - rgb16[i][2];
        const unsigned distance =
            (unsigned)(dr * dr + dg * dg + db * db);
        if (distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return (uint8_t)best;
}

static const uint8_t menu_text_rgb222[16] = {
    0x00, 0x02, 0x08, 0x0a,
    0x20, 0x22, 0x24, 0x2a,
    0x15, 0x17, 0x1d, 0x1f,
    0x35, 0x37, 0x3d, 0x3f
};

static void menu_text_build_pair_lut(void)
{
    for (unsigned attr = 0; attr < 256; ++attr) {
        const uint8_t fg = menu_text_rgb222[attr & 0x0fu];
        const uint8_t bg = menu_text_rgb222[attr >> 4];

        for (unsigned pair = 0; pair < 4; ++pair) {
            const uint8_t p0 = (pair & 1u) ? fg : bg;
            const uint8_t p1 = (pair & 2u) ? fg : bg;
            menu_text_pair_lut[attr][pair] =
                (uint16_t)p0 | ((uint16_t)p1 << 8);
        }
    }
}

static inline void menu_text_put_cell(
        int x, int y, uint8_t ch, uint8_t attr)
{
    if ((unsigned)x >= MENU_TEXT_COLS ||
        (unsigned)y >= MENU_TEXT_ROWS)
        return;

    menu_text_cells[y * MENU_TEXT_COLS + x] =
        (uint16_t)ch | ((uint16_t)attr << 8);
}

static void menu_text_clear(uint8_t attr)
{
    const uint16_t cell = (uint16_t)' ' | ((uint16_t)attr << 8);
    for (unsigned i = 0; i < MENU_TEXT_COLS * MENU_TEXT_ROWS; ++i)
        menu_text_cells[i] = cell;
}

void menu_text_clear_for_mode(void)
{
    menu_text_clear(menu_video_mode == GRAPHICS_VIDEO_COMBINED
                  ? MENU_TEXT_TRANSPARENT_ATTR
                  : 0x17u);
}


// ---------------------------------------------------------------------------
//  Локальные операции с памятью для видеотракта.
//
//  Не вызывают libc из flash. Для выровненных буферов работают 32-битными
//  словами; хвост обрабатывается побайтно.
// ---------------------------------------------------------------------------

static inline __attribute__((always_inline))
void video_fill_u8(uint8_t* dst, uint8_t value, size_t count)
{
    while (count && ((uintptr_t)dst & 3u)) {
        *dst++ = value;
        --count;
    }

    const uint32_t word = (uint32_t)value * 0x01010101u;
    uint32_t* dst32 = (uint32_t*)dst;

    while (count >= 16) {
        dst32[0] = word;
        dst32[1] = word;
        dst32[2] = word;
        dst32[3] = word;
        dst32 += 4;
        count -= 16;
    }
    while (count >= 4) {
        *dst32++ = word;
        count -= 4;
    }

    dst = (uint8_t*)dst32;
    while (count--)
        *dst++ = value;
}

static inline __attribute__((always_inline))
void video_copy_u8(uint8_t* dst, const uint8_t* src, size_t count)
{
    if ((((uintptr_t)dst | (uintptr_t)src) & 3u) == 0u) {
        uint32_t* dst32 = (uint32_t*)dst;
        const uint32_t* src32 = (const uint32_t*)src;

        while (count >= 16) {
            dst32[0] = src32[0];
            dst32[1] = src32[1];
            dst32[2] = src32[2];
            dst32[3] = src32[3];
            dst32 += 4;
            src32 += 4;
            count -= 16;
        }
        while (count >= 4) {
            *dst32++ = *src32++;
            count -= 4;
        }

        dst = (uint8_t*)dst32;
        src = (const uint8_t*)src32;
    }

    while (count--)
        *dst++ = *src++;
}

static inline __attribute__((always_inline))
void video_copy_u32(uint32_t* dst, const uint32_t* src, size_t count)
{
    while (count >= 4) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = src[3];
        dst += 4;
        src += 4;
        count -= 4;
    }
    while (count--)
        *dst++ = *src++;
}

static void __not_in_flash_func(render_menu_text_dvi_line)(
        uint8_t* output, unsigned logical_y)
{
    const unsigned row = logical_y >> 3;
    const unsigned glyph_line = logical_y & 7u;

    if (row >= MENU_TEXT_ROWS) {
        video_fill_u8(output, 0, DVI_FRAME_WIDTH);
        return;
    }

    const uint16_t* cells =
        &menu_text_cells[row * MENU_TEXT_COLS];
#ifdef PICO_RP2040
    const uint8_t* font = menu_font_8x8_sram;
#else
    const uint8_t* font = font_8x8;
#endif
    uint16_t* dst = (uint16_t*)(output + (DVI_FRAME_WIDTH - MENU_TEXT_COLS * 8) / 2);

    for (unsigned x = 0; x < MENU_TEXT_COLS; ++x) {
        const uint16_t cell = cells[x];
        uint8_t glyph =
            font[((uint8_t)cell << 3) + glyph_line];
        const uint16_t* pairs =
            menu_text_pair_lut[(uint8_t)(cell >> 8)];

        *dst++ = pairs[glyph & 3u];
        glyph >>= 2;
        *dst++ = pairs[glyph & 3u];
        glyph >>= 2;
        *dst++ = pairs[glyph & 3u];
        glyph >>= 2;
        *dst++ = pairs[glyph & 3u];
    }
}

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
        doubled_rgb222[c][0] = doubled_symbol_table[b * 21];
        doubled_rgb222[c][1] = doubled_symbol_table[g * 21];
        doubled_rgb222[c][2] = doubled_symbol_table[r * 21];
    }
    tmds_setup_palette24_symbols(pal24, tmds_palette, 64);
}

static void build_blank_line(void) {
    for (unsigned word = 0; word < TMDS_WORDS; ++word)
        blank_tmds[word] = doubled_rgb222[0][0];
}

static void __not_in_flash_func(encode_specialist_line)(
        const uint8_t* pixels, uint32_t* tmds)
{
    const int left = (DWORDS_PER_PLANE - (int)fb_w) / 2 + pic_shift_x;
    const int start = left < 0 ? -left : 0;
    const int first = left < 0 ? 0 : left;
    const int right = left + fb_w;
    const int end = right < DWORDS_PER_PLANE ? right : DWORDS_PER_PLANE;
    const uint32_t black = doubled_rgb222[0][0];

    int x = 0;
    for (; x < first; ++x) {
        tmds[x] = black;
        tmds[x + DWORDS_PER_PLANE] = black;
        tmds[x + 2 * DWORDS_PER_PLANE] = black;
    }
    for (int pixel = start; x < end; ++x, ++pixel) {
        const uint32_t* rgb = doubled_rgb222[pixels[pixel]];
        tmds[x] = rgb[0];
        tmds[x + DWORDS_PER_PLANE] = rgb[1];
        tmds[x + 2 * DWORDS_PER_PLANE] = rgb[2];
    }
    for (; x < DWORDS_PER_PLANE; ++x) {
        tmds[x] = black;
        tmds[x + DWORDS_PER_PLANE] = black;
        tmds[x + 2 * DWORDS_PER_PLANE] = black;
    }
}

// ---------------------------------------------------------------------------
//  Цикл кодирования. Занимает ядро целиком.
// ---------------------------------------------------------------------------

static inline __attribute__((always_inline))
void copy_words(uint32_t *dst, const uint32_t *src, size_t n)
{
    video_copy_u32(dst, src, n);
}

#ifdef PICO_RP2040
static inline __attribute__((always_inline))
void ensure_blank_tmds_buffer(uint32_t* tmdsbuf)
{
    for (unsigned i = 0; i < blank_tmds_count; ++i)
        if (blank_tmds_buffers[i] == tmdsbuf)
            return;

    copy_words(tmdsbuf, blank_tmds, TMDS_WORDS);
    if (blank_tmds_count < DVI_N_TMDS_BUFFERS)
        blank_tmds_buffers[blank_tmds_count++] = tmdsbuf;
}
#endif

void __not_in_flash_func(hdmi_dvi_core_loop)(void) {
    // DMA_IRQ_0 берётся именно здесь, на ядре 1: обработчики прерываний
    // в RP2xxx свои у каждого ядра. Драйвер VGA в этой сборке не линкуется,
    // а звук на ядре 0 занимает DMA_IRQ_1 — пересечения нет.
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);

    uint32_t *tmdsbuf = NULL;
    while (true) {
        for (int y = 0; y < DVI_FRAME_HEIGHT; ++y) {
#ifdef PICO_RP2040
            static bool previousText = false;
            const bool text = menu_video_mode == GRAPHICS_VIDEO_TEXT;
            if (text && !previousText)
                blank_tmds_count = 0;
            previousText = text;
#else
            const bool text = menu_video_mode == GRAPHICS_VIDEO_TEXT;
#endif
            const int border_y = (DVI_FRAME_HEIGHT - (int)fb_h) / 2;
            const int src = y - border_y - pic_shift_y;

            queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);

            if (text) {
                render_menu_text_dvi_line(line_buf, (unsigned)y);
#ifdef PICO_RP2040
                ensure_blank_tmds_buffer(tmdsbuf);
                tmds_encode_palette_data_span(
                    (const uint32_t*)(line_buf + 80),
                    tmds_palette, tmdsbuf,
                    DVI_FRAME_WIDTH, 80, 640, 6);
#else
                tmds_encode_palette_data(
                    (const uint32_t*)line_buf, tmds_palette,
                    tmdsbuf, DVI_FRAME_WIDTH, 6);
#endif
                queue_add_blocking_u32(
                    &dvi0.q_tmds_valid, &tmdsbuf);
                continue;
            }

            const uint8_t* from = NULL;
            if (src >= 0 && src < (int)fb_h) {
#ifdef SPECIALIST_SCANLINES
                from = graphics_read_scanline((unsigned)src);
#else
                if (fb_data)
                    from = fb_data + (size_t)src * fb_stride;
#endif
            }
            if (!from) {
                copy_words(tmdsbuf, blank_tmds, TMDS_WORDS);
            } else {
                encode_specialist_line(from, tmdsbuf);
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
#ifdef PICO_RP2040
    video_copy_u8(menu_font_8x8_sram, font_8x8,
                  sizeof(menu_font_8x8_sram));
#endif
    menu_text_build_pair_lut();
    menu_text_clear_for_mode();

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
#if defined(ZERO2)
    // RP2350-PiZero: разъём HDMI разведён на GPIO 32..38, а PIO по умолчанию
    // адресует окно GPIO 0..31. Сдвигаем окно на 16 (охватывает 16..47) ДО
    // dvi_init(), иначе pio_gpio_init() на ногах >=32 не сработает.
    pio_set_gpio_base(dvi0.ser_cfg.pio, 16);
#endif
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

#if defined(PICO_RP2350)
    // HDMI audio, как в PICO-BK для 800x600@60. Пиксельная частота
    // этого режима 40 МГц; N=6144 и CTS=40000 дают ровно 48 кГц.
    dvi_audio_sample_buffer_set(
        &dvi0, hdmi_audio_buffer, HDMI_AUDIO_BUFFER_SIZE);
    dvi_set_audio_freq(&dvi0, HDMI_AUDIO_RATE, 40000, 6144);
    hdmi_resampler_started = false;
    hdmi_resampler_phase = 0;
    hdmi_audio_ready = true;
#endif
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
    fb_stride = width;   // по умолчанию шаг строки равен ширине
}

void graphics_set_line_stride(uint16_t stride) {
    fb_stride = stride;
}

uint16_t graphics_get_line_stride(void) {
    return fb_stride;
}

void graphics_set_video_content_mode(graphics_video_content_mode_t mode)
{
    if (mode < GRAPHICS_VIDEO_FRAMEBUFFER || mode > GRAPHICS_VIDEO_COMBINED)
        mode = GRAPHICS_VIDEO_FRAMEBUFFER;

    __asm volatile ("" ::: "memory");
    menu_video_mode = mode;
    menu_text_clear_for_mode();
    __asm volatile ("" ::: "memory");
}

void graphics_set_menu_text_mode(bool enabled)
{
    graphics_set_video_content_mode(enabled
                                  ? GRAPHICS_VIDEO_TEXT
                                  : GRAPHICS_VIDEO_FRAMEBUFFER);
}

uint32_t graphics_get_width(void) {
    return menu_text_active()
         ? MENU_TEXT_COLS * MENU_TEXT_CELL_W
         : fb_w;
}

uint32_t graphics_get_height(void) {
    return menu_text_active()
         ? MENU_TEXT_ROWS * MENU_TEXT_CELL_H
         : fb_h;
}

uint32_t graphics_get_visible_height(void) {
    return graphics_get_height();
}

uint8_t *graphics_get_frame(void) {
    return menu_text_active() ? NULL : fb_data;
}

uint32_t graphics_get_font_width(void) {
    return MENU_TEXT_CELL_W;
}

uint32_t graphics_get_font_height(void) {
    return menu_text_active() ? MENU_TEXT_CELL_H : 8;
}

int graphics_get_picture_shift_x(void) {
    return pic_shift_x;
}

int graphics_get_picture_shift_y(void) {
    return pic_shift_y;
}

void graphics_inc_x(void) {
    graphics_set_offset(pic_shift_x + 1, pic_shift_y);
}

void graphics_dec_x(void) {
    graphics_set_offset(pic_shift_x - 1, pic_shift_y);
}

void graphics_inc_y(void) {
    graphics_set_offset(pic_shift_x, pic_shift_y + 1);
}

void graphics_dec_y(void) {
    graphics_set_offset(pic_shift_x, pic_shift_y - 1);
}

void graphics_set_offset(const int x, const int y) {
    pic_shift_x = x < -128 ? -128 : x > 128 ? 128 : x;
    pic_shift_y = y < -128 ? -128 : y > 128 ? 128 : y;
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
    fb_data[(size_t)fb_stride * y + x] = color;
}

void plot(int x, int y, uint8_t color)
{
    if (menu_text_active()) {
        const int cx = x / MENU_TEXT_CELL_W;
        const int cy = y / MENU_TEXT_CELL_H;
        if ((unsigned)cx < MENU_TEXT_COLS &&
            (unsigned)cy < MENU_TEXT_ROWS) {
            const uint16_t old =
                menu_text_cells[cy * MENU_TEXT_COLS + cx];
            const uint8_t attr = (uint8_t)(old >> 8);
            const uint8_t fg = menu_color_index(color);
            menu_text_put_cell(cx, cy, (uint8_t)old,
                               (uint8_t)((attr & 0xf0u) | fg));
        }
        return;
    }

    _plot(x, y, color);
}

static void hdmi_line(
        int32_t x0, int32_t y0, int32_t x1, int32_t y1,
        uint8_t color)
{
    if (x0 == x1) {
        if (y1 < y0) {
            int32_t t = y0; y0 = y1; y1 = t;
        }
        for (int32_t yi = y0; yi <= y1; ++yi)
            _plot(x0, yi, color);
        return;
    }

    if (y0 == y1) {
        if (x1 < x0) {
            int32_t t = x0; x0 = x1; x1 = t;
        }
        for (int32_t xi = x0; xi <= x1; ++xi)
            _plot(xi, y0, color);
        return;
    }

    const int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1;
    const int32_t dy = y1 > y0 ? y1 - y0 : y0 - y1;
    if (dx > dy) {
        for (int32_t xi = 0; xi <= dx; ++xi)
            _plot(x0 + (x1 > x0 ? xi : -xi),
                  y0 + (y1 > y0
                      ? xi * dy / dx : -(xi * dy / dx)),
                  color);
    } else {
        for (int32_t yi = 0; yi <= dy; ++yi)
            _plot(x0 + (x1 > x0
                      ? yi * dx / dy : -(yi * dx / dy)),
                  y0 + (y1 > y0 ? yi : -yi),
                  color);
    }
}

void line(int x0, int y0, int x1, int y1, uint8_t color)
{
    if (menu_text_active()) {
        int cx0 = x0 / MENU_TEXT_CELL_W;
        int cx1 = x1 / MENU_TEXT_CELL_W;
        int cy0 = y0 / MENU_TEXT_CELL_H;
        int cy1 = y1 / MENU_TEXT_CELL_H;
        const uint8_t fg = menu_color_index(color);

        if (cx0 > cx1) {
            int t = cx0; cx0 = cx1; cx1 = t;
        }
        if (cy0 > cy1) {
            int t = cy0; cy0 = cy1; cy1 = t;
        }

        if (cy0 == cy1) {
            for (int x = cx0; x <= cx1; ++x) {
                const uint16_t old =
                    ((unsigned)x < MENU_TEXT_COLS &&
                     (unsigned)cy0 < MENU_TEXT_ROWS)
                    ? menu_text_cells[cy0 * MENU_TEXT_COLS + x]
                    : 0x0720u;
                menu_text_put_cell(
                    x, cy0, '-',
                    (uint8_t)(((old >> 8) & 0xf0u) | fg));
            }
        } else if (cx0 == cx1) {
            for (int y = cy0; y <= cy1; ++y) {
                const uint16_t old =
                    ((unsigned)cx0 < MENU_TEXT_COLS &&
                     (unsigned)y < MENU_TEXT_ROWS)
                    ? menu_text_cells[y * MENU_TEXT_COLS + cx0]
                    : 0x0720u;
                menu_text_put_cell(
                    cx0, y, '|',
                    (uint8_t)(((old >> 8) & 0xf0u) | fg));
            }
        }
        return;
    }

    hdmi_line(x0, y0, x1, y1, color);
}

void graphics_rect(
        int32_t x0, int32_t y0,
        uint32_t width, uint32_t height,
        uint8_t color)
{
    if (menu_text_active()) {
        const int x = x0 / MENU_TEXT_CELL_W;
        const int y = y0 / MENU_TEXT_CELL_H;
        const int x1 =
            (x0 + (int32_t)width) / MENU_TEXT_CELL_W;
        const int y1 =
            (y0 + (int32_t)height) / MENU_TEXT_CELL_H;
        const uint8_t fg = menu_color_index(color);

        for (int cx = x; cx <= x1; ++cx) {
            menu_text_put_cell(cx, y, '-', fg);
            menu_text_put_cell(cx, y1, '-', fg);
        }
        for (int cy = y; cy <= y1; ++cy) {
            menu_text_put_cell(x, cy, '|', fg);
            menu_text_put_cell(x1, cy, '|', fg);
        }
        menu_text_put_cell(x, y, '+', fg);
        menu_text_put_cell(x1, y, '+', fg);
        menu_text_put_cell(x, y1, '+', fg);
        menu_text_put_cell(x1, y1, '+', fg);
        return;
    }

    const int32_t x1 = x0 + (int32_t)width;
    const int32_t y1 = y0 + (int32_t)height;
    hdmi_line(x0, y0, x1, y0, color);
    hdmi_line(x1, y0, x1, y1, color);
    hdmi_line(x1, y1, x0, y1, color);
    hdmi_line(x0, y1, x0, y0, color);
}

void graphics_fill(
        int32_t x0, int32_t y0,
        uint32_t width, uint32_t height,
        uint8_t bgcolor)
{
    if (menu_text_active()) {
        if (width == 0 || height == 0)
            return;

        const int cx0 = x0 / MENU_TEXT_CELL_W;
        const int cy0 = y0 / MENU_TEXT_CELL_H;
        const int cx1 =
            (x0 + (int32_t)width - 1) / MENU_TEXT_CELL_W;
        const int cy1 =
            (y0 + (int32_t)height - 1) / MENU_TEXT_CELL_H;
        const uint8_t bg = menu_color_index(bgcolor);
        const uint8_t fg =
            (bg == 0 || bg == 1 || bg == 4 ||
             bg == 5 || bg == 8) ? 7u : 0u;

        for (int cy = cy0; cy <= cy1; ++cy)
            for (int cx = cx0; cx <= cx1; ++cx)
                menu_text_put_cell(
                    cx, cy, ' ',
                    (uint8_t)((bg << 4) | fg));
        return;
    }

    const int32_t x1 = x0 + (int32_t)width;
    const int32_t y1 = y0 + (int32_t)height;
    for (int32_t xi = x0; xi <= x1; ++xi)
        hdmi_line(xi, y0, xi, y1, bgcolor);
}

void graphics_type(
        int x, int y, uint8_t color,
        const char *msg, size_t msg_len)
{
    if (menu_text_active()) {
        int cx = x / MENU_TEXT_CELL_W;
        const int cy = y / MENU_TEXT_CELL_H;
        const uint8_t fg = menu_color_index(color);

        for (size_t i = 0;
             i < msg_len && cx < MENU_TEXT_COLS;
             ++i, ++cx) {
            if (cx < 0 || (unsigned)cy >= MENU_TEXT_ROWS)
                continue;

            const uint16_t old =
                menu_text_cells[cy * MENU_TEXT_COLS + cx];
            const uint8_t attr =
                (uint8_t)(((old >> 8) & 0xf0u) | fg);
            menu_text_put_cell(
                cx, cy, (uint8_t)msg[i], attr);
        }
        return;
    }

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
