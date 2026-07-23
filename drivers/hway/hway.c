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
// Одна запись ковокса — 10 посылок, около 14 мкс. На частоте
// дискретизации это больше половины периода: таймер не успевает, и
// вместо сигнала получается шум. Обновляем реже — защёлка port B
// держит уровень сама.
// Время восстановления шины AY между соседними циклами. Одиночная
// операция проходит надёжно, а идущие вплотную теряются выборочно:
// какой регистр не дописался — тот и меняет звучание. В PICO-BK записи
// приходят от плеера с естественными промежутками, поэтому там это не
// проявляется. Пауза ставится в конце каждой законченной операции.
#ifndef HWAY_BUS_GAP_US
#define HWAY_BUS_GAP_US 3
#endif

#ifndef HWAY_DAC_DECIM
#define HWAY_DAC_DECIM 4
#endif

#define AY_RES     0x0300u   // сброс
#define AY_Z       0x4C00u   // снять выбор с обоих чипов
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

// В PICO-BK записи AY и ковокс идут из одного контекста и не вытесняют
// друг друга. У нас ковокс вызывается из прерывания таймера аудио и
// может влезть между защёлкой адреса и записью данных, переключив выбор
// на второй чип. Поэтому запоминаем последний адрес, выставленный на
// AY0, и восстанавливаем его, если шину успел занять ковокс.
static uint8_t s_ay0_reg = 0;
static volatile bool s_bus_dirty = false;

#define LO(x) (ctrl &= ~(uint16_t)(x))
#define HI(x) (ctrl |= (uint16_t)(x))

// Побитовая посылка — версия из pico-spec. Задержка калибруется от
// системной частоты: на 400 МГц три подряд gpio_put дают импульс около
// 7 нс, тогда как 74HC595 требует не менее 20. У PICO-BK частота ниже и
// это проходило, у нас биты терялись выборочно.
#define HWAY_30MHZ 30000000u

static inline void __not_in_flash_func(wait_to_adjust)(uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        __asm volatile("nop");
}

static void __not_in_flash_func(send595)(uint16_t data) {
    static uint32_t wait_nops = 0;
    if (wait_nops == 0)
        wait_nops = clock_get_hz(clk_sys) / (HWAY_30MHZ * 5);
    gpio_put(HWAY_CLK_PIN, 0);
    wait_to_adjust(wait_nops);
    for (int i = 0; i < 16; ++i) {
        gpio_put(HWAY_DATA_PIN, (0x8000 & data));
        data <<= 1;
        gpio_put(HWAY_CLK_PIN, 1);
        wait_to_adjust(wait_nops);
        gpio_put(HWAY_CLK_PIN, 0);
        wait_to_adjust(wait_nops);
    }
    gpio_put(HWAY_LATCH_PIN, 1);
    wait_to_adjust(wait_nops);
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
    send595(HI(AY_RES));                     // линия сброса AY
    send595(HI(AY_Z));                       // снять выбор с обоих чипов
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
    s_ay0_reg = n;
    s_bus_dirty = false;
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
    return;
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
    s_bus_dirty = true;                      // выбор уведён на второй чип
    restore_interrupts(irq);
}

void __not_in_flash_func(hway_dac_out)(int16_t l, int16_t r) {
    if (!s_ready || !s_dac_on)
        return;
    static unsigned dec = 0;
    if (++dec < HWAY_DAC_DECIM)
        return;
    dec = 0;

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
        hway_set_dac_enabled(false);
        hway_reset();
        hway_ay_address(0);  hway_ay_data(249);   // период A (1.75 МГц / 16 / 440)
        hway_ay_address(1);  hway_ay_data(0);
        hway_ay_address(8);  hway_ay_data(0x0F);  // громкость A
        hway_ay_address(7);  hway_ay_data(0xFE);  // разрешён только тон A
    } else {
        hway_ay_address(8);  hway_ay_data(0x00);
        hway_ay_address(7);  hway_ay_data(0xFF);
        hway_reset();
    }
}

bool hway_test_tone_on(void) { return s_test_tone; }
void hway_covox_test(bool on) { s_covox_test = on; }
bool hway_covox_test_on(void) { return s_covox_test; }

// --- Диагностика тракта 595 по светодиодам CS ---
// Каждое нажатие переводит управляющее слово в следующее из четырёх
// состояний. Данные не пишутся, AY не участвует, BDIR/BC1 не трогаются —
// меняются только линии выбора чипов, и ожидаемая картина известна заранее:
//   0: оба CS низкие   -> оба светодиода погашены
//   1: только CS_AY0    -> горит первый
//   2: только CS_AY1    -> горит второй
//   3: оба CS высокие   -> горят оба
// Если светодиоды повторяют этот цикл при каждом проходе, слово доезжает
// целиком и тракт детерминирован. Если картина скачет или не совпадает —
// слово приходит со сдвигом, и дело в разводке или в самой посылке.
static uint8_t s_cs_step = 0;

void hway_cs_step(void) {
    if (!s_ready)
        return;
    s_cs_step = (uint8_t)((s_cs_step + 1) & 3);
    uint32_t irq = save_and_disable_interrupts();
    LO(CS_AY0 | CS_AY1);
    if (s_cs_step & 1) HI(CS_AY0);
    if (s_cs_step & 2) HI(CS_AY1);
    send595(ctrl);
    restore_interrupts(irq);
}

uint8_t hway_cs_state(void) { return s_cs_step; }
