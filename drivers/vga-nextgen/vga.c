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
volatile static bool one_bit_buffer = false;
static int client_buffer_width = 320;
static int client_buffer_height = 240;
static int graphics_buffer_width = 640;
static int graphics_buffer_height = 480;
static int graphics_buffer_shift_x = 0;
static int graphics_buffer_shift_y = 0;

static bool is_flash_line = false;
static bool is_flash_frame = false;

static uint32_t bg_color[2];
static uint16_t palette16_mask = 0;

static uint16_t txt_palette[16];

//буфер 2К текстовой палитры для быстрой работы
static uint16_t* txt_palette_fast = NULL;
//static uint16_t txt_palette_fast[256*4];

enum graphics_mode_t graphics_mode;

#define bitRead(value, bit) (((value) >> (bit)) & 0x01)

void __time_critical_func() dma_handler_VGA() {
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
        if (screen_line == N_lines_visible | screen_line == N_lines_visible + 3) {
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

    if (!input_buffer) {
        dma_channel_set_read_addr(dma_chan_ctrl, &lines_pattern[0], false);
        return;
    } //если нет видеобуфера - рисуем пустую строку

    int y, line_number;

    uint32_t* * output_buffer = &lines_pattern[2 + (screen_line & 1)];
    switch (graphics_mode) {
        case GRAPHICSMODE_DEFAULT:
        case GMODE_800_600:
        case GMODE_1024_768:
            if (screen_line % 2) return;
            line_number = screen_line / 2;
            y = line_number + graphics_buffer_shift_y;
            break;
        default: {
            dma_channel_set_read_addr(dma_chan_ctrl, &lines_pattern[0], false);
            return;
        }
    }

    if (y < 0 || y >= client_buffer_height) {
        // заполнение линии цветом фона
        dma_channel_set_read_addr(dma_chan_ctrl, &lines_pattern[0], false);
        return;
    };
    //зона прорисовки изображения
    //начальные точки буферов
    uint8_t* input_buffer_8bit = input_buffer + ( one_bit_buffer ? ((y * client_buffer_width) >> 3) : y * client_buffer_width);

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
    if (xoff2 > graphics_buffer_width) xoff2 = graphics_buffer_width;
    switch (graphics_mode) {
        case GRAPHICSMODE_DEFAULT:
        case GMODE_800_600:
        case GMODE_1024_768:
            for  (register int x = 0; x < xoff1; ++x) {
                *output_buffer_8bit++ = 0xC0;
            }
            if (one_bit_buffer) {
                if (duplicatePixels) {
                    for  (register int x = xoff1 < 0 ? -xoff1 / 2 : 0; x < (width >> 1); ++x) {
                        register uint8_t c = (bitRead(input_buffer_8bit[x >> 3], (x & 7)) ? 0xFF : 0xC0);
                        *output_buffer_8bit++ = c;
                        *output_buffer_8bit++ = c;
                    }
                } else {
                    for  (register int x = xoff1 < 0 ? -xoff1 : 0; x < width; ++x) {
                        *output_buffer_8bit++ = (bitRead(input_buffer_8bit[x >> 3], (x & 7)) ? 0xFF : 0xC0);
                    }
                }
            } else {
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
            }
            for  (register int x = 0; x < xoff2; ++x) {
                *output_buffer_8bit++ = 0xC0;
            }
            break;
        default:
            break;
    }
    dma_channel_set_read_addr(dma_chan_ctrl, output_buffer, false);
}

static void adjust_shift_x() {
    if (client_buffer_width * 2 <= graphics_buffer_width)
        graphics_buffer_shift_x = (graphics_buffer_width - (client_buffer_width << 1)) >> 1;
    else
        graphics_buffer_shift_x = (graphics_buffer_width - client_buffer_width) >> 1;
}
static void adjust_shift_y() {
    graphics_buffer_shift_y = (client_buffer_height - (graphics_buffer_height >> 1)) >> 1;
}

enum graphics_mode_t graphics_get_mode() {
    return graphics_mode;
}

void graphics_set_mode(enum graphics_mode_t mode) {
    if (_SM_VGA < 0) return; // если  VGA не инициализирована -

    graphics_mode = mode;

    // Если мы уже проиницилизированы - выходим
    if (txt_palette_fast && lines_pattern_data) {
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

    switch (graphics_mode) {
        case GRAPHICSMODE_DEFAULT:
            graphics_buffer_width = 640;
            graphics_buffer_height = 480;
            TMPL_LINE8 = 0b11000000;
            HS_SHIFT = 328 * 2;
            HS_SIZE = 48 * 2;
            line_size = 400 * 2;
            shift_picture = line_size - HS_SHIFT;
            palette16_mask = 0xc0c0;
            visible_line_size = 320;
            N_lines_total = 525;
            N_lines_visible = 480;
            line_VS_begin = 490;
            line_VS_end = 491;
            fdiv = clock_get_hz(clk_sys) / 25175000.0; //частота пиксельклока
            break;
        case GMODE_800_600:
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
            break;
        case GMODE_1024_768:
            graphics_buffer_width = 1024;
            graphics_buffer_height = 768;
            TMPL_LINE8 = 0b11000000;
            // XGA Signal 1024 x 768 @ 60 Hz timing
            HS_SHIFT = 1024 + 24; // Front porch + Visible area
            HS_SIZE = 160; // Back porch
            line_size = 1344;
            shift_picture = line_size - HS_SHIFT;
            visible_line_size = 1024 / 2;
            N_lines_visible = 768;
            line_VS_begin = 768 + 3; // + Front porch
            line_VS_end = 768 + 3 + 6; // ++ Sync pulse 2?
            N_lines_total = 806; // Whole frame
            fdiv = clock_get_hz(clk_sys) / 65000000;  // частота пиксельклока 65.0 MHz
            break;
        default:
            return;
    }
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

void graphics_set_1bit_buffer(uint8_t* buffer, const uint16_t width, const uint16_t height) {
    graphics_buffer = buffer;
    if (client_buffer_width != width) {
        client_buffer_width = width;
        adjust_shift_x();
    }
    if (client_buffer_height != height) {
        client_buffer_height = height;
        adjust_shift_y();
    }
    one_bit_buffer = true;
}

void graphics_set_buffer(uint8_t* buffer, const uint16_t width, const uint16_t height) {
    graphics_buffer = buffer;
    if (client_buffer_width != width) {
        client_buffer_width = width;
        adjust_shift_x();
    }
    if (client_buffer_height != height) {
        client_buffer_height = height;
        adjust_shift_y();
    }
    one_bit_buffer = false;
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
    //инициализация палитры по умолчанию
    //текстовая палитра
    for (int i = 0; i < 16; i++) {
        const uint8_t b = i & 1 ? (i >> 3 ? 3 : 2) : 0;
        const uint8_t r = i & 4 ? (i >> 3 ? 3 : 2) : 0;
        const uint8_t g = i & 2 ? (i >> 3 ? 3 : 2) : 0;

        const uint8_t c = r << 4 | g << 2 | b;

        txt_palette[i] = c & 0x3f | 0xc0;
    }
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
    graphics_set_mode(GMODE_1024_768);
    dma_channel_set_irq0_enabled(dma_chan_ctrl, true);
    irq_set_enabled(VGA_DMA_IRQ, true);
    dma_start_channel_mask(1u << dma_chan);
}

uint32_t graphics_get_width() {
    return client_buffer_width;
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
    register uint32_t idx = w * y + x;
    if (one_bit_buffer) {
        bitWrite(graphics_buffer[idx >> 3], idx & 7, (((color >> 4) & 3) > 1) && (((color >> 2) & 3) > 1) && ((color & 3) > 1));
    } else {
        graphics_buffer[idx] = color;
    }
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
