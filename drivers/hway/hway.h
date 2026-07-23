#pragma once
// Реальный PSG (AY-3-8910/YM2149) через два сдвиговых регистра 74595.
// Точное повторение режима AY_TYPE=HWAY из PICO-BK: последовательности
// посылок и тайминги воспроизведены как есть, без оптимизаций.
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void hway_init(void);
void hway_deinit(void);
void hway_reset(void);

void hway_ay_address(uint8_t reg);         // защёлкнуть номер регистра на AY0
void hway_ay_data(uint8_t val);            // записать данные в выбранный регистр
void hway_dac_out(int16_t l, int16_t r);   // ковокс -> port B второго чипа

void hway_set_dac_enabled(bool on);
bool hway_dac_enabled(void);

// Тактовый выход AY. В PICO-BK это tspin_mode: 0 — не выдавать (на плате
// свой кварц, цепь тактирования с RP2350 не связана), 1 — основная нога,
// 2 — альтернативная.
void hway_set_ayclk_mode(uint8_t mode);
uint8_t hway_ayclk_mode(void);
void hway_ayclk_refresh(void);

// Диагностика
void hway_test_tone(bool on);
bool hway_test_tone_on(void);
void hway_covox_test(bool on);
bool hway_covox_test_on(void);
#ifdef __cplusplus
}
#endif
