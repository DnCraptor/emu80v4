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
#define TMDS_PALETTE_SPAN_GRANULARITY 80

static uint32_t* vector_blank_tmds_buffers[DVI_N_TMDS_BUFFERS];
static uint8_t vector_blank_tmds_count = 0;
static int vector_span_x = -1;
static int vector_span_width = -1;
#endif

// Символы TMDS для 64 цветов: шесть слов на цвет (две полярности x три канала)
static uint32_t __attribute__((aligned(4))) tmds_palette[64 * 6];

static uint8_t border_color = 0;

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
 * Stage 1: the HDMI/DVI text surface and drawing backend are implemented,
 * but hdmi_dvi_core_loop() does not render it yet. This deliberately
 * separates menu/framebuffer isolation from DVI scan-line composition.
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

#ifdef PICO_RP2040
/*
 * RP2040 HDMI/DVI timing diagnostics:
 *
 * 0 - normal path: Vector rendering + TMDS encoding;
 * 1 - TMDS encoding only, from a prebuilt RGB222 test line;
 * 2 - Vector rendering only; output uses the already encoded blank line.
 *
 * A red fallback line from libdvi means this core failed to enqueue the
 * requested TMDS line before the transmitter needed it.
 */
#ifndef HDMI_RP2040_DIAG_MODE
#define HDMI_RP2040_DIAG_MODE 0
#endif

#if HDMI_RP2040_DIAG_MODE < 0 || HDMI_RP2040_DIAG_MODE > 2
#error HDMI_RP2040_DIAG_MODE must be 0, 1 or 2
#endif

#define VECTOR_LUT_COUNT 3

typedef struct {
    const uint8_t* memory;
    uint8_t palette[16];
    uint8_t border_color;
    uint8_t line_offset;
    uint8_t lut_index;
    bool mode512;
    bool show_border;
    bool enabled;
    uint32_t border_pattern;
} vector_video_state_t;

static vector_video_state_t vector_video_state;
static volatile uint32_t vector_video_seq = 0;
static uint32_t vector_pair_lut[VECTOR_LUT_COUNT][256];
static volatile uint8_t vector_lut_in_use = 0xff;
static vector_video_state_t vector_frame_state;
static bool vector_frame_state_valid = false;

static inline void vector_video_barrier(void)
{
    __asm volatile ("" ::: "memory");
}

static bool __not_in_flash_func(vector_video_snapshot)(
        vector_video_state_t* state)
{
    for (;;) {
        const uint32_t seq0 = vector_video_seq;
        if (seq0 & 1u)
            continue;
        vector_video_barrier();
        *state = vector_video_state;
        vector_video_barrier();
        const uint32_t seq1 = vector_video_seq;
        if (seq0 == seq1 && !(seq1 & 1u))
            return state->enabled && state->memory;
    }
}

static inline uint8_t vector_color_byte(
        const vector_video_state_t* state, uint8_t color)
{
    return state->palette[color];
}

#if HDMI_RP2040_DIAG_MODE == 1
static void build_rp2040_tmds_test_line(void)
{
    /*
     * Eight 100-pixel bars.  The buffer is prepared once on core0; core1 only
     * executes the existing full-width palette TMDS encoder.
     */
    static const uint8_t bars[8] = {
        0x00, 0x03, 0x0c, 0x0f,
        0x30, 0x33, 0x3c, 0x3f
    };

    for (unsigned x = 0; x < DVI_FRAME_WIDTH; ++x)
        line_buf[x] = bars[x / 100u];
}
#endif

