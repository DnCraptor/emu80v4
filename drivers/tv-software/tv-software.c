#
//программный композит
#include <stdio.h>
#include "graphics.h"
#include "font8x8.h"
#include "font8x16.h"
#include "hardware/clocks.h"
#include <stdalign.h>

#include "hardware/structs/pll.h"
#include "hardware/structs/systick.h"

#include "hardware/dma.h"
#include "hardware/irq.h"
#include <string.h>
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "stdlib.h"

// Выход композита использует тот же резисторный ЦАП, что и VGA, поэтому база
// берётся из заголовка платы. В наброске она была жёстко задана как 6, что на
// murmulator2 попадает на SPI SD-карты (6, 7) и на аудио (9, 10), а самого
// видео-ЦАП (12..19) касается лишь краем — картинки при этом нет.
// Заголовок платы подключается через pico/stdlib.h, то есть уже виден здесь.
#if defined(VGA_BASE_PIN)
#undef TV_BASE_PIN
#define TV_BASE_PIN VGA_BASE_PIN
#endif

// graphics_set_palette() принимает 24-битный цвет и разбирает его на R, G, B
// сдвигами. Общий RGB888 из graphics.h — это, наоборот, упаковка в 6 бит
// (по два на канал): именно такие байты эмулятор кладёт в кадровый буфер.
// Одно имя, два разных смысла. Если задавать палитру драйвера общим макросом,
// R и G всегда оказываются нулями и все 256 цветов становятся чёрными —
// синхронизация есть, картинки нет. Поэтому у палитры драйвера свой макрос.
#define TV_RGB24(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

// Начало области палитры под все 64 кода кадрового буфера. Индексы 64..127
// свободны: по умолчанию там серая шкала, а именованные цвета лежат в 200..216.
#define TV_PAL_BASE 64

#define c_ntscShiftY 3
#pragma GCC optimize("Ofast")


//пины
//пин синхросигнала(для совместимости с RGB по ч.б.) 0-7
#define SYNC_PIN (6)
//максимальное значение DAC
#define MAX_DAC (63)
//перераспределение порядка пинов(для совместимости с RGB по ч.б.)
#define CONV_DAC(x) ((((x)<<2)&0x30)|(((x)>>2)&0x0c)|((x)&0x03))

//буферы строк
//количество буферов задавать кратно степени двойки
//например 2^2=4 буфера
#define N_LINE_BUF_log2 (2)


#define N_LINE_BUF_DMA (1<<N_LINE_BUF_log2)
#define N_LINE_BUF (N_LINE_BUF_DMA)

//максимальный размер строки(кратно 4)
#define LINE_SIZE_MAX (1152)
//указатели на буферы строк
//выравнивание нужно для кольцевого буфера
static uint32_t rd_addr_DMA_CTRL[N_LINE_BUF * 2]__attribute__ ((aligned (4*N_LINE_BUF_DMA)));
static uint32_t transfer_count_DMA_CTRL[N_LINE_BUF * 2]__attribute__ ((aligned (4*N_LINE_BUF_DMA)));
//непосредственно буферы строк

static uint32_t lines_buf[N_LINE_BUF][LINE_SIZE_MAX / 4];


static int SM_video = -1;


//DMA каналы
//каналы работы с первичным графическим буфером
static int dma_chan_ctrl = -1;
static int dma_chan_ctrl2 = -1;
static int dma_chan = -1;


//ДМА палитра для конвертации(256 знач)
static uint32_t conv_colorNORM[2][256]; //2к
static uint32_t conv_colorINV[2][256]; //2к
static uint32_t* conv_color[2];

//палитра сохранённая
static uint8_t __scratch_x("buff4") paletteRGB[3][256]; //768 байт

static repeating_timer_t video_timer;

#ifdef PICO_RP2040
#define MENU_TEXT_COLS 50
#define MENU_TEXT_ROWS 18
#define MENU_TEXT_CELL_W 16
#define MENU_TEXT_CELL_H 16
#define MENU_TEXT_GLYPH_W 8
#define MENU_TEXT_GLYPH_H 8
#define MENU_TEXT_SOURCE_W (MENU_TEXT_COLS * MENU_TEXT_GLYPH_W)
#define MENU_TEXT_TRANSPARENT_ATTR 0xeeu
#define MENU_TEXT_TRANSPARENT_CELL \
    ((uint16_t)' ' | ((uint16_t)MENU_TEXT_TRANSPARENT_ATTR << 8))

/*
 * This is the same logical text surface used by vga-nextgen:
 * one 16-bit cell, character in the low byte and VGA-style attribute in the
 * high byte. Only the final scanline conversion differs for composite TV.
 */
static uint16_t menu_text_cells[MENU_TEXT_COLS * MENU_TEXT_ROWS];
static uint8_t menu_font_8x8_sram[256 * MENU_TEXT_GLYPH_H];

/*
 * Number of composite samples emitted for each doubled logical glyph pixel.
 * The plan is rebuilt only when PAL/NTSC changes the active output width.
 */
static uint8_t menu_text_runs[MENU_TEXT_SOURCE_W];
static int menu_text_runs_width = -1;

static inline uint8_t menu_color_index(uint8_t color)
{
    static const uint8_t __not_in_flash("rgb16") rgb16[16][3] = {
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

static inline void menu_text_put_cell(int x, int y, uint8_t ch, uint8_t attr)
{
    if ((unsigned)x >= MENU_TEXT_COLS || (unsigned)y >= MENU_TEXT_ROWS)
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

/*
 * TV menu geometry is deliberately coarser than VGA:
 * 50x18 characters, font 8x8, every glyph pixel doubled horizontally and
 * every glyph scanline doubled vertically. The public cell is therefore
 * 16x16, while the hot renderer only processes 400 source pixels per line.
 */
static void menu_text_build_runs(int output_width)
{
    for (unsigned i = 0; i < MENU_TEXT_SOURCE_W; ++i)
        menu_text_runs[i] = 0;

    if (output_width <= 0)
        return;

    int accumulator = 0;
    unsigned source_x = 0;
    for (int out_x = 0;
         out_x < output_width && source_x < MENU_TEXT_SOURCE_W;
         ++out_x) {
        ++menu_text_runs[source_x];
        accumulator += MENU_TEXT_SOURCE_W;
        if (accumulator >= output_width) {
            accumulator -= output_width;
            ++source_x;
        }
    }
    menu_text_runs_width = output_width;
}

static inline __attribute__((always_inline))
void menu_text_emit_run(uint8_t** output_ptr, uint32_t waveform,
                        unsigned* phase_ptr, unsigned run)
{
    uint8_t* output = *output_ptr;
    unsigned phase = *phase_ptr;

    if (run >= 1) {
        *output++ =
            (uint8_t)(waveform >> ((phase & 3u) * 8u));
        ++phase;
    }
    if (run >= 2) {
        *output++ =
            (uint8_t)(waveform >> ((phase & 3u) * 8u));
        ++phase;
    }
    if (run >= 3) {
        *output++ =
            (uint8_t)(waveform >> ((phase & 3u) * 8u));
        ++phase;
    }

    *output_ptr = output;
    *phase_ptr = phase;
}

static void __not_in_flash_func(render_menu_text_line_tv)(
        uint8_t* output, int raster_y, int raster_height,
        int output_width, int li)
{
    if (output_width <= 0 || raster_height <= 0)
        return;

    if (menu_text_runs_width != output_width)
        menu_text_build_runs(output_width);

    /*
     * PAL: 18 * 8 * 2 = 288 physical lines exactly.
     * NTSC: rescale the same 288-line logical surface to the available raster.
     */
    unsigned logical_y =
        ((uint32_t)raster_y *
         (MENU_TEXT_ROWS * MENU_TEXT_CELL_H)) /
        (uint32_t)raster_height;
    if (logical_y >= MENU_TEXT_ROWS * MENU_TEXT_CELL_H)
        logical_y = MENU_TEXT_ROWS * MENU_TEXT_CELL_H - 1;

    const unsigned row = logical_y >> 4;
    const unsigned glyph_line = (logical_y >> 1) & 7u;
    const uint16_t* cells =
        &menu_text_cells[row * MENU_TEXT_COLS];

    unsigned source_x = 0;
    unsigned phase = 0;

    for (unsigned cell_x = 0;
         cell_x < MENU_TEXT_COLS;
         ++cell_x) {
        const uint16_t cell = cells[cell_x];
        const uint8_t glyph =
            menu_font_8x8_sram[
                ((uint8_t)cell << 3) + glyph_line];
        const uint8_t attr = (uint8_t)(cell >> 8);
        const uint32_t fg =
            conv_color[li][200u + (attr & 0x0fu)];
        const uint32_t bg =
            conv_color[li][200u + (attr >> 4)];

        for (unsigned bit = 0; bit < 8; ++bit, ++source_x) {
            const uint32_t waveform =
                (glyph & (1u << bit)) ? fg : bg;
            menu_text_emit_run(
                &output, waveform, &phase,
                menu_text_runs[source_x]);
        }
    }
}
#else
void menu_text_clear_for_mode(void) {}
#endif

void graphics_set_video_content_mode(graphics_video_content_mode_t mode)
{
    if (mode < GRAPHICS_VIDEO_VECTOR || mode > GRAPHICS_VIDEO_COMBINED)
        mode = GRAPHICS_VIDEO_VECTOR;

    __asm volatile ("" ::: "memory");
    menu_video_mode = mode;
    menu_text_clear_for_mode();
    __asm volatile ("" ::: "memory");
}

/*
 * Горячий тракт композитного видео не должен вызывать libc из XIP flash.
 * Все используемые адреса строковых буферов выровнены как минимум на 4 байта,
 * но helper сохраняет корректность и для коротких невыровненных участков
 * синхронизации/цветовой вспышки.
 */
static inline __attribute__((always_inline))
void tv_fill_u8(void* dst_void, int value, size_t count)
{
    uint8_t* dst = (uint8_t*)dst_void;
    const uint8_t byte = (uint8_t)value;

    while (count && ((uintptr_t)dst & 3u)) {
        *dst++ = byte;
        --count;
    }

    const uint32_t word = (uint32_t)byte * 0x01010101u;
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
        *dst++ = byte;
}

static inline __attribute__((always_inline))
void tv_copy_u8(void* dst_void, const void* src_void, size_t count)
{
    uint8_t* dst = (uint8_t*)dst_void;
    const uint8_t* src = (const uint8_t*)src_void;

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

static uint8_t map64colors[64] = { 0 };

//параметры по умолчанию
// Вектор-06Ц выдаёт PAL, и кадровый буфер эмулятора повторяет его развёртку,
// поэтому PAL здесь основной режим. NTSC остаётся собираемым по ключу TV_NTSC.
tv_out_mode_t tv_out_mode = {
#if defined(TV_NTSC)
    .tv_system = g_TV_OUT_NTSC,
    .N_lines = _524_lines,
    .mode_bpp = GRAPHICSMODE_DEFAULT,
    .c_freq = _3579545,
#else
    .tv_system = g_TV_OUT_PAL,
    .N_lines = _624_lines,
    .mode_bpp = GRAPHICSMODE_DEFAULT,
    .c_freq = _4433619,
#endif
    .color_index = 1.0, //0-1
    .cb_sync_PI_shift_lines = false,
    .cb_sync_PI_shift_half_frame = true
};

//параметры по умолчанию
tv_out_mode_t tv_out_mode1 = {
    .tv_system = g_TV_OUT_PAL,
    .N_lines = _624_lines,
    .mode_bpp = GRAPHICSMODE_DEFAULT,
    .c_freq = _4433619,
    .color_index = 1.0, //0-1
    .cb_sync_PI_shift_lines = false,
    .cb_sync_PI_shift_half_frame = true
};

//программы PIO
//программа видеовывода
static uint16_t pio_program_TV_instructions[] = {
    //	 .wrap_target

    //	 .wrap_target
    0x6008, //  0: out	pins, 8
    //	 .wrap
    //	 .wrap
};

static const struct pio_program program_pio_TV = {
    .instructions = pio_program_TV_instructions,
    .length = 1,
    .origin = -1,
};

typedef struct TV_MODE {
    int H_len;
    int begin_img_shx;
    int img_W;

    int N_lines;

    int sync_size;
    uint8_t SYNC_TMPL;
    uint8_t NO_SYNC_TMPL;
    uint8_t LVL_C_MAX;
    uint8_t LVL_BLACK;
    uint8_t LVL_BLACK_TMPL;
    uint8_t LVL_Y_MAX;
} TV_MODE;

typedef struct G_BUFFER {
    uint width;
    uint height;
    uint stride;   // физический шаг строки; по умолчанию равен width
    int shift_x;
    int shift_y;
} G_BUFFER;


//режим видеовыхода
static TV_MODE video_mode = {
    .H_len = 512,
    .N_lines = 525,
    .SYNC_TMPL = 0,
    .NO_SYNC_TMPL = 0,
    .LVL_C_MAX = 0,
    .LVL_BLACK = 0,
    .LVL_Y_MAX = 0,
};


static G_BUFFER graphics_buffer = {
    .shift_x = 0,
    .shift_y = 0,
    .height = 288,   // PAL: столько строк рисует эмулятор
    .width = 626,
    .stride = 626
};

// Указатель на кадровый буфер. Прежний набросок его не сохранял вовсе и брал
// строки через внешний getLineBuffer() из порта ZX Spectrum.
static uint8_t* graphics_framebuffer = NULL;

#ifdef PICO_RP2040
#define VECTOR_TV_LUT_COUNT 3
#define VECTOR_TV_MAX_WIDTH 626

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
} vector_tv_state_t;

static vector_tv_state_t vector_tv_state;
static volatile uint32_t vector_tv_seq = 0;
static vector_tv_state_t vector_tv_frame_state;
static bool vector_tv_frame_state_valid = false;
static uint32_t vector_tv_pair_lut[VECTOR_TV_LUT_COUNT][256];
static volatile uint8_t vector_tv_lut_in_use = 0xff;

/*
 * Final composite samples for four Vector pixels. Variants:
 * 0/1 = normal PAL/NTSC phase tables, 2/3 = inverted phase tables.
 * Built on core0 together with the palette LUT, read from SRAM by DMA IRQ.
 */
static uint32_t vector_tv_composite_lut[4][256][4];

/*
 * Horizontal scaler plan. Each entry tells how many composite samples to emit
 * for one source pixel. It replaces the per-output fixed-point accumulator in
 * the hot scanline loop.
 */
static uint8_t vector_tv_runs[VECTOR_TV_MAX_WIDTH];
static int vector_tv_runs_source_width = -1;
static int vector_tv_runs_output_width = -1;

static inline __attribute__((always_inline))
void vector_tv_barrier(void)
{
    __asm volatile ("" ::: "memory");
}

static bool __not_in_flash_func(vector_tv_snapshot)(
        vector_tv_state_t* state)
{
    for (;;) {
        const uint32_t seq0 = vector_tv_seq;
        if (seq0 & 1u)
            continue;
        vector_tv_barrier();
        *state = vector_tv_state;
        vector_tv_barrier();
        const uint32_t seq1 = vector_tv_seq;
        if (seq0 == seq1 && !(seq1 & 1u))
            return state->enabled && state->memory;
    }
}

static inline __attribute__((always_inline))
uint8_t vector_tv_color(
        const vector_tv_state_t* state, uint8_t color)
{
    return state->palette[color];
}

static void vector_tv_build_lut(
        vector_tv_state_t* state, uint32_t* lut)
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
            p0 = vector_tv_color(state, c0 & 0x03u);
            p1 = vector_tv_color(state, c0 & 0x0cu);
            p2 = vector_tv_color(state, c1 & 0x03u);
            p3 = vector_tv_color(state, c1 & 0x0cu);
        } else {
            p0 = p1 = vector_tv_color(state, c0);
            p2 = p3 = vector_tv_color(state, c1);
        }

        lut[i] = (uint32_t)p0 | ((uint32_t)p1 << 8) |
                 ((uint32_t)p2 << 16) | ((uint32_t)p3 << 24);
    }

    const uint8_t border = state->border_color;
    if (state->mode512) {
        const uint8_t p0 =
            vector_tv_color(state, border & 0x03u);
        const uint8_t p1 =
            vector_tv_color(state, border & 0x0cu);
        state->border_pattern =
            (uint32_t)p0 | ((uint32_t)p1 << 8) |
            ((uint32_t)p0 << 16) | ((uint32_t)p1 << 24);
    } else {
        const uint8_t p = vector_tv_color(state, border);
        state->border_pattern = (uint32_t)p * 0x01010101u;
    }
}

