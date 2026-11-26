#ifndef ORION_MENU_H
#define ORION_MENU_H

#include <stdint.h>

enum class OrionModel : uint8_t { Monitor1, Monitor2, Monitor31, Moscow32, Dsdos, Count };
enum class OrionCpu : uint8_t { I8080, Z80 };
struct OrionSettings {
    OrionModel model;
    OrionCpu cpu;
};
extern OrionSettings orionSettings;
void orionLoadSettings();
void orionMenu();
const char* orionModelName(OrionModel model);

#endif
