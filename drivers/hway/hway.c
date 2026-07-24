#include <pico.h>
#include <stdbool.h>
#include "hway.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "pico/time.h"

// no blocking for now, it may brek input from keyboard
//#include "hardware/sync.h"
#define save_and_disable_interrupts() 0
#define restore_interrupts(x)

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
// Пауза между соседними циклами шины AY. В pico-spec её нет, поэтому по
// умолчанию выключена. Если плотный поток записей из игры окажется
// ненадёжным, задайте 2-5 мкс и сравните.
#ifndef HWAY_BUS_GAP_US
#define HWAY_BUS_GAP_US 0
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

#ifndef HWAY_MIN_NOPS
#define HWAY_MIN_NOPS 8u      // ~20 нс при 400 МГц
#endif

static inline void __not_in_flash_func(wait_to_adjust)(uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        __asm volatile("nop");
}

static void __not_in_flash_func(send595)(uint16_t data) {
    static uint32_t wait_nops = 0;
    if (wait_nops == 0)
        wait_nops = clock_get_hz(clk_sys) / (HWAY_30MHZ * 5);
        // На 400 МГц формула даёт 2 nop: импульс CLK около 7 нс, тогда как
        // 74HC595 требует не менее 20. Держим нижнюю границу.
        if (wait_nops < HWAY_MIN_NOPS)
            wait_nops = HWAY_MIN_NOPS;
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
    // pico-spec выставляет выбор чипа заново перед каждой записью данных,
    // не полагаясь на сохранённое состояние. Повторяем.
    HI(CS_AY0); LO(CS_AY1);
    send595(LO(BDIR) | val);
    send595(HI(BDIR) | val);
    send595(LO(BDIR) | val);
#if HWAY_BUS_GAP_US
    busy_wait_us(HWAY_BUS_GAP_US);
#endif
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

// --- Очередь записей в AY с временными метками ---
// Эмуляция исполняет кадр рывком: около 65 записей выдаётся за ~8.6 мс,
// затем пауза ~12 мс. Программному микшеру это безразлично, он считает
// звук по эмулированному времени, а буфер вывода растягивает результат.
// Реальная микросхема буфера не имеет и играет то, что пришло, тогда,
// когда пришло: оцифровка проигрывается втрое быстрее и обрывается
// паузой. Поэтому записи складываются сюда с эмулированным временем и
// отдаются в чип по реальному, восстанавливая исходный темп.
#define HWAY_Q_SIZE 2048u
#define HWAY_Q_RESYNC_US 60000  // отставание, после которого сверяемся заново

typedef struct { uint32_t t; uint8_t reg; uint8_t val; } hway_ev_t;

static hway_ev_t s_q[HWAY_Q_SIZE];
static volatile uint32_t s_qw = 0;
static volatile uint32_t s_qr = 0;
static bool s_q_sync = false;
static uint32_t s_q_emu0 = 0;
static uint32_t s_q_real0 = 0;

void __not_in_flash_func(hway_ay_queue)(uint8_t reg, uint8_t val, uint32_t emu_us) {
    if (!s_ready)
        return;
    const uint32_t nw = (s_qw + 1u) % HWAY_Q_SIZE;
    if (nw == s_qr)
        return;                     // переполнение: теряем самые новые
    s_q[s_qw].t = emu_us;
    s_q[s_qw].reg = reg;
    s_q[s_qw].val = val;
    s_qw = nw;
}

void __not_in_flash_func(hway_queue_drain)(void) {
    if (!s_ready)
        return;
    while (s_qr != s_qw) {
        const uint32_t now = time_us_32();
        if (!s_q_sync) {
            s_q_sync = true;
            s_q_emu0 = s_q[s_qr].t;
            s_q_real0 = now;
        }
        const int32_t due = (int32_t)(s_q[s_qr].t - s_q_emu0)
                          - (int32_t)(now - s_q_real0);
        if (due > 0)
            break;                  // время этой записи ещё не пришло
        if (due < -HWAY_Q_RESYNC_US)
            s_q_sync = false;       // отстали слишком сильно, сверимся заново
        hway_ay_address(s_q[s_qr].reg);
        hway_ay_data(s_q[s_qr].val);
        s_qr = (s_qr + 1u) % HWAY_Q_SIZE;
    }
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

// --- Диагностика битов данных: чип сам показывает, что доехало ---
// Три канала настроены на заметно разные ноты и одинаковую громкость.
// Каждое нажатие пишет в R7 одно известное значение, разрешающее ровно
// один тон. Если звучит ожидаемый канал — младшие биты данных доезжают
// верно. Если другой или шум — биты переставлены либо искажены, и тогда
// дело в разводке второго 595, а не в логике записи.
//   шаг 0: R7 = 0xFE -> только тон A (низкая нота)
//   шаг 1: R7 = 0xFD -> только тон B (средняя)
//   шаг 2: R7 = 0xFB -> только тон C (высокая)
//   шаг 3: R7 = 0xC7 -> только шум на всех каналах (заведомое шипение)
static uint8_t s_r7_step = 0;

static const uint8_t s_r7_seq[4] = { 0xFE, 0xFD, 0xFB, 0xC7 };

void hway_r7_step(void) {
    if (!s_ready)
        return;
    if (s_r7_step == 0) {           // разовая настройка каналов
        hway_ay_address(0);  hway_ay_data(249);   // A ~440 Гц
        hway_ay_address(1);  hway_ay_data(0);
        hway_ay_address(2);  hway_ay_data(125);   // B ~880 Гц
        hway_ay_address(3);  hway_ay_data(0);
        hway_ay_address(4);  hway_ay_data(62);    // C ~1760 Гц
        hway_ay_address(5);  hway_ay_data(0);
        hway_ay_address(6);  hway_ay_data(16);    // период шума
        hway_ay_address(8);  hway_ay_data(0x0F);
        hway_ay_address(9);  hway_ay_data(0x0F);
        hway_ay_address(10); hway_ay_data(0x0F);
    }
    hway_ay_address(7);
    hway_ay_data(s_r7_seq[s_r7_step]);
    s_r7_step = (uint8_t)((s_r7_step + 1) & 3);
}

uint8_t hway_r7_state(void) { return s_r7_step; }