static void vector_tv_build_composite_lut(
        const uint32_t* pixel_lut)
{
    const uint32_t* tables[4] = {
        conv_colorNORM[0], conv_colorNORM[1],
        conv_colorINV[0],  conv_colorINV[1]
    };

    for (unsigned index = 0; index < 256; ++index) {
        const uint32_t pixels = pixel_lut[index];
        for (unsigned variant = 0; variant < 4; ++variant) {
            const uint32_t* table = tables[variant];
            vector_tv_composite_lut[variant][index][0] =
                table[map64colors[(uint8_t)pixels & 0x3fu]];
            vector_tv_composite_lut[variant][index][1] =
                table[map64colors[(uint8_t)(pixels >> 8) & 0x3fu]];
            vector_tv_composite_lut[variant][index][2] =
                table[map64colors[(uint8_t)(pixels >> 16) & 0x3fu]];
            vector_tv_composite_lut[variant][index][3] =
                table[map64colors[(uint8_t)(pixels >> 24) & 0x3fu]];
        }
    }
}

static inline __attribute__((always_inline))
uint32_t vector_tv_rotate_pattern(uint32_t pattern, unsigned bytes)
{
    bytes &= 3u;
    return bytes ? (pattern >> (bytes * 8u)) |
                   (pattern << ((4u - bytes) * 8u)) : pattern;
}

static inline __attribute__((always_inline))
void vector_tv_fill_pattern(
        uint8_t* dst, int count, uint32_t pattern)
{
    while (count && ((uintptr_t)dst & 3u)) {
        *dst++ = (uint8_t)pattern;
        pattern = vector_tv_rotate_pattern(pattern, 1);
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
        pattern = vector_tv_rotate_pattern(pattern, 1);
    }
}

static inline __attribute__((always_inline))
uint32_t vector_tv_plane_indices4(
        const vector_tv_state_t* state,
        int active_x, int n_line)
{
    /*
     * One byte from every Vector plane contains all 16 pixels of the source
     * block. Decode its four 4-pixel groups at once. The previous code called
     * vector_tv_plane_index() four times and re-read the same four SRAM bytes
     * on every call.
     */
    const uint8_t roll_off =
        (uint8_t)(state->line_offset - n_line + 40);
    const int offset = ((active_x & 0x1f0) << 4) | roll_off;
    const uint8_t* memory = state->memory;
    const unsigned by = memory[0x8000 + offset];
    const unsigned br = memory[0xa000 + offset];
    const unsigned bg = memory[0xc000 + offset];
    const unsigned bb = memory[0xe000 + offset];

#define VECTOR_TV_INDEX_AT(shift) \
    (((((by >> (shift)) & 3u) << 6) | \
      (((br >> (shift)) & 3u) << 4) | \
      (((bg >> (shift)) & 3u) << 2) | \
       ((bb >> (shift)) & 3u)))

    const uint32_t result =
        (uint32_t)VECTOR_TV_INDEX_AT(6) |
        ((uint32_t)VECTOR_TV_INDEX_AT(4) << 8) |
        ((uint32_t)VECTOR_TV_INDEX_AT(2) << 16) |
        ((uint32_t)VECTOR_TV_INDEX_AT(0) << 24);

#undef VECTOR_TV_INDEX_AT
    return result;
}

static inline __attribute__((always_inline))
void vector_tv_emit_run(
        uint8_t** dst_ptr, uint32_t waveform,
        unsigned* phase_ptr, unsigned run)
{
    uint8_t* dst = *dst_ptr;
    unsigned phase = *phase_ptr;

    if (run >= 1) {
        *dst++ = (uint8_t)(waveform >> ((phase & 3u) * 8u));
        ++phase;
    }
    if (run >= 2) {
        *dst++ = (uint8_t)(waveform >> ((phase & 3u) * 8u));
        ++phase;
    }
    if (run >= 3) {
        *dst++ = (uint8_t)(waveform >> ((phase & 3u) * 8u));
        ++phase;
    }

    *dst_ptr = dst;
    *phase_ptr = phase;
}

static void vector_tv_build_runs(int source_width, int output_width)
{
    tv_fill_u8(vector_tv_runs, 0, sizeof(vector_tv_runs));

    if (source_width <= 0 || output_width <= 0)
        return;

    uint16_t di =
        (uint16_t)((0x100u * (uint)source_width) /
                   (uint)output_width);
    if (di == 0)
        di = 1;

    int next_source = 0x100;
    int source_x = 0;

    for (int out_x = 0;
         out_x < output_width && source_x < source_width;
         ++out_x) {
        ++vector_tv_runs[source_x];
        next_source -= di;
        if (next_source <= 0) {
            ++source_x;
            next_source += 0x100;
        }
    }

    vector_tv_runs_source_width = source_width;
    vector_tv_runs_output_width = output_width;
}

typedef struct {
    int block_x;
    uint32_t pixels[4];
} vector_tv_pixel_cache_t;