/* Runs on core0 when the frame state is published. */
static void vector_build_lut(vector_video_state_t* state, uint32_t* lut)
{
    for (unsigned i = 0; i < 256; ++i) {
        const unsigned y = i >> 6;
        const unsigned r = (i >> 4) & 3u;
        const unsigned g = (i >> 2) & 3u;
        const unsigned b = i & 3u;
        const uint8_t c0 = (uint8_t)(((y & 2u) << 2) |
                                     ((r & 2u) << 1) |
                                     (g & 2u) | (b >> 1));
        const uint8_t c1 = (uint8_t)(((y & 1u) << 3) |
                                     ((r & 1u) << 2) |
                                     ((g & 1u) << 1) | (b & 1u));
        uint8_t p0, p1, p2, p3;
        if (state->mode512) {
            p0 = vector_color_byte(state, c0 & 0x03u);
            p1 = vector_color_byte(state, c0 & 0x0cu);
            p2 = vector_color_byte(state, c1 & 0x03u);
            p3 = vector_color_byte(state, c1 & 0x0cu);
        } else {
            p0 = p1 = vector_color_byte(state, c0);
            p2 = p3 = vector_color_byte(state, c1);
        }
        lut[i] = (uint32_t)p0 | ((uint32_t)p1 << 8) |
                 ((uint32_t)p2 << 16) | ((uint32_t)p3 << 24);
    }

    const uint8_t border = state->border_color;
    if (state->mode512) {
        const uint8_t p0 = vector_color_byte(state, border & 0x03u);
        const uint8_t p1 = vector_color_byte(state, border & 0x0cu);
        state->border_pattern = (uint32_t)p0 | ((uint32_t)p1 << 8) |
                                ((uint32_t)p0 << 16) | ((uint32_t)p1 << 24);
    } else {
        const uint8_t p = vector_color_byte(state, border);
        state->border_pattern = (uint32_t)p * 0x01010101u;
    }
}

static inline uint32_t vector_rotate_pattern(uint32_t pattern, unsigned bytes)
{
    bytes &= 3u;
    return bytes ? (pattern >> (bytes * 8u)) |
                   (pattern << ((4u - bytes) * 8u)) : pattern;
}

static inline void vector_fill32(uint8_t* dst, int count, uint32_t pattern)
{
    while (count && ((uintptr_t)dst & 3u)) {
        *dst++ = (uint8_t)pattern;
        pattern = vector_rotate_pattern(pattern, 1);
        --count;
    }
    uint32_t* dst32 = (uint32_t*)dst;
    while (count >= 16) {
        dst32[0] = pattern;
        dst32[1] = pattern;
        dst32[2] = pattern;
        dst32[3] = pattern;
        dst32 += 4;
        count -= 16;
    }
    while (count >= 4) {
        *dst32++ = pattern;
        count -= 4;
    }
    dst = (uint8_t*)dst32;
    while (count--) {
        *dst++ = (uint8_t)pattern;
        pattern = vector_rotate_pattern(pattern, 1);
    }
}

static inline uint8_t vector_pixel_slow(
        const vector_video_state_t* state, int x, uint8_t roll_off)
{
    const int offset = ((x & 0x1f0) << 4) | roll_off;
    const uint8_t mask = (uint8_t)(0x80u >> ((x & 0x0e) >> 1));
    const uint8_t* memory = state->memory;
    uint8_t color = 0;
    if (memory[0x8000 + offset] & mask) color |= 0x08;
    if (memory[0xa000 + offset] & mask) color |= 0x04;
    if (memory[0xc000 + offset] & mask) color |= 0x02;
    if (memory[0xe000 + offset] & mask) color |= 0x01;
    if (state->mode512)
        color = (x & 1) ? (color & 0x0c) : (color & 0x03);
    return vector_color_byte(state, color);
}

static void __not_in_flash_func(render_vector_active_pixels)(
        uint8_t* output, int source_x, int count, int n_line,
        const vector_video_state_t* state)
{
    const uint8_t roll_off =
        (uint8_t)(state->line_offset - n_line + 40);
    int x = source_x;
    int remaining = count;

    while (remaining && ((x & 15) || ((uintptr_t)output & 3u))) {
        *output++ = vector_pixel_slow(state, x++, roll_off);
        --remaining;
    }

    const uint8_t* memory = state->memory;
    const uint32_t* lut = vector_pair_lut[state->lut_index];
    uint32_t* out32 = (uint32_t*)output;
    while (remaining >= 16) {
        const int offset = ((x & 0x1f0) << 4) | roll_off;
        const uint8_t by = memory[0x8000 + offset];
        const uint8_t br = memory[0xa000 + offset];
        const uint8_t bg = memory[0xc000 + offset];
        const uint8_t bb = memory[0xe000 + offset];

#define VECTOR_PAIR_INDEX(shift) \
        (((((unsigned)by >> (shift)) & 3u) << 6) | \
         ((((unsigned)br >> (shift)) & 3u) << 4) | \
         ((((unsigned)bg >> (shift)) & 3u) << 2) | \
          (((unsigned)bb >> (shift)) & 3u))
        out32[0] = lut[VECTOR_PAIR_INDEX(6)];
        out32[1] = lut[VECTOR_PAIR_INDEX(4)];
        out32[2] = lut[VECTOR_PAIR_INDEX(2)];
        out32[3] = lut[VECTOR_PAIR_INDEX(0)];
#undef VECTOR_PAIR_INDEX
        out32 += 4;
        x += 16;
        remaining -= 16;
    }

    output = (uint8_t*)out32;
    while (remaining--)
        *output++ = vector_pixel_slow(state, x++, roll_off);
}

