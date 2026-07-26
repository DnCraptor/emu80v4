#pragma once
#include "stdbool.h"
#include "stdint.h"

#define PIO_VGA (pio0)
#ifndef VGA_BASE_PIN
#define VGA_BASE_PIN (6)
#endif
#define VGA_DMA_IRQ (DMA_IRQ_0)

#define TEXTMODE_COLS 80
#define TEXTMODE_ROWS 30

void vga_system_clock_changed();


#ifdef PICO_RP2040
// RP2040 VGA path: render Vector-06C video directly from guest RAM.
// The driver snapshots the current palette and video registers, then builds
// each VGA scan line in the DMA IRQ without allocating a frame buffer.
void graphics_set_vector_source(const uint8_t* memory, const uint8_t* palette,
                                uint8_t border_color, uint8_t line_offset,
                                bool mode512, bool show_border);
#endif