static inline __attribute__((always_inline))
void vector_tv_load_active_block(
        vector_tv_pixel_cache_t* cache,
        const vector_tv_state_t* state,
        int active_x, int n_line)
{
    const int block_x = active_x & ~15;
    const uint8_t roll_off =
        (uint8_t)(state->line_offset - n_line + 40);
    const int offset = ((block_x & 0x1f0) << 4) | roll_off;
    const uint8_t* memory = state->memory;
    const uint8_t by = memory[0x8000 + offset];
    const uint8_t br = memory[0xa000 + offset];
    const uint8_t bg = memory[0xc000 + offset];
    const uint8_t bb = memory[0xe000 + offset];
    const uint32_t* lut =
        vector_tv_pair_lut[state->lut_index];

#define VECTOR_TV_CACHE_INDEX(shift) \
    (((((unsigned)by >> (shift)) & 3u) << 6) | \
     ((((unsigned)br >> (shift)) & 3u) << 4) | \
     ((((unsigned)bg >> (shift)) & 3u) << 2) | \
      (((unsigned)bb >> (shift)) & 3u))

    cache->pixels[0] = lut[VECTOR_TV_CACHE_INDEX(6)];
    cache->pixels[1] = lut[VECTOR_TV_CACHE_INDEX(4)];
    cache->pixels[2] = lut[VECTOR_TV_CACHE_INDEX(2)];
    cache->pixels[3] = lut[VECTOR_TV_CACHE_INDEX(0)];
    cache->block_x = block_x;

#undef VECTOR_TV_CACHE_INDEX
}

static inline __attribute__((always_inline))
uint8_t vector_tv_cached_source_pixel(
        vector_tv_pixel_cache_t* cache,
        const vector_tv_state_t* state,
        int source_x, int source_y)
{
    const int n_line =
        source_y + (state->show_border ? 24 : 40);

    if (state->show_border) {
        if (n_line < 40 || n_line >= 296 ||
            source_x < 57 || source_x >= 569) {
            return (uint8_t)(
                state->border_pattern >>
                ((source_x & 3) * 8));
        }
        source_x -= 57;
    }

    const int block_x = source_x & ~15;
    if (cache->block_x != block_x)
        vector_tv_load_active_block(
            cache, state, source_x, n_line);

    const uint32_t word =
        cache->pixels[(source_x & 15) >> 2];
    return (uint8_t)(word >> ((source_x & 3) * 8));
}

static inline __attribute__((always_inline))
uint8_t vector_tv_pixel_slow(
        const vector_tv_state_t* state, int x, uint8_t roll_off)
{
    const int offset = ((x & 0x1f0) << 4) | roll_off;
    const uint8_t mask =
        (uint8_t)(0x80u >> ((x & 0x0eu) >> 1));
    const uint8_t* memory = state->memory;
    uint8_t color = 0;

    if (memory[0x8000 + offset] & mask) color |= 0x08;
    if (memory[0xa000 + offset] & mask) color |= 0x04;
    if (memory[0xc000 + offset] & mask) color |= 0x02;
    if (memory[0xe000 + offset] & mask) color |= 0x01;

    if (state->mode512)
        color = (x & 1) ? (color & 0x0cu) : (color & 0x03u);

    return vector_tv_color(state, color);
}

static void __not_in_flash_func(vector_tv_render_active)(
        uint8_t* output, int source_x, int count, int n_line,
        const vector_tv_state_t* state)
{
    const uint8_t roll_off =
        (uint8_t)(state->line_offset - n_line + 40);
    int x = source_x;
    int remaining = count;

    while (remaining &&
           ((x & 15) || ((uintptr_t)output & 3u))) {
        *output++ =
            vector_tv_pixel_slow(state, x++, roll_off);
        --remaining;
    }

    const uint8_t* memory = state->memory;
    const uint32_t* lut =
        vector_tv_pair_lut[state->lut_index];
    uint32_t* out32 = (uint32_t*)output;

    while (remaining >= 16) {
        const int offset = ((x & 0x1f0) << 4) | roll_off;
        const uint8_t by = memory[0x8000 + offset];
        const uint8_t br = memory[0xa000 + offset];
        const uint8_t bg = memory[0xc000 + offset];
        const uint8_t bb = memory[0xe000 + offset];

#define VECTOR_TV_PAIR_INDEX(shift) \
        (((((unsigned)by >> (shift)) & 3u) << 6) | \
         ((((unsigned)br >> (shift)) & 3u) << 4) | \
         ((((unsigned)bg >> (shift)) & 3u) << 2) | \
          (((unsigned)bb >> (shift)) & 3u))

        out32[0] = lut[VECTOR_TV_PAIR_INDEX(6)];
        out32[1] = lut[VECTOR_TV_PAIR_INDEX(4)];
        out32[2] = lut[VECTOR_TV_PAIR_INDEX(2)];
        out32[3] = lut[VECTOR_TV_PAIR_INDEX(0)];

#undef VECTOR_TV_PAIR_INDEX
        out32 += 4;
        x += 16;
        remaining -= 16;
    }

    output = (uint8_t*)out32;
    while (remaining--)
        *output++ =
            vector_tv_pixel_slow(state, x++, roll_off);
}

static void __not_in_flash_func(vector_tv_render_line)(
        uint8_t* output, int source_y,
        const vector_tv_state_t* state)
{
    const int source_width = state->show_border ? 626 : 512;
    const int n_line =
        source_y + (state->show_border ? 24 : 40);

    if (!state->show_border) {
        vector_tv_render_active(
            output, 0, source_width, n_line, state);
        return;
    }

    if (n_line < 40 || n_line >= 296) {
        vector_tv_fill_pattern(
            output, source_width, state->border_pattern);
        return;
    }

    vector_tv_fill_pattern(
        output, 57, state->border_pattern);
    vector_tv_render_active(
        output + 57, 0, 512, n_line, state);
    vector_tv_fill_pattern(
        output + 569, 57,
        vector_tv_rotate_pattern(state->border_pattern, 569));
}
#endif

// Частота цветовой поднесущей (Гц) для текущего режима: NTSC 3.579545 МГц,
// PAL 4.43361875 МГц. Вынесено в отдельную функцию, чтобы одна и та же таблица
// использовалась и при установке режима, и при пересчёте делителя PIO.
static double __not_in_flash_func(tv_color_freq)(const tv_out_mode_t* mode) {
    switch (mode->c_freq) {
        case _3579545: return 3.579545e6;
        case _4433619: return 4.43361875e6;
    }
    return 4.43361875e6;
}

// Делитель PIO задаёт частоту выборок ЦАП композита: ровно 4 отсчёта на период
// цветовой поднесущей. Он привязан к системной частоте, поэтому пересчитывается
// как при смене видеорежима, так и при смене частоты RP2350. Функция в ОЗУ:
// вызывается из graphics_system_clock_changed() в критической секции сразу
// после set_sys_clock_khz(), когда обращаться во flash ещё нельзя.
static void __not_in_flash_func(tv_apply_clkdiv)(void) {
    if (SM_video == -1) return;
    const double color_freq = tv_color_freq(&tv_out_mode);
    // Здесь настраивается уже работающий автомат состояний, а не заготовка
    // конфигурации: sm_config_set_clkdiv() принимает pio_sm_config*, тогда как
    // PIO_VIDEO->sm — массив регистров pio_sm_hw_t. Для живого SM нужен
    // pio_sm_set_clkdiv().
    pio_sm_set_clkdiv(PIO_VIDEO, SM_video,
                      (float)(clock_get_hz(clk_sys) / (color_freq * 4)));
}

// Вызывается из graphics_system_clock_changed() после смены системной частоты.
// В отличие от VGA (там пересчитывается делитель пиксельклока), здесь заново
// вычисляется делитель под частоту цветовой поднесущей — иначе после смены
// частоты RP2350 ломается строчная/кадровая синхронизация композита.
void __not_in_flash_func(tv_software_system_clock_changed)(void) {
    tv_apply_clkdiv();
}


void graphics_set_modeTV(tv_out_mode_t mode) {
    if (SM_video == -1) return;
    //можно добавить проверку на валидность данных, но пока так
    tv_out_mode = mode;
    for (int i = 0; i < 256; i++) {
        graphics_set_palette(i, (paletteRGB[2][i] << 16) | (paletteRGB[1][i] << 8) | (paletteRGB[0][i] << 0));
    };

    switch (tv_out_mode.N_lines) {
        case _624_lines:
            video_mode.N_lines = 312;
            break;
        case _625_lines:
            video_mode.N_lines = 625;
            break;
        case _524_lines:
            video_mode.N_lines = 262;
            break;
        case _525_lines:
            video_mode.N_lines = 525;
            break;
    }


    const double color_freq = tv_color_freq(&tv_out_mode);

    video_mode.H_len = ((color_freq * 4) / 1e6) * 63.9;
    video_mode.H_len &= 0xfffffff8;

    video_mode.sync_size = 4.7 * video_mode.H_len / 64;
    video_mode.sync_size &= 0xfffffff8;

    video_mode.begin_img_shx = 10.5 * video_mode.H_len / 64;
    video_mode.img_W = video_mode.H_len - ((12 * video_mode.H_len) / 64);
    video_mode.img_W &= 0xfffffffc;

    video_mode.LVL_C_MAX = 15;
    video_mode.SYNC_TMPL = 0;
    video_mode.NO_SYNC_TMPL = CONV_DAC(video_mode.LVL_C_MAX) | (1 << SYNC_PIN);
    video_mode.LVL_BLACK = 0 + video_mode.LVL_C_MAX;

    video_mode.LVL_Y_MAX = 40;

    if (tv_out_mode.tv_system == g_TV_OUT_NTSC) {
        video_mode.LVL_BLACK += 2;
        video_mode.LVL_Y_MAX += 3;
    };
    video_mode.LVL_BLACK_TMPL = CONV_DAC(video_mode.LVL_BLACK) | (1 << SYNC_PIN);

    // Делитель PIO зависит от системной частоты и пересчитывается общей
    // функцией — той же, что вызывается при смене частоты RP2350.
    tv_apply_clkdiv();

};


static uint32_t cbNORM[2][10]; //цветовая вспышка 80байт
static uint32_t cbINV[2][10]; //цветовая вспышка	инвертированная 80 байт

