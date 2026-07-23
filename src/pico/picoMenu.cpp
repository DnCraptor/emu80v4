#include "picoMenu.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <iterator>
#include <string>

#include <pico/time.h>

#include "graphics.h"
#include "../Globals.h"
#include "../Emulation.h"
#include "../Vector.h"
#include "../SoundMixer.h"
#include "hway.h"
#include "pico/picoPal.h"

extern PalKeyCodeAction getKey();

char* appendText(char* dst, const char* text)
{
    while (*text)
        *dst++ = *text++;
    return dst;
}

char* appendUnsigned(char* dst, unsigned value)
{
    char digits[10];
    int count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count != 0)
        *dst++ = digits[--count];
    return dst;
}

namespace {

struct MenuPage;

struct MenuItem {
    const char* title;
    const char* (*getTitle)();
    const MenuPage* submenu;
    void (*action)();
    bool (*isEnabled)();
    bool (*isChecked)();
    bool keepOpen = false;
};

struct MenuPage {
    const char* title;
    // Для страниц, чей заголовок зависит от состояния машины
    const char* (*getTitle)();
    const MenuItem* items;
    int itemCount;
    // Радиогруппа: текущее значение и его установка. Для обычных страниц null.
    int (*getValue)();
    void (*setValue)(int);
    const char* (*getStatusLine1)() = nullptr;
    const char* (*getStatusLine2)() = nullptr;
};

// --- Processor -------------------------------------------------------------

int cpuGetValue()
{
    if (!g_emulation || !g_emulation->getVector())
        return 0;
    return static_cast<int>(g_emulation->getVector()->getCpuType());
}

void cpuSetValue(int value)
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (!core)
        return;
    core->setCpuType(static_cast<VectorCpuType>(value));
}

static const MenuItem processorItems[] = {
    {"KR580VM80A (i8080)", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"Zilog Z80", nullptr, nullptr, nullptr, nullptr, nullptr},
};

static const MenuPage processorPage {
    "Processor",
    nullptr,
    processorItems,
    static_cast<int>(sizeof(processorItems) / sizeof(processorItems[0])),
    cpuGetValue,
    cpuSetValue
};

static constexpr unsigned cpuClockValues[] = {
    3000000, 3500000, 6000000, 7000000, 12000000,
    14000000, 20000000, 24000000, 28000000
};

int cpuClockGetValue()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (!core)
        return 0;

    const unsigned frequency = core->getCpuFrequency();
    for (int i = 0; i < static_cast<int>(sizeof(cpuClockValues) / sizeof(cpuClockValues[0])); ++i)
        if (cpuClockValues[i] == frequency)
            return i;
    return 0;
}

void cpuClockSetValue(int value)
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (!core || value < 0
        || value >= static_cast<int>(sizeof(cpuClockValues) / sizeof(cpuClockValues[0])))
        return;
    core->setCpuFrequency(cpuClockValues[value]);
}


char cpuFpsStatusBuffer[24];
char cpuLoadStatusBuffer[24];

bool menuItemDisabled()
{
    return false;
}

const char* cpuFpsStatus()
{
    if (!g_emulation || !g_emulation->performanceStatsReady())
        return "FPS: measuring...";

    char* dst = appendText(cpuFpsStatusBuffer, "FPS: ");
    dst = appendUnsigned(dst, g_emulation->getVideoFps());
    dst = appendText(dst, " Hz");
    *dst = '\0';
    return cpuFpsStatusBuffer;
}


const char* cpuLoadStatus()
{
    if (!g_emulation || !g_emulation->performanceStatsReady())
        return "CPU Load: measuring...";

    char* dst = appendText(cpuLoadStatusBuffer, "CPU Load: ");
    dst = appendUnsigned(dst, g_emulation->getCpuLoad());
    *dst++ = '%';
    *dst = '\0';
    return cpuLoadStatusBuffer;
}

static const MenuItem cpuClockItems[] = {
    {"3.0 MHz", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"3.5 MHz", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"6 MHz", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"7 MHz", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"12 MHz", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"14 MHz", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"20 MHz", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"24 MHz", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"28 MHz", nullptr, nullptr, nullptr, nullptr, nullptr},
    {nullptr, cpuFpsStatus, nullptr, nullptr, menuItemDisabled, nullptr},
    {nullptr, cpuLoadStatus, nullptr, nullptr, menuItemDisabled, nullptr},
};

static const MenuPage cpuClockPage {
    "CPU-Clock",
    nullptr,
    cpuClockItems,
    static_cast<int>(sizeof(cpuClockItems) / sizeof(cpuClockItems[0])),
    cpuClockGetValue,
    cpuClockSetValue
};

// --- Storage / floppy drives ----------------------------------------------

char driveTitleBuffer[2][96];

const char* driveTitle(VectorFloppyDrive drive)
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    const std::string fileName = core ? core->getFloppyFileName(drive) : std::string();
    char* buffer = driveTitleBuffer[static_cast<int>(drive)];
    constexpr char prefix[] = "Drive A: ";
    std::memcpy(buffer, prefix, sizeof(prefix));
    buffer[6] = drive == VectorFloppyDrive::A ? 'A' : 'B';
    if (fileName.empty()) {
        constexpr char empty[] = "empty";
        std::memcpy(buffer + sizeof(prefix) - 1, empty, sizeof(empty));
        return buffer;
    }
    const size_t slash = fileName.find_last_of("/\\");
    const char* base = fileName.c_str() + (slash == std::string::npos ? 0 : slash + 1);
    const size_t prefixLen = sizeof(prefix) - 1;
    constexpr char readOnlySuffix[] = " (ro)";
    constexpr char readWriteSuffix[] = " (rw)";
    const bool readOnly = core && core->floppyImageReadOnly(drive);
    const char* suffix = readOnly ? readOnlySuffix : readWriteSuffix;
    const size_t suffixLen = sizeof(readOnlySuffix) - 1;
    const size_t baseLen = std::min(std::strlen(base), sizeof(driveTitleBuffer[0]) - prefixLen - suffixLen - 1);
    std::memcpy(buffer + prefixLen, base, baseLen);
    std::memcpy(buffer + prefixLen + baseLen, suffix, suffixLen);
    buffer[prefixLen + baseLen + suffixLen] = '\0';
    return buffer;
}

bool driveHasImage(VectorFloppyDrive drive)
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    return core && core->floppyImagePresent(drive);
}
void driveInsert(VectorFloppyDrive drive) { if (g_emulation && g_emulation->getVector()) g_emulation->getVector()->chooseFloppyImage(drive); }
void driveEject(VectorFloppyDrive drive) { if (g_emulation && g_emulation->getVector()) g_emulation->getVector()->ejectFloppyImage(drive); }
bool driveReadOnly(VectorFloppyDrive drive)
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    return core && core->floppyReadOnlyMode(drive);
}
bool driveReadOnlyEnabled(VectorFloppyDrive drive)
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (!core)
        return false;
    return core->floppyReadOnlyMode(drive)
        ? core->canSetFloppyReadOnly(drive, false)
        : core->canSetFloppyReadOnly(drive, true);
}
void driveToggleReadOnly(VectorFloppyDrive drive)
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (core)
        core->setFloppyReadOnly(drive, !core->floppyReadOnlyMode(drive));
}

const char* driveATitle() { return driveTitle(VectorFloppyDrive::A); }
const char* driveBTitle() { return driveTitle(VectorFloppyDrive::B); }
bool driveAHasImage() { return driveHasImage(VectorFloppyDrive::A); }
bool driveBHasImage() { return driveHasImage(VectorFloppyDrive::B); }
void driveAInsert() { driveInsert(VectorFloppyDrive::A); }
void driveBInsert() { driveInsert(VectorFloppyDrive::B); }
void driveAEject() { driveEject(VectorFloppyDrive::A); }
void driveBEject() { driveEject(VectorFloppyDrive::B); }
bool driveAReadOnly() { return driveReadOnly(VectorFloppyDrive::A); }
bool driveBReadOnly() { return driveReadOnly(VectorFloppyDrive::B); }
bool driveAReadOnlyEnabled() { return driveReadOnlyEnabled(VectorFloppyDrive::A); }
bool driveBReadOnlyEnabled() { return driveReadOnlyEnabled(VectorFloppyDrive::B); }
void driveAToggleReadOnly() { driveToggleReadOnly(VectorFloppyDrive::A); }
void driveBToggleReadOnly() { driveToggleReadOnly(VectorFloppyDrive::B); }

