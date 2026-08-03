#include <string.h>

#include "graphics.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "font8x8.h"

#define TV_RGB888(r, g, b) \
    (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

#define SCREEN_WIDTH (320)
#define SCREEN_HEIGHT (240)

static uint8_t map64colors[64] = { 0 };
static uint8_t color_map64[64];
static uint8_t bw_map64[64];

static uint8_t* framebuffer = NULL;
static uint16_t framebuffer_stride = SCREEN_WIDTH;

//программы PIO

//программа конвертации адреса для TV_OUT
uint16_t pio_program_instructions_conv_TV[] = {
    //	 .wrap_target
    0x80a0, //  0: pull   block
    0x40e8, //  1: in	 osr, 8
    0x4037, //  2: in	 x, 23
    0x8020, //  3: push   block
    //	 .wrap
};

const struct pio_program pio_program_conv_addr_TV = {
    .instructions = pio_program_instructions_conv_TV,
    .length = 4,
    .origin = -1,
};

//программа видеовывода VGA
static uint16_t pio_program_TV_instructions[] = {
    //	 .wrap_target
    0x6008, //  0: out	pins, 8
    //	 .wrap
};

static const struct pio_program program_pio_TV = {
    .instructions = pio_program_TV_instructions,
    .length = 1,
    .origin = -1,
};

typedef struct {
    int H_len;
    int begin_img_shx;
    int img_size_x;

    int N_lines;

    int sync_size;
    uint8_t SYNC_TMPL;
    uint8_t NO_SYNC_TMPL;

    double CLK_SPD;
} TV_MODE;

typedef struct {
    uint width;
    uint height;
    int shift_x;
    int shift_y;
} graphics_buffer_t;

//режим видеовыхода
static TV_MODE v_mode = {
    .H_len = 512,
    .N_lines = 525,
    .SYNC_TMPL = 241,
    .NO_SYNC_TMPL = 240,
    .CLK_SPD = 31500000.0
};

static graphics_buffer_t graphics_buffer = {
    .shift_x = 0,
    .shift_y = 0,
    .width = SCREEN_WIDTH,
    .height = SCREEN_HEIGHT,
};

//буферы строк
//количество буферов задавать кратно степени двойки
//
#define N_LINE_BUF_log2 (2)
#define N_LINE_BUF_DMA (1<<N_LINE_BUF_log2)
#define N_LINE_BUF (N_LINE_BUF_DMA)

//максимальный размер строки
#define LINE_SIZE_MAX (800)

//указатели на буферы строк
//выравнивание нужно для кольцевого буфера
static uint32_t rd_addr_DMA_CTRL[N_LINE_BUF * 2]__attribute__ ((aligned (4*N_LINE_BUF_DMA)));
//непосредственно буферы строк

static uint32_t lines_buf[N_LINE_BUF][LINE_SIZE_MAX / 4];


static int SM_video = -1;
static int SM_conv = -1;


//DMA каналы
//каналы работы с первичным графическим буфером
static int dma_chan_ctrl = -1;
static int dma_chan = -1;
//каналы работы с конвертацией палитры
static int dma_chan_pal_conv_ctrl = -1;
static int dma_chan_pal_conv = -1;

//ДМА палитра для конвертации
static __aligned(512) __scratch_x("palette_conv") uint32_t conv_color[128];

static enum graphics_mode_t graphics_mode = GRAPHICSMODE_DEFAULT;
static output_format_e active_output_format;
static repeating_timer_t video_timer;


//программа установки начального адреса массива-конвертора
static void pio_set_x(PIO pio, const int sm, const uint32_t v) {
    const uint instr_shift = pio_encode_in(pio_x, 4);
    const uint instr_mov = pio_encode_mov(pio_x, pio_isr);
    for (int i = 0; i < 8; i++) {
        const uint32_t nibble = v >> i * 4 & 0xf;
        pio_sm_exec(pio, sm, pio_encode_set(pio_x, nibble));
        pio_sm_exec(pio, sm, instr_shift);
    }
    pio_sm_exec(pio, sm, instr_mov);
}


//определение палитры
void graphics_set_palette(uint8_t i, uint32_t color888) {
    if (i >= 240) return;
    uint8_t conv0[] = { 0b00, 0b00, 0b01, 0b10, 0b10, 0b10, 0b11, 0b11 };
    uint8_t conv1[] = { 0b00, 0b01, 0b01, 0b01, 0b10, 0b11, 0b11, 0b11 };

    uint8_t B = (color888 & 0xff) / 42;
    uint8_t G = (color888 >> 8 & 0xff) / 42;
    uint8_t R = (color888 >> 16 & 0xff) / 42;

    uint8_t c_hi = conv0[R] << 4 | conv0[G] << 2 | conv0[B];
    uint8_t c_lo = conv1[R] << 4 | conv1[G] << 2 | conv1[B];

    uint16_t palette16_mask = 0xc0 << 8 | 0xc0;

    uint16_t* conv_color16 = (uint16_t *)conv_color;
    conv_color16[i] = (c_hi << 8 | c_lo) & 0x3f3f | palette16_mask;
}

static inline uint8_t* getLineBuffer(int line)
{
    if (!framebuffer || line < 0 || line >= (int)graphics_buffer.height)
        return NULL;
    return framebuffer + (size_t)line * framebuffer_stride;
}

// Заполнение строкового буфера из SRAM. Вызов libc memset() из
// __scratch_x-функции возвращал исполнение во Flash на каждом участке строки.
static inline void __scratch_x("tv_fill8")
tv_fill8(uint8_t* dst, uint8_t value, size_t count)
{
    while (count--)
        *dst++ = value;
}

//основная функция заполнения буферов видеоданных
static void __scratch_x("tv_main_loop") main_video_loopTV() {
    static uint dma_inx_out = 0;
    static uint lines_buf_inx = 0;

    if (dma_chan_ctrl == -1) return; //не определен дма канал

    //получаем индекс выводимой строки
    uint dma_inx = (N_LINE_BUF_DMA - 2 + (dma_channel_hw_addr(dma_chan_ctrl)->read_addr - (uint32_t)rd_addr_DMA_CTRL) /
                    4) % (N_LINE_BUF_DMA);

    //uint n_loop=(N_LINE_BUF_DMA+dma_inx-dma_inx_out)%N_LINE_BUF_DMA;

    static uint32_t line_active = 0;
    static uint8_t* input_buffer = NULL;
    static uint32_t frame_i = 0;

    //while(n_loop--)
    while (dma_inx_out != dma_inx) {
        //режим VGA
        line_active++;
        if (line_active == v_mode.N_lines) {
            line_active = 0;
            frame_i++;
        }
        lines_buf_inx = (lines_buf_inx + 1) % N_LINE_BUF;
        uint8_t* output_buffer = (uint8_t *)lines_buf[lines_buf_inx];

        bool is_line_visible = true;

        // if (false)
        switch (active_output_format) {
            case TV_OUT_PAL:
                switch (line_active) {
                    case 0:
                    case 1:
                        //|___|--|___|--| type=1
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, (v_mode.H_len / 2) - v_mode.sync_size);
                        output_buffer += (v_mode.H_len / 2) - v_mode.sync_size;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, v_mode.sync_size);
                        output_buffer += v_mode.sync_size;
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, (v_mode.H_len / 2) - v_mode.sync_size);
                        output_buffer += (v_mode.H_len / 2) - v_mode.sync_size;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, v_mode.sync_size);
                        is_line_visible = false;
                        break;

                    case 2:
                        // ____|--|_|----type=2
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, (v_mode.H_len / 2) - v_mode.sync_size);
                        output_buffer += (v_mode.H_len / 2) - v_mode.sync_size;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, v_mode.sync_size);
                        output_buffer += v_mode.sync_size;
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size / 2);
                        output_buffer += v_mode.sync_size / 2;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len / 2) - (v_mode.sync_size / 2));
                        is_line_visible = false;
                        break;
                    case 3:
                    case 4: //|_|----|_|---- type=0
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size / 2);
                        output_buffer += v_mode.sync_size / 2;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len / 2) - (v_mode.sync_size / 2));
                        output_buffer += (v_mode.H_len / 2) - (v_mode.sync_size / 2);
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size / 2);
                        output_buffer += v_mode.sync_size / 2;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len / 2) - (v_mode.sync_size / 2));
                        is_line_visible = false;
                        break;

                    case 5: break; //шаблон как у видимой строки, но без изображения


                    case 310:
                    case 311:
                        //|_|----|_|---- type=0
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size / 2);
                        output_buffer += v_mode.sync_size / 2;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len / 2) - (v_mode.sync_size / 2));
                        output_buffer += (v_mode.H_len / 2) - (v_mode.sync_size / 2);
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size / 2);
                        output_buffer += v_mode.sync_size / 2;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len / 2) - (v_mode.sync_size / 2));
                        is_line_visible = false;
                        break;
                    case 312:
                        //|_|---|____|--| type=3
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size / 2);
                        output_buffer += v_mode.sync_size / 2;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len / 2) - (v_mode.sync_size / 2));
                        output_buffer += (v_mode.H_len / 2) - (v_mode.sync_size / 2);
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, (v_mode.H_len / 2) - v_mode.sync_size);
                        output_buffer += (v_mode.H_len / 2) - v_mode.sync_size;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, v_mode.sync_size);
                        is_line_visible = false;
                        break;
                    case 313:
                    case 314:
                        //|___|--|___|--| type=1
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, (v_mode.H_len / 2) - v_mode.sync_size);
                        output_buffer += (v_mode.H_len / 2) - v_mode.sync_size;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, v_mode.sync_size);
                        output_buffer += v_mode.sync_size;
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, (v_mode.H_len / 2) - v_mode.sync_size);
                        output_buffer += (v_mode.H_len / 2) - v_mode.sync_size;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, v_mode.sync_size);
                        is_line_visible = false;
                        break;
                    case 315:
                    case 316:
                        //|_|----|_|---- type=0
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size / 2);
                        output_buffer += v_mode.sync_size / 2;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len / 2) - (v_mode.sync_size / 2));
                        output_buffer += (v_mode.H_len / 2) - (v_mode.sync_size / 2);
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size / 2);
                        output_buffer += v_mode.sync_size / 2;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len / 2) - (v_mode.sync_size / 2));
                        is_line_visible = false;
                        break;
                    case 317:
                        //|_|---------type=4
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size / 2);
                        output_buffer += v_mode.sync_size / 2;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len) - (v_mode.sync_size / 2));
                        is_line_visible = false;
                        break;
                    case 622:
                        //|__|---|_|----type=5
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size);
                        output_buffer += v_mode.sync_size;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len / 2) - (v_mode.sync_size));
                        output_buffer += (v_mode.H_len / 2) - (v_mode.sync_size);
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size / 2);
                        output_buffer += v_mode.sync_size / 2;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len / 2) - (v_mode.sync_size / 2));
                        is_line_visible = false;
                        break;
                }
                break;
            case TV_OUT_NTSC:
                switch (line_active) {
                    case 0:
                    case 1:
                    case 2:
                        //|_|----|_|---- type=0
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size / 2);
                        output_buffer += v_mode.sync_size / 2;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len / 2) - (v_mode.sync_size / 2));
                        output_buffer += (v_mode.H_len / 2) - (v_mode.sync_size / 2);
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size / 2);
                        output_buffer += v_mode.sync_size / 2;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len / 2) - (v_mode.sync_size / 2));
                        is_line_visible = false;
                        break;
                    case 3:
                    case 4:
                    case 5:
                        //|___|--|___|--| type=1
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, (v_mode.H_len / 2) - v_mode.sync_size);
                        output_buffer += (v_mode.H_len / 2) - v_mode.sync_size;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, v_mode.sync_size);
                        output_buffer += v_mode.sync_size;
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, (v_mode.H_len / 2) - v_mode.sync_size);
                        output_buffer += (v_mode.H_len / 2) - v_mode.sync_size;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, v_mode.sync_size);
                        is_line_visible = false;
                        break;
                    case 6:
                    case 7:
                    case 8:
                        //|_|----|_|---- type=0
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size / 2);
                        output_buffer += v_mode.sync_size / 2;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len / 2) - (v_mode.sync_size / 2));
                        output_buffer += (v_mode.H_len / 2) - (v_mode.sync_size / 2);
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size / 2);
                        output_buffer += v_mode.sync_size / 2;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len / 2) - (v_mode.sync_size / 2));
                        is_line_visible = false;
                        break;

                    case 262:
                        //|__|---|_|----type=5
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size);
                        output_buffer += v_mode.sync_size;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len / 2) - (v_mode.sync_size));
                        output_buffer += (v_mode.H_len / 2) - (v_mode.sync_size);
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size / 2);
                        output_buffer += v_mode.sync_size / 2;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len / 2) - (v_mode.sync_size / 2));
                        is_line_visible = false;
                        break;
                    case 263:
                    case 264:
                        //|_|----|_|---- type=0
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size / 2);
                        output_buffer += v_mode.sync_size / 2;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len / 2) - (v_mode.sync_size / 2));
                        output_buffer += (v_mode.H_len / 2) - (v_mode.sync_size / 2);
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size / 2);
                        output_buffer += v_mode.sync_size / 2;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len / 2) - (v_mode.sync_size / 2));
                        is_line_visible = false;
                        break;
                    case 265:

                        //|_|---|____|--| type=3
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size / 2);
                        output_buffer += v_mode.sync_size / 2;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len / 2) - (v_mode.sync_size / 2));
                        output_buffer += (v_mode.H_len / 2) - (v_mode.sync_size / 2);
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, (v_mode.H_len / 2) - v_mode.sync_size);
                        output_buffer += (v_mode.H_len / 2) - v_mode.sync_size;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, v_mode.sync_size);
                        is_line_visible = false;
                        break;
                    case 266:
                    case 267:
                        //|___|--|___|--| type=1
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, (v_mode.H_len / 2) - v_mode.sync_size);
                        output_buffer += (v_mode.H_len / 2) - v_mode.sync_size;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, v_mode.sync_size);
                        output_buffer += v_mode.sync_size;
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, (v_mode.H_len / 2) - v_mode.sync_size);
                        output_buffer += (v_mode.H_len / 2) - v_mode.sync_size;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, v_mode.sync_size);
                        is_line_visible = false;
                        break;
                    case 268:

                        // ____|--|_|----type=2
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, (v_mode.H_len / 2) - v_mode.sync_size);
                        output_buffer += (v_mode.H_len / 2) - v_mode.sync_size;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, v_mode.sync_size);
                        output_buffer += v_mode.sync_size;
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size / 2);
                        output_buffer += v_mode.sync_size / 2;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len / 2) - (v_mode.sync_size / 2));
                        is_line_visible = false;
                        break;
                    case 269:
                    case 270:
                        //|_|----|_|---- type=0
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size / 2);
                        output_buffer += v_mode.sync_size / 2;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len / 2) - (v_mode.sync_size / 2));
                        output_buffer += (v_mode.H_len / 2) - (v_mode.sync_size / 2);
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size / 2);
                        output_buffer += v_mode.sync_size / 2;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len / 2) - (v_mode.sync_size / 2));
                        is_line_visible = false;
                        break;
                    case 271:

                        //|_|---------type=4
                        tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size / 2);
                        output_buffer += v_mode.sync_size / 2;
                        tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, (v_mode.H_len) - (v_mode.sync_size / 2));
                        is_line_visible = false;
                        break;


                    default:
                        break;
                }


                break;
        }

        //ТВ строка с изображением
        if (is_line_visible) {
            tv_fill8(output_buffer, v_mode.SYNC_TMPL, v_mode.sync_size);
            tv_fill8(output_buffer + v_mode.sync_size, v_mode.NO_SYNC_TMPL, v_mode.begin_img_shx - v_mode.sync_size);
            const int post_img_clear =
                v_mode.H_len -
                v_mode.begin_img_shx -
                v_mode.img_size_x;
            tv_fill8(output_buffer + v_mode.begin_img_shx +
                       v_mode.img_size_x, v_mode.NO_SYNC_TMPL, post_img_clear);
            output_buffer += v_mode.begin_img_shx;

            int y = -1;
            switch (active_output_format) {
                case TV_OUT_PAL:
                    if ((line_active > 4) && (line_active < 310)) { y = line_active - 23; };
                    if ((line_active > 317) && (line_active < 622)) { y = line_active - 335; };
                    y -= 24;
                    break;
                case TV_OUT_NTSC:
                    if ((line_active > 8) && (line_active < 262)) { y = line_active - 20; };
                    if ((line_active > 271)) { y = line_active - 282; };
                    break;
            }
            int source_y = -1;
            if (y >= 0 && y < SCREEN_HEIGHT && graphics_buffer.height) {
                // Строка 0 framebuffer должна оставаться видимой:
                // лишние строки обрезаются снизу, а не симметрично.
                source_y = y + graphics_buffer.shift_y;
                input_buffer = getLineBuffer(source_y);
            }
            if (source_y < 0 || input_buffer == NULL) {
                //вне изображения
                tv_fill8(output_buffer, v_mode.NO_SYNC_TMPL, v_mode.H_len - v_mode.begin_img_shx);
            }
            else {
                //зона изображения
                switch (graphics_mode) {
                    default:
                    case GRAPHICSMODE_DEFAULT: {
                        // Горизонталь выводится 1:1, без интерполяции.
                        if (input_buffer != NULL) {
                            const int output_width = v_mode.img_size_x;
                            const int source_width =
                                (int)graphics_buffer.width;
                            const int source_left =
                                (output_width - source_width) / 2;
                            const int shift_x = graphics_buffer.shift_x;

                            for (int x = 0; x < output_width; ++x) {
                                const int source_x =
                                    x - shift_x - source_left;
                                if ((unsigned)source_x >=
                                    (unsigned)source_width) {
                                    *output_buffer++ = v_mode.NO_SYNC_TMPL;
                                    continue;
                                }

                                const uint8_t c = input_buffer[source_x];
                                *output_buffer++ =
                                    map64colors[c & 0x3fu];
                            }
                        }
                        break;
                    }
                }
            }
            // //test
            // tv_fill8(out_buf8, 0, v_mode.H_len-v_mode.begin_img_shx);
            // for(int i=0;i<240;i++) *out_buf8++=i;
        }

        rd_addr_DMA_CTRL[dma_inx_out] = (uint32_t)&lines_buf[lines_buf_inx];
        //включаем заполненный буфер в данные для вывода
        dma_inx_out = (dma_inx_out + 1) % (N_LINE_BUF_DMA);
        dma_inx = (N_LINE_BUF_DMA - 2 + (dma_channel_hw_addr(dma_chan_ctrl)->read_addr - (uint32_t)rd_addr_DMA_CTRL) /
                   4) % (N_LINE_BUF_DMA);
    }
}

