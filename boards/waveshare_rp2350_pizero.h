/*
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

// This header may be included by other board headers as "boards/waveshare_rp2350_pizero.h"

#ifndef _BOARDS_WAVESHARE_RP2350_PIZERO
#define _BOARDS_WAVESHARE_RP2350_PIZERO

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// For board detection
#define RASPBERRYPI_RP2350_PIZERO

// --- RP2350 VARIANT ---
#define PICO_RP2350A 0

// --- UART ---
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// --- I2C ---
#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 0
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 10
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 11
#endif

// --- SPI ---
#ifndef PICO_DEFAULT_SPI
#define PICO_DEFAULT_SPI 1
#endif
#ifndef PICO_DEFAULT_SPI_SCK_PIN
#define PICO_DEFAULT_SPI_SCK_PIN 30
#endif
#ifndef PICO_DEFAULT_SPI_TX_PIN
#define PICO_DEFAULT_SPI_TX_PIN 31
#endif
#ifndef PICO_DEFAULT_SPI_RX_PIN
#define PICO_DEFAULT_SPI_RX_PIN 40
#endif
#ifndef PICO_DEFAULT_SPI_CSN_PIN
#define PICO_DEFAULT_SPI_CSN_PIN 43
#endif

// --- FLASH ---

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (4 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (4 * 1024 * 1024)
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#define ZERO2 1

// SDCARD
#define SDCARD_SPI_BUS spi1
#define SDCARD_PIN_SPI0_SCK 30
#define SDCARD_PIN_SPI0_MOSI 31
#define SDCARD_PIN_SPI0_MISO 40
#define SDCARD_PIN_SPI0_CS 43

// PS2KBD
#define PS2KBD_GPIO_FIRST 2      // GP2 = CLOCK, GP3 = DATA

// NES Gamepad
#define NES_GPIO_CLK 4           // GP4
#define NES_GPIO_LAT 5           // GP5
#define NES_GPIO_DATA 7          // GP7 = joy1; joy2 = DATA+1 = GP8

#define LOAD_WAV_PIO 17          // GP17

// HDMI 8 pins starts from pin:
#define HDMI_BASE_PIN 32

// Sound
#if defined(AUDIO_PWM)
#define AUDIO_PWM_PIN 10
#endif

#define AUDIO_DATA_PIN 10
#define AUDIO_CLOCK_PIN 11

#define PICO_DEFAULT_LED_PIN 25


// Emu80-specific peripherals
#define KBD_CLOCK_PIN PS2KBD_GPIO_FIRST
#define KBD_DATA_PIN (PS2KBD_GPIO_FIRST + 1)
#define PWM_PIN0 AUDIO_DATA_PIN
#define PWM_PIN1 AUDIO_CLOCK_PIN
#define BEEPER_PIN 0
#define SOUND_FREQUENCY 48000
#define I2S_FREQUENCY 48000

// HWAY: сдвиговый регистр 74595 (на ногах I2S) + такт AY. Под свою разводку.
#define HWAY_DATA_PIN   AUDIO_DATA_PIN         // 595 SER
#define HWAY_CLK_PIN    AUDIO_CLOCK_PIN        // 595 SRCLK
#define HWAY_LATCH_PIN  (AUDIO_CLOCK_PIN + 1)  // 595 RCLK (была I2S LRCLK)
#define HWAY_AYCLK_PIN  (AUDIO_CLOCK_PIN + 2)  // тактовый вход AY (~1.75 МГц)
#define BUTTER_PSRAM_GPIO_RP2350A 8
#define BUTTER_PSRAM_GPIO_RP2350B 47
#define DEFAULT_THROTTLING 0

#endif
