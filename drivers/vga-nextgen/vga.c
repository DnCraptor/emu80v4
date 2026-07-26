#include "graphics.h"
#include "hardware/clocks.h"
#include "stdbool.h"
#include "hardware/structs/pll.h"
#include "hardware/structs/systick.h"

#include "hardware/dma.h"
#include "hardware/irq.h"
#include <string.h>
#include <stdio.h>
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "stdlib.h"
#include "font8x8.h"

uint16_t pio_program_VGA_instructions[] = {
    //     .wrap_target
    0x6008, //  0: out    pins, 8
    //     .wrap
};

const struct pio_program pio_program_VGA = {
    .instructions = pio_program_VGA_instructions,
    .length = 1,
    .origin = -1,
};


static uint32_t* lines_pattern[4];
static uint32_t* lines_pattern_data = NULL;
static int _SM_VGA = -1;

static int N_lines_total = 525;
static int N_lines_visible = 480;
static int line_VS_begin = 490;
static int line_VS_end = 491;
static int shift_picture = 0;

static int visible_line_size = 640 / 2;

static int dma_chan_ctrl;
static int dma_chan;

volatile static uint8_t* graphics_buffer = 0;
static int client_buffer_width = 320;
static int client_buffer_stride = 320;   // физ. шаг строки; по умолчанию = width
static int client_buffer_height = 240;
static int graphics_buffer_width = 640;
static int graphics_buffer_height = 480;
static int graphics_buffer_shift_x = 0;
static int graphics_buffer_shift_y = 0;
static bool duplicateLines = true;

static bool is_flash_line = false;
static bool is_flash_frame = false;

static uint32_t bg_color[2];
static uint16_t palette16_mask = 0;

#ifdef PICO_RP2040
#define VECTOR_LUT_COUNT 3

/*
 * Diagnostic mode: bypass guest RAM and palette decoding completely.
 * The VGA IRQ only fills the visible line with a cheap test pattern.
 * Set to 0 after verifying that sync is stable.
 */
#ifndef VECTOR_RP2040_DIAG_TEST_PATTERN
#define VECTOR_RP2040_DIAG_TEST_PATTERN 0
#endif

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

/* Diagnostic publication state, written on core0 and read on core1:
 * 0 - no video API call yet;
 * 1 - graphics_set_vector_source() called with invalid/NULL memory;
 * 2 - graphics_set_vector_source() published valid state;
 * 3 - graphics_set_buffer() disabled direct Vector rendering.
 */
static volatile uint8_t vector_video_publish_state = 0;

/*
 * One byte from each of the four Vector planes describes eight source bits,
 * which become sixteen VGA pixels.  Each 2-bit slice of the four bytes forms
 * an 8-bit LUT index and produces four packed VGA pixels.  Three LUT copies
 * let core0 prepare a new palette without touching the table currently used
 * by the core1 scan-line IRQ.
 */
static uint32_t vector_pair_lut[VECTOR_LUT_COUNT][256];
static volatile uint8_t vector_lut_in_use = 0xff;
static vector_video_state_t vector_frame_state;
static bool vector_frame_state_valid;

static inline void vector_video_barrier(void) {
    __asm volatile ("" ::: "memory");
}