static const MenuItem driveAItems[] = {
    {"Insert image [Alt+A]...", nullptr, nullptr, driveAInsert, nullptr, nullptr},
    {"Read only", nullptr, nullptr, driveAToggleReadOnly, driveAReadOnlyEnabled, driveAReadOnly, true},
    {"Eject", nullptr, nullptr, driveAEject, driveAHasImage, nullptr},
};
static const MenuItem driveBItems[] = {
    {"Insert image [Alt+B]...", nullptr, nullptr, driveBInsert, nullptr, nullptr},
    {"Read only", nullptr, nullptr, driveBToggleReadOnly, driveBReadOnlyEnabled, driveBReadOnly, true},
    {"Eject", nullptr, nullptr, driveBEject, driveBHasImage, nullptr},
};
char hddTitleBuffer[96];

const char* hddTitle()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    const std::string fileName = core ? core->getHddFileName() : std::string();
    constexpr char prefix[] = "HDD: ";
    std::memcpy(hddTitleBuffer, prefix, sizeof(prefix));
    if (fileName.empty()) {
        constexpr char empty[] = "empty";
        std::memcpy(hddTitleBuffer + sizeof(prefix) - 1, empty, sizeof(empty));
        return hddTitleBuffer;
    }
    const size_t slash = fileName.find_last_of("/\\");
    const char* base = fileName.c_str() + (slash == std::string::npos ? 0 : slash + 1);
    const size_t prefixLen = sizeof(prefix) - 1;
    const size_t baseLen = std::min(std::strlen(base), sizeof(hddTitleBuffer) - prefixLen - 1);
    std::memcpy(hddTitleBuffer + prefixLen, base, baseLen);
    hddTitleBuffer[prefixLen + baseLen] = '\0';
    return hddTitleBuffer;
}

bool hddHasImage()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    return core && core->hddImagePresent();
}
bool hddEnabled()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    return core && core->getHddEnabled();
}
const char* hddToggleTitle() { return hddEnabled() ? "Disable" : "Enable"; }
void toggleHdd()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (core)
        core->setHddEnabled(!core->getHddEnabled());
}
void hddInsert() { if (g_emulation && g_emulation->getVector()) g_emulation->getVector()->chooseHddImage(); }
void hddEject() { if (g_emulation && g_emulation->getVector()) g_emulation->getVector()->ejectHddImage(); }

static const MenuItem hddItems[] = {
    {nullptr, hddToggleTitle, nullptr, toggleHdd, nullptr, nullptr, true},
    {"Insert image [Alt+F4]...", nullptr, nullptr, hddInsert, nullptr, nullptr},
    {"Eject", nullptr, nullptr, hddEject, hddHasImage, nullptr},
};
static const MenuPage driveAPage {"Drive A", driveATitle, driveAItems, static_cast<int>(sizeof(driveAItems) / sizeof(driveAItems[0])), nullptr, nullptr};
static const MenuPage driveBPage {"Drive B", driveBTitle, driveBItems, static_cast<int>(sizeof(driveBItems) / sizeof(driveBItems[0])), nullptr, nullptr};
static const MenuPage hddPage {"HDD", hddTitle, hddItems, static_cast<int>(sizeof(hddItems) / sizeof(hddItems[0])), nullptr, nullptr};

void invokeSysReq(SysReq request)
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (core)
        core->sysReq(request);
}

void eddOpen() { invokeSysReq(SR_OPENRAMDISK); }
void eddSaveAs() { invokeSysReq(SR_SAVERAMDISKAS); }
void edd2Open() { invokeSysReq(SR_OPENRAMDISK2); }
void edd2SaveAs() { invokeSysReq(SR_SAVERAMDISK2AS); }

bool ramDiskEnabled(int diskNum)
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    return core && core->ramDiskEnabled(diskNum);
}

bool eddEnabled() { return ramDiskEnabled(0); }
bool edd2Enabled() { return ramDiskEnabled(1); }

const char* eddToggleTitle() { return eddEnabled() ? "Disable" : "Enable"; }
const char* edd2ToggleTitle() { return edd2Enabled() ? "Disable" : "Enable"; }

void toggleRamDisk(int diskNum)
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (core)
        core->setRamDiskEnabled(diskNum, !core->ramDiskEnabled(diskNum));
}

void toggleEdd() { toggleRamDisk(0); }
void toggleEdd2() { toggleRamDisk(1); }

static const MenuItem eddItems[] = {
    {nullptr, eddToggleTitle, nullptr, toggleEdd, nullptr, nullptr, true},
    {"Load image [Alt+E]...", nullptr, nullptr, eddOpen, nullptr, nullptr},
    {"Save image as [Alt+O]...", nullptr, nullptr, eddSaveAs, nullptr, nullptr},
};
static const MenuItem edd2Items[] = {
    {nullptr, edd2ToggleTitle, nullptr, toggleEdd2, nullptr, nullptr, true},
    {"Load image [Alt+Shift+E]...", nullptr, nullptr, edd2Open, nullptr, nullptr},
    {"Save image as [Alt+Shift+O]...", nullptr, nullptr, edd2SaveAs, nullptr, nullptr},
};
static const MenuPage eddPage {"EDD", nullptr, eddItems, static_cast<int>(sizeof(eddItems) / sizeof(eddItems[0])), nullptr, nullptr};
static const MenuPage edd2Page {"EDD2", nullptr, edd2Items, static_cast<int>(sizeof(edd2Items) / sizeof(edd2Items[0])), nullptr, nullptr};
void romLoadAndRun() { invokeSysReq(SR_LOADRUN); }
static const MenuItem romItems[] = {
    {"Load and run...", nullptr, nullptr, romLoadAndRun, nullptr, nullptr},
};
static const MenuPage romPage {"ROM", nullptr, romItems, static_cast<int>(sizeof(romItems) / sizeof(romItems[0])), nullptr, nullptr};

static const MenuItem storageItems[] = {
    {"Drive A", driveATitle, &driveAPage, nullptr, nullptr, nullptr},
    {"Drive B", driveBTitle, &driveBPage, nullptr, nullptr, nullptr},
    {"HDD", hddTitle, &hddPage, nullptr, nullptr, nullptr},
    {"EDD", nullptr, &eddPage, nullptr, nullptr, nullptr},
    {"EDD2", nullptr, &edd2Page, nullptr, nullptr, nullptr},
    {"ROM", nullptr, &romPage, nullptr, nullptr, nullptr},
};
static const MenuPage storagePage {"Storage", nullptr, storageItems, static_cast<int>(sizeof(storageItems) / sizeof(storageItems[0])), nullptr, nullptr};

const char* soundTitle()
{
    if (palAudioIsHwAy())
        return "Sound (HWAY)";
    return palAudioIsI2S() ? "Sound (I2S)" : "Sound (PWM)";
}

const char* soundOutputTitle()
{
    // Порядок перебора: PWM -> I2S -> HWAY -> PWM
    if (palAudioIsHwAy())
        return "Output: HWAY (to PWM)";
    if (palAudioIsI2S())
        return "Output: I2S (to HWAY)";
    return "Output: PWM (to I2S)";
}

bool soundOutputEnabled()
{
    return palAudioOutputCanSwitch();
}

void toggleSoundOutput()
{
    if (palAudioIsHwAy()) {            // HWAY -> PWM
        palSetAudioOutputHwAy(false);
        palSetAudioOutputI2S(false);
    } else if (palAudioIsI2S()) {      // I2S -> HWAY
        palSetAudioOutputI2S(false);
        palSetAudioOutputHwAy(true);
    } else {                           // PWM -> I2S
        palSetAudioOutputI2S(true);
    }
}

bool s_userMuted = false;

int volumeGetValue()
{
    if (s_userMuted)
        return 0;
    SoundMixer* mixer = g_emulation ? g_emulation->getSoundMixer() : nullptr;
    return mixer ? mixer->getVolume() : 5;
}

void volumeSetValue(int value)
{
    SoundMixer* mixer = g_emulation ? g_emulation->getSoundMixer() : nullptr;
    if (!mixer || value < 0 || value > 7)
        return;
    s_userMuted = value == 0;
    if (value != 0)
        mixer->setVolume(value);
    mixer->setMuted(s_userMuted);
}

static const MenuItem volumeItems[] = {
    {"Mute (0%)", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"14%", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"29%", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"43%", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"57%", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"71%", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"86%", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"100%", nullptr, nullptr, nullptr, nullptr, nullptr},
};
static const MenuPage volumePage {"Volume", nullptr, volumeItems, static_cast<int>(sizeof(volumeItems) / sizeof(volumeItems[0])), volumeGetValue, volumeSetValue};

bool psgStereoChecked()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    return core && core->getPsgStereo();
}

void togglePsgStereo()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (core)
        core->setPsgStereo(!core->getPsgStereo());
}

bool psgEnabled()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    return core && core->getPsgEnabled();
}

const char* psgToggleTitle() { return psgEnabled() ? "Disable" : "Enable"; }

