#include "OrionMenu.h"
#include "PalKeys.h"
#include "Emulation.h"
#include "Globals.h"
#include "graphics.h"
#include "ff.h"
#include <pico/stdlib.h>
#include <hardware/watchdog.h>
#include <hardware/structs/watchdog.h>
#include <cstring>
#include <cstdio>

OrionSettings orionSettings{OrionModel::Monitor2, OrionCpu::I8080};
static const char* const names[] = {"Monitor 1", "Monitor 2", "Monitor 3.1", "Moscow / Monitor 3.2", "DSDOS / 512K"};
static constexpr uint32_t magic = 0x4F523138;
static constexpr const char* configPath = "/emu80/orion.cfg";
extern PalKeyCodeAction getKey();

const char* orionModelName(OrionModel model) { return names[static_cast<unsigned>(model)]; }

static bool decode(uint32_t value, OrionSettings& out)
{
    unsigned model = value & 0xff, cpu = value >> 8;
    if (model >= static_cast<unsigned>(OrionModel::Count) || cpu > 1) return false;
    out = {static_cast<OrionModel>(model), static_cast<OrionCpu>(cpu)};
    if (out.model == OrionModel::Moscow32) out.cpu = OrionCpu::Z80;
    return true;
}

static uint32_t encode(OrionSettings s)
{
    return static_cast<unsigned>(s.model) | (static_cast<unsigned>(s.cpu) << 8);
}

void orionLoadSettings()
{
    if (watchdog_hw->scratch[0] == magic) {
        bool valid = decode(watchdog_hw->scratch[1], orionSettings);
        watchdog_hw->scratch[0] = 0;
        if (valid) return;
    }
    FIL file;
    if (f_open(&file, configPath, FA_READ) != FR_OK) return;
    uint32_t data[2]; UINT count;
    if (f_read(&file, data, sizeof(data), &count) == FR_OK && count == sizeof(data) && data[0] == magic)
        decode(data[1], orionSettings);
    f_close(&file);
}

static void restart(OrionSettings s)
{
    g_emulation->prepareOrionRestart();
    const uint32_t data[] = {magic, encode(s)};
    FIL file;
    if (f_open(&file, configPath, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
        UINT count;
        f_write(&file, data, sizeof(data), &count);
        f_close(&file);
    }
    watchdog_hw->scratch[1] = data[1];
    watchdog_hw->scratch[0] = data[0];
    watchdog_reboot(0, 0, 100);
    while (true) tight_loop_contents();
}

static void text(int x, int y, const char* str, uint8_t fg = RGB(0xffffff))
{
    graphics_type(x, y, fg, str, strlen(str));
}

void orionMenu()
{
    static bool open = false;
    if (open || !g_emulation || !graphics_get_frame()) return;
    open = true;
    OrionSettings selected = orionSettings;
    unsigned row = 0;
    static const char* const labels[] = {"Model", "CPU", "Load and run", "Drive A", "Drive B", "HDD", "RAM disk", "Reset", "Apply and restart", "Resume"};
    constexpr unsigned rows = sizeof(labels) / sizeof(labels[0]);
    constexpr int x = 16, y = 12, w = 352, h = 232;
    SysReq action = SR_NONE;
    auto draw = [&]() {
        graphics_fill(x, y, w, h, RGB(0x000080));
        graphics_rect(x, y, w, h, RGB(0xffffff));
        graphics_fill(x+1, y+1, w-2, 20, RGB(0x008080));
        text(x+12, y+6, "Orion-128 / System settings");
        for (unsigned i = 0; i < rows; ++i) {
            int yy = y+28+16*i;
            if (i == row) graphics_fill(x+4, yy-3, w-8, 13, RGB(0x808080));
            text(x+10, yy, labels[i]);
            if (i == 0) text(x+84, yy, orionModelName(selected.model));
            if (i == 1) text(x+84, yy, selected.cpu == OrionCpu::Z80 ? "Z80 / 5 MHz" : "8080 / 2.5 MHz");
        }
        text(x+10, y+h-28, "Arrows: change   Enter: select");
        text(x+10, y+h-15, "Esc / F12: resume");
    };
    draw();
    while (true) {
        auto key = getKey();
        if (!key.pressed) { sleep_ms(2); continue; }
        if (key.vk == PK_ESC || key.vk == PK_F12) break;
        if (key.vk == PK_UP) row = (row + rows - 1) % rows;
        else if (key.vk == PK_DOWN) row = (row + 1) % rows;
        else if (key.vk == PK_LEFT || key.vk == PK_RIGHT || key.vk == PK_ENTER) {
            if (row == 0) {
                constexpr int n = static_cast<int>(OrionModel::Count);
                int m = static_cast<int>(selected.model);
                selected.model = static_cast<OrionModel>((m + (key.vk == PK_LEFT ? n-1 : 1)) % n);
                if (selected.model == OrionModel::Moscow32) selected.cpu = OrionCpu::Z80;
            } else if (row == 1 && selected.model != OrionModel::Moscow32) {
                selected.cpu = selected.cpu == OrionCpu::Z80 ? OrionCpu::I8080 : OrionCpu::Z80;
            } else if (key.vk == PK_ENTER && row >= 2) {
                if (row == 8) restart(selected);
                if (row == 9) break;
                static const SysReq actions[] = {SR_LOADRUN, SR_DISKA, SR_DISKB, SR_HDD, SR_OPENRAMDISK, SR_RESET};
                action = actions[row - 2];
                break;
            }
        }
        draw();
    }
    // Releases eaten by the menu must not leave keys held on the emulated machine.
    g_emulation->resetOrionKeys();
    if (action != SR_NONE) g_emulation->orionAction(action);
    g_emulation->restoreOrionScreen();
    open = false;
}
