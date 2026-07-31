#pragma once
// Pimoroni "Pico DV Demo Base" — плата-носитель под Raspberry Pi Pico 2 /
// Pico Plus 2. Разводка снята с pico-speccy (сборка PICO_DV, тег DVp2).
//
// Штатная периферия платы:
//   HDMI-розетка : TMDS clk GP6/7, D0 GP8/9, D1 GP10/11, D2 GP12/13
//   microSD      : SCK GP5, MOSI GP18, MISO GP19, CS GP22
//   I2S ЦАП      : PCM5100A — DIN GP26, BCK GP27, LRCK GP28 (линейный выход)
//
// Свободные выводы гребёнки: GP0..GP4, GP14..GP17, GP20, GP21 — на них
// навешаны клавиатура PS/2, NES-геймпад и вход магнитофона (см. ниже).
#if PICO_RP2350
#include "boards/pico2.h"
#else
#include "boards/pico.h"
#endif

#define PICO_DV 1
#define CPU_FREQ 400

// SDCARD.
// GP5 не является выводом аппаратного SPI (SPI0 SCK — это GP2/GP6/GP18),
// поэтому карта обслуживается программным SPI на PIO. На RP2350 берём pio2:
// pio1 уже занят (I2S — sm0, PS/2-клавиатура и NES-геймпад забирают свободные
// sm), и памяти инструкций pio1 на четвёртую программу не хватает.
#define SDCARD_PIN_SPI0_SCK 5
#define SDCARD_PIN_SPI0_MOSI 18
#define SDCARD_PIN_SPI0_MISO 19
#define SDCARD_PIN_SPI0_CS 22
#if PICO_RP2350
#define SDCARD_PIO pio2
#define SDCARD_PIO_SM 0
#else
#define SDCARD_PIO pio1
#define SDCARD_PIO_SM 3
#endif

// PS2KBD: GP14 = CLOCK, GP15 = DATA (как в pico-speccy)
#define PS2KBD_GPIO_FIRST 14

// NES Gamepad: свободные выводы гребёнки, joy2 = NES_GPIO_DATA + 1
#define NES_GPIO_CLK 16
#define NES_GPIO_LAT 17
#define NES_GPIO_DATA 20
#define NES_GPIO_DATA2 21

// VGA 8 pins starts from pin (на плате не разведён, значение — для сборки):
#define VGA_BASE_PIN 6

// HDMI 8 pins starts from pin:
#define HDMI_BASE_PIN 6

// Sound: штатный I2S ЦАП PCM5100A (LRCK = AUDIO_CLOCK_PIN + 1)
#if defined(AUDIO_PWM)
#define AUDIO_PWM_PIN 26
#endif

#define AUDIO_DATA_PIN 26
#define AUDIO_CLOCK_PIN 27
#define AUDIO_LCK_PIN 28

#define PICO_DEFAULT_LED_PIN 25

// Emu80-specific peripherals
#define KBD_CLOCK_PIN PS2KBD_GPIO_FIRST
#define KBD_DATA_PIN (PS2KBD_GPIO_FIRST + 1)
// Вход реального магнитофона — свободный GP4 (в pico-speccy это GP20, но там
// он делится с линией данных геймпада).
#define LOAD_WAV_PIO 4
#define PWM_PIN0 AUDIO_DATA_PIN
#define PWM_PIN1 AUDIO_CLOCK_PIN
#define BEEPER_PIN 0
#define SOUND_FREQUENCY 48000
#define I2S_FREQUENCY 48000
// QSPI-PSRAM: своей микросхемы на плате нет, но Pimoroni "Pico Plus 2"
// (RP2350B) несёт 8 МБ с CS на GP47. На корпусе RP2350A (обычный Pico 2)
// вывода 47 не существует — прозвонка вернёт нулевой размер, то есть PSRAM
// просто считается отсутствующей, и ни один рабочий вывод платы не занимается.
#define BUTTER_PSRAM_GPIO_RP2350A 47
#define BUTTER_PSRAM_GPIO_RP2350B 47
#define DEFAULT_THROTTLING 0