void togglePsg()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (core)
        core->setPsgEnabled(!core->getPsgEnabled());
}

// Панорамирование каналов PSG выполняет программный микшер. При HWAY звук
// формирует сама микросхема, разводка её каналов задана платой, поэтому
// Stereo и порядок ABC/ACB на результат не влияют.
bool psgOrderEnabled() { return psgEnabled() && !palAudioIsHwAy(); }
bool psgStereoEnabled() { return !palAudioIsHwAy(); }

// Вывод спикера/ковокса в port B второго чипа. Каждая запись занимает шину
// 595, поэтому при подозрении на помехи звучанию PSG его можно отключить.
bool hwayDacEnabled() { return palAudioIsHwAy(); }
bool hwayDacChecked() { return hway_dac_enabled(); }
void toggleHwayDac() { hway_set_dac_enabled(!hway_dac_enabled()); }

// Диагностика: тон 440 Гц пишется в AY0 напрямую, минуя эмулятор.
bool hwayToneChecked() { return hway_test_tone_on(); }
void toggleHwayTone() { hway_test_tone(!hway_test_tone_on()); }


// Проверка R-2R на port B: пила подаётся прямо в регистр, минуя эмуляцию.
const char* hwayCsTitle()
{
    switch (hway_cs_state()) {
        case 1:  return "595 CS step: CS0 only";
        case 2:  return "595 CS step: CS1 only";
        case 3:  return "595 CS step: both high";
        default: return "595 CS step: both low";
    }
}
void doHwayCsStep() { hway_cs_step(); }

bool hwayCovoxTestChecked() { return hway_covox_test_on(); }
void toggleHwayCovoxTest() { hway_covox_test(!hway_covox_test_on()); }

// Нога тактового выхода AY: в PICO-BK их две на выбор (GP21 / GP29).
const char* hwayAyClkTitle()
{
    switch (hway_ayclk_mode()) {
        case 0:  return "AY clock: off (board xtal)";
        case 2:  return "AY clock: alt pin";
        default: return "AY clock: main pin";
    }
}
void toggleHwayAyClk() { hway_set_ayclk_mode((hway_ayclk_mode() + 1) % 3); }

bool psgAbcChecked()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    return core && !core->getPsgAcbOrder();
}

bool psgAcbChecked()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    return core && core->getPsgAcbOrder();
}

void setPsgAbc()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (core)
        core->setPsgAcbOrder(false);
}

void setPsgAcb()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (core)
        core->setPsgAcbOrder(true);
}

static const MenuItem psgItems[] = {
    {nullptr, psgToggleTitle, nullptr, togglePsg, nullptr, nullptr, true},
    {"ABC", nullptr, nullptr, setPsgAbc, psgOrderEnabled, psgAbcChecked, true},
    {"ACB", nullptr, nullptr, setPsgAcb, psgOrderEnabled, psgAcbChecked, true},
};
static const MenuPage psgPage {"PSG", nullptr, psgItems, static_cast<int>(sizeof(psgItems) / sizeof(psgItems[0])), nullptr, nullptr};

static const MenuItem soundItems[] = {
    {nullptr, soundOutputTitle, nullptr, toggleSoundOutput, soundOutputEnabled, nullptr, true},
    {"Volume", nullptr, &volumePage, nullptr, nullptr, nullptr},
    {"Stereo", nullptr, nullptr, togglePsgStereo, psgStereoEnabled, psgStereoChecked, true},
    {"Covox out (port B)", nullptr, nullptr, toggleHwayDac, hwayDacEnabled, hwayDacChecked, true},
    {"Test tone 440Hz (AY0)", nullptr, nullptr, toggleHwayTone, hwayDacEnabled, hwayToneChecked, true},
    {"595 CS step", hwayCsTitle, nullptr, doHwayCsStep, hwayDacEnabled, nullptr, true},
    {"Test covox ramp (port B)", nullptr, nullptr, toggleHwayCovoxTest, hwayDacEnabled, hwayCovoxTestChecked, true},
    {"AY clock", hwayAyClkTitle, nullptr, toggleHwayAyClk, hwayDacEnabled, nullptr, true},
    {"PSG", nullptr, &psgPage, nullptr, nullptr, nullptr},
};
static const MenuPage soundPage {"Sound", nullptr, soundItems, static_cast<int>(sizeof(soundItems) / sizeof(soundItems[0])), nullptr, nullptr};

char tapeInputStatusBuffer[96];
char tapeOutputStatusBuffer[96];

const char* tapeStatus(char* buffer, const char* prefix, const std::string& fileName)
{
    char* dst = appendText(buffer, prefix);
    if (fileName.empty()) {
        dst = appendText(dst, "empty");
    } else {
        const size_t slash = fileName.find_last_of("/\\");
        const char* base = fileName.c_str() + (slash == std::string::npos ? 0 : slash + 1);
        const size_t used = static_cast<size_t>(dst - buffer);
        const size_t len = std::min(std::strlen(base), sizeof(tapeInputStatusBuffer) - used - 1);
        std::memcpy(dst, base, len);
        dst += len;
    }
    *dst = '\0';
    return buffer;
}

const char* tapeInputStatus()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    return tapeStatus(tapeInputStatusBuffer, "Input: ",
                      core ? core->getTapeInputFileName() : std::string());
}

const char* tapeOutputStatus()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    return tapeStatus(tapeOutputStatusBuffer, "Output: ",
                      core ? core->getTapeOutputFileName() : std::string());
}

bool tapeHooksChecked()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    return core && core->tapeHooksEnabled();
}

void toggleTapeHooks()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (core)
        core->setTapeHooksEnabled(!core->tapeHooksEnabled());
}

void tapeLoad()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (core)
        core->chooseTapeInput();
}

void tapeCreate()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (core)
        core->chooseTapeOutput();
}

bool tapeEjectEnabled()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    return core && core->tapeFilePresent();
}

void tapeEject()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (core)
        core->ejectTapeFiles();
}

static const MenuItem tapeItems[] = {
    {"Redirect tape I/O", nullptr, nullptr, toggleTapeHooks, nullptr, tapeHooksChecked, true},
    {"Load tape image...", nullptr, nullptr, tapeLoad, nullptr, nullptr},
    {"Create new tape...", nullptr, nullptr, tapeCreate, nullptr, nullptr},
    {"Eject", nullptr, nullptr, tapeEject, tapeEjectEnabled, nullptr, true},
};
static const MenuPage tapePage {
    "Tape", nullptr, tapeItems,
    static_cast<int>(sizeof(tapeItems) / sizeof(tapeItems[0])),
    nullptr, nullptr, tapeInputStatus, tapeOutputStatus
};
void showSnapshotMessage(const char* title, const char* line1, const char* line2 = nullptr)
{
    const int screenW = graphics_get_width();
    const int screenH = graphics_get_height();
    const int visibleH = static_cast<int>(graphics_get_visible_height());
    const int fontW = graphics_get_font_width();
    const int fontH = graphics_get_font_height();
    const int rowH = fontH + 2;

    size_t longest = std::max(std::strlen(title), std::strlen(line1));
    if (line2)
        longest = std::max(longest, std::strlen(line2));
    longest = std::max(longest, std::strlen("Enter / Esc - close"));

    int w = static_cast<int>(longest + 4) * fontW;
    int h = fontH + 7 + (line2 ? 3 : 2) * rowH + 4;
    w = std::min(w, screenW - 8);
    h = std::min(h, visibleH - 8);
    int x = (screenW - w) / 2 - graphics_get_picture_shift_x();
    int y = (visibleH - h) / 2 - graphics_get_picture_shift_y();
    x = std::max(0, std::min(x, screenW - w));
    y = std::max(0, std::min(y, screenH - h));

    graphics_fill(x + 4, y + 4, w, h, RGB888(32, 32, 32));
    graphics_fill(x, y, w, h, RGB888(232, 232, 232));
    graphics_rect(x, y, w, h, RGB888(0, 0, 0));
    graphics_fill(x + 1, y + 1, w - 2, fontH + 4, RGB888(0, 48, 128));
    graphics_type(x + 5, y + 3, RGB888(255, 255, 255), title, std::strlen(title));

    int lineY = y + fontH + 7;
    graphics_type(x + 5, lineY, RGB888(0, 0, 0), line1, std::strlen(line1));
    lineY += rowH;
    if (line2) {
        graphics_type(x + 5, lineY, RGB888(0, 0, 0), line2, std::strlen(line2));
        lineY += rowH;
    }
    static const char closeText[] = "Enter / Esc - close";
    graphics_type(x + 5, lineY, RGB888(0, 0, 0), closeText, sizeof(closeText) - 1);

    while (true) {
        sleep_ms(100);
        palInputTick();
        const PalKeyCodeAction key = getKey();
        if (key.pressed && (key.vk == PK_ESC || key.vk == PK_ENTER
                         || key.vk == PK_KP_ENTER || key.vk == PK_SPACE))
            return;
    }
}