static uint32_t* cb[2]; //цветовая вспышка
//определение палитры(переделать)
void graphics_set_palette(uint8_t i, uint32_t color888) {
    conv_color[0] = conv_colorNORM[0];
    conv_color[1] = conv_colorNORM[1];
    cb[0] = cbNORM[0];
    cb[1] = cbNORM[1];

    uint8_t R8 = (color888 >> 16) & 0xff;
    uint8_t G8 = (color888 >> 8) & 0xff;
    uint8_t B8 = (color888 >> 0) & 0xff;
    paletteRGB[2][i] = R8;
    paletteRGB[1][i] = G8;
    paletteRGB[0][i] = B8;
    float R = R8 / 255.0;
    float G = G8 / 255.0;
    float B = B8 / 255.0;

    float Y = 0.299 * R + 0.587 * G + 0.114 * B;
    // if (active_out==g_TV_OUT_NTSC) Y=0.299*R+0.587*G+0.114*B;
    uint8_t base8 = video_mode.LVL_BLACK;

    const int cycle_size = 4;
    int8_t Y8 = ((int)(Y * video_mode.LVL_Y_MAX)) + base8;

    uint32_t cd0_32, cd1_32;
    // Доступ к слову по байтам: приведение обязательно, иначе типы
    // несовместимы
    int8_t* cd0 = (int8_t*)&cd0_32;
    int8_t* cd1 = (int8_t*)&cd1_32;

    float sin[] = { 0, 1, 0, -1 };
    // float sin[]={-1,1,1,-1,-1};//test

    float cos[] = { 1, 0, -1, 0 };
    switch (tv_out_mode.tv_system) {
        case g_TV_OUT_PAL: {
            float U = 0.493 * (B - Y);
            float V = 0.877 * (R - Y);

            int ph = 2;
            int dph = 0;
            if (tv_out_mode.cb_sync_PI_shift_lines) {
                dph = -1;
            }
            for (int i = 0; i < cycle_size; i++) {
                float k = 1.3 * tv_out_mode.color_index;
                //подобрать , чтобы не было перегруза 1.25 или увеличить для более ярких цветов
                int max_v = video_mode.LVL_C_MAX;
                int P = k * max_v * (U * sin[(i + ph + 1 + dph) % 4] + V * cos[(i + ph + 1 + dph) % 4]) + 0.0; //+1
                int M = k * max_v * (U * sin[(i + ph) % 4] - V * cos[(i + ph) % 4]) + 0.0;


                P = P < -max_v ? -max_v : P;
                P = P > max_v ? max_v : P;


                M = M < -max_v ? -max_v : M;
                M = M > max_v ? max_v : M;


                cd0[i] = (M);
                cd1[i] = (P);
            }
            // ph=0;

            //заполнение цветовой вспышки
            uint8_t* cb8_0 = (uint8_t *)cb[0];
            uint8_t* cb8_1 = (uint8_t *)cb[1];
            uint8_t* cb8_0_i = (uint8_t *)cbINV[0];
            uint8_t* cb8_1_i = (uint8_t *)cbINV[1];

            uint8_t ampl = 0;
            uint8_t max_ampl = video_mode.LVL_C_MAX;

            // изменение функций синуса и косинуса(доворот на пи/4)
            float Q = 0.7;
            float I = 0.7;
            for (int i = 0; i < cycle_size; i++) {
                cos[i] = cos[i] * Q - sin[i] * I;
                sin[i] = cos[i] * I + sin[i] * Q;
            }

            ph = 3; //3
            Q = 1;
            I = 0;
            //  Q=0.8;
            //  I=-0.1;
            if (tv_out_mode.cb_sync_PI_shift_lines) dph = 3;

            for (int i = 0; i < 40; i++) {
                ampl = max_ampl * 1;
                if (i < cycle_size * 1) ampl = i * max_ampl / cycle_size;
                if (i > (cycle_size * 9)) ampl = (cycle_size * 10 - i) * (max_ampl) / cycle_size;

                if (tv_out_mode.color_index == 0) ampl = 0; //полное отклюение цвета

                int bb = ampl * (Q * sin[(i + ph) % 4] + I * cos[(i + ph) % 4]) + 0.0;

                bb = (bb > max_ampl) ? max_ampl : bb;
                bb = (bb < -max_ampl) ? -max_ampl : bb;
                cb8_0[i] = max_ampl + bb;

                bb = ampl * (Q * sin[(i + ph + dph) % 4] + I * cos[(i + ph + dph) % 4]) + 0.0;

                bb = (bb > max_ampl) ? max_ampl : bb;
                bb = (bb < -max_ampl) ? -max_ampl : bb;
                cb8_1[i] = max_ampl + bb;


                cb8_0[i] = CONV_DAC(cb8_0[i]) | (1 << SYNC_PIN);
                cb8_1[i] = CONV_DAC(cb8_1[i]) | (1 << SYNC_PIN);

                //инверсная вспышка
                bb = -ampl * (Q * sin[(i + ph) % 4] + I * cos[(i + ph) % 4]) + 0.0;

                bb = (bb > max_ampl) ? max_ampl : bb;
                bb = (bb < -max_ampl) ? -max_ampl : bb;
                cb8_0_i[i] = max_ampl + bb;

                bb = -ampl * (Q * sin[(i + ph + dph) % 4] + I * cos[(i + ph + dph) % 4]) + 0.0;

                bb = (bb > max_ampl) ? max_ampl : bb;
                bb = (bb < -max_ampl) ? -max_ampl : bb;
                cb8_1_i[i] = max_ampl + bb;


                cb8_0_i[i] = CONV_DAC(cb8_0_i[i]) | (1 << SYNC_PIN);
                cb8_1_i[i] = CONV_DAC(cb8_1_i[i]) | (1 << SYNC_PIN);
            }
        }
        break;
        case g_TV_OUT_NTSC: {
            float Q = 0.4127 * (B - Y) + 0.4778 * (R - Y);
            float I = -0.268 * (B - Y) + 0.7358 * (R - Y);
            // Q*=0.7;
            // I*=0.8;
            // I=0;
            // I=-I;
            // int ph=3;

            int ph = 3;
            for (int i = 0; i < cycle_size; i++) {
                float k = 1.5 * tv_out_mode.color_index; //127;
                int max_v = video_mode.LVL_C_MAX;
                int C = ((int)(k * max_v * (Q * sin[(i + ph) % 4] + I * cos[(i + ph) % 4])));
                C = C < -max_v ? -max_v : C;
                C = C > max_v ? max_v : C;
                cd0[i] = (C);
                cd1[i] = (-C);
            }


            uint8_t* cb8_0 = (uint8_t *)cb[0];
            uint8_t* cb8_1 = (uint8_t *)cb[1];
            uint8_t ampl = 127;
            uint8_t max_ampl = video_mode.LVL_C_MAX;


            //  Q=0.75;
            //  I=0.75;
            Q = 1;
            I = -1;
            for (int i = 0; i < 40; i++) {
                ampl = max_ampl * 0.5; //уменьшение амплитуды - ярче цвета, но и больше размазывание цвета
                if (i < cycle_size * 1) ampl = i * max_ampl / cycle_size;
                if (i > (cycle_size * 9)) ampl = (cycle_size * 10 - i) * (max_ampl) / cycle_size;

                if (tv_out_mode.color_index == 0) ampl = 0; //полное отклюение цвета

                // cb8_0[i]=ampl+ampl*(sin(ph))+0.5;
                // cb8_1[i]=ampl+ampl*(sin(ph+3*M_PI/2))+0.5;

                // cb8_0[i]=127+ampl*(sin[i%4])+0.5;
                // cb8_1[i]=127+ampl*(sin[(i+2)%4])+0.5;   //+3

                int dd = ampl * (Q * sin[i % 4] + I * cos[i % 4]);
                dd = dd > max_ampl ? max_ampl : dd;
                dd = dd < -max_ampl ? -max_ampl : dd;


                cb8_0[i] = max_ampl + dd;
                cb8_1[i] = max_ampl - dd; //+3


                cb8_0[i] = CONV_DAC(cb8_0[i]) | (1 << SYNC_PIN);
                cb8_1[i] = CONV_DAC(cb8_1[i]) | (1 << SYNC_PIN);
            }
        }
        break;
        default:
            break;
    }


    uint32_t Y32 = (Y8 << 24) | (Y8 << 16) | (Y8 << 8) | (Y8 << 0);
    int8_t* yi = (int8_t*)&Y32;
    int8_t* ci = (int8_t*)&cd0_32;

    for (int i = 0; i < 4; i++) { yi[i] = CONV_DAC(yi[i]+ci[i]) | (1 << SYNC_PIN); };
    conv_color[0][i] = Y32;

    Y32 = (Y8 << 24) | (Y8 << 16) | (Y8 << 8) | (Y8 << 0);
    ci = (int8_t*)&cd1_32;
    for (int i = 0; i < 4; i++) { yi[i] = CONV_DAC(yi[i]+ci[i]) | (1 << SYNC_PIN); };
    conv_color[1][i] = Y32;

    //цвет со сдвигом фазы
    uint32_t c32 = conv_color[0][i];
    conv_colorINV[0][i] = (c32 >> 16) | ((c32 & 0xffff) << 16);
    c32 = conv_color[1][i];
    conv_colorINV[1][i] = (c32 >> 16) | ((c32 & 0xffff) << 16);
}


// Строка кадрового буфера. Шаг строки берётся из stride (в обычном режиме он
// равен width; в режиме обрезки буфер физически шире полезной строки).
static inline uint8_t* getLineBuffer(int line) {
    if (!graphics_framebuffer || line < 0 || line >= (int)graphics_buffer.height)
        return NULL;
    return graphics_framebuffer + (uint)line * graphics_buffer.stride;
}

