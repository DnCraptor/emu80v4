#pragma once
#if PICO_RP2350
#include "boards/pico2.h"

#else
#include "boards/pico.h"
#endif

#define ZERO 1
#define CPU_FREQ 252

// SDCARD
#define SDCARD_PIN_SPI0_SCK 18
#define SDCARD_PIN_SPI0_MOSI 19
#define SDCARD_PIN_SPI0_MISO 20
#define SDCARD_PIN_SPI0_CS 21

// PS2KBD
#define PS2KBD_GPIO_FIRST 0

// NES Gamepad
#define NES_GPIO_CLK 7
#define NES_GPIO_LAT 8
#define NES_GPIO_DATA 9
#define NES_GPIO_DATA2 10

//#define NES_GPIO_DATA 28

// VGA 8 pins starts from pin:
#define VGA_BASE_PIN 22

// HDMI 8 pins starts from pin:
#define HDMI_BASE_PIN 22

// TFT
#define TFT_CS_PIN 22
#define TFT_RST_PIN 24
#define TFT_LED_PIN 25
#define TFT_DC_PIN 26
#define TFT_DATA_PIN 28
#define TFT_CLK_PIN 29

#define SMS_SINGLE_FILE 1

// Sound
#if defined(AUDIO_PWM)
#define AUDIO_PWM_PIN 11
/// TODO: remove it
#define AUDIO_DATA_PIN 11
#define AUDIO_CLOCK_PIN 12
#define AUDIO_LCK_PIN 13
#else
// I2S Sound
#define AUDIO_DATA_PIN 11
#define AUDIO_CLOCK_PIN 12
#define AUDIO_LCK_PIN 13
#endif

// Emu80-specific peripherals
#define KBD_CLOCK_PIN PS2KBD_GPIO_FIRST
#define KBD_DATA_PIN (PS2KBD_GPIO_FIRST + 1)
#define PWM_PIN0 AUDIO_DATA_PIN
#define PWM_PIN1 AUDIO_CLOCK_PIN
#define BEEPER_PIN 0
#define SOUND_FREQUENCY 50000
#define I2S_FREQUENCY 96000
#define DEFAULT_THROTTLING 0