void saveSnapshotSlot(unsigned slot)
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (!core || !core->saveSnapshot(slot))
        showSnapshotMessage("Snapshot", "Unable to save snapshot.");
}

void loadSnapshotSlot(unsigned slot)
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (!core)
        return;

    std::string firmwareVersion;
    uint16_t fileFormatVersion = 0;
    const VectorCore::SnapshotLoadResult result =
        core->loadSnapshot(slot, &firmwareVersion, &fileFormatVersion);

    switch (result) {
    case VectorCore::SnapshotLoadResult::Ok:
        return;
    case VectorCore::SnapshotLoadResult::NotFound:
        showSnapshotMessage("Snapshot", "Snapshot is empty.");
        return;
    case VectorCore::SnapshotLoadResult::IncompatibleFormat: {
        char versions[64];
        char* dst = appendText(versions, "Format: expected ");
        dst = appendUnsigned(dst, VectorCore::snapshotFormatVersion());
        dst = appendText(dst, ", file ");
        dst = appendUnsigned(dst, fileFormatVersion);
        *dst = '\0';

        char firmware[64];
        dst = appendText(firmware, "Created by: ");
        dst = appendText(dst, firmwareVersion.empty() ? "unknown" : firmwareVersion.c_str());
        *dst = '\0';
        showSnapshotMessage("Incompatible snapshot", versions, firmware);
        return;
    }
    case VectorCore::SnapshotLoadResult::InvalidFile:
        showSnapshotMessage("Snapshot", "Invalid snapshot file.");
        return;
    default:
        showSnapshotMessage("Snapshot", "Unable to load snapshot.");
        return;
    }
}

void removeSnapshotSlot(unsigned slot)
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (!core || !core->removeSnapshot(slot))
        showSnapshotMessage("Snapshot", "Unable to remove snapshot.");
}

const char* snapshotSlotTitle(unsigned slot)
{
    static char titles[12][64];
    char* dst = appendText(titles[slot - 1], "Snapshot ");
    dst = appendUnsigned(dst, slot);
    dst = appendText(dst, "  ");

    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    VectorCore::SnapshotInfo info;
    if (!core || !core->readSnapshotInfo(slot, info))
        dst = appendText(dst, info.present ? "(invalid)" : "(error)");
    else if (!info.present)
        dst = appendText(dst, "(empty)");
    else
        dst = appendText(dst, info.firmwareVersion.empty()
                              ? "unknown" : info.firmwareVersion.c_str());
    *dst = '\0';
    return titles[slot - 1];
}

const char* snapshotTitle1() { return snapshotSlotTitle(1); }
const char* snapshotTitle2() { return snapshotSlotTitle(2); }
const char* snapshotTitle3() { return snapshotSlotTitle(3); }
const char* snapshotTitle4() { return snapshotSlotTitle(4); }
const char* snapshotTitle5() { return snapshotSlotTitle(5); }
const char* snapshotTitle6() { return snapshotSlotTitle(6); }
const char* snapshotTitle7() { return snapshotSlotTitle(7); }
const char* snapshotTitle8() { return snapshotSlotTitle(8); }
const char* snapshotTitle9() { return snapshotSlotTitle(9); }
const char* snapshotTitle10() { return snapshotSlotTitle(10); }
const char* snapshotTitle11() { return snapshotSlotTitle(11); }
const char* snapshotTitle12() { return snapshotSlotTitle(12); }

void saveSnapshot1() { saveSnapshotSlot(1); }
void loadSnapshot1() { loadSnapshotSlot(1); }
void removeSnapshot1() { removeSnapshotSlot(1); }
static const MenuItem snapshot1Items[] = {
    {"Save", nullptr, nullptr, saveSnapshot1, nullptr, nullptr},
    {"Load", nullptr, nullptr, loadSnapshot1, nullptr, nullptr},
    {"Remove", nullptr, nullptr, removeSnapshot1, nullptr, nullptr},
};
static const MenuPage snapshot1Page {
    "Snapshot 1", nullptr, snapshot1Items,
    static_cast<int>(sizeof(snapshot1Items) / sizeof(snapshot1Items[0])), nullptr, nullptr
};

void saveSnapshot2() { saveSnapshotSlot(2); }
void loadSnapshot2() { loadSnapshotSlot(2); }
void removeSnapshot2() { removeSnapshotSlot(2); }
static const MenuItem snapshot2Items[] = {
    {"Save", nullptr, nullptr, saveSnapshot2, nullptr, nullptr},
    {"Load", nullptr, nullptr, loadSnapshot2, nullptr, nullptr},
    {"Remove", nullptr, nullptr, removeSnapshot2, nullptr, nullptr},
};
static const MenuPage snapshot2Page {
    "Snapshot 2", nullptr, snapshot2Items,
    static_cast<int>(sizeof(snapshot2Items) / sizeof(snapshot2Items[0])), nullptr, nullptr
};

void saveSnapshot3() { saveSnapshotSlot(3); }
void loadSnapshot3() { loadSnapshotSlot(3); }
void removeSnapshot3() { removeSnapshotSlot(3); }
static const MenuItem snapshot3Items[] = {
    {"Save", nullptr, nullptr, saveSnapshot3, nullptr, nullptr},
    {"Load", nullptr, nullptr, loadSnapshot3, nullptr, nullptr},
    {"Remove", nullptr, nullptr, removeSnapshot3, nullptr, nullptr},
};
static const MenuPage snapshot3Page {
    "Snapshot 3", nullptr, snapshot3Items,
    static_cast<int>(sizeof(snapshot3Items) / sizeof(snapshot3Items[0])), nullptr, nullptr
};

void saveSnapshot4() { saveSnapshotSlot(4); }
void loadSnapshot4() { loadSnapshotSlot(4); }
void removeSnapshot4() { removeSnapshotSlot(4); }
static const MenuItem snapshot4Items[] = {
    {"Save", nullptr, nullptr, saveSnapshot4, nullptr, nullptr},
    {"Load", nullptr, nullptr, loadSnapshot4, nullptr, nullptr},
    {"Remove", nullptr, nullptr, removeSnapshot4, nullptr, nullptr},
};
static const MenuPage snapshot4Page {
    "Snapshot 4", nullptr, snapshot4Items,
    static_cast<int>(sizeof(snapshot4Items) / sizeof(snapshot4Items[0])), nullptr, nullptr
};

void saveSnapshot5() { saveSnapshotSlot(5); }
void loadSnapshot5() { loadSnapshotSlot(5); }
void removeSnapshot5() { removeSnapshotSlot(5); }
static const MenuItem snapshot5Items[] = {
    {"Save", nullptr, nullptr, saveSnapshot5, nullptr, nullptr},
    {"Load", nullptr, nullptr, loadSnapshot5, nullptr, nullptr},
    {"Remove", nullptr, nullptr, removeSnapshot5, nullptr, nullptr},
};
static const MenuPage snapshot5Page {
    "Snapshot 5", nullptr, snapshot5Items,
    static_cast<int>(sizeof(snapshot5Items) / sizeof(snapshot5Items[0])), nullptr, nullptr
};

void saveSnapshot6() { saveSnapshotSlot(6); }
void loadSnapshot6() { loadSnapshotSlot(6); }
void removeSnapshot6() { removeSnapshotSlot(6); }
static const MenuItem snapshot6Items[] = {
    {"Save", nullptr, nullptr, saveSnapshot6, nullptr, nullptr},
    {"Load", nullptr, nullptr, loadSnapshot6, nullptr, nullptr},
    {"Remove", nullptr, nullptr, removeSnapshot6, nullptr, nullptr},
};
static const MenuPage snapshot6Page {
    "Snapshot 6", nullptr, snapshot6Items,
    static_cast<int>(sizeof(snapshot6Items) / sizeof(snapshot6Items[0])), nullptr, nullptr
};

void saveSnapshot7() { saveSnapshotSlot(7); }
void loadSnapshot7() { loadSnapshotSlot(7); }
void removeSnapshot7() { removeSnapshotSlot(7); }
static const MenuItem snapshot7Items[] = {
    {"Save", nullptr, nullptr, saveSnapshot7, nullptr, nullptr},
    {"Load", nullptr, nullptr, loadSnapshot7, nullptr, nullptr},
    {"Remove", nullptr, nullptr, removeSnapshot7, nullptr, nullptr},
};
static const MenuPage snapshot7Page {
    "Snapshot 7", nullptr, snapshot7Items,
    static_cast<int>(sizeof(snapshot7Items) / sizeof(snapshot7Items[0])), nullptr, nullptr
};