//основная функция заполнения буферов видеоданных
static bool __time_critical_func(video_timer_callbackTV)(repeating_timer_t* rt) {
    static uint dma_inx_out = 0;
    static uint lines_buf_inx = N_LINE_BUF - 1;

    if (dma_chan_ctrl == -1) return 1; //не определен дма канал

#ifdef PICO_RP2040
    /*
     * RP2040 path is driven once per completed main-DMA scanline.
     * Render exactly one future line; do not inspect or chase DMA pointers.
     */
    uint dma_inx = (dma_inx_out + 1u) % N_LINE_BUF_DMA;
#else
    // Старый RP2350 путь: догоняем DMA по положению кольца дескрипторов.
    uint dma_inx =
        (N_LINE_BUF_DMA - 2 +
         ((dma_channel_hw_addr(dma_chan_ctrl)->read_addr -
           (uint32_t)rd_addr_DMA_CTRL) / 4)) %
        N_LINE_BUF_DMA;
#endif

    static uint32_t line_active = UINT32_MAX;
    static uint32_t frame_i = 0;
    static uint32_t g_str_index = 1;
    //while(n_loop--)
    while (dma_inx_out != dma_inx) {
        //режим VGA
        line_active++;
        g_str_index++;

        int dec_str = 0;


        if (line_active >= video_mode.N_lines) {
            line_active = 0;
            frame_i++;
#ifdef PICO_RP2040
            vector_tv_frame_state_valid =
                vector_tv_snapshot(&vector_tv_frame_state);
            if (vector_tv_frame_state_valid) {
                vector_tv_lut_in_use =
                    vector_tv_frame_state.lut_index;
                graphics_buffer.width =
                    vector_tv_frame_state.show_border ? 626u : 512u;
                graphics_buffer.height =
                    vector_tv_frame_state.show_border ? 288u : 256u;
                graphics_buffer.stride = graphics_buffer.width;
            }
#endif
        }

        lines_buf_inx = (lines_buf_inx + 1) % N_LINE_BUF;
        uint8_t* output_buffer8 = (uint8_t *)lines_buf[lines_buf_inx];

        bool is_line_visible = true;
        // if (false)
        switch (tv_out_mode.N_lines) {
            case _624_lines:
            case _625_lines:

                switch (line_active) {
                    case 0:
                    case 1:
                        //|___|--|___|--| type=1
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, (video_mode.H_len / 2) - video_mode.sync_size);
                        output_buffer8 += (video_mode.H_len / 2) - video_mode.sync_size;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL, video_mode.sync_size);
                        output_buffer8 += video_mode.sync_size;
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, (video_mode.H_len / 2) - video_mode.sync_size);
                        output_buffer8 += (video_mode.H_len / 2) - video_mode.sync_size;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL, video_mode.sync_size);
                        is_line_visible = false;
                        break;

                    case 2:
                        // ____|--|_|----type=2
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, (video_mode.H_len / 2) - video_mode.sync_size);
                        output_buffer8 += (video_mode.H_len / 2) - video_mode.sync_size;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL, video_mode.sync_size);
                        output_buffer8 += video_mode.sync_size;
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size / 2);
                        output_buffer8 += video_mode.sync_size / 2;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len / 2) - (video_mode.sync_size / 2));
                        is_line_visible = false;
                        break;
                    case 3:
                    case 4: //|_|----|_|---- type=0
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size / 2);
                        output_buffer8 += video_mode.sync_size / 2;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len / 2) - (video_mode.sync_size / 2));
                        output_buffer8 += (video_mode.H_len / 2) - (video_mode.sync_size / 2);
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size / 2);
                        output_buffer8 += video_mode.sync_size / 2;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len / 2) - (video_mode.sync_size / 2));
                        is_line_visible = false;
                        break;

                    case 5: break; //шаблон как у видимой строки, но без изображения


                    case 310:
                    case 311:
                        //|_|----|_|---- type=0
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size / 2);
                        output_buffer8 += video_mode.sync_size / 2;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len / 2) - (video_mode.sync_size / 2));
                        output_buffer8 += (video_mode.H_len / 2) - (video_mode.sync_size / 2);
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size / 2);
                        output_buffer8 += video_mode.sync_size / 2;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len / 2) - (video_mode.sync_size / 2));
                        is_line_visible = false;
                        break;
                    case 312:
                        //|_|---|____|--| type=3
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size / 2);
                        output_buffer8 += video_mode.sync_size / 2;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len / 2) - (video_mode.sync_size / 2));
                        output_buffer8 += (video_mode.H_len / 2) - (video_mode.sync_size / 2);
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, (video_mode.H_len / 2) - video_mode.sync_size);
                        output_buffer8 += (video_mode.H_len / 2) - video_mode.sync_size;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL, video_mode.sync_size);
                        is_line_visible = false;
                        break;
                    case 313:
                    case 314:
                        //|___|--|___|--| type=1
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, (video_mode.H_len / 2) - video_mode.sync_size);
                        output_buffer8 += (video_mode.H_len / 2) - video_mode.sync_size;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL, video_mode.sync_size);
                        output_buffer8 += video_mode.sync_size;
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, (video_mode.H_len / 2) - video_mode.sync_size);
                        output_buffer8 += (video_mode.H_len / 2) - video_mode.sync_size;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL, video_mode.sync_size);
                        is_line_visible = false;
                        break;
                    case 315:
                    case 316:
                        //|_|----|_|---- type=0
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size / 2);
                        output_buffer8 += video_mode.sync_size / 2;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len / 2) - (video_mode.sync_size / 2));
                        output_buffer8 += (video_mode.H_len / 2) - (video_mode.sync_size / 2);
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size / 2);
                        output_buffer8 += video_mode.sync_size / 2;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len / 2) - (video_mode.sync_size / 2));
                        is_line_visible = false;
                        break;
                    case 317:
                        //|_|---------type=4
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size / 2);
                        output_buffer8 += video_mode.sync_size / 2;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len) - (video_mode.sync_size / 2));
                        is_line_visible = false;
                        break;
                    case 622:
                        //|__|---|_|----type=5
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size);
                        output_buffer8 += video_mode.sync_size;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len / 2) - (video_mode.sync_size));
                        output_buffer8 += (video_mode.H_len / 2) - (video_mode.sync_size);
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size / 2);
                        output_buffer8 += video_mode.sync_size / 2;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len / 2) - (video_mode.sync_size / 2));
                        is_line_visible = false;
                        break;
                }
                break;
            case _524_lines:
            case _525_lines:

                switch (line_active) {
                    case 0:
                    case 1:
                    case 2:
                        //|_|----|_|---- type=0
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size / 2);
                        output_buffer8 += video_mode.sync_size / 2;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len / 2) - (video_mode.sync_size / 2));
                        output_buffer8 += (video_mode.H_len / 2) - (video_mode.sync_size / 2);
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size / 2);
                        output_buffer8 += video_mode.sync_size / 2;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len / 2) - (video_mode.sync_size / 2));
                        is_line_visible = false;
                        break;
                    case 3:
                    case 4:
                    case 5:
                        //|___|--|___|--| type=1
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, (video_mode.H_len / 2) - video_mode.sync_size);
                        output_buffer8 += (video_mode.H_len / 2) - video_mode.sync_size;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL, video_mode.sync_size);
                        output_buffer8 += video_mode.sync_size;
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, (video_mode.H_len / 2) - video_mode.sync_size);
                        output_buffer8 += (video_mode.H_len / 2) - video_mode.sync_size;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL, video_mode.sync_size);
                        is_line_visible = false;
                        break;
                    case 6:
                    case 7:
                    case 8:

                        //|_|----|_|---- type=0
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size / 2);
                        output_buffer8 += video_mode.sync_size / 2;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len / 2) - (video_mode.sync_size / 2));
                        output_buffer8 += (video_mode.H_len / 2) - (video_mode.sync_size / 2);
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size / 2);
                        output_buffer8 += video_mode.sync_size / 2;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len / 2) - (video_mode.sync_size / 2));
                        is_line_visible = false;
                        break;

                    case 262:
                        //|__|---|_|----type=5
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size);
                        output_buffer8 += video_mode.sync_size;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len / 2) - (video_mode.sync_size));
                        output_buffer8 += (video_mode.H_len / 2) - (video_mode.sync_size);
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size / 2);
                        output_buffer8 += video_mode.sync_size / 2;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len / 2) - (video_mode.sync_size / 2));
                        is_line_visible = false;
                        break;
                    case 263:
                    case 264:
                        //|_|----|_|---- type=0
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size / 2);
                        output_buffer8 += video_mode.sync_size / 2;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len / 2) - (video_mode.sync_size / 2));
                        output_buffer8 += (video_mode.H_len / 2) - (video_mode.sync_size / 2);
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size / 2);
                        output_buffer8 += video_mode.sync_size / 2;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len / 2) - (video_mode.sync_size / 2));
                        is_line_visible = false;
                        break;
                    case 265:
                        //
                        //|_|---|____|--| type=3
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size / 2);
                        output_buffer8 += video_mode.sync_size / 2;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len / 2) - (video_mode.sync_size / 2));
                        output_buffer8 += (video_mode.H_len / 2) - (video_mode.sync_size / 2);
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, (video_mode.H_len / 2) - video_mode.sync_size);
                        output_buffer8 += (video_mode.H_len / 2) - video_mode.sync_size;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL, video_mode.sync_size);
                        is_line_visible = false;
                        break;
                    case 266:
                    case 267:

                        // PIO_VIDEO->txf[SM_video]=v_mode.NO_SYNC_TMPL|(v_mode.NO_SYNC_TMPL<<8);
                        //|___|--|___|--| type=1
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, (video_mode.H_len / 2) - video_mode.sync_size);
                        output_buffer8 += (video_mode.H_len / 2) - video_mode.sync_size;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL, video_mode.sync_size);
                        output_buffer8 += video_mode.sync_size;
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, (video_mode.H_len / 2) - video_mode.sync_size);
                        output_buffer8 += (video_mode.H_len / 2) - video_mode.sync_size;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL, video_mode.sync_size);
                        is_line_visible = false;
                        break;
                    case 268:

                        // ____|--|_|----type=2
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, (video_mode.H_len / 2) - video_mode.sync_size);
                        output_buffer8 += (video_mode.H_len / 2) - video_mode.sync_size;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL, video_mode.sync_size);
                        output_buffer8 += video_mode.sync_size;
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size / 2);
                        output_buffer8 += video_mode.sync_size / 2;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len / 2) - (video_mode.sync_size / 2));
                        is_line_visible = false;
                        break;
                    case 269:
                    case 270:
                        //|_|----|_|---- type=0
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size / 2);
                        output_buffer8 += video_mode.sync_size / 2;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len / 2) - (video_mode.sync_size / 2));
                        output_buffer8 += (video_mode.H_len / 2) - (video_mode.sync_size / 2);
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size / 2);
                        output_buffer8 += video_mode.sync_size / 2;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len / 2) - (video_mode.sync_size / 2));
                        is_line_visible = false;
                        break;
                    case 271:

                        //|_|---------type=4
                        tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size / 2);
                        output_buffer8 += video_mode.sync_size / 2;
                        tv_fill_u8(output_buffer8, video_mode.NO_SYNC_TMPL,
                               (video_mode.H_len) - (video_mode.sync_size / 2));
                        is_line_visible = false;
                        break;


                    default:
                        break;
                }
                break;
        }

        int li = 0;

        if (tv_out_mode.tv_system == g_TV_OUT_PAL) {
            li = g_str_index & 1;


            // static bool is_even_frame;

            //   dec_str+=2;
            //625 строк
            //  if (false)
            if (tv_out_mode.cb_sync_PI_shift_lines) {
                if (tv_out_mode.cb_sync_PI_shift_half_frame) {
                    if ((line_active == 0) ||
                        ((line_active == video_mode.N_lines / 2) && (
                             (tv_out_mode.N_lines == _625_lines) || (tv_out_mode.N_lines == _525_lines)))) {
                        dec_str += 2;
                        static bool is_inv;
                        if (is_inv) {
                            cb[0] = cbINV[0];
                            conv_color[0] = conv_colorINV[0];

                            cb[1] = cbINV[1];
                            conv_color[1] = conv_colorINV[1];
                        }
                        else {
                            cb[0] = cbNORM[0];
                            conv_color[0] = conv_colorNORM[0];
                            cb[1] = cbNORM[1];
                            conv_color[1] = conv_colorNORM[1];
                        }
                        is_inv = !is_inv;
                    } //нейтрализация сдвига фазы "лишней строки"(не кратной 4)
                    // if ((tv_out_mode.N_lines==_625_lines)||(tv_out_mode.N_lines==_525_lines))
                    // 	if (line_active==v_mode.N_lines/2)  {g_str_index+=1;dec_str+=2;}
                }
            }
            else {
                if (tv_out_mode.cb_sync_PI_shift_half_frame) {
                    if ((line_active == 0)) {
                        g_str_index += 2;
                        dec_str += 2;
                    } //нейтрализация сдвига фазы "лишней строки"(не кратной 4)
                    if ((tv_out_mode.N_lines == _625_lines) || (tv_out_mode.N_lines == _525_lines))
                        if (line_active == video_mode.N_lines / 2) {
                            g_str_index += 2;
                            dec_str += 2;
                        }
                }

                dec_str += 1;

                switch (g_str_index & 3) {
                    case 0:
                        cb[0] = cbNORM[0];
                        conv_color[0] = conv_colorNORM[0];
                        break;
                    case 3:
                        cb[1] = cbNORM[1];
                        conv_color[1] = conv_colorNORM[1];
                        break;
                    case 2:
                        cb[0] = cbINV[0];
                        conv_color[0] = conv_colorINV[0];
                        break;
                    case 1:
                        cb[1] = cbINV[1];
                        conv_color[1] = conv_colorINV[1];


                    default:
                        break;
                }
            }
        }

        else {
            if (tv_out_mode.cb_sync_PI_shift_lines) //сдвиг фазы поднесущей соседних строк относительно ССИ на пи
            {
                dec_str = 2;
            }
            else {
                g_str_index += 1;
            }

            if (tv_out_mode.cb_sync_PI_shift_half_frame) {
                if ((line_active == 0)) {
                    dec_str ^= 2;
                    g_str_index -= 1;
                } //нейтрализация сдвига фазы "лишней строки"(не кратной 4)
                if ((tv_out_mode.N_lines == _625_lines) || (tv_out_mode.N_lines == _525_lines))
                    if (line_active == video_mode.N_lines / 2) {
                        dec_str ^= 2;
                        g_str_index -= 1;
                    }
            }
            li = (g_str_index) & 1;
        };


        //ТВ строка с изображением
        if (is_line_visible) {
            tv_fill_u8(output_buffer8, video_mode.SYNC_TMPL, video_mode.sync_size);
            tv_fill_u8(output_buffer8 + video_mode.sync_size, video_mode.NO_SYNC_TMPL,
                   video_mode.begin_img_shx - video_mode.sync_size);
            int post_img_clear = 60;
            tv_fill_u8(output_buffer8 + (video_mode.H_len - post_img_clear), video_mode.NO_SYNC_TMPL, post_img_clear);


            // // //цветовая вспышка
            int mul_sh = 19;
            if (tv_out_mode.c_freq == _4433619) mul_sh = 23; //сдвиг вспышки для более высокой частоты
            if (li) tv_copy_u8(output_buffer8 + 0 + mul_sh * 4, cb[1], 40);
            else tv_copy_u8(output_buffer8 + 0 + mul_sh * 4, cb[0], 40);

            //цветовая вспышка V2

            // uint32_t* out_buf32_cb=(uint32_t*)lines_buf[lines_buf_inx];
            // out_buf32_cb+=19;//сдвиг начала цветовой вспышки

            // uint32_t* cb32=cb[li];
            // for(int i=10;i--;)*out_buf32_cb++=*cb32++;


            output_buffer8 += video_mode.begin_img_shx;
            int d_end = 0;
            int buffer_shift = 0;
            int y = -1;

            switch (tv_out_mode.N_lines) {
                case _624_lines:
                case _625_lines:
                    d_end = (tv_out_mode.c_freq == _4433619) ? 152 : 118;
                    buffer_shift = (tv_out_mode.c_freq == _4433619) ? 72 : 60;
                    // Прежний вычет 24 строк подгонял 240-строчную картинку
                    // порта ZX под центр PAL-кадра. У нас строк 288, то есть
                    // почти вся активная область, и подгонять нечего.
                    if ((line_active > 4) && (line_active < 310)) { y = line_active - 23; };
                    if ((line_active > 317) && (line_active < 622)) { y = line_active - 335; };
                    if (y >= 0) y -= graphics_buffer.shift_y;
                    break;
                case _524_lines:
                case _525_lines:


                    if ((line_active > 8) && (line_active < 262)) { y = line_active - 20; };
                    if ((line_active > 271)) { y = line_active - 282; };
                    if (y >= 0) y -= graphics_buffer.shift_y;
                    break;
            }
            // Высота берётся из кадрового буфера: у Вектора это 288 строк PAL,
            // а не 240, как было жёстко записано в наброске
            const int img_H = (int)graphics_buffer.height;
            uint8_t* input_buffer = NULL;
#ifndef PICO_RP2040
            if (y < img_H && y >= 0)
                input_buffer = getLineBuffer(y);
            const bool source_valid = input_buffer != NULL;
#else
            const bool source_valid =
                y >= 0 && y < img_H &&
                (menu_text_active() ||
                 vector_tv_frame_state_valid);
#endif

            if (!source_valid) {
                // вне изображения
                tv_fill_u8(output_buffer8, video_mode.LVL_BLACK_TMPL,
                           video_mode.img_W);
            }
            else {
                int next_ibuf = 0x100;

                {
                    int shx = buffer_shift + graphics_buffer.shift_x;
                    const int room =
                        (int)(LINE_SIZE_MAX - video_mode.begin_img_shx) -
                        (video_mode.img_W - d_end);

                    /*
                     * Do not clamp the image to begin_img_shx. NTSC has
                     * buffer_shift == 0, which previously made every negative
                     * shift impossible. The real left limit is the end of the
                     * colour burst: moving farther would overwrite burst data.
                     */
                    const int burst_end =
                        ((tv_out_mode.c_freq == _4433619) ? 23 : 19) * 4 + 40;
                    int min_shx = burst_end - video_mode.begin_img_shx;
                    min_shx = (min_shx / 4) * 4;

                    if (shx < min_shx) shx = min_shx;
                    if (shx > room) shx = room > min_shx ? room : min_shx;
                    output_buffer8 += shx;
                }

#ifdef PICO_RP2040
                if (menu_video_mode == GRAPHICS_VIDEO_TEXT) {
                    const int output_width =
                        video_mode.img_W - d_end;
                    tv_fill_u8(output_buffer8,
                               video_mode.LVL_BLACK_TMPL,
                               output_width);
                    render_menu_text_line_tv(
                        output_buffer8, y, img_H,
                        output_width, li);
                    break;
                }
#endif

                switch (tv_out_mode.mode_bpp) {
                    case GRAPHICSMODE_DEFAULT: {
#ifdef PICO_RP2040
                        const int source_width =
                            (int)graphics_buffer.width;
                        const int output_width =
                            video_mode.img_W - d_end;

                        if (vector_tv_runs_source_width != source_width ||
                            vector_tv_runs_output_width != output_width) {
                            vector_tv_build_runs(
                                source_width, output_width);
                        }

                        const unsigned variant =
                            (conv_color[li] == conv_colorINV[li])
                                ? (unsigned)(2 + li)
                                : (unsigned)li;
                        unsigned phase = 0;
                        int x = 0;
                        const int n_line =
                            y + (vector_tv_frame_state.show_border
                                 ? 24 : 40);

                        /*
                         * Outside active Vector lines 40..295 only the border
                         * must be emitted. Without this clipping roll_off wraps
                         * as uint8_t and the top of VRAM is repeated below.
                         */
                        if (vector_tv_frame_state.show_border &&
                            (n_line < 40 || n_line >= 296)) {
                            const uint8_t border_rgb =
                                (uint8_t)vector_tv_frame_state.border_pattern;
                            const uint32_t border_wave =
                                conv_color[li][
                                    map64colors[border_rgb & 0x3fu]];

                            for (int bx = 0; bx < source_width; ++bx) {
                                vector_tv_emit_run(
                                    &output_buffer8, border_wave, &phase,
                                    vector_tv_runs[bx]);
                            }
                            break;
                        }

                        if (vector_tv_frame_state.show_border) {
                            const uint8_t border_rgb =
                                (uint8_t)vector_tv_frame_state.border_pattern;
                            const uint32_t border_wave =
                                conv_color[li][
                                    map64colors[border_rgb & 0x3fu]];
                            for (; x < 57; ++x)
                                vector_tv_emit_run(
                                    &output_buffer8, border_wave, &phase,
                                    vector_tv_runs[x]);
                        }

                        const int active_end =
                            vector_tv_frame_state.show_border ? 569 : 512;
                        int active_x =
                            vector_tv_frame_state.show_border ? 0 : x;

                        for (; x + 15 < active_end;
                             x += 16, active_x += 16) {
                            const uint32_t indices =
                                vector_tv_plane_indices4(
                                    &vector_tv_frame_state,
                                    active_x, n_line);

#define VECTOR_TV_EMIT_GROUP(group, shift) do {                         \
    const uint32_t* waves =                                             \
        vector_tv_composite_lut[variant]                                \
                               [(uint8_t)(indices >> (shift))];          \
    vector_tv_emit_run(                                                 \
        &output_buffer8, waves[0], &phase,                              \
        vector_tv_runs[x + (group) * 4 + 0]);                           \
    vector_tv_emit_run(                                                 \
        &output_buffer8, waves[1], &phase,                              \
        vector_tv_runs[x + (group) * 4 + 1]);                           \
    vector_tv_emit_run(                                                 \
        &output_buffer8, waves[2], &phase,                              \
        vector_tv_runs[x + (group) * 4 + 2]);                           \
    vector_tv_emit_run(                                                 \
        &output_buffer8, waves[3], &phase,                              \
        vector_tv_runs[x + (group) * 4 + 3]);                           \
} while (0)

                            VECTOR_TV_EMIT_GROUP(0, 0);
                            VECTOR_TV_EMIT_GROUP(1, 8);
                            VECTOR_TV_EMIT_GROUP(2, 16);
                            VECTOR_TV_EMIT_GROUP(3, 24);

#undef VECTOR_TV_EMIT_GROUP
                        }

                        /*
                         * Only the 512-pixel active area is normally handled
                         * above. Keep a small generic tail for unusual clipping
                         * or future geometry changes.
                         */
                        for (; x + 3 < active_end;
                             x += 4, active_x += 4) {
                            const uint32_t indices =
                                vector_tv_plane_indices4(
                                    &vector_tv_frame_state,
                                    active_x, n_line);
                            const uint32_t* waves =
                                vector_tv_composite_lut[variant]
                                                       [(uint8_t)indices];

                            vector_tv_emit_run(
                                &output_buffer8, waves[0], &phase,
                                vector_tv_runs[x + 0]);
                            vector_tv_emit_run(
                                &output_buffer8, waves[1], &phase,
                                vector_tv_runs[x + 1]);
                            vector_tv_emit_run(
                                &output_buffer8, waves[2], &phase,
                                vector_tv_runs[x + 2]);
                            vector_tv_emit_run(
                                &output_buffer8, waves[3], &phase,
                                vector_tv_runs[x + 3]);
                        }

                        for (; x < active_end; ++x, ++active_x) {
                            vector_tv_pixel_cache_t cache;
                            cache.block_x = -1;
                            const uint8_t rgb =
                                vector_tv_cached_source_pixel(
                                    &cache, &vector_tv_frame_state,
                                    x, y);
                            const uint32_t wave =
                                conv_color[li][
                                    map64colors[rgb & 0x3fu]];
                            vector_tv_emit_run(
                                &output_buffer8, wave, &phase,
                                vector_tv_runs[x]);
                        }

                        if (vector_tv_frame_state.show_border) {
                            const uint8_t border_rgb =
                                (uint8_t)vector_tv_frame_state.border_pattern;
                            const uint32_t border_wave =
                                conv_color[li][
                                    map64colors[border_rgb & 0x3fu]];
                            for (; x < source_width; ++x)
                                vector_tv_emit_run(
                                    &output_buffer8, border_wave, &phase,
                                    vector_tv_runs[x]);
                        }
#else
                        const int output_width =
                            video_mode.img_W - d_end;
                        uint16_t di =
                            (uint16_t)((0x100u *
                                        (uint)graphics_buffer.width) /
                                       (uint)output_width);
                        if (di == 0)
                            di = 1;

                        register int x = 0;
                        register uint8_t c = input_buffer[x++];
                        uint8_t color =
                            map64colors[c & 0x3fu];
                        uint32_t cout32 = conv_color[li][color];
                        uint8_t* c_4 = (uint8_t*)&cout32;

#pragma unroll(640)
                        for (int i = 0;
                             i < output_width;
                             ++i) {
                            *output_buffer8++ = c_4[i & 3];
                            next_ibuf -= di;
                            if (next_ibuf <= 0) {
                                c = input_buffer[x++];
                                color = map64colors[c & 0x3fu];
                                cout32 = conv_color[li][color];
                                next_ibuf += 0x100;
                            }
                        }
#endif
                    }
                    break;
                }
            }
        }

        // управление длиной строки
        transfer_count_DMA_CTRL[dma_inx_out] = video_mode.H_len - dec_str;
        rd_addr_DMA_CTRL[dma_inx_out] = (uint32_t)&lines_buf[lines_buf_inx];
        // включаем заполненный буфер в данные для вывода
        dma_inx_out = (dma_inx_out + 1) % (N_LINE_BUF_DMA);
