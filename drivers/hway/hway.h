#pragma once
// Порт режима AY_TYPE=HWAY из PICO-BK: реальный PSG (AY-3-8910/YM2149) через
// два сдвиговых регистра 74595. Занимает те же ноги, что I2S.
#ifdef HWAY
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void hway_init(void);                      // 595 + тактовый генератор AY
void hway_reset(void);                     // сброс/инициализация чипов
void hway_ay_address(uint8_t reg);         // защёлкнуть номер регистра на AY0
void hway_ay_data(uint8_t val);            // записать данные в выбранный регистр
void hway_dac_out(int16_t l, int16_t r);   // не-AY звук -> port B AY1 (ЦАП)
#ifdef __cplusplus
}
#endif
#endif // HWAY