void saveSnapshot8() { saveSnapshotSlot(8); }
void loadSnapshot8() { loadSnapshotSlot(8); }
void removeSnapshot8() { removeSnapshotSlot(8); }
static const MenuItem snapshot8Items[] = {
    {"Save", nullptr, nullptr, saveSnapshot8, nullptr, nullptr},
    {"Load", nullptr, nullptr, loadSnapshot8, nullptr, nullptr},
    {"Remove", nullptr, nullptr, removeSnapshot8, nullptr, nullptr},
};
static const MenuPage snapshot8Page {
    "Snapshot 8", nullptr, snapshot8Items,
    static_cast<int>(sizeof(snapshot8Items) / sizeof(snapshot8Items[0])), nullptr, nullptr
};

void saveSnapshot9() { saveSnapshotSlot(9); }
void loadSnapshot9() { loadSnapshotSlot(9); }
void removeSnapshot9() { removeSnapshotSlot(9); }
static const MenuItem snapshot9Items[] = {
    {"Save", nullptr, nullptr, saveSnapshot9, nullptr, nullptr},
    {"Load", nullptr, nullptr, loadSnapshot9, nullptr, nullptr},
    {"Remove", nullptr, nullptr, removeSnapshot9, nullptr, nullptr},
};
static const MenuPage snapshot9Page {
    "Snapshot 9", nullptr, snapshot9Items,
    static_cast<int>(sizeof(snapshot9Items) / sizeof(snapshot9Items[0])), nullptr, nullptr
};

void saveSnapshot10() { saveSnapshotSlot(10); }
void loadSnapshot10() { loadSnapshotSlot(10); }
void removeSnapshot10() { removeSnapshotSlot(10); }
static const MenuItem snapshot10Items[] = {
    {"Save", nullptr, nullptr, saveSnapshot10, nullptr, nullptr},
    {"Load", nullptr, nullptr, loadSnapshot10, nullptr, nullptr},
    {"Remove", nullptr, nullptr, removeSnapshot10, nullptr, nullptr},
};
static const MenuPage snapshot10Page {
    "Snapshot 10", nullptr, snapshot10Items,
    static_cast<int>(sizeof(snapshot10Items) / sizeof(snapshot10Items[0])), nullptr, nullptr
};

void saveSnapshot11() { saveSnapshotSlot(11); }
void loadSnapshot11() { loadSnapshotSlot(11); }
void removeSnapshot11() { removeSnapshotSlot(11); }
static const MenuItem snapshot11Items[] = {
    {"Save", nullptr, nullptr, saveSnapshot11, nullptr, nullptr},
    {"Load", nullptr, nullptr, loadSnapshot11, nullptr, nullptr},
    {"Remove", nullptr, nullptr, removeSnapshot11, nullptr, nullptr},
};
static const MenuPage snapshot11Page {
    "Snapshot 11", nullptr, snapshot11Items,
    static_cast<int>(sizeof(snapshot11Items) / sizeof(snapshot11Items[0])), nullptr, nullptr
};

void saveSnapshot12() { saveSnapshotSlot(12); }
void loadSnapshot12() { loadSnapshotSlot(12); }
void removeSnapshot12() { removeSnapshotSlot(12); }
static const MenuItem snapshot12Items[] = {
    {"Save", nullptr, nullptr, saveSnapshot12, nullptr, nullptr},
    {"Load", nullptr, nullptr, loadSnapshot12, nullptr, nullptr},
    {"Remove", nullptr, nullptr, removeSnapshot12, nullptr, nullptr},
};
static const MenuPage snapshot12Page {
    "Snapshot 12", nullptr, snapshot12Items,
    static_cast<int>(sizeof(snapshot12Items) / sizeof(snapshot12Items[0])), nullptr, nullptr
};

static const MenuItem snapshotItems[] = {
    {"Snapshot 1", snapshotTitle1, &snapshot1Page, nullptr, nullptr, nullptr},
    {"Snapshot 2", snapshotTitle2, &snapshot2Page, nullptr, nullptr, nullptr},
    {"Snapshot 3", snapshotTitle3, &snapshot3Page, nullptr, nullptr, nullptr},
    {"Snapshot 4", snapshotTitle4, &snapshot4Page, nullptr, nullptr, nullptr},
    {"Snapshot 5", snapshotTitle5, &snapshot5Page, nullptr, nullptr, nullptr},
    {"Snapshot 6", snapshotTitle6, &snapshot6Page, nullptr, nullptr, nullptr},
    {"Snapshot 7", snapshotTitle7, &snapshot7Page, nullptr, nullptr, nullptr},
    {"Snapshot 8", snapshotTitle8, &snapshot8Page, nullptr, nullptr, nullptr},
    {"Snapshot 9", snapshotTitle9, &snapshot9Page, nullptr, nullptr, nullptr},
    {"Snapshot 10", snapshotTitle10, &snapshot10Page, nullptr, nullptr, nullptr},
    {"Snapshot 11", snapshotTitle11, &snapshot11Page, nullptr, nullptr, nullptr},
    {"Snapshot 12", snapshotTitle12, &snapshot12Page, nullptr, nullptr, nullptr},
};
static const MenuPage snapshotPage {
    "Snapshots", nullptr, snapshotItems,
    static_cast<int>(sizeof(snapshotItems) / sizeof(snapshotItems[0])), nullptr, nullptr
};
static const MenuPage videoPage     {"Video", nullptr, nullptr, 0, nullptr, nullptr};

static constexpr uint16_t coreVoltageValues[] = {1300, 1400, 1500, 1600, 1650};

int systemClockGetValue()
{
    uint32_t count = 0;
    const uint32_t* clocks = graphics_get_supported_system_clocks(&count);
    const uint32_t current = palGetSystemClockMHz();
    for (uint32_t i = 0; i < count; ++i)
        if (clocks[i] == current)
            return static_cast<int>(i);
    return 0;
}

void systemClockSetValue(int value)
{
    uint32_t count = 0;
    const uint32_t* clocks = graphics_get_supported_system_clocks(&count);
    if (value >= 0 && static_cast<uint32_t>(value) < count)
        palSetSystemClockMHz(clocks[value]);
}

bool systemClockItemEnabled()
{
    return graphics_system_clock_can_change();
}

int coreVoltageGetValue()
{
    const uint16_t current = palGetCoreVoltageMv();
    for (int i = 0; i < static_cast<int>(sizeof(coreVoltageValues) / sizeof(coreVoltageValues[0])); ++i)
        if (coreVoltageValues[i] == current)
            return i;
    return 0;
}

void coreVoltageSetValue(int value)
{
    if (value >= 0 && value < static_cast<int>(sizeof(coreVoltageValues) / sizeof(coreVoltageValues[0])))
        palSetCoreVoltageMv(coreVoltageValues[value]);
}

static const MenuItem systemClockItems[] = {
#ifdef HDMI_DVI
    {"400 MHz", nullptr, nullptr, nullptr, menuItemDisabled, nullptr},
#else
    {"400 MHz", nullptr, nullptr, nullptr, systemClockItemEnabled, nullptr},
    {"440 MHz", nullptr, nullptr, nullptr, systemClockItemEnabled, nullptr},
    {"480 MHz", nullptr, nullptr, nullptr, systemClockItemEnabled, nullptr},
    {"520 MHz", nullptr, nullptr, nullptr, systemClockItemEnabled, nullptr},
    {"540 MHz", nullptr, nullptr, nullptr, systemClockItemEnabled, nullptr},
#endif
};
static const MenuPage systemClockPage {
    "RP2350 frequency", nullptr, systemClockItems,
    static_cast<int>(sizeof(systemClockItems) / sizeof(systemClockItems[0])),
    systemClockGetValue, systemClockSetValue
};

static const MenuItem coreVoltageItems[] = {
    {"1.30 V", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"1.40 V", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"1.50 V", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"1.60 V", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"1.65 V", nullptr, nullptr, nullptr, nullptr, nullptr},
};
static const MenuPage coreVoltagePage {
    "Core voltage", nullptr, coreVoltageItems,
    static_cast<int>(sizeof(coreVoltageItems) / sizeof(coreVoltageItems[0])),
    coreVoltageGetValue, coreVoltageSetValue
};

static const MenuItem systemItems[] = {
    {"RP2350 frequency", nullptr, &systemClockPage, nullptr, nullptr, nullptr},
    {"Core voltage", nullptr, &coreVoltagePage, nullptr, nullptr, nullptr},
};
static const MenuPage systemPage {
    "System", nullptr, systemItems,
    static_cast<int>(sizeof(systemItems) / sizeof(systemItems[0])), nullptr, nullptr
};