#ifdef PICO_RP2040
        dma_inx = dma_inx_out;
#else
        dma_inx =
            (N_LINE_BUF_DMA - 2 +
             ((dma_channel_hw_addr(dma_chan_ctrl)->read_addr -
               (uint32_t)rd_addr_DMA_CTRL) / 4)) %
            N_LINE_BUF_DMA;
#endif
    }
    return true;
}

#ifdef PICO_RP2040
/*
 * The main DMA IRQ fires after one complete composite scanline has left the
 * buffer. That buffer is now safe to refill. The chained control DMAs have
 * already started the following line, so rendering proceeds in parallel with
 * output exactly as in the VGA driver.
 */
static void __time_critical_func(tv_soft_dma_handler)(void)
{
    dma_hw->ints1 = 1u << dma_chan;
    (void)video_timer_callbackTV(NULL);
}
#endif

void graphics_set_vector_source(
        const uint8_t* memory, const uint8_t* palette,
        uint8_t border_color, uint8_t line_offset,
        bool mode512, bool show_border)
{
#ifdef PICO_RP2040
    vector_tv_state_t next = vector_tv_state;
    next.memory = memory;
    next.border_color = border_color;
    next.line_offset = line_offset;
    next.mode512 = mode512;
    next.show_border = show_border;
    next.enabled = memory != NULL;

    if (palette)
        tv_copy_u8(next.palette, palette, sizeof(next.palette));

    uint8_t lut_index = (uint8_t)(next.lut_index + 1u);
    if (lut_index >= VECTOR_TV_LUT_COUNT)
        lut_index = 0;
    if (lut_index == vector_tv_lut_in_use) {
        ++lut_index;
        if (lut_index >= VECTOR_TV_LUT_COUNT)
            lut_index = 0;
    }
    next.lut_index = lut_index;
    vector_tv_build_lut(
        &next, vector_tv_pair_lut[lut_index]);
    vector_tv_build_composite_lut(
        vector_tv_pair_lut[lut_index]);

    vector_tv_seq++;
    vector_tv_barrier();
    vector_tv_state = next;
    vector_tv_barrier();
    vector_tv_seq++;
#else
    (void)memory;
    (void)palette;
    (void)border_color;
    (void)line_offset;
    (void)mode512;
    (void)show_border;
#endif
}

