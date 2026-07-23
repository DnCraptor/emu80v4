#include <pico.h>
#include <stdbool.h>
#include "hway.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "pico/time.h"

// --- Ноги. В PICO-BK: LATCH=26, CLK=27, DATA=28, такт AY = 21 или 29. ---
#ifndef HWAY_LATCH_PIN
#define HWAY_LATCH_PIN  AUDIO_DATA_PIN
#endif
#ifndef HWAY_CLK_PIN
#define HWAY_CLK_PIN    AUDIO_CLOCK_PIN
#endif
#ifndef HWAY_DATA_PIN
#define HWAY_DATA_PIN   (AUDIO_CLOCK_PIN + 1)
#endif
#ifndef HWAY_AYCLK_PIN
#define HWAY_AYCLK_PIN  (AUDIO_CLOCK_PIN + 2)
#endif
#ifndef HWAY_AYCLK_PIN_ALT
#define HWAY_AYCLK_PIN_ALT (AUDIO_CLOCK_PIN - 6)
#endif

// Разрядка управляющего слова — как в PinSerialData_595.h.
#define CS_SAA1099 (1u << 15)
#define AY_Enable  (1u << 14)
#define SAVE       (1u << 13)
#define Beeper     (1u << 12)
#define CS_AY1     (1u << 11)
#define CS_AY0     (1u << 10)
#define BDIR       (1u << 9)
#define BC1        (1u << 8)

static uint16_t ctrl = 0;
static bool s_ready = false;
static bool s_dac_on = true;
static uint8_t s_ayclk_mode = 1;
static bool s_test_tone = false;
static bool s_covox_test = false;

#define LO(x) (ctrl &= ~(uint16_t)(x))
#define HI(x) (ctrl |= (uint16_t)(x))

// Побитовая посылка — точная копия send_to_595 из PICO-BK, включая тройную
// запись уровней и микросекундную выдержку защёлки. Здесь ничего не
// сокращаем: именно эти тайминги проверены на живом железе.
static void __not_in_flash_func(send595)(uint16_t data) {
    for (int i = 0; i < 16; i++) {
        gpio_put(HWAY_CLK_PIN, 0);
        gpio_put(HWAY_CLK_PIN, 0);
        gpio_put(HWAY_CLK_PIN, 0);
        gpio_put(HWAY_DATA_PIN, (data & 0x8000) != 0);
        data <<= 1;
        gpio_put(HWAY_CLK_PIN, 1);
        gpio_put(HWAY_CLK_PIN, 1);
        gpio_put(HWAY_CLK_PIN, 1);
    }
    gpio_put(HWAY_LATCH_PIN, 1);
    gpio_put(HWAY_LATCH_PIN, 1);
    gpio_put(HWAY_LATCH_PIN, 1);
    busy_wait_us(1);
    gpio_put(HWAY_CLK_PIN, 0);
    gpio_put(HWAY_CLK_PIN, 0);
    gpio_put(HWAY_LATCH_PIN, 0);
    gpio_put(HWAY_LATCH_PIN, 0);
}

static unsigned ayclk_pin(void) {
    return s_ayclk_mode == 2 ? (unsigned)HWAY_AYCLK_PIN_ALT
                             : (unsigned)HWAY_AYCLK_PIN;
}

// Init_PWM_175: меандр ~1.75 МГц, wrap=3, уровень=2.
static void ayclk_init(void) {
    if (s_ayclk_mode == 0)
        return;
    const unsigned pin = ayclk_pin();
    gpio_set_function(pin, GPIO_FUNC_PWM);
    unsigned slice = pwm_gpio_to_slice_num(pin);
    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv(&c, clock_get_hz(clk_sys) / (4.0f * 1750000.0f));
    pwm_config_set_wrap(&c, 3);
    pwm_init(slice, &c, true);
    pwm_set_gpio_level(pin, 2);
}

void hway_ayclk_refresh(void) {
    if (s_ready)
        ayclk_init();
}

void hway_set_ayclk_mode(uint8_t mode) {
    if (mode > 2 || mode == s_ayclk_mode)
        return;
    if (s_ready && s_ayclk_mode != 0) {
        unsigned old = ayclk_pin();
        pwm_set_enabled(pwm_gpio_to_slice_num(old), false);
        gpio_deinit(old);
    }
    s_ayclk_mode = mode;
    if (s_ready)
        ayclk_init();
}

uint8_t hway_ayclk_mode(void) { return s_ayclk_mode; }

