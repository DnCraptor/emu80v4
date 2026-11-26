#pragma once
#if PICO_RP2350
#include "boards/pico2.h"

#else
#include "boards/pico.h"
#endif

// SDCARD
#define SDCARD_PIN_SPI0_CS 5
#define SDCARD_PIN_SPI0_SCK 2
#define SDCARD_PIN_SPI0_MOSI 3
#define SDCARD_PIN_SPI0_MISO 4

// PS2KBD
#define PS2KBD_GPIO_FIRST 0

// NES Gamepad
#define NES_GPIO_CLK 14
#define NES_GPIO_DATA 16
#define NES_GPIO_LAT 15

// VGA 8 pins starts from pin:
#define VGA_BASE_PIN 6

// HDMI 8 pins starts from pin:
#define HDMI_BASE_PIN 6

// TFT
#define TFT_CS_PIN 6
#define TFT_RST_PIN 8
#define TFT_LED_PIN 9
#define TFT_DC_PIN 10
#define TFT_DATA_PIN 12
#define TFT_CLK_PIN 13


// Sound
#if defined(AUDIO_PWM)
#define AUDIO_PWM_PIN 26
#endif
// I2S Sound
#define AUDIO_DATA_PIN 26
#define AUDIO_CLOCK_PIN 27

// Emu80-specific peripherals
#define KBD_CLOCK_PIN PS2KBD_GPIO_FIRST
#define KBD_DATA_PIN (PS2KBD_GPIO_FIRST + 1)
#define LOAD_WAV_PIO 22
#define PWM_PIN0 26
#define PWM_PIN1 27
#define BEEPER_PIN 28
#define SOUND_FREQUENCY 48000
#define I2S_FREQUENCY 48000
#define PSRAM 1
#define PSRAM_SPINLOCK 1
#define PSRAM_ASYNC 1
#define PSRAM_PIN_CS 18
#define PSRAM_PIN_SCK 19
#define PSRAM_PIN_MOSI 20
#define PSRAM_PIN_MISO 21
#define BUTTER_PSRAM_GPIO_RP2350A 19
#define BUTTER_PSRAM_GPIO_RP2350B 47
#define DEFAULT_THROTTLING 0

