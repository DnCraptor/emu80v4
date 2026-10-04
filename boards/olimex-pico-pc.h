#pragma once
#if PICO_RP2350
#include "boards/pico2.h"

#else
#include "boards/pico.h"
#endif

#define PICO_PC 1
#define CPU_FREQ 378

// SDCARD
#define SDCARD_PIN_SPI0_SCK 6
#define SDCARD_PIN_SPI0_MOSI 7
#define SDCARD_PIN_SPI0_MISO 4
#define SDCARD_PIN_SPI0_CS 22

// PS2KBD
#define PS2KBD_GPIO_FIRST 0

// NES Gamepad (UEXT). CLK on GPIO5 (UEXT pin 10): GPIO8 is the PSRAM CS
// on this board, as in the other PCp2 builds of the pico-class kit.
#define NES_GPIO_CLK 5
#define NES_GPIO_LAT 9
#define NES_GPIO_DATA 20
#define NES_GPIO_DATA2 21

// VGA 8 pins starts from pin:
#define VGA_BASE_PIN 12

// HDMI 8 pins starts from pin:
#define HDMI_BASE_PIN 12

// TFT
#define TFT_CS_PIN 12
#define TFT_RST_PIN 14
#define TFT_LED_PIN 15
#define TFT_DC_PIN 16
#define TFT_DATA_PIN 18
#define TFT_CLK_PIN 19

#define SMS_SINGLE_FILE 1

// Sound
#if defined(AUDIO_PWM)
#define AUDIO_PWM_PIN 26
/// TODO: remove it
#define AUDIO_DATA_PIN 26
#define AUDIO_CLOCK_PIN 27
#define AUDIO_LCK_PIN 28
#else
// I2S Sound
#define AUDIO_DATA_PIN 26
#define AUDIO_CLOCK_PIN 27
#define AUDIO_LCK_PIN 28
#endif

// Emu80-specific peripherals
#define KBD_CLOCK_PIN PS2KBD_GPIO_FIRST
#define KBD_DATA_PIN (PS2KBD_GPIO_FIRST + 1)
// Audio jack: left channel GPIO28, right channel GPIO27 (PWM_L / PWM_R on
// the board). GPIO26 is DVI_CEC and is not wired to the jack.
#define PWM_PIN0 28
#define PWM_PIN1 27
#define BEEPER_PIN 0
#define SOUND_FREQUENCY 48000
#define I2S_FREQUENCY 48000
// Optional PSRAM chip on the PICO-PC: CS on GPIO8 (GPIO19 is HDMI here)
#define BUTTER_PSRAM_GPIO_RP2350A 8
#define BUTTER_PSRAM_GPIO_RP2350B 47
#define DEFAULT_THROTTLING 0