void drawAboutDialog()
{
    static const char* const lines[] = {
        "Vector-06C emulator for",
        "Murmulator 1.x / Murmulator 2.0",
        "and Raspberry Pi Pico 2 (RP2350)",
        "",
        "This firmware is a port of the",
        "Vector-06C platform from the",
        "multi-system emulator emu80v4.",
        "",
        "Original emulator:",
        "Victor Pykhonyn (Pyk)",
        "https://emu80.org/",
        "",
        "Port author: @Michael_V1973",
        "",
        "Special thanks to Victor Pykhonyn (Pyk)",
        "for his invaluable contribution to",
        "preserving and promoting classic",
        "8-bit computer platforms.",
        "",
        "Murmulator community:",
        "https://t.me/ZX_MURMULATOR/279905",
        "",
        "Build: " __DATE__ " " __TIME__,
        "",
        "Enter / Esc - close"
    };

    const int screenW = graphics_get_width();
    const int screenH = graphics_get_height();
    const int visibleH = static_cast<int>(graphics_get_visible_height());
    const int fontW = graphics_get_font_width();
    const int fontH = graphics_get_font_height();
    const int rowH = fontH + 2;
    const int lineCount = static_cast<int>(sizeof(lines) / sizeof(lines[0]));

    size_t longest = std::strlen("About");
    for (const char* line : lines)
        longest = std::max(longest, std::strlen(line));

    int w = static_cast<int>(longest + 4) * fontW;
    int h = fontH + 7 + lineCount * rowH + 4;
    w = std::min(w, screenW - 8);
    h = std::min(h, visibleH - 8);

    // TV-out может сдвигать видимую область относительно framebuffer.
    int x = (screenW - w) / 2 - graphics_get_picture_shift_x();
    int y = (visibleH - h) / 2 - graphics_get_picture_shift_y();
    x = std::max(0, std::min(x, screenW - w));
    y = std::max(0, std::min(y, screenH - h));

    graphics_fill(x + 4, y + 4, w, h, RGB888(32, 32, 32));
    graphics_fill(x, y, w, h, RGB888(232, 232, 232));
    graphics_rect(x, y, w, h, RGB888(0, 0, 0));
    graphics_fill(x + 1, y + 1, w - 2, fontH + 4, RGB888(0, 48, 128));

    const char title[] = "About";
    graphics_type(x + 5, y + 3, RGB888(255, 255, 255), title, sizeof(title) - 1);

    int lineY = y + fontH + 7;
    for (const char* line : lines) {
        const int len = static_cast<int>(std::strlen(line));
        const int lineX = x + std::max(5, (w - len * fontW) / 2);
        graphics_type(lineX, lineY, RGB888(0, 0, 0), line, len);
        lineY += rowH;
    }
}

void showAboutDialog()
{
    drawAboutDialog();
    while (true) {
        sleep_ms(100);
        palInputTick();
        const PalKeyCodeAction key = getKey();
        if (key.pressed && (key.vk == PK_ESC || key.vk == PK_ENTER
                         || key.vk == PK_KP_ENTER || key.vk == PK_SPACE))
            return;
    }
}

static const MenuItem rootItems[] = {
    {"Processor", nullptr, &processorPage, nullptr, nullptr, nullptr},
    {"CPU-Clock", nullptr, &cpuClockPage, nullptr, nullptr, nullptr},
    {"Storage", nullptr, &storagePage, nullptr, nullptr, nullptr},
    {"Sound", soundTitle, &soundPage, nullptr, nullptr, nullptr},
    {"Tape", nullptr, &tapePage, nullptr, nullptr, nullptr},
    {"Snapshots", nullptr, &snapshotPage, nullptr, nullptr, nullptr},
    {"Video", nullptr, &videoPage, nullptr, nullptr, nullptr},
    {"System", nullptr, &systemPage, nullptr, nullptr, nullptr},
    {"About", nullptr, nullptr, showAboutDialog, nullptr, nullptr},
};

// Заголовок корневой страницы отражает установленное ядро
const char* rootTitle()
{
    if (g_emulation && g_emulation->getVector()
        && g_emulation->getVector()->getCpuType() == VECTOR_CPU_Z80)
        return "Vector-06C Z80";
    return "Vector-06C 80A";
}

static const MenuPage rootPage {
    "Vector-06C 80A",
    rootTitle,
    rootItems,
    static_cast<int>(sizeof(rootItems) / sizeof(rootItems[0])),
    nullptr,
    nullptr
};

// ---------------------------------------------------------------------------
//  Подложка под каскад подменю.
//
//  Главное меню фон не сохраняет: закрывается оно вместе со снятием паузы,
//  и рендерер перерисовывает кадр целиком за один проход.
//
//  А вот подменю раскрываются вбок, вправо от родителя, поверх изображения
//  машины, и по «влево» область под ними должна вернуться к прежнему виду.
//  Поэтому фон запоминается для каждого уровня отдельно, стеком: вход на
//  уровень кладёт прямоугольник в пул, возврат снимает его оттуда.
// ---------------------------------------------------------------------------
// Задержка до начала автоповтора и период повторов
static constexpr uint64_t c_repeatDelayUs = 400000;
static constexpr uint64_t c_repeatRateUs  = 60000;

static constexpr int c_menuBackupPool = 65536;
static constexpr int c_menuMaxDepth = 8;

static uint8_t s_menuBackup[c_menuBackupPool];
static int s_backupUsed = 0;

struct BackupRec {
    int x, y, w, h, offset;
    bool valid;
};
static BackupRec s_backup[c_menuMaxDepth];

void pushBackground(int depth, int x, int y, int w, int h)
{
    BackupRec& r = s_backup[depth];
    r.valid = false;
    uint8_t* frame = graphics_get_frame();
    if (!frame || depth < 0 || depth >= c_menuMaxDepth)
        return;

    const int screenW = graphics_get_width();
    const int screenH = graphics_get_height();
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > screenW) w = screenW - x;
    if (y + h > screenH) h = screenH - y;
    if (w <= 0 || h <= 0 || s_backupUsed + w * h > c_menuBackupPool)
        return;   // не влезло — просто не восстановим, кадр всё равно перерисуется

    r = {x, y, w, h, s_backupUsed, true};
    for (int row = 0; row < h; ++row)
        std::memcpy(s_menuBackup + r.offset + row * w,
                    frame + (y + row) * screenW + x, w);
    s_backupUsed += w * h;
}

void popBackground(int depth)
{
    if (depth < 0 || depth >= c_menuMaxDepth)
        return;
    BackupRec& r = s_backup[depth];
    uint8_t* frame = graphics_get_frame();
    if (r.valid && frame) {
        const int screenW = graphics_get_width();
        for (int row = 0; row < r.h; ++row)
            std::memcpy(frame + (r.y + row) * screenW + r.x,
                        s_menuBackup + r.offset + row * r.w, r.w);
        s_backupUsed = r.offset;
    }
    r.valid = false;
}

int pageHeight(const MenuPage& page)
{
    const int fontH = graphics_get_font_height();
    const int rowH = fontH + 3;
    const int rows = page.itemCount > 0 ? page.itemCount : 1;
    const int statusRows = (page.getStatusLine1 ? 1 : 0) + (page.getStatusLine2 ? 1 : 0);
    return fontH + 6 + (rows + statusRows) * rowH + 2;
}

int backupW = 0;
int backupH = 0;
bool backupValid = false;

// Геометрия строки списка внутри страницы
int rowTop(int y, int index)
{
    const int fontH = graphics_get_font_height();
    return y + fontH + 5 + index * (fontH + 3);
}

// Одна строка списка. Вынесена отдельно, чтобы при перемещении выделения
// перерисовывать только две строки — ту, с которой ушли, и ту, на которую
// пришли. Полная перерисовка страницы на каждое нажатие давала заметное
// мигание.
void drawItem(const MenuPage& page, int index, int selected, int x, int y, int w)
{
    if (!page.items || index < 0 || index >= page.itemCount)
        return;

    const int fontW = graphics_get_font_width();
    const int fontH = graphics_get_font_height();
    const int rowH = fontH + 3;
    const int rowY = rowTop(y, index);
    const bool current = index == selected;
    const bool enabled = !page.items[index].isEnabled || page.items[index].isEnabled();
    const uint8_t fg = !enabled ? RGB888(128, 128, 128)
                     : current ? RGB888(255, 255, 255) : RGB888(0, 0, 0);

    graphics_fill(x + 2, rowY, w - 4, rowH,
                  current ? RGB888(64, 96, 192) : RGB888(232, 232, 232));
    const char* itemTitle = page.items[index].getTitle
                          ? page.items[index].getTitle()
                          : page.items[index].title;
    graphics_type(x + 6, rowY + 1, fg, itemTitle, std::strlen(itemTitle));

    if ((page.getValue && page.getValue() == index)
        || (page.items[index].isChecked && page.items[index].isChecked())) {
        const char mark[] = "*";
        graphics_type(x + w - fontW - 6, rowY + 1, fg, mark, 1);
    }
    if (page.items[index].submenu) {
        const char arrow[] = ">";
        graphics_type(x + w - fontW - 6, rowY + 1, fg, arrow, 1);
    }
}

