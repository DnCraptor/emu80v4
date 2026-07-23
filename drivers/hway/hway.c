#include <pico.h>
#ifdef HWAY
#include "hway.h"
#include <stdbool.h>
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "pico/time.h"

// --- ноги (по умолчанию = ноги I2S + такт AY следом). Переопределяются в
//     заголовке платы под реальную разводку. ---
#ifndef HWAY_DATA_PIN
#define HWAY_DATA_PIN   AUDIO_DATA_PIN         // 595 SER  (данные)
#endif
#ifndef HWAY_CLK_PIN
#define HWAY_CLK_PIN    AUDIO_CLOCK_PIN        // 595 SRCLK (тактирование сдвига)
#endif
#ifndef HWAY_LATCH_PIN
#define HWAY_LATCH_PIN  (AUDIO_CLOCK_PIN + 1)  // 595 RCLK  (защёлка)
#endif
#ifndef HWAY_AYCLK_PIN
#define HWAY_AYCLK_PIN  (AUDIO_CLOCK_PIN + 2)  // тактовый вход AY (~1.75 МГц)
#endif

// масштаб суммарного семпла микшера -> 8 бит ЦАП. Подбирается на железе.
#ifndef HWAY_DAC_SHIFT
#define HWAY_DAC_SHIFT  8
#endif

// --- 16-битное управляющее слово (два 595). Разрядка как в PICO-BK, чтобы
//     работало то же железо TurboSound. Биты 0..7 — шина данных AY. ---
#define CS_SAA1099 (1u << 15)
#define AY_Enable  (1u << 14)
#define SAVE       (1u << 13)
#define Beeper     (1u << 12)
#define CS_AY1     (1u << 11)
#define CS_AY0     (1u << 10)
#define BDIR       (1u << 9)
#define BC1        (1u << 8)

static uint16_t ctrl = 0;
// Psg3910::reset() выполняется при построении машины, то есть раньше
// init_sound(). До hway_init() ноги не настроены, поэтому все обращения к
// чипу до этого момента игнорируются.
static bool s_ready = false;

#define LO(x) (ctrl &= ~(uint16_t)(x))
#define HI(x) (ctrl |= (uint16_t)(x))

static void __not_in_flash_func(send595)(uint16_t data) {
    for (int i = 0; i < 16; i++) {
        gpio_put(HWAY_CLK_PIN, 0);
        gpio_put(HWAY_CLK_PIN, 0);
        gpio_put(HWAY_DATA_PIN, (data & 0x8000) != 0);
        data <<= 1;
        gpio_put(HWAY_CLK_PIN, 1);
        gpio_put(HWAY_CLK_PIN, 1);
    }
    gpio_put(HWAY_LATCH_PIN, 1);
    gpio_put(HWAY_LATCH_PIN, 1);
    gpio_put(HWAY_CLK_PIN, 0);
    gpio_put(HWAY_LATCH_PIN, 0);
}

static void ayclk_init(void) {
    gpio_set_function(HWAY_AYCLK_PIN, GPIO_FUNC_PWM);
    unsigned slice = pwm_gpio_to_slice_num(HWAY_AYCLK_PIN);
    pwm_config c = pwm_get_default_config();
    // wrap=3, уровень=2 -> меандр clk_sys/(4*div). div даёт ~1.75 МГц.
    pwm_config_set_clkdiv(&c, clock_get_hz(clk_sys) / (4.0f * 1750000.0f));
    pwm_config_set_wrap(&c, 3);
    pwm_init(slice, &c, true);
    pwm_set_gpio_level(HWAY_AYCLK_PIN, 2);
}

void hway_init(void) {
    ayclk_init();
    gpio_init(HWAY_CLK_PIN);   gpio_set_dir(HWAY_CLK_PIN, GPIO_OUT);
    gpio_init(HWAY_DATA_PIN);  gpio_set_dir(HWAY_DATA_PIN, GPIO_OUT);
    gpio_init(HWAY_LATCH_PIN); gpio_set_dir(HWAY_LATCH_PIN, GPIO_OUT);
    s_ready = true;
    hway_reset();
}

void hway_reset(void) {
    if (!s_ready) return;
    // Разрешить чипы, снять все стробы.
    send595(HI(AY_Enable | CS_SAA1099 | CS_AY0 | CS_AY1 | Beeper | BDIR | BC1 | SAVE));
    // Разово настроить port B второго чипа (AY1) на вывод: R7 бит7 = 1.
    HI(CS_AY1); LO(CS_AY0);
    send595(HI(BDIR | BC1) | 7);            // защёлка R7
    send595(LO(BDIR | BC1) | 7);
    send595(LO(BDIR) | 0x80);               // запись R7 = portB out
    send595(HI(BDIR) | 0x80);
    send595(LO(BDIR) | 0x80);
    send595(HI(BDIR | BC1) | 15);           // оставить выбранным R15 (portB)
    send595(LO(BDIR | BC1) | 15);
}

// --- AY0: защёлка регистра и запись данных (шинный цикл AY-3-8910) ---
void __not_in_flash_func(hway_ay_address)(uint8_t reg) {
    if (!s_ready) return;
    HI(CS_AY0); LO(CS_AY1);                 // выбрать первый чип
    reg &= 0x0F;
    send595(HI(BDIR | BC1) | reg);          // latch address
    send595(LO(BDIR | BC1) | reg);
}

void __not_in_flash_func(hway_ay_data)(uint8_t val) {
    if (!s_ready) return;
    send595(LO(BDIR) | val);                // write to reg (BC1=0, импульс BDIR)
    send595(HI(BDIR) | val);
    send595(LO(BDIR) | val);
}

// --- суммарный не-AY звук -> port B AY1 как 8-битный ЦАП ---
void __not_in_flash_func(hway_dac_out)(int16_t l, int16_t r) {
    if (!s_ready) return;
    int v = (((int)l + (int)r) >> (HWAY_DAC_SHIFT + 1)) + 128;
    if (v < 0) v = 0; else if (v > 255) v = 255;
    static int last = -1;
    if (v == last) return;                  // пишем только при изменении
    last = v;
    HI(CS_AY1); LO(CS_AY0);
    send595(HI(BDIR | BC1) | 15);           // выбрать R15 (portB) AY1
    send595(LO(BDIR | BC1) | 15);
    send595(LO(BDIR) | (uint8_t)v);
    send595(HI(BDIR) | (uint8_t)v);
    send595(LO(BDIR) | (uint8_t)v);
}
#endif // HWAY