void graphics_set_buffer(uint8_t* buffer, const uint16_t width, const uint16_t height) {
    graphics_framebuffer = buffer;
    graphics_buffer.width = width;
    graphics_buffer.height = height;
    graphics_buffer.stride = width;   // по умолчанию шаг строки равен ширине
}

void graphics_set_line_stride(uint16_t stride) {
    graphics_buffer.stride = stride;
}

uint16_t graphics_get_line_stride(void) {
    return (uint16_t)graphics_buffer.stride;
}

//выделение и настройка общих ресурсов - 4 DMA канала, PIO программ и 2 SM
void graphics_init() {
#ifdef PICO_RP2040
    tv_copy_u8(menu_font_8x8_sram, font_8x8,
               sizeof(menu_font_8x8_sram));
    menu_text_clear(0x17u);
#endif
    //настройка PIO
    SM_video = pio_claim_unused_sm(PIO_VIDEO, true);
    //выделение  DMA каналов
    dma_chan_ctrl = dma_claim_unused_channel(true);
    dma_chan_ctrl2 = dma_claim_unused_channel(true);
    dma_chan = dma_claim_unused_channel(true);


    //---------------

    //заполнение палитры по умолчанию(ч.б.)
    for (int ci = 0; ci < 256; ci++) graphics_set_palette(ci, (ci << 16) | (ci << 8) | ci); //


    //настройка рабочей SM TV

    uint offs_prg0 = 0;
    offs_prg0 = pio_add_program(PIO_VIDEO, &program_pio_TV);
    uint16_t* conv_color16 = (uint16_t *)conv_color;

    pio_sm_config c_c = pio_get_default_sm_config();

    sm_config_set_wrap(&c_c, offs_prg0, offs_prg0 + (program_pio_TV.length - 1));
    for (int i = 0; i < 8; i++) {
        gpio_set_slew_rate(TV_BASE_PIN + i, GPIO_SLEW_RATE_FAST);
        pio_gpio_init(PIO_VIDEO, TV_BASE_PIN + i);
        gpio_set_drive_strength(TV_BASE_PIN + i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(TV_BASE_PIN + i, GPIO_SLEW_RATE_FAST);
    }
    pio_sm_set_consecutive_pindirs(PIO_VIDEO, SM_video, TV_BASE_PIN, 8, true); //конфигурация пинов на выход
    sm_config_set_out_pins(&c_c, TV_BASE_PIN, 8);

    sm_config_set_out_shift(&c_c, true, true, 8); //16,32
    sm_config_set_fifo_join(&c_c, PIO_FIFO_JOIN_TX);

    pio_sm_init(PIO_VIDEO, SM_video, offs_prg0, &c_c);
    pio_sm_set_enabled(PIO_VIDEO, SM_video, true);

    //установка параметров по умолчанию
    graphics_set_modeTV(tv_out_mode);

    // У NTSC в растр попадает 242 строки из 288, снизу срезается 46.
    // Активная часть кадра Вектора занимает строки 16..271 буфера, поэтому
    // по умолчанию приподнимаем картинку, чтобы обрезка легла симметрично.
    // Меню компенсирует этот сдвиг своим началом и остаётся на месте экрана.
    if (tv_out_mode.N_lines == _524_lines || tv_out_mode.N_lines == _525_lines)
        graphics_buffer.shift_y = -c_ntscShiftY;

    //настройки DMA

    //основной рабочий канал
    dma_channel_config cfg_dma = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_8);
    channel_config_set_chain_to(&cfg_dma, dma_chan_ctrl2); // chain to other channel

    channel_config_set_read_increment(&cfg_dma, true);
    channel_config_set_write_increment(&cfg_dma, false);


    uint dreq = DREQ_PIO1_TX0 + SM_video;
    if (PIO_VIDEO == pio0) dreq = DREQ_PIO0_TX0 + SM_video;
    channel_config_set_dreq(&cfg_dma, dreq);

    dma_channel_configure(
        dma_chan,
        &cfg_dma,
        &PIO_VIDEO->txf[SM_video], // Write address
        lines_buf[0], // read address
        video_mode.H_len / 1, //
        false // Don't start yet
    );

    //контрольный канал для основного(адрес чтения)
    cfg_dma = dma_channel_get_default_config(dma_chan_ctrl);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_32);
    channel_config_set_chain_to(&cfg_dma, dma_chan); // chain to other channel

    channel_config_set_read_increment(&cfg_dma, true);
    channel_config_set_write_increment(&cfg_dma, false);
    channel_config_set_ring(&cfg_dma,false, 2 + N_LINE_BUF_log2);


    dma_channel_configure(
        dma_chan_ctrl,
        &cfg_dma,
        &dma_hw->ch[dma_chan].read_addr, // Write address
        // &dma_hw->ch[dma_chan].al2_transfer_count,
        rd_addr_DMA_CTRL, // read address
        1, //
        false // Don't start yet
    );


    //контрольный канал для основного(количество транзакций)
    cfg_dma = dma_channel_get_default_config(dma_chan_ctrl2);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_32);
    channel_config_set_chain_to(&cfg_dma, dma_chan_ctrl); // chain to other channel

    channel_config_set_read_increment(&cfg_dma, true);
    channel_config_set_write_increment(&cfg_dma, false);
    channel_config_set_ring(&cfg_dma,false, 2 + N_LINE_BUF_log2);

    for (int i = 0; i < N_LINE_BUF_DMA; i++) {
        transfer_count_DMA_CTRL[i] = video_mode.H_len;
        rd_addr_DMA_CTRL[i] =
            (uint32_t)&lines_buf[i & (N_LINE_BUF - 1)];
    }

    dma_channel_configure(
        dma_chan_ctrl2,
        &cfg_dma,
        &dma_hw->ch[dma_chan].transfer_count, // Write address
        // &dma_hw->ch[dma_chan].al2_transfer_count,
        transfer_count_DMA_CTRL, // read address
        1, //
        false // Don't start yet
    );


#ifdef PICO_RP2040
    /*
     * Prepare the first four lines before DMA starts. Afterwards every main
     * DMA completion frees exactly one of these four buffers and IRQ refills
     * it with the line four positions ahead.
     */
    for (int i = 0; i < N_LINE_BUF; ++i)
        (void)video_timer_callbackTV(NULL);

    dma_hw->ints1 = 1u << dma_chan;
    irq_set_exclusive_handler(DMA_IRQ_1, tv_soft_dma_handler);
    dma_channel_set_irq1_enabled(dma_chan, true);
    irq_set_enabled(DMA_IRQ_1, true);
#endif

    dma_start_channel_mask((1u << dma_chan_ctrl2));

#ifndef PICO_RP2040
    int hz = 30000;
    if (!alarm_pool_add_repeating_timer_us(
            alarm_pool_create(2, 16), 1000000 / hz,
            video_timer_callbackTV, NULL, &video_timer)) {
        return;
    }