void drawPage(const MenuPage& page, int selected, int x, int y, int w, int h)
{
    const int fontH = graphics_get_font_height();

    graphics_fill(x + 4, y + 4, w, h, RGB888(32, 32, 32));
    graphics_fill(x, y, w, h, RGB888(232, 232, 232));
    graphics_rect(x, y, w, h, RGB888(0, 0, 0));
    graphics_fill(x + 1, y + 1, w - 2, fontH + 4, RGB888(0, 48, 128));

    const char* title = page.getTitle ? page.getTitle() : page.title;
    graphics_type(x + 5, y + 3, RGB888(255, 255, 255), title, std::strlen(title));

    if (!page.items || page.itemCount == 0) {
        const char emptyText[] = "Not implemented yet";
        graphics_type(x + 6, y + fontH + 6, RGB888(0, 0, 0), emptyText, sizeof(emptyText) - 1);
        return;
    }

    for (int i = 0; i < page.itemCount; ++i)
        drawItem(page, i, selected, x, y, w);

    int statusIndex = page.itemCount;
    if (page.getStatusLine1) {
        const char* text = page.getStatusLine1();
        graphics_type(x + 6, rowTop(y, statusIndex++) + 1,
                      RGB888(0, 0, 0), text, std::strlen(text));
    }
    if (page.getStatusLine2) {
        const char* text = page.getStatusLine2();
        graphics_type(x + 6, rowTop(y, statusIndex) + 1,
                      RGB888(0, 0, 0), text, std::strlen(text));
    }
}

// Начало меню на экране. Пока меню открыто, кнопки сдвига двигают его, а не
// картинку: иначе, сдвинув изображение для удобства, часть меню и файлового
// диалога уезжает за пределы видимой области и вернуть её нечем.
// Положение меню хранится в координатах ЭКРАНА, а не кадрового буфера.
// Меню рисуется в тот же буфер, что и картинка, поэтому сдвиг картинки уносил
// бы его вместе с собой. Перевод в координаты буфера делается при раскладке:
// сколько прибавили экрану, столько вычитается у меню. Заодно это само собой
// даёт нужное поведение в NTSC, где картинка по умолчанию приподнята.
int s_menuScreenX = 0;
int s_menuScreenY = 0;

struct MenuState {
    bool open = false;
    // Автоповтор навигации. Ни HID-слой, ни PS/2-слой повторов не шлют:
    // события идут только на изменение состояния клавиши, а repeat_handler()
    // в Main.cpp — пустая заглушка. Файловый диалог обходится опросом
    // pressed_key[] в собственном цикле, а меню событийное, поэтому повтор
    // отсчитывается здесь, по тикам основного цикла.
    PalKeyCode repeatKey = PK_NONE;
    uint64_t repeatAt = 0;
    bool wasPaused = false;
    bool wasMuted = false;
    SoundMixer* mixer = nullptr;
    const MenuPage* stack[c_menuMaxDepth] = {};
    int selected[c_menuMaxDepth] = {};
    int pageX[c_menuMaxDepth] = {};
    int pageY[c_menuMaxDepth] = {};
    int pageW[c_menuMaxDepth] = {};
    int pageH[c_menuMaxDepth] = {};
    int depth = 0;
    int screenH = 0;
    int menuW = 0;
};

MenuState menu;

// Перерисовывает только текущий уровень: родительские страницы остаются
// на экране, каскад никуда не двигается
void redrawMenu()
{
    if (!menu.open)
        return;
    const int d = menu.depth;
    drawPage(*menu.stack[d], menu.selected[d],
             menu.pageX[d], menu.pageY[d], menu.pageW[d], menu.pageH[d]);
}

// Обновляет выбранный пункт родителя и текущую страницу. Текущая
// страница рисуется последней, поэтому каскад остаётся поверх родителя.
void redrawMenuAndParentItem()
{
    if (!menu.open)
        return;
    const int d = menu.depth;
    if (d > 0) {
        drawItem(*menu.stack[d - 1], menu.selected[d - 1], menu.selected[d - 1],
                 menu.pageX[d - 1], menu.pageY[d - 1], menu.pageW[d - 1]);
    }
    redrawMenu();
}


// Перемещение выделения: перерисовываются ровно две строки
void redrawSelection(int oldSel, int newSel)
{
    if (!menu.open || oldSel == newSel)
        return;
    const int d = menu.depth;
    const MenuPage& page = *menu.stack[d];
    drawItem(page, oldSel, newSel, menu.pageX[d], menu.pageY[d], menu.pageW[d]);
    drawItem(page, newSel, newSel, menu.pageX[d], menu.pageY[d], menu.pageW[d]);
}


// Шаг выделения с заворотом. Возвращает true, если позиция изменилась.
bool moveSelection(int delta)
{
    const int d = menu.depth;
    const MenuPage* page = menu.stack[d];
    if (page->itemCount <= 0)
        return false;
    const int oldSel = menu.selected[d];
    int sel = oldSel + delta;
    if (sel < 0) sel = page->itemCount - 1;
    else if (sel >= page->itemCount) sel = 0;
    menu.selected[d] = sel;
    redrawSelection(oldSel, sel);
    return sel != oldSel;
}


// Ширина страницы по самому длинному пункту
int pageWidth(const MenuPage& page)
{
    const int fontW = graphics_get_font_width();
    size_t longest = std::strlen(page.getTitle ? page.getTitle() : page.title);
    for (int i = 0; i < page.itemCount; ++i) {
        const char* itemTitle = page.items[i].getTitle
                              ? page.items[i].getTitle()
                              : page.items[i].title;
        longest = std::max(longest, std::strlen(itemTitle));
    }
    if (page.itemCount == 0)
        longest = std::max(longest, sizeof("Not implemented yet") - 1);
    if (page.getStatusLine1)
        longest = std::max(longest, std::strlen(page.getStatusLine1()));
    if (page.getStatusLine2)
        longest = std::max(longest, std::strlen(page.getStatusLine2()));
    return static_cast<int>(longest + 4) * fontW;
}


// Геометрия одного уровня каскада. Корневой уровень стоит в начале меню,
// подменю раскрывается вправо от родителя с выравниванием по выбранному пункту.
void layoutLevel(int d)
{
    const int screenW = graphics_get_width();
    const MenuPage& page = *menu.stack[d];

    int nw = std::min(pageWidth(page), screenW - 4);
    int nh = std::min(pageHeight(page), menu.screenH);
    int nx, ny;

    if (d == 0) {
        // из экранных координат в координаты буфера
        nx = s_menuScreenX - graphics_get_picture_shift_x();
        ny = s_menuScreenY - graphics_get_picture_shift_y();
    } else {
        const int fontH = graphics_get_font_height();
        nx = menu.pageX[d - 1] + menu.pageW[d - 1] + 2;
        ny = menu.pageY[d - 1] + fontH + 5 + menu.selected[d - 1] * (fontH + 3) - 2;
    }

    if (nx + nw + 4 > screenW) nx = std::max(0, screenW - nw - 4);
    if (ny + nh + 4 > menu.screenH) ny = std::max(0, menu.screenH - nh - 4);
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;

    menu.pageX[d] = nx;
    menu.pageY[d] = ny;
    menu.pageW[d] = nw;
    menu.pageH[d] = nh;
}

// Полная перекладка каскада: нужна при открытии меню и при его сдвиге.
// Все уровни снимаются в обратном порядке, затем раскладываются заново
// от нового начала и рисуются сверху вниз.
//
// Фон сохраняется и для корневого уровня. Раньше он не сохранялся, потому что
// при закрытии кадр всё равно перерисовывается целиком, — но меню теперь можно
// двигать, а без сохранённого фона сдвинутая страница оставляла бы за собой
// след: стереть его нечем, эмуляция стоит на паузе.
void relayoutMenu()
{
    for (int d = menu.depth; d >= 0; --d)
        popBackground(d);
    s_backupUsed = 0;

    for (int d = 0; d <= menu.depth; ++d) {
        layoutLevel(d);
        pushBackground(d, menu.pageX[d], menu.pageY[d],
                       menu.pageW[d] + 5, menu.pageH[d] + 5);
        drawPage(*menu.stack[d], menu.selected[d],
                 menu.pageX[d], menu.pageY[d], menu.pageW[d], menu.pageH[d]);
    }
}

} // namespace

