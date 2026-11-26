#ifndef ORION_MENU_H
#define ORION_MENU_H

#include <stdint.h>
#include <string>

enum class OrionModel : uint8_t { Monitor1, Monitor2, Monitor31, Moscow32, Dsdos, Count };
enum class OrionCpu : uint8_t { I8080, Z80 };
struct OrionSettings {
    OrionModel model;
    OrionCpu cpu;
};
extern OrionSettings orionSettings;
void orionLoadSettings();
void orionApplyMenuSettings();
void orionMenu();
// Exactly 2 KiB, mapped at F800-FFFF. Null selects the model's built-in ROM.
const uint8_t* orionMonitorData();
bool orionIsMonitorFile(const std::string& path);
bool orionLoadMonitorFile(const std::string& path);
// Modal dialog on the independent text layer; callers redraw after return.
bool orionConfirm(const char* title, const char* message, bool confirm = false);
const char* orionModelName(OrionModel model);

#endif