void graphics_set_buffer(uint8_t* buffer, const uint16_t width, const uint16_t height) {
    framebuffer = buffer;
    graphics_buffer.height = height;
    graphics_buffer.width = width;
    framebuffer_stride = width;
}

void graphics_set_line_stride(uint16_t stride) {
    framebuffer_stride = stride;
}

uint16_t graphics_get_line_stride(void) {
    return framebuffer_stride;
}

void graphics_set_offset(const int x, const int y) {
    graphics_buffer.shift_x = x;
    graphics_buffer.shift_y = y;
};

static bool __not_in_flash_func(video_timer_callbackTV(repeating_timer_t *rt)) {
    main_video_loopTV();
    return true;
}


//выделение и настройка общих ресурсов - 4 DMA канала, PIO программ и 2 SM
void tv_init(const output_format_e output_format) {
    active_output_format = output_format;

    /*
     * RGB TV не привязан к цветовой поднесущей PAL/NTSC. При 448 МГц
     * используем точный делитель PIO 18:
     *
     *   CLK_SPD = clk_sys / (2 * 18)
     *
     * После выравнивания получается 792 отсчёта на строку. Активная
     * область Вектора занимает 626 отсчётов и выводится строго 1:1.
     */
    v_mode.CLK_SPD = (double)clock_get_hz(clk_sys) / 36.0;
    v_mode.H_len = v_mode.CLK_SPD / 1e6 * 63.9;
    v_mode.H_len &= 0xfffffffc;

    v_mode.sync_size = 4.7 * v_mode.H_len / 64;
    v_mode.img_size_x = 626;
    v_mode.begin_img_shx =
        (v_mode.H_len - v_mode.img_size_x) / 2;

    switch (active_output_format) {
        case TV_OUT_NTSC:
            v_mode.N_lines = 525;
            break;

        case TV_OUT_PAL:
            v_mode.N_lines = 625;
            break;
    }

    //настройка PIO
    SM_video = pio_claim_unused_sm(PIO_VIDEO, true);
    SM_conv = pio_claim_unused_sm(PIO_VIDEO_ADDR, true);
    //выделение  DMA каналов
    dma_chan_ctrl = dma_claim_unused_channel(true);
    dma_chan = dma_claim_unused_channel(true);
    dma_chan_pal_conv_ctrl = dma_claim_unused_channel(true);
    dma_chan_pal_conv = dma_claim_unused_channel(true);

    //заполнение палитры по умолчанию(ч.б.)
    for (int ci = 0; ci < 240; ci++) graphics_set_palette(ci, (ci << 16) | (ci << 8) | ci); //

    //---------------

    uint offs_prg0 = 0;
    uint offs_prg1 = 0;
    const int base_inx = 240;

    offs_prg1 = pio_add_program(PIO_VIDEO_ADDR, &pio_program_conv_addr_TV);
    offs_prg0 = pio_add_program(PIO_VIDEO, &program_pio_TV);
    pio_set_x(PIO_VIDEO_ADDR, SM_conv, (uint32_t)conv_color >> 9);
    uint16_t* conv_color16 = (uint16_t *)conv_color;

    conv_color16[base_inx] = 0b1100000011000000; //нет синхры
    conv_color16[base_inx + 1] = 0b1000000010000000; //есть синхра


    //настройка PIO SM для конвертации

    pio_sm_config c_c = pio_get_default_sm_config();

    sm_config_set_wrap(&c_c, offs_prg1, offs_prg1 + (pio_program_conv_addr_TV.length - 1));
    sm_config_set_in_shift(&c_c, true, false, 32);

    pio_sm_init(PIO_VIDEO_ADDR, SM_conv, offs_prg1, &c_c);
    pio_sm_set_enabled(PIO_VIDEO_ADDR, SM_conv, true);

    //настройка PIO SM для вывода данных
    c_c = pio_get_default_sm_config();


    //настройка рабочей SM TV
    sm_config_set_wrap(&c_c, offs_prg0, offs_prg0 + (program_pio_TV.length - 1));
    for (int i = 0; i < 8; i++) {
        gpio_set_slew_rate(TV_BASE_PIN + i, GPIO_SLEW_RATE_FAST);
        pio_gpio_init(PIO_VIDEO, TV_BASE_PIN + i);
        gpio_set_drive_strength(TV_BASE_PIN + i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(TV_BASE_PIN + i, GPIO_SLEW_RATE_FAST);
    }
    pio_sm_set_consecutive_pindirs(PIO_VIDEO, SM_video, TV_BASE_PIN, 8, true); //конфигурация пинов на выход
    sm_config_set_out_pins(&c_c, TV_BASE_PIN, 8);

    sm_config_set_out_shift(&c_c, true, true, 16);
    sm_config_set_fifo_join(&c_c, PIO_FIFO_JOIN_TX);

    sm_config_set_clkdiv(&c_c, clock_get_hz(clk_sys) / (2 * v_mode.CLK_SPD));
    pio_sm_init(PIO_VIDEO, SM_video, offs_prg0, &c_c);
    pio_sm_set_enabled(PIO_VIDEO, SM_video, true);

    //настройки DMA

    //основной рабочий канал
    dma_channel_config cfg_dma = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_8);
    channel_config_set_chain_to(&cfg_dma, dma_chan_ctrl); // chain to other channel

    channel_config_set_read_increment(&cfg_dma, true);
    channel_config_set_write_increment(&cfg_dma, false);


    uint dreq = DREQ_PIO1_TX0 + SM_conv;
    if (PIO_VIDEO_ADDR == pio0) dreq = DREQ_PIO0_TX0 + SM_conv;
    channel_config_set_dreq(&cfg_dma, dreq);

    dma_channel_configure(
        dma_chan,
        &cfg_dma,
        &PIO_VIDEO_ADDR->txf[SM_conv], // Write address
        lines_buf[0], // read address
        v_mode.H_len / 1, //
        false // Don't start yet
    );

    //контрольный канал для основного
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
        rd_addr_DMA_CTRL, // read address
        1, //
        false // Don't start yet
    );

    //канал - конвертер палитры
    cfg_dma = dma_channel_get_default_config(dma_chan_pal_conv);

    const int n_trans_data = 1;

    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_16);


    channel_config_set_chain_to(&cfg_dma, dma_chan_pal_conv_ctrl); // chain to other channel

    channel_config_set_read_increment(&cfg_dma, false);
    channel_config_set_write_increment(&cfg_dma, false);

    dreq = DREQ_PIO1_TX0 + SM_video;
    if (PIO_VIDEO == pio0) dreq = DREQ_PIO0_TX0 + SM_video;
    channel_config_set_dreq(&cfg_dma, dreq);

    dma_channel_configure(
        dma_chan_pal_conv,
        &cfg_dma,
        &PIO_VIDEO->txf[SM_video], // Write address
        &conv_color[0], // read address
        n_trans_data, //
        false // Don't start yet
    );

    //канал управления конвертером палитры

    cfg_dma = dma_channel_get_default_config(dma_chan_pal_conv_ctrl);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_32);
    channel_config_set_chain_to(&cfg_dma, dma_chan_pal_conv); // chain to other channel

    channel_config_set_read_increment(&cfg_dma, false);
    channel_config_set_write_increment(&cfg_dma, false);

    dreq = DREQ_PIO1_RX0 + SM_conv;
    if (PIO_VIDEO_ADDR == pio0) dreq = DREQ_PIO0_RX0 + SM_conv;

    channel_config_set_dreq(&cfg_dma, dreq);

    dma_channel_configure(
        dma_chan_pal_conv_ctrl,
        &cfg_dma,
        &dma_hw->ch[dma_chan_pal_conv].read_addr, // Write address
        &PIO_VIDEO_ADDR->rxf[SM_conv], // read address
        1, //
        true // start yet
    );


    dma_start_channel_mask(1u << dma_chan_ctrl);

    int hz = 50000;

    if (!alarm_pool_add_repeating_timer_us(alarm_pool_create(2, 16),1000000 / hz, video_timer_callbackTV, NULL, &video_timer)) {
        return;
    }
};


