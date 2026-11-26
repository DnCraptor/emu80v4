#include "OrionMenu.h"
#include "Pal.h"
#include "pico/OrionOsd.h"
#include "pico/picoPal.h"
#include <iterator>
#include "DiskImage.h"
#include "Cpu.h"
#include "KbdLayout.h"
#include "SoundMixer.h"
#include "TapeRedirector.h"
#include "hardware/clocks.h"
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
static constexpr const char* names[] = {"Monitor 1", "Monitor 2", "Monitor 3.1", "Moscow / Monitor 3.2", "DSDOS / 512K"};
static constexpr uint32_t magic = 0x4F523138;
static constexpr const char* configPath = "/orion/orion.cfg";
static constexpr const char* tempPath = "/orion/orion.tmp";
static constexpr const char* backupPath = "/orion/orion.bak";
static constexpr unsigned monitorSize = 0x800;
static uint8_t monitorRom[monitorSize];
static bool customMonitor = false;
// These synchronous core 0 operations never hold two configuration files open.
static FIL configFile;

// Optional trailer to the existing header and raw monitor image.
struct MenuSettings {
    uint32_t signature = 0x4F534431; // OSD1
    int16_t offsets[5][2]{};
    uint8_t offsetMask = 0;
    uint8_t keyboard = KbdLayout::KLM_QWERTY;
    uint8_t volume = 6;
    uint8_t muted = 0;
    uint8_t audio = 0; // 0 = autodetect, 1 = PWM, 2 = I2S
    uint8_t color = 0; // 0 = model default, 1 = monochrome, 2 = color
    uint8_t cpuClock = 0; // 0 = model default
    uint8_t reserved = 0;
};
static_assert(sizeof(MenuSettings) == 32);
static MenuSettings menuSettings;
static bool menuSettingsDirty = false;

#if defined(HDMI_DVI)
static constexpr int videoOutput = 0;
#elif defined(VGA_DRV)
static constexpr int videoOutput = 1;
#elif defined(RGB_TV)
static constexpr int videoOutput = 2;
#elif defined(TV_NTSC)
static constexpr int videoOutput = 4;
#else
static constexpr int videoOutput = 3;
#endif



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

