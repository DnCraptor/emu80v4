#include "OrionMenu.h"
#include "Pal.h"
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
#include <algorithm>

OrionSettings orionSettings{OrionModel::Monitor2, OrionCpu::I8080};
static const char* const names[] = {"Monitor 1", "Monitor 2", "Monitor 3.1", "Moscow / Monitor 3.2", "DSDOS / 512K"};
static constexpr uint32_t magic = 0x4F523138;
static constexpr const char* configPath = "/orion/orion.cfg";
static constexpr const char* tempPath = "/orion/orion.tmp";
static constexpr const char* backupPath = "/orion/orion.bak";
static constexpr unsigned monitorSize = 0x800;
static uint8_t monitorRom[monitorSize];
static bool customMonitor = false;
extern PalKeyCodeAction getKey();

const char* orionModelName(OrionModel model) { return names[static_cast<unsigned>(model)]; }
const uint8_t* orionMonitorData() { return customMonitor ? monitorRom : nullptr; }

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

// The old 8-byte configuration remains valid. A custom monitor adds exactly
// 2048 raw bytes, so it is independent of the original SD file after selection.
static bool readSettings(const char* path)
{
    FIL file;
    if (f_open(&file, path, FA_READ) != FR_OK) return false;
    const auto size = f_size(&file);
    uint32_t data[2]; UINT count = 0;
    OrionSettings decoded = orionSettings;
    bool ok = (size == sizeof(data) || size == sizeof(data) + monitorSize) &&
        f_read(&file, data, sizeof(data), &count) == FR_OK && count == sizeof(data) &&
        data[0] == magic && decode(data[1], decoded);
    const bool hasImage = size == sizeof(data) + monitorSize;
    if (ok && hasImage)
        ok = f_read(&file, monitorRom, monitorSize, &count) == FR_OK && count == monitorSize;
    f_close(&file);
    if (ok) {
        orionSettings = decoded;
        customMonitor = hasImage;
    }
    return ok;
}

void orionLoadSettings()
{
    customMonitor = false;
    if (!readSettings(configPath)) readSettings(backupPath);
    if (watchdog_hw->scratch[0] == magic) {
        decode(watchdog_hw->scratch[1], orionSettings);
        watchdog_hw->scratch[0] = 0;
    }
}