void graphics_set_color_mode(bool colorMode)
{
    memcpy(map64colors, colorMode ? color_map64 : bw_map64,
           sizeof(map64colors));
}

void graphics_set_video_content_mode(graphics_video_content_mode_t mode)
{
    (void)mode;
    menu_video_mode = GRAPHICS_VIDEO_VECTOR;
}

void graphics_set_menu_text_mode(bool enabled) { (void)enabled; }
void menu_text_clear_for_mode(void) {}
void graphics_set_duplicateLines(bool enabled) { (void)enabled; }

void graphics_inc_x(void) { ++graphics_buffer.shift_x; }
void graphics_dec_x(void) { --graphics_buffer.shift_x; }
void graphics_inc_y(void) { ++graphics_buffer.shift_y; }
void graphics_dec_y(void) { --graphics_buffer.shift_y; }

int graphics_get_picture_shift_x(void) { return graphics_buffer.shift_x; }
int graphics_get_picture_shift_y(void) { return graphics_buffer.shift_y; }

uint32_t graphics_get_width(void) { return graphics_buffer.width; }
uint32_t graphics_get_height(void) { return graphics_buffer.height; }
uint32_t graphics_get_visible_height(void) { return graphics_buffer.height; }
uint8_t* graphics_get_frame(void) { return framebuffer; }
uint32_t graphics_get_font_width(void) { return 8; }
uint32_t graphics_get_font_height(void) { return 8; }