// An existing 8-byte configuration can also carry a 2 KiB monitor image.
// The OSD trailer follows the image when one is present.
static bool readSettings(const char* path)
{
    FIL& file = configFile;
    if (f_open(&file, path, FA_READ) != FR_OK) return false;
    const auto size = f_size(&file);
    uint32_t data[2];
    constexpr auto base = sizeof(data), withImage = base + monitorSize;
    const bool hasImage = size == withImage || size == withImage + sizeof(MenuSettings);
    const bool hasMenu = size == base + sizeof(MenuSettings) || size == withImage + sizeof(MenuSettings);
    UINT count = 0;
    OrionSettings decoded = orionSettings;
    bool ok = (size == base || hasImage || hasMenu) &&
        f_read(&file, data, sizeof(data), &count) == FR_OK && count == sizeof(data) &&
        data[0] == magic && decode(data[1], decoded);
    if (ok && hasImage)
        ok = f_read(&file, monitorRom, monitorSize, &count) == FR_OK && count == monitorSize;
    MenuSettings settings;
    if (ok && hasMenu &&
        f_read(&file, &settings, sizeof(settings), &count) == FR_OK && count == sizeof(settings) &&
        settings.signature == menuSettings.signature) {
        menuSettings = settings;
    }
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

static constexpr SysReq keyboardActions[] = {SR_QUERTY, SR_JCUKEN, SR_SMART};
static constexpr int guestClocks[] = {0, 2500000, 5000000};

static void applyGuestClock(Cpu& cpu, unsigned option)
{
    const int modelClock = orionSettings.cpu == OrionCpu::Z80 ? 5000000 : 2500000;
    cpu.setFrequency(guestClocks[option] ? guestClocks[option] : modelClock);
}

void orionApplyMenuSettings()
{
    if (menuSettings.keyboard < std::size(keyboardActions))
        g_emulation->orionAction(keyboardActions[menuSettings.keyboard]);
    auto* mixer = g_emulation->getSoundMixer();
    mixer->setVolume(menuSettings.volume);
    mixer->setMuted(menuSettings.muted == 1);
    if (menuSettings.audio == 1 || menuSettings.audio == 2)
        palSetAudioOutputI2S(menuSettings.audio == 2);
    if (menuSettings.color == 1 || menuSettings.color == 2) {
        auto* renderer = g_emulation->findObject("orion.crtRenderer");
        const bool color = renderer->getPropertyStringValue("colorMode") == "color";
        if (color != (menuSettings.color == 2)) g_emulation->orionAction(SR_COLOR);
    }
    if (menuSettings.cpuClock < std::size(guestClocks) && menuSettings.cpuClock != 0) {
        auto* cpu = static_cast<Cpu*>(g_emulation->findObject("orion.cpu"));
        applyGuestClock(*cpu, menuSettings.cpuClock);
    }
    if (menuSettings.offsetMask & (1u << videoOutput)) {
        const auto& offset = menuSettings.offsets[videoOutput];
        graphics_set_offset(std::clamp<int>(offset[0], -128, 128),
                            std::clamp<int>(offset[1], -128, 128));
    }
}

static bool saveSettings(OrionSettings s, const uint8_t* image)
{
    const uint32_t data[] = {magic, encode(s)};
    FIL& file = configFile;
    if (f_open(&file, tempPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    UINT count = 0;
    bool ok = f_write(&file, data, sizeof(data), &count) == FR_OK && count == sizeof(data);
    if (ok && image)
        ok = f_write(&file, image, monitorSize, &count) == FR_OK && count == monitorSize;
    if (ok)
        ok = f_write(&file, &menuSettings, sizeof(menuSettings), &count) == FR_OK && count == sizeof(menuSettings);
    if (f_close(&file) != FR_OK) ok = false;
    if (!ok) { f_unlink(tempPath); return false; }
    // FAT cannot replace an existing file with f_rename. Preserve a backup
    // until the new file has been installed; boot also accepts that backup.
    const FRESULT status = f_stat(configPath, nullptr);
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

static void saveMenuSettings()
{
    if (!menuSettingsDirty) return;
    if (saveSettings(orionSettings, orionMonitorData())) {
        menuSettingsDirty = false;
    } else {
        orionConfirm("Settings", "Cannot save /orion/orion.cfg");
    }
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
    FIL& file = configFile;
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
using namespace OrionOsd;

bool dialog(const char* caption, const char* const* lines, int lineCount, bool confirm)
{
    const auto previous = graphics_get_video_content_mode();
    if (previous == GRAPHICS_VIDEO_FRAMEBUFFER)
        graphics_set_menu_text_mode(true);
    Screen screen;
    const char* hint = confirm ? "Enter: yes   Esc: no" : "Enter / Esc: close";
    int longest = int(std::max(strlen(caption), strlen(hint)));
    for (int i = 0; i < lineCount; ++i)
        longest = std::max(longest, int(strlen(lines[i])));
    const int w = std::min(screen.cols - 2, longest + 4);
    const int h = lineCount + 4;
    const int x = (screen.cols - w) / 2, y = (screen.rows - h) / 2;
    screen.window(x, y, w, h, caption);
    for (int i = 0; i < lineCount; ++i)
        screen.text(x + 2, y + 2 + i, lines[i], w - 4);
    screen.text(x + 2, y + h - 1, hint, w - 4, gray);
    bool accepted = false;
    while (true) {
        const auto key = getKey();
        if (key.pressed && enterKey(key.vk)) { accepted = true; break; }
        if (key.pressed && (key.vk == PK_ESC || closeKey(key.vk))) break;
        sleep_ms(2);
    }
    if (previous == GRAPHICS_VIDEO_FRAMEBUFFER)
        graphics_set_video_content_mode(previous);
    if (g_emulation) g_emulation->resetOrionKeys();
    return accepted;
}

void help()
{
    const char* const lines[] = {
        "Alt (tap) / F12: open or close menu",
        "Up / Down: select an item",
        "Right / Enter: open a submenu",
        "Left / Esc: return to parent menu",
        "Alt+F3: open and run a program",
        "Ctrl+Alt+Del: reboot the device",
    };
    dialog("Help", lines, std::size(lines), false);
}

void about()
{
    const char* const lines[] = {
        "Orion-128  " PORT_VERSION,
        "Emu80 v. 4.x / Murmulator",
        orionModelName(orionSettings.model),
        orionSettings.cpu == OrionCpu::Z80 ? "Z80 / 5 MHz" : "8080 / 2.5 MHz",
    };
    dialog("About", lines, std::size(lines), false);
}

enum class Page { Root, Machine, Model, Processor, CpuClock, Keyboard, Media, DriveA, DriveB, Hdd, RamDisk, Tape, Sound, Volume, Video, System, Count };
enum class Command {
    Open, Model, Cpu, CpuClock, InstallRom, RestoreRom, Apply, Keyboard, Request,
    EjectDisk, WriteProtect, TapeInput, TapeOutput, EjectTape,
    Volume, Mute, AudioOutput, Color, ShiftX, ShiftY, Center, Reset, Pause, Reboot, Help, About, Resume
};

struct Item {
    const char* label;
    Command command;
    int value = 0;
};

constexpr Item submenu(const char* label, Page page) { return {label, Command::Open, int(page)}; }
constexpr Item request(const char* label, SysReq req) { return {label, Command::Request, int(req)}; }

struct MenuPage {
    const Item* items;
    int count;
};

template <size_t N>
constexpr MenuPage page(const Item (&items)[N]) { return {items, N}; }

constexpr Item rootItems[] = {
    submenu("Machine", Page::Machine),
    submenu("Keyboard", Page::Keyboard),
    submenu("Programs and media", Page::Media),
    submenu("Tape", Page::Tape),
    submenu("Sound", Page::Sound),
    submenu("Video", Page::Video),
    submenu("System", Page::System),
    {"Help", Command::Help},
    {"About", Command::About},
    {"Resume", Command::Resume},
};

constexpr Item machineItems[] = {
    submenu("Model", Page::Model),
    submenu("Processor", Page::Processor),
    submenu("CPU clock", Page::CpuClock),
    {"Install monitor ROM...", Command::InstallRom},
    {"Restore built-in ROM", Command::RestoreRom},
    {"Apply and restart", Command::Apply},
};

constexpr Item modelItems[] = {
    {names[0], Command::Model, int(OrionModel::Monitor1)},
    {names[1], Command::Model, int(OrionModel::Monitor2)},
    {names[2], Command::Model, int(OrionModel::Monitor31)},
    {names[3], Command::Model, int(OrionModel::Moscow32)},
    {names[4], Command::Model, int(OrionModel::Dsdos)},
};
static_assert(std::size(modelItems) == size_t(OrionModel::Count));

constexpr Item processorItems[] = {
    {"8080 / 2.5 MHz", Command::Cpu, int(OrionCpu::I8080)},
    {"Z80 / 5 MHz", Command::Cpu, int(OrionCpu::Z80)},
};

constexpr Item cpuClockItems[] = {
    {"Model default", Command::CpuClock, 0},
    {"2.5 MHz", Command::CpuClock, 1},
    {"5 MHz", Command::CpuClock, 2},
};

constexpr Item keyboardItems[] = {
    {"QWERTY", Command::Keyboard, KbdLayout::KLM_QWERTY},
    {"JCUKEN", Command::Keyboard, KbdLayout::KLM_JCUKEN},
    {"Smart", Command::Keyboard, KbdLayout::KLM_SMART},
};

constexpr Item mediaItems[] = {
    request("Load and run...", SR_LOADRUN),
    request("Load to RAM disk...", SR_LOAD),
    submenu("Drive A", Page::DriveA),
    submenu("Drive B", Page::DriveB),
    submenu("Hard disk", Page::Hdd),
    submenu("RAM disk", Page::RamDisk),
};

constexpr Item driveAItems[] = {
    request("Mount image...", SR_DISKA),
    {"Write protection", Command::WriteProtect},
    {"Eject", Command::EjectDisk},
};
constexpr Item driveBItems[] = {
    request("Mount image...", SR_DISKB),
    {"Write protection", Command::WriteProtect},
    {"Eject", Command::EjectDisk},
};
constexpr Item hddItems[] = {
    request("Mount image...", SR_HDD),
    {"Write protection", Command::WriteProtect},
    {"Eject", Command::EjectDisk},
};

constexpr Item ramDiskItems[] = {
    request("Open image...", SR_OPENRAMDISK),
    request("Reload image", SR_LOADRAMDISK),
    request("Save image", SR_SAVERAMDISK),
    request("Save image as...", SR_SAVERAMDISKAS),
};

constexpr Item tapeItems[] = {
    request("Monitor tape hooks", SR_TAPEHOOK),
    {"Open input...", Command::TapeInput},
    {"Create output...", Command::TapeOutput},
    {"Close input / output", Command::EjectTape},
};

constexpr Item soundItems[] = {
    {"Output", Command::AudioOutput},
    {"Mute", Command::Mute},
    submenu("Volume", Page::Volume),
};
constexpr Item volumeItems[] = {
    {"1", Command::Volume, 1}, {"2", Command::Volume, 2}, {"3", Command::Volume, 3},
    {"4", Command::Volume, 4}, {"5", Command::Volume, 5}, {"6", Command::Volume, 6},
    {"7 (max)", Command::Volume, 7},
};
constexpr Item videoItems[] = {
    {"Color", Command::Color},
    {"Move left", Command::ShiftX, -1},
    {"Move right", Command::ShiftX, 1},
    {"Move up", Command::ShiftY, -1},
    {"Move down", Command::ShiftY, 1},
    {"Center picture", Command::Center},
};
constexpr Item systemItems[] = {
    {"Pause emulation", Command::Pause},
    {"Reset Orion", Command::Reset},
    {"Reboot device", Command::Reboot},
};

constexpr MenuPage pages[] = {
    page(rootItems), page(machineItems), page(modelItems), page(processorItems), page(cpuClockItems),
    page(keyboardItems), page(mediaItems), page(driveAItems), page(driveBItems),
    page(hddItems), page(ramDiskItems), page(tapeItems), page(soundItems),
    page(volumeItems), page(videoItems), page(systemItems),
};
static_assert(std::size(pages) == size_t(Page::Count));

struct Level {
    Page id = Page::Root;
    int selected = 0;
    int x = 0, y = 0, w = 0, h = 0;
    const MenuPage& content() const { return pages[int(id)]; }
    const Item& item() const { return content().items[selected]; }
};

std::string fileStatus(EmuObject* device, const char* property, const char* prefix = "")
{
    auto path = device->getPropertyStringValue(property);
    if (path.empty()) return std::string(prefix) + "None";
    const auto slash = path.find_last_of("/\\");
    if (slash != std::string::npos) path.erase(0, slash + 1);
    return prefix + path;
}

class Menu {
public:
    void run();

private:
    Screen screen;
    Level levels[3];
    int depth = 0;
    bool done = false;
    SysReq afterClose = SR_NONE;
    NavigationKeys keys;
    OrionSettings selected = orionSettings;
    KbdLayout* keyboard = static_cast<KbdLayout*>(g_emulation->findObject("orion.kbdLayout"));
    SoundMixer* mixer = g_emulation->getSoundMixer();
    EmuObject* renderer = g_emulation->findObject("orion.crtRenderer");
    Cpu* cpu = static_cast<Cpu*>(g_emulation->findObject("orion.cpu"));
    DiskImage* diskA = static_cast<DiskImage*>(g_emulation->findObject("orion.diskA"));
    DiskImage* diskB = static_cast<DiskImage*>(g_emulation->findObject("orion.diskB"));
    DiskImage* hdd = static_cast<DiskImage*>(g_emulation->findObject("orion.hdd"));
    EmuObject* ramDisk = g_emulation->findObject("orion.ramDisk");
    EmuObject* tapeHooks = g_emulation->findObject("orion.tapeGrp");
    TapeRedirector* tapeIn = static_cast<TapeRedirector*>(g_emulation->findObject("orion.tapeInFile"));
    TapeRedirector* tapeOut = static_cast<TapeRedirector*>(g_emulation->findObject("orion.tapeOutFile"));

    Level& current() { return levels[depth]; }
    DiskImage* disk(Page pageId) const;
    bool enabled(const Level& level, const Item& item) const;
    bool checked(const Level& level, const Item& item) const;
    const char* caption(const Level& level, int index) const;
    const char* title(int index) const;
    void layout();
    void drawRow(const Level& level, int row) const;
    void draw() const;
    void __attribute__((noinline)) activate(const Item& item);
};

DiskImage* Menu::disk(Page pageId) const
{
    if (pageId == Page::DriveA) return diskA;
    if (pageId == Page::DriveB) return diskB;
    return pageId == Page::Hdd ? hdd : nullptr;
}

bool Menu::enabled(const Level& level, const Item& item) const
{
    switch (item.command) {
    case Command::Open:
        switch (Page(item.value)) {
        case Page::DriveA: case Page::DriveB: case Page::Hdd: return disk(Page(item.value));
        case Page::RamDisk: return ramDisk;
        case Page::Tape: return tapeHooks;
        default: return true;
        }
    case Command::Cpu: return selected.model != OrionModel::Moscow32 || item.value == int(OrionCpu::Z80);
    case Command::RestoreRom: return customMonitor;
    case Command::Apply: return selected.model != orionSettings.model || selected.cpu != orionSettings.cpu;
    case Command::Request: return item.value != SR_LOAD || ramDisk;
    case Command::EjectDisk: return disk(level.id)->getImagePresent();
    case Command::AudioOutput: return palAudioOutputCanSwitch();
    default: return true;
    }
}

bool Menu::checked(const Level& level, const Item& item) const
{
    switch (item.command) {
    case Command::Model: return item.value == int(selected.model);
    case Command::Cpu: return item.value == int(selected.cpu);
    case Command::CpuClock: return item.value == menuSettings.cpuClock;
    case Command::Color: return renderer->getPropertyStringValue("colorMode") == "color";
    case Command::Keyboard: return item.value == int(keyboard->getMode());
    case Command::Volume: return item.value == mixer->getVolume();
    case Command::Mute: return mixer->getMuted();
    case Command::Pause: return g_emulation->getPausedState();
    case Command::WriteProtect: return disk(level.id)->getWriteProtectStatus();
    case Command::Request: return item.value == SR_TAPEHOOK && tapeHooks->getPropertyStringValue("enabled") == "yes";
    default: return false;
    }
}

const char* Menu::caption(const Level& level, int index) const
{
    if (level.content().items[index].command == Command::AudioOutput)
        return palAudioIsI2S() ? "Output: I2S" : "Output: PWM";
    return level.content().items[index].label;
}

const char* Menu::title(int index) const
{
    if (index > 0) return levels[index - 1].item().label;
    return orionSettings.cpu == OrionCpu::Z80 ? "Orion-128 Z80" : "Orion-128 8080";
}

void Menu::layout()
{
    for (int i = 0; i <= depth; ++i) {
        auto& level = levels[i];
        int width = strlen(title(i));
        for (int row = 0; row < level.content().count; ++row)
            width = std::max(width, int(strlen(caption(level, row))));
        auto* drive = disk(level.id);
        const bool hasStatus = drive || level.id == Page::Machine || level.id == Page::Tape ||
                               level.id == Page::Video || level.id == Page::System || level.id == Page::RamDisk;
        if (hasStatus) width = std::max(width, 20);
        level.w = std::min(width + 4, screen.cols - 1);
        const int statusRows = level.id == Page::Machine ? 3 : level.id == Page::Tape ? 2 : int(hasStatus);
        level.h = 1 + level.content().count + statusRows;
        int x = 1, y = 1;
        if (i) {
            const auto& parent = levels[i - 1];
            x = parent.x + parent.w + 1;
            if (x + level.w + 1 > screen.cols && parent.x > level.w)
                x = parent.x - level.w - 1;
            y = parent.y + 1 + parent.selected;
            // If the title must overlap its parent, keep the active row visible.
            if (x + level.w + 1 > screen.cols) ++y;
        }
        level.x = std::clamp(x, 0, screen.cols - level.w - 1);
        level.y = std::clamp(y, 0, screen.rows - level.h - 1);
    }
}

void Menu::drawRow(const Level& level, int row) const
{
    const auto& item = level.content().items[row];
    const int y = level.y + 1 + row;
    const uint8_t fg = enabled(level, item) ? black : gray;
    screen.fill(level.x + 1, y, level.w - 2, 1, row == level.selected ? selection : panel);
    screen.text(level.x + 1, y, caption(level, row), level.w - 3, fg);
    if (item.command == Command::Open)
        screen.text(level.x + level.w - 2, y, ">", 1, fg);
    else if (checked(level, item))
        screen.text(level.x + level.w - 2, y, "*", 1, fg);
}

void Menu::draw() const
{
    graphics_clear_menu_text();
    for (int i = 0; i <= depth; ++i) {
        const auto& level = levels[i];
        screen.window(level.x, level.y, level.w, level.h, title(i));
        for (int row = 0; row < level.content().count; ++row)
            drawRow(level, row);
        const int status = level.y + 1 + level.content().count;
        auto print = [&](int line, std::string_view message) {
            screen.text(level.x + 1, status + line, message, level.w - 2);
        };
        if (auto* drive = disk(level.id)) {
            print(0, fileStatus(drive, "fileName", "Image: "));
        } else if (level.id == Page::Machine) {
            print(0, orionModelName(selected.model));
            print(1, processorItems[int(selected.cpu)].label);
            print(2, customMonitor ? "Monitor ROM: Custom" : "Monitor ROM: Built-in");
        } else if (level.id == Page::RamDisk) {
            print(0, fileStatus(ramDisk, "fileName", "Image: "));
        } else if (level.id == Page::Tape) {
            print(0, fileStatus(tapeIn, "currentFile", "In:  "));
            print(1, fileStatus(tapeOut, "currentFile", "Out: "));
        } else if (level.id == Page::Video || level.id == Page::System) {
            char value[24];
            if (level.id == Page::Video)
                snprintf(value, sizeof(value), "Offset: %d, %d", graphics_get_picture_shift_x(), graphics_get_picture_shift_y());
            else
                snprintf(value, sizeof(value), "System clock: %lu MHz", (unsigned long)(clock_get_hz(clk_sys) / 1000000));
            print(0, value);
        }
    }
}

void Menu::activate(const Item& item)
{
    auto* drive = disk(current().id);
    switch (item.command) {
    case Command::Open: break;
    case Command::Model:
        selected.model = OrionModel(item.value);
        if (selected.model == OrionModel::Moscow32) selected.cpu = OrionCpu::Z80;
        --depth;
        break;
    case Command::Cpu: selected.cpu = OrionCpu(item.value); --depth; break;
    case Command::CpuClock:
        applyGuestClock(*cpu, item.value);
        menuSettings.cpuClock = item.value;
        menuSettingsDirty = true;
        break;
    case Command::InstallRom: {
        const auto path = palOpenFileDialog("Monitor ROM", "ROM images|*.rom;*.bin", false);
        if (!path.empty()) selectMonitor(path, selected);
        break;
    }
    case Command::RestoreRom:
        if (orionConfirm("Monitor ROM", "Restore built-in ROM and restart?", true))
            restart(selected, nullptr);
        break;
    case Command::Apply:
        if (orionConfirm("Machine", "Apply settings and restart?", true))
            restart(selected, orionMonitorData());
        break;
    case Command::Keyboard:
        g_emulation->orionAction(keyboardActions[item.value]);
        menuSettings.keyboard = item.value;
        menuSettingsDirty = true;
        break;
    case Command::Request:
        if (item.value == SR_LOADRUN) {
            afterClose = SR_LOADRUN;
            done = true;
        } else {
            g_emulation->orionAction(SysReq(item.value));
        }
        break;
    case Command::EjectDisk: drive->assignFileName(""); break;
    case Command::WriteProtect: drive->setWriteProtection(!drive->getWriteProtectStatus()); break;
    case Command::TapeInput: tapeIn->openFile(); break;
    case Command::TapeOutput: tapeOut->openFile(); break;
    case Command::EjectTape: tapeIn->closeFile(); tapeOut->closeFile(); break;
    case Command::Volume:
        mixer->setVolume(item.value);
        menuSettings.volume = item.value;
        menuSettingsDirty = true;
        break;
    case Command::Mute:
        mixer->toggleMute();
        menuSettings.muted = mixer->getMuted();
        menuSettingsDirty = true;
        break;
    case Command::AudioOutput: {
        const bool i2s = !palAudioIsI2S();
        if (palSetAudioOutputI2S(i2s)) {
            menuSettings.audio = i2s ? 2 : 1;
            menuSettingsDirty = true;
        } else {
            orionConfirm("Sound", "Cannot switch audio output");
        }
        break;
    }
    case Command::Color:
        g_emulation->orionAction(SR_COLOR);
        menuSettings.color = renderer->getPropertyStringValue("colorMode") == "color" ? 2 : 1;
        menuSettingsDirty = true;
        break;
    case Command::ShiftX: case Command::ShiftY: case Command::Center: {
        int x = graphics_get_picture_shift_x(), y = graphics_get_picture_shift_y();
#ifdef SOFTTV
        constexpr int step = 4; // Keep the composite colour phase.
#else
        constexpr int step = 1;
#endif
        if (item.command == Command::ShiftX) x += item.value * step;
        else if (item.command == Command::ShiftY) y += item.value;
        else x = y = 0;
        graphics_set_offset(std::clamp(x, -128, 128), std::clamp(y, -128, 128));
        auto& offset = menuSettings.offsets[videoOutput];
        offset[0] = graphics_get_picture_shift_x();
        offset[1] = graphics_get_picture_shift_y();
        menuSettings.offsetMask |= 1u << videoOutput;
        menuSettingsDirty = true;
        break;
    }
    case Command::Reset: afterClose = SR_RESET; done = true; break;
    case Command::Pause: g_emulation->orionAction(SR_PAUSE); break;
    case Command::Reboot:
        if (orionConfirm("System", "Reboot the device?", true)) {
            saveMenuSettings();
            g_emulation->prepareOrionRestart();
            watchdog_reboot(0, 0, 100);
            while (true) tight_loop_contents();
        }
        break;
    case Command::Help: help(); break;
    case Command::About: about(); break;
    case Command::Resume: done = true; break;
    }
}

void Menu::run()
{
    layout();
    draw();
    while (!done) {
        const auto key = keys.next();
        if (key == PK_NONE) { sleep_ms(2); continue; }
        if (closeKey(key)) break;
        if (key == PK_ESC || key == PK_LEFT || key == PK_KP_4) {
            if (!depth) {
                if (key == PK_ESC) break;
                continue;
            }
            --depth;
            draw();
            continue;
        }
        auto& level = current();
        const int count = level.content().count;
        const int old = level.selected;
        if (key == PK_DOWN || key == PK_KP_2 || key == PK_UP || key == PK_KP_8) {
            const int step = key == PK_DOWN || key == PK_KP_2 ? 1 : -1;
            do {
                level.selected = (level.selected + step + count) % count;
            } while (level.selected != old && !enabled(level, level.item()));
            if (old != level.selected) {
                drawRow(level, old);
                drawRow(level, level.selected);
            }
        } else if (enterKey(key) || key == PK_RIGHT || key == PK_KP_6) {
            const auto& item = level.item();
            if (!enabled(level, item)) continue;
            if (item.command == Command::Open && depth + 1 < int(std::size(levels))) {
                auto& child = levels[++depth];
                child = {Page(item.value)};
                for (int row = 0; row < child.content().count; ++row) {
                    const auto& candidate = child.content().items[row];
                    if (candidate.command != Command::Model && candidate.command != Command::Cpu &&
                        candidate.command != Command::CpuClock && candidate.command != Command::Keyboard &&
                        candidate.command != Command::Volume)
                        continue;
                    if (enabled(child, candidate) && checked(child, candidate)) {
                        child.selected = row;
                        break;
                    }
                }
            } else if (enterKey(key)) {
                activate(item);
            } else {
                continue;
            }
            if (!done) {
                layout();
                draw();
            }
        }
    }
    saveMenuSettings();
    graphics_set_menu_text_mode(false);
    g_emulation->resetOrionKeys();
    if (afterClose != SR_NONE) {
        const bool paused = g_emulation->getPausedState();
        if (paused) g_emulation->orionAction(SR_PAUSEOFF);
        g_emulation->orionAction(afterClose);
        if (paused) g_emulation->orionAction(SR_PAUSEON);
    }
    g_emulation->restoreOrionScreen();
}
}

bool orionConfirm(const char* title, const char* message, bool confirm)
{
    return dialog(title, &message, 1, confirm);
}

void orionMenu()
{
    static bool open = false;
    if (open || !g_emulation) return;
    open = true;
    graphics_set_menu_text_mode(true);
    Menu().run();
    open = false;
}