static bool saveSettings(OrionSettings s, const uint8_t* image)
{
    const uint32_t data[] = {magic, encode(s)};
    FIL file;
    if (f_open(&file, tempPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    UINT count = 0;
    bool ok = f_write(&file, data, sizeof(data), &count) == FR_OK && count == sizeof(data);
    if (ok && image)
        ok = f_write(&file, image, monitorSize, &count) == FR_OK && count == monitorSize;
    if (f_close(&file) != FR_OK) ok = false;
    if (!ok) { f_unlink(tempPath); return false; }
    // FAT cannot replace an existing file with f_rename. Preserve a backup
    // until the new file has been installed; boot also accepts that backup.
    FILINFO info;
    const FRESULT status = f_stat(configPath, &info);
    const bool hadConfig = status == FR_OK;
    if (hadConfig) {
        f_unlink(backupPath);
        if (f_rename(configPath, backupPath) != FR_OK) { f_unlink(tempPath); return false; }
    } else if (status != FR_NO_FILE) { f_unlink(tempPath); return false; }
    if (f_rename(tempPath, configPath) != FR_OK) {
        if (hadConfig) f_rename(backupPath, configPath);
        f_unlink(tempPath);
        return false;
    }
    f_unlink(backupPath);
    return true;
}

static bool restart(OrionSettings s, const uint8_t* image)
{
    if (!saveSettings(s, image) && (image || customMonitor)) {
        orionConfirm("ROM / settings", "Cannot save /orion/orion.cfg");
        return false;
    }
    g_emulation->prepareOrionRestart();
    watchdog_hw->scratch[1] = encode(s);
    watchdog_hw->scratch[0] = magic;
    watchdog_reboot(0, 0, 100);
    while (true) tight_loop_contents();
}

bool orionIsMonitorFile(const std::string& path)
{
    const auto n = path.size();
    if (n < 4 || path[n-4] != '.') return false;
    return (path[n-3] == 'r' || path[n-3] == 'R') &&
           (path[n-2] == 'o' || path[n-2] == 'O') &&
           (path[n-1] == 'm' || path[n-1] == 'M');
}

static bool readMonitor(const std::string& path, uint8_t* image)
{
    FIL file;
    if (f_open(&file, path.c_str(), FA_READ) != FR_OK) return false;
    UINT count = 0;
    const bool ok = f_size(&file) == monitorSize &&
        f_read(&file, image, monitorSize, &count) == FR_OK && count == monitorSize;
    f_close(&file);
    return ok;
}

// Avoid putting a 2 KiB temporary image on the small Pico main stack.
static uint8_t pendingRom[monitorSize];
static bool selectMonitor(const std::string& path, OrionSettings settings)
{
    if (!readMonitor(path, pendingRom)) {
        orionConfirm("Invalid monitor ROM", "Expected raw 2048 bytes (F800-FFFF)");
        return false;
    }
    if (!orionConfirm("Monitor ROM", "Install ROM and restart?", true)) return false;
    return restart(settings, pendingRom);
}

bool orionLoadMonitorFile(const std::string& path)
{
    return selectMonitor(path, orionSettings);
}

namespace {
constexpr uint8_t white = RGB(0xffffff), black = RGB(0), blue = RGB(0x000080);
constexpr uint8_t titleColor = RGB(0x003080), highlight = RGB(0x40c0ff);

// Text coordinates are whole cells, shared by all of the imported drivers.
struct Screen {
    int fw = graphics_get_font_width(), fh = graphics_get_font_height();
    int cols = int(graphics_get_width()) / fw;
    int rows = int(graphics_get_visible_height()) / fh;
    void fill(int x, int y, int w, int h, uint8_t color) const {
        graphics_fill(x*fw, y*fh, w*fw, h*fh, color);
    }
    void text(int x, int y, const char* str, int limit, uint8_t fg = black) const {
        const size_t len = std::min(strlen(str), size_t(std::max(0, limit)));
        graphics_type(x*fw, y*fh, fg, str, len);
    }
    void frame(int x, int y, int w, int h, const char* title) const {
        fill(x, y, w, h, white);
        fill(x, y, w, 1, titleColor);
        text(x+1, y, title, w-2, white);
    }
};

bool closeKey(PalKeyCode vk) {
    return vk == PK_ESC || vk == PK_F12 || vk == PK_LALT || vk == PK_RALT;
}

// The Orion scanline backends currently render only GRAPHICS_VIDEO_TEXT.
// COMBINED is declared but not composited in their hot paths.
void enterMenuLayer() {
    graphics_set_video_content_mode(GRAPHICS_VIDEO_TEXT);
}
}

bool orionConfirm(const char* title, const char* message, bool confirm)
{
    const auto previous = graphics_get_video_content_mode();
    enterMenuLayer();
    Screen screen;
    const int w = std::min(screen.cols, 46), h = 5;
    const int x = (screen.cols-w)/2, y = std::max(0,(screen.rows-h)/2);
    screen.frame(x,y,w,h,title);
    screen.text(x+1,y+2,message,w-2);
    screen.text(x+1,y+4,confirm ? "Enter: yes   Esc: no" : "Enter / Esc: close",w-2);
    bool accepted = false;
    while (true) {
        const auto k = getKey();
        if (k.pressed && (k.vk == PK_ENTER || k.vk == PK_KP_ENTER)) { accepted = true; break; }
        if (k.pressed && closeKey(k.vk)) break;
        sleep_ms(2);
    }
    graphics_set_video_content_mode(previous);
    if (g_emulation) g_emulation->resetOrionKeys();
    return accepted;
}

void orionMenu()
{
    static bool open = false;
    if (open || !g_emulation) return;
    open = true;
    enterMenuLayer();
    enum Page { Main, Machine, Media, Keyboard, Help };
    Page page = Main;
    OrionSettings selected = orionSettings;
    int row = 0, parentRow = 0;
    SysReq action = SR_NONE;
    const char* const mainItems[] = {"Machine settings     >", "Files and media      >", "Keyboard layout      >", "Reset machine", "Toggle sound", "Help / About         >", "Resume"};
    const char* const machineItems[] = {"Model", "CPU", "Monitor ROM...", "Use built-in ROM", "Apply and restart", "Back"};
    const char* const mediaItems[] = {"Load and run [Alt+F3]", "Load to RAM disk", "Drive A...", "Drive B...", "HDD...", "Open RAM disk...", "Back"};
    const char* const keyboardItems[] = {"QWERTY", "JCUKEN", "Smart", "Back"};
    const char* const helpItems[] = {"Alt / F12: menu", "Alt+F3: load and run", "Ctrl+Alt+Del: reboot", "ROM: 2048 bytes at F800", "Orion-128 / " PORT_VERSION, "Back"};
    const char* const* items = mainItems;
    int count = 7;
    const char* title = "Orion-128";
    auto configurePage = [&]() {
        switch (page) {
        case Main: items = mainItems; count = 7; title = "Orion-128"; break;
        case Machine: items = machineItems; count = 6; title = "Machine settings"; break;
        case Media: items = mediaItems; count = 7; title = "Files and media"; break;
        case Keyboard: items = keyboardItems; count = 4; title = "Keyboard layout"; break;
        case Help: items = helpItems; count = 6; title = "Help / About"; break;
        }
    };
    Screen screen;
    const int w = std::min(46, screen.cols), h = 11;
    const int x = (screen.cols-w)/2, y = std::max(0,(screen.rows-h)/2);
    auto disabled = [&](int i) {
        if (page == Machine) return (i == 1 && selected.model == OrionModel::Moscow32) || (i == 3 && !customMonitor);
        if (page == Media) return (orionSettings.model == OrionModel::Monitor1 && i >= 1 && i <= 5) || (i == 4 && orionSettings.model != OrionModel::Dsdos);
        if (page == Help) return i != count-1;
        return false;
    };
    auto drawItem = [&](int i) {
        const int yy = y+2+i;
        screen.fill(x, yy, w, 1, i == row ? highlight : white);
        const uint8_t fg = disabled(i) ? RGB(0x808080) : black;
        screen.text(x+1,yy,items[i],w-2,fg);
        if (page == Machine) {
            const char* value = i == 0 ? orionModelName(selected.model) : i == 1 ? (selected.cpu == OrionCpu::Z80 ? "Z80 / 5 MHz" : "8080 / 2.5 MHz") : i == 2 ? (customMonitor ? "Custom" : "Built-in") : nullptr;
            if (value && w > 21) screen.text(x+20,yy,value,w-21,fg);
        }
    };
    auto drawPage = [&]() {
        graphics_clear_menu_text();
        screen.frame(x,y,w,h,title);
        for (int i=0;i<count;++i) drawItem(i);
        screen.fill(x,y+h-1,w,1,blue);
        screen.text(x+1,y+h-1,"Arrows / Enter   Esc: back",w-2,white);
    };
    drawPage();
    while (true) {
        const auto key = getKey();
        if (!key.pressed) { sleep_ms(2); continue; }
        if (key.vk == PK_F12 || key.vk == PK_LALT || key.vk == PK_RALT) break;
        if (key.vk == PK_ESC || ((key.vk == PK_ENTER || key.vk == PK_KP_ENTER) && page != Main && row == count-1)) {
            if (page == Main) break;
            page = Main; row = parentRow; configurePage(); drawPage(); continue;
        }
        const int oldRow = row;
        if (key.vk == PK_UP || key.vk == PK_KP_8) row = (row+count-1)%count;
        else if (key.vk == PK_DOWN || key.vk == PK_KP_2) row = (row+1)%count;
        else if (!disabled(row) && (key.vk == PK_ENTER || key.vk == PK_KP_ENTER || key.vk == PK_LEFT || key.vk == PK_RIGHT)) {
            const bool enter = key.vk == PK_ENTER || key.vk == PK_KP_ENTER;
            if (page == Machine && row == 0) {
                constexpr int n = static_cast<int>(OrionModel::Count);
                selected.model = static_cast<OrionModel>((static_cast<int>(selected.model)+(key.vk==PK_LEFT?n-1:1))%n);
                if (selected.model == OrionModel::Moscow32) selected.cpu = OrionCpu::Z80;
                drawItem(0); drawItem(1);
            } else if (page == Machine && row == 1) {
                selected.cpu = selected.cpu == OrionCpu::Z80 ? OrionCpu::I8080 : OrionCpu::Z80;
                drawItem(1);
            } else if (enter) {
                if (page == Main) {
                    if (row == 3) { action = SR_RESET; break; }
                    if (row == 4) { action = SR_MUTE; break; }
                    if (row == 6) break;
                    parentRow = row;
                    page = row == 0 ? Machine : row == 1 ? Media : row == 2 ? Keyboard : Help;
                    row = 0; configurePage(); drawPage();
                } else if (page == Machine) {
                    if (row == 2) {
                        const auto path = palOpenFileDialog("Monitor ROM", "ROM images|*.rom;*.bin", false);
                        if (!path.empty()) selectMonitor(path, selected);
                    } else if (row == 3) {
                        if (orionConfirm("Monitor ROM", "Restore built-in ROM and restart?",true)) restart(selected,nullptr);
                    } else if (row == 4) restart(selected,orionMonitorData());
                    drawPage();
                } else if (page == Media) {
                    const SysReq actions[] = {SR_LOADRUN, SR_LOAD, SR_DISKA, SR_DISKB, SR_HDD, SR_OPENRAMDISK};
                    action = actions[row]; break;
                } else if (page == Keyboard) {
                    const SysReq actions[] = {SR_QUERTY, SR_JCUKEN, SR_SMART};
                    action = actions[row]; break;
                }
            }
        }
        if (row != oldRow && oldRow < count) { drawItem(oldRow); drawItem(row); }
    }
    g_emulation->resetOrionKeys();
    graphics_set_menu_text_mode(false);
    if (action != SR_NONE) g_emulation->orionAction(action);
    g_emulation->restoreOrionScreen();
    open = false;
}