static bool __time_critical_func(vector_video_snapshot)(vector_video_state_t* state) {
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

static inline __attribute__((always_inline)) uint8_t vector_color_byte(
        const vector_video_state_t* state, uint8_t color) {
    return state->palette[color] | 0xc0;
}

/* Runs on core0, outside the VGA IRQ. */
static void vector_build_lut(vector_video_state_t* state, uint32_t* lut) {
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

static inline __attribute__((always_inline)) uint32_t vector_rotate_pattern(
        uint32_t pattern, unsigned bytes) {
    bytes &= 3u;
    return bytes ? (pattern >> (bytes * 8u)) |
                   (pattern << ((4u - bytes) * 8u)) : pattern;
}

static inline __attribute__((always_inline)) void vector_fill32(
        uint8_t* dst, int count, uint32_t pattern) {
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

static inline __attribute__((always_inline)) uint8_t vector_pixel_slow(
        const vector_video_state_t* state, int x, uint8_t roll_off) {
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

static void __time_critical_func(render_vector_active_pixels)(
        uint8_t* output, int source_x, int count, int n_line,
        const vector_video_state_t* state) {
    const uint8_t roll_off = (uint8_t)(state->line_offset - n_line + 40);
    int x = source_x;
    int remaining = count;

    /* The fast path needs both a 16-pixel source boundary and a word-aligned
       destination.  Standard centred 512/626-pixel modes satisfy both. */
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
    while (remaining--) {
        *output++ = vector_pixel_slow(state, x++, roll_off);
    }
}

static void __time_critical_func(render_vector_vga_line)(
        uint8_t* output, int y, const vector_video_state_t* state) {
#if VECTOR_RP2040_DIAG_TEST_PATTERN
    (void)state;

    /*
     * Four vertical bars plus a line-dependent alternation.  This path does
     * not touch Vector RAM, palette state or LUTs, so it isolates pure
     * DMA/PIO/IRQ timing from guest-video rendering cost.
     */
    const int left = graphics_buffer_shift_x > 0 ? graphics_buffer_shift_x : 0;
    int width = graphics_buffer_width - left;
    if (width < 0)
        width = 0;

    vector_fill32(output, left, 0xc0c0c0c0u);
    output += left;

    const int quarter = width >> 2;
    const uint32_t phase = (y & 16) ? 0x00000000u : 0x3f3f3f3fu;
    vector_fill32(output, quarter, 0xc0c0c0c0u ^ phase);
    output += quarter;
    vector_fill32(output, quarter, 0xd5d5d5d5u ^ phase);
    output += quarter;
    vector_fill32(output, quarter, 0xeaeaeaeau ^ phase);
    output += quarter;
    vector_fill32(output, width - quarter * 3, 0xffffffffu ^ phase);
    return;
#else
    const int source_width = state->show_border ? 626 : 512;
    int left = graphics_buffer_shift_x;
    int source_x = 0;

    if (left < 0) {
        source_x = -left;
        left = 0;
    }
    if (left > graphics_buffer_width)
        left = graphics_buffer_width;

    int drawable = source_width - source_x;
    if (drawable < 0)
        drawable = 0;
    if (drawable > graphics_buffer_width - left)
        drawable = graphics_buffer_width - left;

    vector_fill32(output, left, 0xc0c0c0c0u);
    output += left;
    uint8_t* const drawable_start = output;

    const int n_line = y + (state->show_border ? 24 : 40);
    if (!state->show_border) {
        render_vector_active_pixels(output, source_x, drawable, n_line, state);
    } else if (n_line < 40 || n_line >= 296) {
        vector_fill32(output, drawable,
                      vector_rotate_pattern(state->border_pattern, source_x));
    } else {
        int x = source_x;
        int remaining = drawable;
        if (x < 57) {
            int n = 57 - x;
            if (n > remaining) n = remaining;
            vector_fill32(output, n,
                          vector_rotate_pattern(state->border_pattern, x));
            output += n;
            x += n;
            remaining -= n;
        }
        if (remaining && x < 569) {
            int n = 569 - x;
            if (n > remaining) n = remaining;
            render_vector_active_pixels(output, x - 57, n, n_line, state);
            output += n;
            x += n;
            remaining -= n;
        }
        if (remaining)
            vector_fill32(output, remaining,
                          vector_rotate_pattern(state->border_pattern, x));
    }

    output = drawable_start + drawable;
    vector_fill32(output, graphics_buffer_width - left - drawable,
                  0xc0c0c0c0u);
#endif
}
#endif

void graphics_set_duplicateLines(bool v) {
    duplicateLines = v;
}

#define bitRead(value, bit) (((value) >> (bit)) & 0x01)

void __time_critical_func(dma_handler_VGA)() {
    dma_hw->ints0 = 1u << dma_chan_ctrl;
    static uint32_t frame_number = 0;
    static uint32_t screen_line = 0;
    uint8_t* input_buffer = graphics_buffer;
    screen_line++;

    if (screen_line == N_lines_total) {
        screen_line = 0;
        frame_number++;
    }

    if (screen_line >= N_lines_visible) {
        //заполнение цветом фона
        if (screen_line == N_lines_visible || screen_line == N_lines_visible + 3) {
            uint32_t* output_buffer_32bit = lines_pattern[2 + (screen_line & 1)];
            output_buffer_32bit += shift_picture / 4;
            uint32_t p_i = (screen_line & is_flash_line) + (frame_number & is_flash_frame) & 1;
            uint32_t color32 = bg_color[p_i];
            for (int i = visible_line_size / 2; i--;) {
                *output_buffer_32bit++ = color32;
            }
        }

        //синхросигналы
        if (screen_line >= line_VS_begin && screen_line <= line_VS_end)
            dma_channel_set_read_addr(dma_chan_ctrl, &lines_pattern[1], false); //VS SYNC
        else
            dma_channel_set_read_addr(dma_chan_ctrl, &lines_pattern[0], false);
        return;
    }

    int y, line_number;

    if (duplicateLines) {
        if (screen_line & 1)
            return;
        line_number = screen_line >> 1;
        y = line_number + graphics_buffer_shift_y;
    }
    else {
        line_number = screen_line;
        y = line_number + graphics_buffer_shift_y;
    }

    uint32_t** output_buffer = &lines_pattern[2 + (line_number & 1)];

#ifdef PICO_RP2040
    /*
     * Acquire the frame state before vertical clipping. With the centred
     * Vector image line_number == 0 has a negative y, so returning first
     * skips the only snapshot attempt for the entire VGA frame.
     */
    if (line_number == 0) {
        vector_frame_state_valid =
            vector_video_snapshot(&vector_frame_state);
        if (vector_frame_state_valid)
            vector_lut_in_use = vector_frame_state.lut_index;
    }
#endif

    if (y < 0 || y >= client_buffer_height) {
        // заполнение линии цветом фона
        dma_channel_set_read_addr(dma_chan_ctrl, &lines_pattern[0], false);
        return;
    }

#ifdef PICO_RP2040
#if VECTOR_RP2040_DIAG_TEST_PATTERN
    /*
     * Diagnostic snapshot test.  Do not decode Vector RAM or use the LUT.
     * At the beginning of every VGA frame, try to acquire the state published
     * by core0.  Fill the visible line with:
     *
     *   red   - graphics_set_vector_source() has not published valid state;
     *   green - a valid memory pointer and enabled state were received.
     */
    {
        uint8_t* output_buffer_8bit = (uint8_t*)(*output_buffer);
        output_buffer_8bit += shift_picture;

        uint32_t color;
        switch (vector_video_publish_state) {
        case 2:
            color = vector_frame_state_valid
                    ? 0xccccccccu  /* green: valid snapshot acquired */
                    : 0xffffffffu; /* white: valid source published, snapshot failed */
            break;
        case 1:
            color = 0xc3c3c3c3u;  /* blue: source called with NULL memory */
            break;
        case 3:
            color = 0xf3f3f3f3u;  /* magenta: graphics_set_buffer() disabled it */
            break;
        default:
            color = 0xf0f0f0f0u;  /* red: graphics_set_vector_source() never called */
            break;
        }
        vector_fill32(output_buffer_8bit, graphics_buffer_width, color);

        dma_channel_set_read_addr(dma_chan_ctrl, output_buffer, false);
        return;
    }
#else
    /* The simplified RP2040 model is frame-granular.  Snapshot registers once
       per VGA frame instead of copying the state in every scan-line IRQ. */
    if (vector_frame_state_valid) {
        uint8_t* output_buffer_8bit = (uint8_t*)(*output_buffer);
        output_buffer_8bit += shift_picture;
        render_vector_vga_line(output_buffer_8bit, y, &vector_frame_state);
        dma_channel_set_read_addr(dma_chan_ctrl, output_buffer, false);
        return;
    }
#endif
#endif

    if (!input_buffer) {
        dma_channel_set_read_addr(dma_chan_ctrl, &lines_pattern[0], false);
        return;
    }

    //зона прорисовки изображения
    //начальные точки буферов
    uint8_t* input_buffer_8bit = input_buffer + y * client_buffer_stride;

    uint16_t* output_buffer_16bit = (uint16_t *)(*output_buffer);
    output_buffer_16bit += shift_picture >> 1; //смещение началы вывода на размер синхросигнала

    uint8_t* output_buffer_8bit = (uint8_t*)output_buffer_16bit;
    int width = client_buffer_width;
    bool duplicatePixels = false;
    if (width <= (graphics_buffer_width >> 1)) {
        width *= 2;
        duplicatePixels = true;
    }
    int xoff1 = graphics_buffer_shift_x;
    int xoff2 = graphics_buffer_width - width - xoff1;
    if (xoff1 > graphics_buffer_width) xoff1 = graphics_buffer_width;
    if (xoff2 > graphics_buffer_width) xoff2 = graphics_buffer_width;
    if (xoff2 < 0) xoff2 = 0;
    for  (register int x = 0; x < xoff1; ++x) {
        *output_buffer_8bit++ = 0xC0;
    }
    if (duplicatePixels) {
        for  (register int x = xoff1 < 0 ? -xoff1 / 2 : 0; x < width / 2; ++x) {
            register uint8_t c = input_buffer_8bit[x] | 0xC0;
            *output_buffer_8bit++ = c;
            *output_buffer_8bit++ = c;
        }
    } else {
        for  (register int x = xoff1 < 0 ? -xoff1 : 0; x < width; ++x) {
            *output_buffer_8bit++ = input_buffer_8bit[x] | 0xC0;
        }
    }
    for  (register int x = 0; x < xoff2; ++x) {
        *output_buffer_8bit++ = 0xC0;
    }
    dma_channel_set_read_addr(dma_chan_ctrl, output_buffer, false);
}

static void adjust_shift_x() {
    if (client_buffer_width * 2 < graphics_buffer_width)
        graphics_buffer_shift_x = (graphics_buffer_width - (client_buffer_width << 1)) >> 1;
    else
        graphics_buffer_shift_x = (graphics_buffer_width - client_buffer_width) >> 1;
}
static void adjust_shift_y() {
    if (duplicateLines)
        graphics_buffer_shift_y = (client_buffer_height - (graphics_buffer_height >> 1)) >> 1;
    else
        graphics_buffer_shift_y = (client_buffer_height - graphics_buffer_height) >> 1;
}

void __not_in_flash_func(vga_system_clock_changed)() {
    if (_SM_VGA < 0)
        return;
    const double divider = clock_get_hz(clk_sys) / 40000000.0;
    const uint32_t div32 = (uint32_t)(divider * (1u << 16));
    PIO_VGA->sm[_SM_VGA].clkdiv = div32 & 0xfffff000;
}

void graphics_set_mode() {
    if (_SM_VGA < 0) return; // если  VGA не инициализирована -

    // Если мы уже проиницилизированы - выходим
    if (lines_pattern_data) {
        return;
    };
    uint8_t TMPL_VHS8 = 0;
    uint8_t TMPL_VS8 = 0;
    uint8_t TMPL_HS8 = 0;
    uint8_t TMPL_LINE8 = 0;

    int line_size;
    double fdiv = 100;
    int HS_SIZE = 4;
    int HS_SHIFT = 100;

    graphics_buffer_width = 800;
    graphics_buffer_height = 600;
    TMPL_LINE8 = 0b11000000;
    // SVGA Signal 800 x 600 @ 60 Hz timing
    HS_SHIFT = 800 + 40; // Front porch + Visible area
    HS_SIZE = 88; // Back porch
    line_size = 1056;
    shift_picture = line_size - HS_SHIFT;
    visible_line_size = 800 / 2;
    N_lines_visible = 16 * 37; // 592 < 600
    line_VS_begin = 600 + 1; // + Front porch
    line_VS_end = 600 + 3 + 4; // ++ Sync pulse 2?
    N_lines_total = 628; // Whole frame
    fdiv = clock_get_hz(clk_sys) / 40000000;  // частота пиксельклока 40.0 MHz
    adjust_shift_x();
    adjust_shift_y();

    //корректировка  палитры по маске бит синхры
    bg_color[0] = bg_color[0] & 0x3f3f3f3f | palette16_mask | palette16_mask << 16;
    bg_color[1] = bg_color[1] & 0x3f3f3f3f | palette16_mask | palette16_mask << 16;
///    for (int i = 0; i < 256; i++) {
///        palette[0][i] = palette[0][i] & 0x3f3f | palette16_mask;
///        palette[1][i] = palette[1][i] & 0x3f3f | palette16_mask;
///    }

    //инициализация шаблонов строк и синхросигнала
    if (lines_pattern_data) free(lines_pattern_data);
   // if (!lines_pattern_data) //выделение памяти, если не выделено
    {
        const uint32_t div32 = (uint32_t)(fdiv * (1 << 16) + 0.0);
        PIO_VGA->sm[_SM_VGA].clkdiv = div32 & 0xfffff000; //делитель для конкретной sm
        dma_channel_set_trans_count(dma_chan, line_size / 4, false);

        lines_pattern_data = (uint32_t *)calloc(line_size * 4 / 4, sizeof(uint32_t));

        for (int i = 0; i < 4; i++) {
            lines_pattern[i] = &lines_pattern_data[i * (line_size / 4)];
        }
        // memset(lines_pattern_data,N_TMPLS*1200,0);
        TMPL_VHS8 = TMPL_LINE8 ^ 0b11000000;
        TMPL_VS8 = TMPL_LINE8 ^ 0b10000000;
        TMPL_HS8 = TMPL_LINE8 ^ 0b01000000;

        uint8_t* base_ptr = (uint8_t *)lines_pattern[0];
        //пустая строка
        memset(base_ptr, TMPL_LINE8, line_size);
        //memset(base_ptr+HS_SHIFT,TMPL_HS8,HS_SIZE);
        //выровненная синхра вначале
        memset(base_ptr, TMPL_HS8, HS_SIZE);

        // кадровая синхра
        base_ptr = (uint8_t *)lines_pattern[1];
        memset(base_ptr, TMPL_VS8, line_size);
        //memset(base_ptr+HS_SHIFT,TMPL_VHS8,HS_SIZE);
        //выровненная синхра вначале
        memset(base_ptr, TMPL_VHS8, HS_SIZE);

        //заготовки для строк с изображением
        base_ptr = (uint8_t *)lines_pattern[2];
        memcpy(base_ptr, lines_pattern[0], line_size);
        base_ptr = (uint8_t *)lines_pattern[3];
        memcpy(base_ptr, lines_pattern[0], line_size);
    }
}

void graphics_set_buffer(uint8_t* buffer, const uint16_t width, const uint16_t height) {
#ifdef PICO_RP2040
    vector_video_seq++;
    vector_video_barrier();
    vector_video_state.enabled = false;
    vector_video_publish_state = 3;
    vector_video_barrier();
    vector_video_seq++;
#endif
    graphics_buffer = buffer;
    client_buffer_stride = width;   // по умолчанию шаг строки равен ширине
    if (client_buffer_width != width) {
        client_buffer_width = width;
        adjust_shift_x();
    }
    if (client_buffer_height != height) {
        client_buffer_height = height;
        adjust_shift_y();
    }
}

#ifdef PICO_RP2040
void graphics_set_vector_source(const uint8_t* memory, const uint8_t* palette,
                                uint8_t border_color, uint8_t line_offset,
                                bool mode512, bool show_border) {
    vector_video_state_t next = vector_video_state;
    next.memory = memory;
    memcpy(next.palette, palette, sizeof(next.palette));
    next.border_color = border_color & 0x0f;
    next.line_offset = line_offset;
    next.mode512 = mode512;
    next.show_border = show_border;
    next.enabled = memory != NULL;
    vector_video_publish_state = next.enabled ? 2 : 1;

    /* Pick the LUT which is neither consumed by core1 nor currently published.
       With three copies one slot is always available. */
    uint8_t lut_index = 0;
    while (lut_index == vector_lut_in_use ||
           lut_index == vector_video_state.lut_index)
        ++lut_index;
    next.lut_index = lut_index;
    vector_build_lut(&next, vector_pair_lut[lut_index]);

    vector_video_seq++;
    vector_video_barrier();
    vector_video_state = next;
    vector_video_barrier();
    vector_video_seq++;

    graphics_buffer = NULL;
    const int width = show_border ? 626 : 512;
    const int height = show_border ? 288 : 256;
    if (client_buffer_width != width) {
        client_buffer_width = width;
        adjust_shift_x();
    }
    if (client_buffer_height != height) {
        client_buffer_height = height;
        adjust_shift_y();
    }
}
#endif

void graphics_set_line_stride(uint16_t stride) {
    client_buffer_stride = stride;
}

uint16_t graphics_get_line_stride(void) {
    return (uint16_t)client_buffer_stride;
}

void graphics_inc_x(void) {
    graphics_buffer_shift_x++;
}

void graphics_dec_x(void) {
    graphics_buffer_shift_x--;
}

void graphics_inc_y(void) {
    graphics_buffer_shift_y++;
}

void graphics_dec_y(void) {
    graphics_buffer_shift_y--;
}

void graphics_set_offset(const int x, const int y) {
    graphics_buffer_shift_x = x;
    graphics_buffer_shift_y = y;
}

void graphics_set_flashmode(const bool flash_line, const bool flash_frame) {
    is_flash_frame = flash_frame;
    is_flash_line = flash_line;
}

void graphics_set_bgcolor(const uint32_t color888) {
    const uint8_t conv0[] = { 0b00, 0b00, 0b01, 0b10, 0b10, 0b10, 0b11, 0b11 };
    const uint8_t conv1[] = { 0b00, 0b01, 0b01, 0b01, 0b10, 0b11, 0b11, 0b11 };

    const uint8_t b = (color888 & 0xff) / 42;

    const uint8_t r = (color888 >> 16 & 0xff) / 42;
    const uint8_t g = (color888 >> 8 & 0xff) / 42;

    const uint8_t c_hi = conv0[r] << 4 | conv0[g] << 2 | conv0[b];
    const uint8_t c_lo = conv1[r] << 4 | conv1[g] << 2 | conv1[b];
    bg_color[0] = ((c_hi << 8 | c_lo) & 0x3f3f | palette16_mask) << 16 |
                  ((c_hi << 8 | c_lo) & 0x3f3f | palette16_mask);
    bg_color[1] = ((c_lo << 8 | c_hi) & 0x3f3f | palette16_mask) << 16 |
                  ((c_lo << 8 | c_hi) & 0x3f3f | palette16_mask);
}

void graphics_set_palette(const uint8_t i, const uint32_t color888) {
    /**
    const uint8_t conv0[] = { 0b00, 0b00, 0b01, 0b10, 0b10, 0b10, 0b11, 0b11 };
    const uint8_t conv1[] = { 0b00, 0b01, 0b01, 0b01, 0b10, 0b11, 0b11, 0b11 };

    const uint8_t b = (color888 & 0xff) / 42;

    const uint8_t r = (color888 >> 16 & 0xff) / 42;
    const uint8_t g = (color888 >> 8 & 0xff) / 42;

    const uint8_t c_hi = conv0[r] << 4 | conv0[g] << 2 | conv0[b];
    const uint8_t c_lo = conv1[r] << 4 | conv1[g] << 2 | conv1[b];

    palette[0][i] = (c_hi << 8 | c_lo) & 0x3f3f | palette16_mask;
    palette[1][i] = (c_lo << 8 | c_hi) & 0x3f3f | palette16_mask;
    */
}

void graphics_init() {
    //инициализация PIO
    //загрузка программы в один из PIO
    const uint offset = pio_add_program(PIO_VGA, &pio_program_VGA);
    _SM_VGA = pio_claim_unused_sm(PIO_VGA, true);
    const uint sm = _SM_VGA;

    for (int i = 0; i < 8; i++) {
        gpio_init(VGA_BASE_PIN + i);
        gpio_set_dir(VGA_BASE_PIN + i, GPIO_OUT);
        pio_gpio_init(PIO_VGA, VGA_BASE_PIN + i);
    }; //резервируем под выход PIO

    pio_sm_set_consecutive_pindirs(PIO_VGA, sm, VGA_BASE_PIN, 8, true); //конфигурация пинов на выход

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + 0, offset + (pio_program_VGA.length - 1));

    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX); //увеличение буфера TX за счёт RX до 8-ми
    sm_config_set_out_shift(&c, true, true, 32);
    sm_config_set_out_pins(&c, VGA_BASE_PIN, 8);
    pio_sm_init(PIO_VGA, sm, offset, &c);

    pio_sm_set_enabled(PIO_VGA, sm, true);

    //инициализация DMA
    dma_chan_ctrl = dma_claim_unused_channel(true);
    dma_chan = dma_claim_unused_channel(true);
    //основной ДМА канал для данных
    dma_channel_config c0 = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&c0, DMA_SIZE_32);

    channel_config_set_read_increment(&c0, true);
    channel_config_set_write_increment(&c0, false);

    uint dreq = DREQ_PIO1_TX0 + sm;
    if (PIO_VGA == pio0) dreq = DREQ_PIO0_TX0 + sm;

    channel_config_set_dreq(&c0, dreq);
    channel_config_set_chain_to(&c0, dma_chan_ctrl); // chain to other channel

    dma_channel_configure(
        dma_chan,
        &c0,
        &PIO_VGA->txf[sm], // Write address
        lines_pattern[0], // read address
        600 / 4, //
        false // Don't start yet
    );
    //канал DMA для контроля основного канала
    dma_channel_config c1 = dma_channel_get_default_config(dma_chan_ctrl);
    channel_config_set_transfer_data_size(&c1, DMA_SIZE_32);

    channel_config_set_read_increment(&c1, false);
    channel_config_set_write_increment(&c1, false);
    channel_config_set_chain_to(&c1, dma_chan); // chain to other channel

    dma_channel_configure(
        dma_chan_ctrl,
        &c1,
        &dma_hw->ch[dma_chan].read_addr, // Write address
        &lines_pattern[0], // read address
        1, //
        false // Don't start yet
    );

    irq_set_exclusive_handler(VGA_DMA_IRQ, dma_handler_VGA);
    graphics_set_mode();

    /*
     * dma_channel_configure() above captured lines_pattern[0] while the
     * scan-line buffers were still unallocated (NULL).  graphics_set_mode()
     * allocates and initializes them, so update the main DMA read address
     * before starting the channel.  Otherwise DMA starts reading address 0
     * and the PIO never receives a valid VGA line.
     */
    dma_channel_set_read_addr(dma_chan, lines_pattern[0], false);

    dma_channel_set_irq0_enabled(dma_chan_ctrl, true);
    irq_set_enabled(VGA_DMA_IRQ, true);
    dma_start_channel_mask(1u << dma_chan);
}

uint32_t graphics_get_width() {
    return client_buffer_width;
}
// Строка экрана n показывает строку буфера n + shift_y, то есть рост shift_y
// поднимает картинку вверх — наружу отдаём смещение вниз, с обратным знаком.
int graphics_get_picture_shift_y() {
    return -graphics_buffer_shift_y;
}

int graphics_get_picture_shift_x() {
    return graphics_buffer_shift_x;
}

// У VGA виден весь буфер
uint32_t graphics_get_visible_height() {
    return graphics_get_height();
}

uint32_t graphics_get_height() {
    return client_buffer_height;
}
uint8_t* graphics_get_frame() {
    return graphics_buffer;
}
uint32_t graphics_get_font_width() {
    return 8;
}
uint32_t graphics_get_font_height() {
    return 8;
}

#define bitSet(value, bit) ((value) |= (1UL << (bit)))
#define bitClear(value, bit) ((value) &= ~(1UL << (bit)))
#define bitWrite(value, bit, bitvalue) ((bitvalue) ? bitSet(value, bit) : bitClear(value, bit))

inline static void _plot(int32_t x, int32_t y, uint32_t w, uint32_t h, uint8_t color) {
    if (!graphics_buffer) return;
    if (x < 0 || x >= w) return;
    if (y < 0 || y >= h) return;
    graphics_buffer[client_buffer_stride * y + x] = color;
}

void plot(int x, int y, uint8_t color) {
    _plot(x, y, client_buffer_width, client_buffer_height, color);
}

void line(int x0, int y0, int x1, int y1, uint8_t color) {
    uint32_t w = graphics_get_width();
    uint32_t h = graphics_get_height();
    if (x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y1 == y0) {
        for (int xi = x0; xi <= x1; ++xi) {
            _plot(xi, y0, w, h, color);
        }
        return;
    }
    if (y0 > y1) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    if (x1 == x0) {
        for (int yi = y0; yi <= y1; ++yi) {
            _plot(x0, yi, w, h, color);
        }
        return;
    }
    int dx = x1 - x0;
    int dy = y1 - y0;
    if (dx > dy) {
        double dydx = dy / (dx * 1.0);
        for (int xi = x0; xi <= x1; ++xi) {
            int yi = y0 + dydx * (xi - x0);
            _plot(xi, yi, w, h, color);
        }
        return;
    }
    double dxdy = dx / (dy * 1.0);
    for (int yi = y0; yi <= y1; ++yi) {
        int xi = x0 + dxdy * (yi - y0);
        _plot(xi, yi, w, h, color);
    }
}

void graphics_rect(int32_t x0, int32_t y0, uint32_t width, uint32_t height, uint8_t color) {
    int32_t x1 = x0 + width;
    int32_t y1 = y0 + height;
    line(x0, y0, x1, y0, color);
    line(x1, y0, x1, y1, color);
    line(x1, y1, x0, y1, color);
    line(x0, y1, x0, y0, color);
}

void graphics_fill(int32_t x0, int32_t y0, uint32_t width, uint32_t height, uint8_t bgcolor) {
    int32_t x1 = x0 + width;
    int32_t y1 = y0 + height;
    for(int xi = x0; xi <= x1; ++xi) {
        line(xi, y0, xi, y1, bgcolor);
    }
}

void graphics_type(int x, int y, uint8_t color, const char* msg, size_t msg_len) {
    for (size_t i = 0; i < msg_len; ++i) {
        char ch = msg[i];
        const uint8_t* pf_line0 = font_8x8 + ch*8;
        uint32_t xt = x + i * graphics_get_font_width();
        for (size_t j = 0; j < graphics_get_font_height(); ++j) {
            uint32_t yt = y + j;
            uint8_t chl = pf_line0[j];
            for (uint8_t z = 0; z < 8; ++z) {
                if ((chl >> z) & 1) plot(xt + z, yt, color);
            }
        }
    }
}