static inline void tv_plot(int x, int y, uint8_t color)
{
    if (!framebuffer ||
        (unsigned)x >= graphics_buffer.width ||
        (unsigned)y >= graphics_buffer.height)
        return;
    framebuffer[(size_t)y * framebuffer_stride + x] = color;
}

void plot(int x, int y, uint8_t color)
{
    tv_plot(x, y, color);
}

static inline int tv_iabs(int32_t value)
{
    const int32_t mask = value >> 31;
    return (value ^ mask) - mask;
}

void line(int x0, int y0, int x1, int y1, uint8_t color)
{
    const int dx = tv_iabs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -tv_iabs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        tv_plot(x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        const int e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void graphics_rect(
        int32_t x, int32_t y, uint32_t width, uint32_t height,
        uint8_t color)
{
    line(x, y, x + (int)width, y, color);
    line(x + (int)width, y, x + (int)width, y + (int)height, color);
    line(x + (int)width, y + (int)height, x, y + (int)height, color);
    line(x, y + (int)height, x, y, color);
}

void graphics_fill(
        int32_t x, int32_t y, uint32_t width, uint32_t height,
        uint8_t color)
{
    for (uint32_t row = 0; row <= height; ++row)
        line(x, y + (int)row, x + (int)width, y + (int)row, color);
}

void graphics_type(
        int x, int y, uint8_t color, const char* msg, size_t msg_len)
{
    for (size_t i = 0; i < msg_len; ++i) {
        const uint8_t* glyph = font_8x8 + (uint8_t)msg[i] * 8;
        for (unsigned row = 0; row < 8; ++row)
            for (unsigned bit = 0; bit < 8; ++bit)
                if (glyph[row] & (1u << bit))
                    tv_plot(x + (int)i * 8 + (int)bit,
                            y + (int)row, color);
    }
}

void graphics_init() {
    tv_init(TV_OUT_PAL);

    /*
     * Use the same native RGB222 palette as VGA: framebuffer bits are
     * RR GG BB and each two-bit component expands to 0, 85, 170 or 255.
     * Keep a one-to-one mapping instead of collapsing 64 colours into the
     * inherited 16-colour Spectrum table.
     */
    static const uint8_t level[4] = {0, 85, 170, 255};

    for (uint8_t color = 0; color < 64; ++color) {
        const uint8_t r = level[(color >> 4) & 3u];
        const uint8_t g = level[(color >> 2) & 3u];
        const uint8_t b = level[color & 3u];

        map64colors[color] = color;
        graphics_set_palette(color, TV_RGB888(r, g, b));
    }

    memcpy(color_map64, map64colors, sizeof(color_map64));

    graphics_set_palette(217, TV_RGB888(0x00, 0x00, 0x00));
    graphics_set_palette(218, TV_RGB888(0x55, 0x55, 0x55));
    graphics_set_palette(219, TV_RGB888(0xAA, 0xAA, 0xAA));
    graphics_set_palette(220, TV_RGB888(0xFF, 0xFF, 0xFF));

    for (unsigned color = 0; color < 64; ++color) {
        const unsigned r = (color >> 4) & 3u;
        const unsigned g = (color >> 2) & 3u;
        const unsigned b = color & 3u;
        const unsigned y =
            (77u * r + 150u * g + 29u * b + 128u) >> 8;
        bw_map64[color] = (uint8_t)(217u + y);
    }

    graphics_set_color_mode(true);
}

void graphics_set_mode(const enum graphics_mode_t mode) {
///    graphics_mode = mode;
}