static void __not_in_flash_func(render_vector_dvi_line)(
        uint8_t* output, int source_y,
        const vector_video_state_t* state)
{
    const int source_width = state->show_border ? 626 : 512;
    int left = (DVI_FRAME_WIDTH - source_width) / 2 + pic_shift_x;
    int source_x = 0;

    if (left < 0) {
        source_x = -left;
        left = 0;
    }
    if (left > DVI_FRAME_WIDTH)
        left = DVI_FRAME_WIDTH;

    int drawable = source_width - source_x;
    if (drawable < 0)
        drawable = 0;
    if (drawable > DVI_FRAME_WIDTH - left)
        drawable = DVI_FRAME_WIDTH - left;

    /*
     * line_buf is cleared once at the beginning of the DVI frame.  Every
     * source line overwrites the same drawable horizontal interval, so
     * clearing all 800 bytes again here is redundant and was enough to push
     * Vector rendering + TMDS encoding beyond the RP2040 line budget.
     */
    output += left;

    const int n_line = source_y + (state->show_border ? 24 : 40);
    if (!state->show_border) {
        render_vector_active_pixels(
            output, source_x, drawable, n_line, state);
        return;
    }

    if (n_line < 40 || n_line >= 296) {
        vector_fill32(
            output, drawable,
            vector_rotate_pattern(state->border_pattern, source_x));
        return;
    }

    int x = source_x;
    int remaining = drawable;
    if (x < 57) {
        int n = 57 - x;
        if (n > remaining) n = remaining;
        vector_fill32(
            output, n,
            vector_rotate_pattern(state->border_pattern, x));
        output += n;
        x += n;
        remaining -= n;
    }
    if (remaining && x < 569) {
        int n = 569 - x;
        if (n > remaining) n = remaining;
        render_vector_active_pixels(
            output, x - 57, n, n_line, state);
        output += n;
        x += n;
        remaining -= n;
    }
    if (remaining)
        vector_fill32(
            output, remaining,
            vector_rotate_pattern(state->border_pattern, x));
}
#endif

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

    if (row >= MENU_TEXT_ROWS)
        return;

    const uint16_t* cells =
        &menu_text_cells[row * MENU_TEXT_COLS];
#ifdef PICO_RP2040
    const uint8_t* font = menu_font_8x8_sram;
#else
    const uint8_t* font = font_8x8;
#endif
    uint16_t* dst = (uint16_t*)(output + 80);

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
    }
    tmds_setup_palette24_symbols(pal24, tmds_palette, 64);
}

