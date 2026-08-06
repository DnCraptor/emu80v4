#pragma once
#include "stdbool.h"

#define PIO_VGA (pio0)
#ifndef VGA_BASE_PIN
#define VGA_BASE_PIN (6)
#endif
#define VGA_DMA_IRQ (DMA_IRQ_0)

#define TEXTMODE_COLS 80
#define TEXTMODE_ROWS 30

// Пересчёт делителя PIO при смене системной частоты (единый контракт graphics.h,
// вызывается из graphics_system_clock_changed()).
void vga_system_clock_changed(void);