#endif
    // graphics_get_default_modeTV();
    graphics_set_modeTV(tv_out_mode);

    // Полная таблица цветов.
    //
    // Прежняя сводила все 64 кода к семнадцати именованным цветам палитры,
    // причём расписано было лишь 45 кодов из 64 — остальные попадали в default
    // и выводились белым. Вместо приближения каждому коду отдаётся собственная
    // запись палитры: место под них есть, индексы 64..127 ничем не заняты.
    //
    // Цвет собирается прямо из кода в том порядке бит, который задаёт RGB888
    // из graphics.h: [5:4] = R, [3:2] = G, [1:0] = B. Поэтому перестановка
    // красного с синим здесь больше не нужна — она была следствием того, что
    // старая таблица была расписана в обратном порядке.
    {
        static const uint8_t lvl[4] = { 0, 85, 170, 255 };   // два бита на канал
        for (uint8_t c = 0; c <= 0b00111111; ++c) {
            const uint8_t r = (c >> 4) & 0x03;
            const uint8_t g = (c >> 2) & 0x03;
            const uint8_t b = (c >> 0) & 0x03;
            graphics_set_palette(TV_PAL_BASE + c, TV_RGB24(lvl[r], lvl[g], lvl[b]));
            map64colors[c] = TV_PAL_BASE + c;
        }
    }

    graphics_set_palette(200, TV_RGB24(0x00, 0x00, 0x00)); //black
    graphics_set_palette(201, TV_RGB24(0x00, 0x00, 0xC4)); //blue
    graphics_set_palette(202, TV_RGB24(0x00, 0xC4, 0x00)); //green
    graphics_set_palette(203, TV_RGB24(0x00, 0xC4, 0xC4)); //cyan
    graphics_set_palette(204, TV_RGB24(0xC4, 0x00, 0x00)); //red
    graphics_set_palette(205, TV_RGB24(0xC4, 0x00, 0xC4)); //magenta
    graphics_set_palette(206, TV_RGB24(0xC4, 0x7E, 0x00)); //brown
    graphics_set_palette(207, TV_RGB24(0xC4, 0xC4, 0xC4)); //light gray
//    graphics_set_palette(208, TV_RGB24(0x4E, 0x4E, 0x4E)); //dark gray
    graphics_set_palette(208, TV_RGB24(0xC4, 0xC4, 0x00)); //yellow
    graphics_set_palette(209, TV_RGB24(0x4E, 0x4E, 0xDC)); //light blue
    graphics_set_palette(210, TV_RGB24(0x4E, 0xDC, 0x4E)); //light green
    graphics_set_palette(211, TV_RGB24(0x4E, 0xF3, 0xF3)); //light cyan
    graphics_set_palette(212, TV_RGB24(0xDC, 0x4E, 0x4E)); //light red
    graphics_set_palette(213, TV_RGB24(0xF3, 0x4E, 0xF3)); //light magenta
    graphics_set_palette(214, TV_RGB24(0xF3, 0xF3, 0x4E)); //light yellow
    graphics_set_palette(215, TV_RGB24(0xFF, 0xFF, 0xFF)); //white
    graphics_set_palette(216, TV_RGB24(0xFF, 0x7E, 0x00)); //orange
};

void graphics_set_offset(const int x, const int y) {
    /*
     * Old configuration files may contain arbitrary one-sample offsets.
     * Round toward zero to keep the composite colour phase unchanged.
     */
    graphics_buffer.shift_x = (x / 4) * 4;
    graphics_buffer.shift_y = y;
};

// SOFTTV однорежимен (растр задаётся PAL/NTSC, а не graphics_mode_t). Заглушка
// под общий контракт: NumLock переключения видимого эффекта не даёт.
enum graphics_mode_t graphics_get_mode(void) {
    return GRAPHICSMODE_DEFAULT;
}

void graphics_set_mode(const enum graphics_mode_t mode) {
///    tv_out_mode.mode_bpp = mode;
    // tv_out_mode.color_index = TEXTMODE_DEFAULT == mode ? 0.0 : 1.0;
    // tv_out_mode.cb_sync_PI_shift_lines = TEXTMODE_DEFAULT == mode ? true : false;
     // tv_out_mode.color_index = tv_out_mode.color_index < 1 ? 0 : 1;
    graphics_set_modeTV(tv_out_mode);
///    clrScr(0);
}


// ---------------------------------------------------------------------------
//  Примитивы рисования.
//
//  В наброске их не было вовсе: они реализованы в vga.c, и сборка с SOFTTV
//  просто не линковалась. Реализация повторяет вариант VGA, включая
//  включительные границы у rect и fill — на них рассчитан код меню.
// ---------------------------------------------------------------------------

uint32_t graphics_get_width() {
#ifdef PICO_RP2040
    if (menu_text_active())
        return MENU_TEXT_COLS * MENU_TEXT_CELL_W;
#endif
    return graphics_buffer.width;
}

uint32_t graphics_get_height() {
#ifdef PICO_RP2040
    if (menu_text_active())
        return MENU_TEXT_ROWS * MENU_TEXT_CELL_H;
#endif
    return graphics_buffer.height;
}

// Сколько строк кадрового буфера реально попадает в растр. Границы те же, что
// и в вычислении y при отрисовке строки. У NTSC строк заметно меньше высоты
// буфера, и без этого низ окон оказывается за пределами экрана.
// Строка буфера y попадает на строку растра y + 23 + shift_y, то есть рост
// shift_y опускает картинку вниз — знак уже такой, какой нужен наружу.
int graphics_get_picture_shift_y() {
#ifdef PICO_RP2040
    if (menu_text_active()) return 0;
#endif
    return graphics_buffer.shift_y;
}

int graphics_get_picture_shift_x() {
#ifdef PICO_RP2040
    if (menu_text_active()) return 0;
#endif
    return graphics_buffer.shift_x;
}

uint32_t graphics_get_visible_height() {
#ifdef PICO_RP2040
    if (menu_text_active())
        return MENU_TEXT_ROWS * MENU_TEXT_CELL_H;
#endif
    int last;
    switch (tv_out_mode.N_lines) {
        case _624_lines:
        case _625_lines:
            last = 309 - 23;   // line_active < 310, y = line_active - 23
            break;
        default:
            last = 261 - 20;   // line_active < 262, y = line_active - 20
            break;
    }
    last -= graphics_buffer.shift_y;
    if (last < 0)
        return 0;
    if ((uint32_t)(last + 1) > graphics_buffer.height)
        return graphics_buffer.height;
    return (uint32_t)(last + 1);
}

uint8_t* graphics_get_frame() {
    return graphics_framebuffer;
}

uint32_t graphics_get_font_width() {
#ifdef PICO_RP2040
    if (menu_text_active())
        return MENU_TEXT_CELL_W;
#endif
    return 8;
}

uint32_t graphics_get_font_height() {
#ifdef PICO_RP2040
    if (menu_text_active())
        return MENU_TEXT_CELL_H;
#endif
    return 8;
}

void graphics_set_duplicateLines(bool v) {
    (void)v;
}

// Сдвиг картинки с клавиатуры (серые + - * / на цифровом блоке).
// В патче X это были заглушки, поэтому для TV кнопки не работали.
// Для NTSC особенно нужно: реальных строк растра там меньше, чем строк кадра.
/*
 * Composite chroma is encoded with four DAC samples per subcarrier period.
 * Moving the picture by a non-multiple of four rotates the chroma phase and
 * visibly changes colours, so horizontal movement is quantised to one full
 * colour cycle.
 */
void graphics_inc_x(void) {
#ifdef PICO_RP2040
    if (menu_text_active()) return;
#endif
    graphics_buffer.shift_x += 4;
}
void graphics_dec_x(void) {
#ifdef PICO_RP2040
    if (menu_text_active()) return;
#endif
    graphics_buffer.shift_x -= 4;
}
void graphics_inc_y(void) {
#ifdef PICO_RP2040
    if (menu_text_active()) return;
#endif
    graphics_buffer.shift_y++;
}
void graphics_dec_y(void) {
#ifdef PICO_RP2040
    if (menu_text_active()) return;
#endif
    graphics_buffer.shift_y--;
}

static inline void _plot(int32_t x, int32_t y, uint8_t color) {
    if (!graphics_framebuffer) return;
    if (x < 0 || x >= (int32_t)graphics_buffer.width) return;
    if (y < 0 || y >= (int32_t)graphics_buffer.height) return;
    graphics_framebuffer[graphics_buffer.stride * y + x] = color;
}

static void tv_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color) {
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
            _plot(x0 + (x1 > x0 ? xi : -xi), y0 + (y1 > y0 ? xi * dy / dx : -(xi * dy / dx)), color);
    } else {
        for (int32_t yi = 0; yi <= dy; ++yi)
            _plot(x0 + (x1 > x0 ? yi * dx / dy : -(yi * dx / dy)), y0 + (y1 > y0 ? yi : -yi), color);
    }
}

void graphics_rect(int32_t x0, int32_t y0, uint32_t width, uint32_t height, uint8_t color) {
#ifdef PICO_RP2040
    if (menu_text_active()) {
        const int x1 = (x0 + (int32_t)width) / MENU_TEXT_CELL_W;
        const int y1 = (y0 + (int32_t)height) / MENU_TEXT_CELL_H;
        const int x = x0 / MENU_TEXT_CELL_W;
        const int y = y0 / MENU_TEXT_CELL_H;
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
#endif
    const int32_t x1 = x0 + (int32_t)width;
    const int32_t y1 = y0 + (int32_t)height;
    tv_line(x0, y0, x1, y0, color);
    tv_line(x1, y0, x1, y1, color);
    tv_line(x1, y1, x0, y1, color);
    tv_line(x0, y1, x0, y0, color);
}

void graphics_fill(int32_t x0, int32_t y0, uint32_t width, uint32_t height, uint8_t bgcolor) {
#ifdef PICO_RP2040
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
        for (int cy = cy0; cy <= cy1; ++cy)
            for (int cx = cx0; cx <= cx1; ++cx) {
                const uint8_t fg =
                    (bg == 0 || bg == 1 || bg == 4 ||
                     bg == 5 || bg == 8) ? 7u : 0u;
                menu_text_put_cell(
                    cx, cy, ' ',
                    (uint8_t)((bg << 4) | fg));
            }
        return;
    }
#endif
    const int32_t x1 = x0 + (int32_t)width;
    const int32_t y1 = y0 + (int32_t)height;
    for (int32_t xi = x0; xi <= x1; ++xi)
        tv_line(xi, y0, xi, y1, bgcolor);
}

void graphics_type(int x, int y, uint8_t color, const char* msg, size_t msg_len) {
#ifdef PICO_RP2040
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
#endif
    for (size_t i = 0; i < msg_len; ++i) {
        const uint8_t ch = (uint8_t)msg[i];
        const uint8_t* glyph = font_8x8 + ch * 8;
        const uint32_t xt = x + i * graphics_get_font_width();
        for (uint32_t j = 0; j < graphics_get_font_height(); ++j) {
            const uint8_t row = glyph[j];
            for (uint32_t k = 0; k < 8; ++k)
                if (row & (1u << k))
                    _plot(xt + k, y + j, color);
        }
    }
}