void palSnapshotHotkey(unsigned slot, bool save)
{
    if (!g_emulation || slot < 1 || slot > 12)
        return;

    // Тот же порядок, что при открытии меню: пауза и тишина на время
    // блокирующей операции, затем возврат прежнего состояния. Снимок тоже
    // содержит признак паузы, но приоритет остаётся за состоянием машины
    // до нажатия — так же ведёт себя загрузка из меню.
    SoundMixer* mixer = g_emulation->getSoundMixer();
    const bool wasPaused = g_emulation->getPausedState();
    const bool wasMuted = mixer && mixer->getMuted();

    g_emulation->setPaused(true);
    if (mixer)
        mixer->setMuted(true);

    if (save)
        saveSnapshotSlot(slot);
    else
        loadSnapshotSlot(slot);

    if (mixer)
        mixer->setMuted(wasMuted);
    g_emulation->setPaused(wasPaused);
}

void palOpenMainMenu()
{
    if (menu.open || !g_emulation)
        return;

    menu.mixer = g_emulation->getSoundMixer();
    menu.wasPaused = g_emulation->getPausedState();
    menu.wasMuted = menu.mixer && menu.mixer->getMuted();
    s_userMuted = menu.wasMuted;

    g_emulation->setPaused(true);
    if (menu.mixer)
        menu.mixer->setMuted(true);

    const int screenW = graphics_get_width();
    menu.screenH = graphics_get_height();

    s_backupUsed = 0;
    for (int i = 0; i < c_menuMaxDepth; ++i)
        s_backup[i].valid = false;

    menu.menuW = std::min(pageWidth(rootPage), screenW - 4);
    menu.stack[0] = &rootPage;
    std::fill(std::begin(menu.selected), std::end(menu.selected), 0);
    menu.depth = 0;
    menu.open = true;
    relayoutMenu();
}

void palCloseMainMenu()
{
    if (!menu.open)
        return;

    // Уровни снимаются в обратном порядке, чтобы фон вернулся на место.
    // Корневой уровень тоже: его фон теперь сохраняется, потому что меню
    // можно двигать.
    for (int d = menu.depth; d >= 0; --d)
        popBackground(d);
    menu.open = false;
    if (menu.mixer)
        menu.mixer->setMuted(s_userMuted);
    if (g_emulation)
        g_emulation->setPaused(menu.wasPaused);
    menu.mixer = nullptr;
}

bool palMainMenuIsOpen()
{
    return menu.open;
}

bool palMainMenuHandleKey(PalKeyCode keyCode, bool isPressed)
{
    if (!menu.open)
        return false;

    if (!isPressed) {
        // Отпускание клавиши гасит автоповтор
        if (keyCode == menu.repeatKey)
            menu.repeatKey = PK_NONE;
        return true;
    }

    // Новое нажатие всегда перезапускает отсчёт
    menu.repeatKey = PK_NONE;

    const MenuPage* page = menu.stack[menu.depth];
    const bool enter = keyCode == PK_ENTER || keyCode == PK_KP_ENTER
                    || keyCode == PK_SPACE;

    if (keyCode == PK_ESC || keyCode == PK_LEFT) {
        // Влево и Esc возвращают на уровень выше, с верхнего — закрывают меню
        if (menu.depth == 0) {
            if (keyCode == PK_ESC)
                palCloseMainMenu();
            return true;
        }
        // Фон под закрываемым подменю возвращается на место
        popBackground(menu.depth);
        --menu.depth;
        return true;   // родительская страница уже на экране, перерисовка не нужна
    } else if (page->itemCount > 0 && (keyCode == PK_UP || keyCode == PK_DOWN)) {
        moveSelection(keyCode == PK_UP ? -1 : 1);
        menu.repeatKey = keyCode;
        menu.repeatAt = time_us_64() + c_repeatDelayUs;
        return true;   // перерисованы только две строки, полная не нужна
    } else if (page->itemCount > 0 && (keyCode == PK_RIGHT || enter)) {
        const int sel = menu.selected[menu.depth];
        const MenuItem& item = page->items[sel];
        if (item.isEnabled && !item.isEnabled())
            return true;

        if (page->setValue) {
            // Страница-радиогруппа: выбор пункта меняет значение.
            // Нужную реакцию (включая reset при смене ядра) выполняет setter.
            if (page->getValue && page->getValue() != sel) {
                page->setValue(sel);
                palCloseMainMenu();
                return true;
            }
            if (keyCode == PK_RIGHT)
                return true;   // значение уже выбрано, вправо ничего не делает
            palCloseMainMenu();
            return true;
        }

        if (item.action) {
            item.action();
            if (item.keepOpen) {
                if (item.action == driveAToggleReadOnly || item.action == driveBToggleReadOnly
                    || item.action == toggleSoundOutput || item.action == togglePsgStereo
                    || item.action == tapeEject)
                    redrawMenuAndParentItem();
                else
                    redrawMenu();
                return true;
            }
            palCloseMainMenu();
            return true;
        }

        const MenuPage* submenu = item.submenu;
        if (!submenu)
            return true;
        if (menu.depth + 1 >= static_cast<int>(sizeof(menu.stack) / sizeof(menu.stack[0])))
            return true;
        menu.stack[menu.depth + 1] = submenu;
        ++menu.depth;
        layoutLevel(menu.depth);
        // graphics_fill и graphics_rect работают по включительным границам,
        // а тень уходит на 4 пикселя вправо и вниз, поэтому сохраняем
        // на пиксель больше по каждой оси
        pushBackground(menu.depth, menu.pageX[menu.depth], menu.pageY[menu.depth],
                       menu.pageW[menu.depth] + 5, menu.pageH[menu.depth] + 5);
        // В радиогруппе изначально выделено текущее значение
        menu.selected[menu.depth] = submenu->getValue ? submenu->getValue() : 0;
        if (menu.selected[menu.depth] < 0 || menu.selected[menu.depth] >= submenu->itemCount)
            menu.selected[menu.depth] = 0;
    } else {
        return true;
    }

    if (menu.open)
        redrawMenu();
    return true;
}


void palMessageBox(const char* title, const char* text)
{
    const int screenW = graphics_get_width();
    const int screenH = graphics_get_height();
    const int fontW = graphics_get_font_width();
    const int fontH = graphics_get_font_height();

    const int textLen = static_cast<int>(std::strlen(text));
    const int titleLen = static_cast<int>(std::strlen(title));
    int w = (std::max(textLen, titleLen) + 3) * fontW;
    if (w > screenW - 8) w = screenW - 8;
    const int h = fontH * 2 + 12;
    const int x = (screenW - w) / 2;
    const int y = (screenH - h) / 2;

    graphics_fill(x + 4, y + 4, w, h, RGB888(32, 32, 32));            // тень
    graphics_fill(x, y, w, h, RGB888(232, 232, 232));
    graphics_rect(x, y, w, h, RGB888(0, 0, 0));
    graphics_fill(x + 1, y + 1, w - 2, fontH + 4, RGB888(0, 48, 128)); // заголовок
    graphics_type(x + 5, y + 3, RGB888(255, 255, 255), title, titleLen);
    graphics_type(x + 5, y + fontH + 8, RGB888(0, 0, 0), text, textLen);
}


void palMainMenuTick()
{
    if (!menu.open || menu.repeatKey == PK_NONE)
        return;

    const uint64_t now = time_us_64();
    if (now < menu.repeatAt)
        return;

    moveSelection(menu.repeatKey == PK_UP ? -1 : 1);
    menu.repeatAt = now + c_repeatRateUs;
}


void palMainMenuShift(int dx, int dy)
{
    if (!menu.open)
        return;

    const int screenW = graphics_get_width();
    const int visibleH = (int)graphics_get_visible_height();

    s_menuScreenX += dx;
    s_menuScreenY += dy;

    // Меню не должно уходить за пределы видимой области, иначе его уже
    // не вернуть теми же кнопками
    if (s_menuScreenX < 0) s_menuScreenX = 0;
    if (s_menuScreenY < 0) s_menuScreenY = 0;
    if (s_menuScreenX > screenW - 16) s_menuScreenX = std::max(0, screenW - 16);
    if (s_menuScreenY > visibleH - 16) s_menuScreenY = std::max(0, visibleH - 16);

    relayoutMenu();
}

// Наружу отдаются координаты буфера: файловый диалог рисуется туда же
int palMainMenuOriginX() {return s_menuScreenX - graphics_get_picture_shift_x();}
int palMainMenuOriginY() {return s_menuScreenY - graphics_get_picture_shift_y();}