static void build_blank_line(void) {
    video_fill_u8(line_buf, border_color, sizeof(line_buf));
    tmds_encode_palette_data((const uint32_t *)line_buf, tmds_palette,
                             blank_tmds, DVI_FRAME_WIDTH, 6);
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
void vector_reset_blank_buffer_cache(void)
{
    vector_blank_tmds_count = 0;
}

static inline __attribute__((always_inline))
void vector_ensure_blank_tmds_buffer(uint32_t* tmdsbuf)
{
    for (unsigned i = 0; i < vector_blank_tmds_count; ++i)
        if (vector_blank_tmds_buffers[i] == tmdsbuf)
            return;

    copy_words(tmdsbuf, blank_tmds, TMDS_WORDS);
    if (vector_blank_tmds_count < DVI_N_TMDS_BUFFERS)
        vector_blank_tmds_buffers[vector_blank_tmds_count++] = tmdsbuf;
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
            if (y == 0) {
#if HDMI_RP2040_DIAG_MODE != 1
                video_fill_u8(line_buf, 0, sizeof(line_buf));
#endif
                vector_frame_state_valid =
                    vector_video_snapshot(&vector_frame_state);
                if (vector_frame_state_valid)
                    vector_lut_in_use =
                        vector_frame_state.lut_index;
            }

            const int vector_height =
                vector_frame_state.show_border ? 288 : 256;
            const int border_y =
                (DVI_FRAME_HEIGHT - vector_height) / 2;
            const int src = y - border_y - pic_shift_y;
#else
            // Строка кадрового буфера, попадающая в эту строку кадра.
            const int border_y =
                (DVI_FRAME_HEIGHT - (int)fb_h) / 2;
            const int src = y - border_y - pic_shift_y;
#endif

            queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);

            if (menu_video_mode == GRAPHICS_VIDEO_TEXT) {
                render_menu_text_dvi_line(line_buf, (unsigned)y);
#ifdef PICO_RP2040
                vector_ensure_blank_tmds_buffer(tmdsbuf);
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

#ifdef PICO_RP2040
#if HDMI_RP2040_DIAG_MODE == 1
            /*
             * Encoder-only test.  No Vector state, RAM or LUT is touched.
             * Every logical line encodes the same prebuilt 800-pixel pattern.
             */
            tmds_encode_palette_data(
                (const uint32_t*)line_buf, tmds_palette,
                tmdsbuf, DVI_FRAME_WIDTH, 6);
#elif HDMI_RP2040_DIAG_MODE == 2
            /*
             * Renderer-only test.  Build the same Vector line as normal, but
             * do not encode it; enqueue the pre-encoded blank TMDS line.
             */
            if (vector_frame_state_valid &&
                src >= 0 && src < vector_height)
                render_vector_dvi_line(
                    line_buf, src, &vector_frame_state);
            copy_words(tmdsbuf, blank_tmds, TMDS_WORDS);
#else
            if (!vector_frame_state_valid ||
                src < 0 || src >= vector_height) {
                copy_words(tmdsbuf, blank_tmds, TMDS_WORDS);
            } else {
                /*
                 * Both Vector layouts fit completely inside x=80..719:
                 *
                 *   border mode: 626 pixels at nominal x=87
                 *   crop mode:   512 pixels at nominal x=144
                 *
                 * Encode one fixed 640-pixel span. The start and width are
                 * both compatible with the assembly loop's 80-pixel
                 * granularity. The outer 80-pixel columns remain the
                 * pre-encoded black template.
                 *
                 * Running disparity is restarted at x=80 in this RP2040 fast
                 * path. The resulting symbols remain valid TMDS symbols, and
                 * removing one more 80-pixel block is necessary to meet the
                 * scanline deadline.
                 */
                const int encode_x = 80;
                const int encode_width = 640;

                if (encode_x != vector_span_x ||
                    encode_width != vector_span_width) {
                    vector_span_x = encode_x;
                    vector_span_width = encode_width;
                    vector_reset_blank_buffer_cache();
                }

                render_vector_dvi_line(
                    line_buf, src, &vector_frame_state);

                if (encode_width > 0) {
                    vector_ensure_blank_tmds_buffer(tmdsbuf);
                    tmds_encode_palette_data_span(
                        (const uint32_t*)(line_buf + encode_x),
                        tmds_palette, tmdsbuf,
                        DVI_FRAME_WIDTH,
                        (size_t)encode_x,
                        (size_t)encode_width, 6);
                } else {
                    copy_words(tmdsbuf, blank_tmds, TMDS_WORDS);
                }
            }
#endif
#else
            if (!fb_data || src < 0 || src >= (int)fb_h) {
                copy_words(tmdsbuf, blank_tmds, TMDS_WORDS);
            } else {
                // Поля заполняются каждый раз: горизонтальный сдвиг может
                // измениться между строками, а отдельно отслеживать это дороже,
                // чем просто записать 800 байт.
                video_fill_u8(line_buf, border_color, sizeof(line_buf));

                int len = fb_w < PICTURE_W ? fb_w : PICTURE_W;
                // Центрируем по ширине от полезной ширины (fb_w), а не от
                // константы: при обрезке fb_w меньше и картинка иначе не
                // окажется по центру. Шаг строки в буфере — fb_stride.
                int at = (DVI_FRAME_WIDTH - len) / 2 + pic_shift_x;
                const uint8_t *from = fb_data + (size_t)src * fb_stride;

                // Обрезка по краям строки, чтобы сдвиг не вышел за буфер
                if (at < 0) {
                    from -= at;
                    len += at;
                    at = 0;
                }
                if (at + len > DVI_FRAME_WIDTH)
                    len = DVI_FRAME_WIDTH - at;
                if (len > 0)
                    video_copy_u8(line_buf + at, from, (size_t)len);

                tmds_encode_palette_data((const uint32_t *)line_buf, tmds_palette,
                                         tmdsbuf, DVI_FRAME_WIDTH, 6);
            }
#endif

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
#if defined(PICO_RP2040) && HDMI_RP2040_DIAG_MODE == 1
    build_rp2040_tmds_test_line();
#endif

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

void graphics_set_vector_source(
        const uint8_t* memory, const uint8_t* palette,
        uint8_t vector_border_color, uint8_t line_offset,
        bool mode512, bool show_border)
{
#ifdef PICO_RP2040
    vector_video_state_t next = vector_video_state;
    next.memory = memory;
    next.border_color = vector_border_color;
    next.line_offset = line_offset;
    next.mode512 = mode512;
    next.show_border = show_border;
    next.enabled = memory != NULL;

    if (palette)
        memcpy(next.palette, palette, sizeof(next.palette));

    uint8_t lut_index = (uint8_t)(next.lut_index + 1u);
    if (lut_index >= VECTOR_LUT_COUNT)
        lut_index = 0;
    if (lut_index == vector_lut_in_use) {
        ++lut_index;
        if (lut_index >= VECTOR_LUT_COUNT)
            lut_index = 0;
    }
    next.lut_index = lut_index;
    vector_build_lut(&next, vector_pair_lut[lut_index]);

    vector_video_seq++;
    vector_video_barrier();
    vector_video_state = next;
    vector_video_barrier();
    vector_video_seq++;
#else
    (void)memory;
    (void)palette;
    (void)vector_border_color;
    (void)line_offset;
    (void)mode512;
    (void)show_border;
#endif
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
    if (mode < GRAPHICS_VIDEO_VECTOR || mode > GRAPHICS_VIDEO_COMBINED)
        mode = GRAPHICS_VIDEO_VECTOR;

    __asm volatile ("" ::: "memory");
    menu_video_mode = mode;
    menu_text_clear_for_mode();
    __asm volatile ("" ::: "memory");
}

void graphics_set_menu_text_mode(bool enabled)
{
    graphics_set_video_content_mode(enabled
                                  ? GRAPHICS_VIDEO_TEXT
                                  : GRAPHICS_VIDEO_VECTOR);
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
    return menu_text_active() ? 0 : pic_shift_x;
}

int graphics_get_picture_shift_y(void) {
    return menu_text_active() ? 0 : pic_shift_y;
}

void graphics_inc_x(void) {
#ifndef PICO_RP2040
    pic_shift_x += menu_text_active() ? MENU_TEXT_CELL_W : 1;
#endif
}

void graphics_dec_x(void) {
#ifndef PICO_RP2040
    pic_shift_x -= menu_text_active() ? MENU_TEXT_CELL_W : 1;
#endif
}

void graphics_inc_y(void) {
    pic_shift_y += menu_text_active() ? MENU_TEXT_CELL_H : 1;
}

void graphics_dec_y(void) {
    pic_shift_y -= menu_text_active() ? MENU_TEXT_CELL_H : 1;
}

void graphics_set_offset(const int x, const int y) {
#ifdef PICO_RP2040
    /*
     * The RP2040 fast path encodes one fixed 640-pixel TMDS span at x=80.
     * Horizontal movement would move Vector pixels outside that span and can
     * make the encoder miss its line deadline. Keep the image centered.
     */
    (void)x;
    pic_shift_x = 0;
#else
    pic_shift_x = x;
#endif
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