void hway_init(void) {
    if (s_ready)
        return;
    // Ноги 595 могут быть заняты PWM (BEEPER_PIN совпадает с линией данных
    // на murmulator), поэтому забираем их в SIO до настройки такта.
    gpio_init(HWAY_CLK_PIN);   gpio_set_dir(HWAY_CLK_PIN, GPIO_OUT);
    gpio_init(HWAY_DATA_PIN);  gpio_set_dir(HWAY_DATA_PIN, GPIO_OUT);
    gpio_init(HWAY_LATCH_PIN); gpio_set_dir(HWAY_LATCH_PIN, GPIO_OUT);
    ayclk_init();
    s_ready = true;
    hway_reset();
}

void hway_deinit(void) {
    if (!s_ready)
        return;
    s_ready = false;
    if (s_ayclk_mode != 0 && gpio_get_function(ayclk_pin()) == GPIO_FUNC_PWM) {
        unsigned slice = pwm_gpio_to_slice_num(ayclk_pin());
        pwm_set_enabled(slice, false);
        pwm_config c = pwm_get_default_config();
        pwm_init(slice, &c, false);
        gpio_deinit(ayclk_pin());
    }
    gpio_deinit(HWAY_CLK_PIN);
    gpio_deinit(HWAY_DATA_PIN);
    gpio_deinit(HWAY_LATCH_PIN);
}

// AY_reset: снять разрешение, затем выставить исходное состояние линий.
void hway_reset(void) {
    if (!s_ready)
        return;
    uint32_t irq = save_and_disable_interrupts();
    send595(LO(AY_Enable));
    send595(HI(AY_Enable | CS_SAA1099 | CS_AY0 | Beeper | CS_AY1 | BDIR | BC1 | SAVE));
    restore_interrupts(irq);
}

// AY_write_address
void __not_in_flash_func(hway_ay_address)(uint8_t reg) {
    if (!s_ready)
        return;
    const uint8_t n = reg & 0x0F;
    uint32_t irq = save_and_disable_interrupts();
    HI(CS_AY0); LO(CS_AY1);                  // выбор первого чипа
    send595(HI(BDIR | BC1) | n);
    send595(LO(BDIR | BC1) | n);
    restore_interrupts(irq);
}

// AY_set_reg
void __not_in_flash_func(hway_ay_data)(uint8_t val) {
    if (!s_ready)
        return;
    uint32_t irq = save_and_disable_interrupts();
    send595(LO(BDIR) | val);
    send595(HI(BDIR) | val);
    send595(LO(BDIR) | val);
    restore_interrupts(irq);
}

// Ковокс: полная последовательность из PICO-BK — выбор второго чипа,
// R7 = 0x80 (port B на вывод), затем R15 = значение. R7 переписывается
// каждый раз, как в оригинале. Посылка только при изменении значения.
static void __not_in_flash_func(covox_write)(uint8_t val) {
    uint32_t irq = save_and_disable_interrupts();
    HI(CS_AY1); LO(CS_AY0);
    send595(HI(BDIR | BC1) | 7);
    send595(LO(BDIR | BC1) | 7);
    send595(LO(BDIR) | 0x80);
    send595(HI(BDIR) | 0x80);
    send595(LO(BDIR) | 0x80);
    send595(HI(BDIR | BC1) | 15);
    send595(LO(BDIR | BC1) | 15);
    send595(LO(BDIR) | val);
    send595(HI(BDIR) | val);
    send595(LO(BDIR) | val);
    restore_interrupts(irq);
}

void __not_in_flash_func(hway_dac_out)(int16_t l, int16_t r) {
    if (!s_ready || !s_dac_on)
        return;
    static int latch_val = -1;
    uint8_t val;
    if (s_covox_test) {
        static uint8_t ramp = 0;
        ramp += 4;
        val = ramp;
    } else {
        int v = (((int)l + (int)r) >> 9) + 128;
        if (v < 0) v = 0; else if (v > 255) v = 255;
        val = (uint8_t)v;
    }
    if ((int)val == latch_val)
        return;
    latch_val = val;
    covox_write(val);
}

void hway_set_dac_enabled(bool on) { s_dac_on = on; }
bool hway_dac_enabled(void) { return s_dac_on; }

// --- Диагностика: тон 440 Гц в AY0, минуя эмулятор ---
void hway_test_tone(bool on) {
    if (!s_ready)
        return;
    s_test_tone = on;
    if (on) {
        hway_ay_address(0);  hway_ay_data(249);   // период A (1.75 МГц / 16 / 440)
        hway_ay_address(1);  hway_ay_data(0);
        hway_ay_address(8);  hway_ay_data(0x0F);  // громкость A
        hway_ay_address(7);  hway_ay_data(0xFE);  // разрешён только тон A
    } else {
        hway_ay_address(8);  hway_ay_data(0x00);
        hway_ay_address(7);  hway_ay_data(0xFF);
    }
}

bool hway_test_tone_on(void) { return s_test_tone; }
void hway_covox_test(bool on) { s_covox_test = on; }
bool hway_covox_test_on(void) { return s_covox_test; }
