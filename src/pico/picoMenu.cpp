#include "picoMenu.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <memory>
#include <iterator>
#include <string>

#include <pico/time.h>
#include "ff.h"

#include "graphics.h"
#include "../Globals.h"
#include "../Emulation.h"
#include "../Specialist.h"
#include "../SoundMixer.h"
#include "pico/picoPal.h"

extern PalKeyCodeAction getKey();
extern "C" FIL g_file;

#if defined(SPECIALIST_SCANLINES) || defined(HDMI_DVI)
#define SCANLINE_TEXT_MENU 1
#endif

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


// --- Persistent editable settings ------------------------------------------

constexpr const char* c_stateFileName = "/.config/specialist.cfg";
// Промежуточного буфера на весь файл больше нет: конфиг пишется прямо в файл
// по кускам, а читается построчно в маленький стековый буфер. SRAM дорога.

// Смещение картинки — это подстройка конкретного устройства вывода, а не
// свойство эмуляции: у PAL/NTSC-композита, VGA и HDMI/DVI разная геометрия
// растра и своя центровка на «железе». Поэтому в общем файле состояния офсеты
// (а вместе с ними и положение меню, которое считается от них) хранятся под
// собственным для каждого режима ключом. Одна SD-карта, переставляемая между
// прошивками, больше не смешивает подстройки разных экранов.
#if defined(SOFTTV) && defined(TV_NTSC)
    #define VIDEO_OFFSET_PREFIX "ntsc_"
#elif defined(SOFTTV)
    #define VIDEO_OFFSET_PREFIX "pal_"
#elif defined(HDMI_DVI)
    #define VIDEO_OFFSET_PREFIX "dvi_"
#elif defined(VGA_DRV)
    #define VIDEO_OFFSET_PREFIX "vga_"
#elif defined(HDMI)
    #define VIDEO_OFFSET_PREFIX "hdmi_"
#elif defined(RGB_TV)
    #define VIDEO_OFFSET_PREFIX "rgbtv_"
#else
    // TFT/ST7789 и прочие: сохраняем прежние ключи video_offset_*.
    #define VIDEO_OFFSET_PREFIX ""
#endif

#define VIDEO_OFFSET_X_KEY VIDEO_OFFSET_PREFIX "video_offset_x"
#define VIDEO_OFFSET_Y_KEY VIDEO_OFFSET_PREFIX "video_offset_y"

char* trimText(char* text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        ++text;
    char* end = text + std::strlen(text);
    while (end != text && (end[-1] == ' ' || end[-1] == '\t'
                           || end[-1] == '\r' || end[-1] == '\n'))
        --end;
    *end = '\0';
    return text;
}

bool parseUnsignedValue(const char* text, unsigned& value)
{
    if (!text || !*text || *text == '-')
        return false;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 0);
    if (end == text || *trimText(end) != '\0')
        return false;
    value = static_cast<unsigned>(parsed);
    return true;
}

bool parseSignedValue(const char* text, int& value)
{
    if (!text || !*text)
        return false;
    char* end = nullptr;
    const long parsed = std::strtol(text, &end, 0);
    if (end == text || *trimText(end) != '\0')
        return false;
    value = static_cast<int>(parsed);
    return true;
}


bool parseJoystickMap(const char* text, PalKeyCode out[PAL_JOY_CONTROL_COUNT])
{
    for (unsigned i = 0; i < PAL_JOY_CONTROL_COUNT; ++i) {
        while (*text == ' ' || *text == '\t' || *text == ',')
            ++text;
        if (!*text)
            return false;
        char* end = nullptr;
        const long value = std::strtol(text, &end, 0);
        if (end == text || value <= PK_NONE || value > PK_JOY_RIGHT)
            return false;
        out[i] = static_cast<PalKeyCode>(value);
        text = end;
        while (*text == ' ' || *text == '\t')
            ++text;
        if (i + 1 < PAL_JOY_CONTROL_COUNT) {
            if (*text != ',')
                return false;
            ++text;
        }
    }
    return *trimText(const_cast<char*>(text)) == '\0';
}

bool textEquals(const char* a, const char* b)
{
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb)
            return false;
    }
    return *a == '\0' && *b == '\0';
}

bool parseBoolValue(const char* text, bool& value)
{
    if (textEquals(text, "1") || textEquals(text, "yes")
        || textEquals(text, "true") || textEquals(text, "on")) {
        value = true;
        return true;
    }
    if (textEquals(text, "0") || textEquals(text, "no")
        || textEquals(text, "false") || textEquals(text, "off")) {
        value = false;
        return true;
    }
    return false;
}

static void cfgPut(const char* s)
{
    UINT bw = 0;
    f_write(&g_file, s, static_cast<UINT>(std::strlen(s)), &bw);
}

static void cfgPutUnsigned(unsigned value)
{
    char digits[10];
    int count = 0;
    do { digits[count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
    char out[10];
    for (int i = 0; i < count; ++i) out[i] = digits[count - 1 - i];
    UINT bw = 0;
    f_write(&g_file, out, static_cast<UINT>(count), &bw);
}

static void cfgPutSigned(int value)
{
    if (value < 0) { cfgPut("-"); cfgPutUnsigned(static_cast<unsigned>(-value)); }
    else cfgPutUnsigned(static_cast<unsigned>(value));
}

static void cfgPutBool(bool value) { cfgPut(value ? "yes" : "no"); }

// Читает одну строку конфигурации из файла в маленький буфер. Возвращает false
// на конце файла (последняя строка без '\n' тоже отдаётся). '\r' отбрасывается,
// хвост слишком длинной строки игнорируется (key=value помещается в начале).
static bool readConfigLine(FIL* fp, char* buf, size_t size)
{
    size_t n = 0;
    for (;;) {
        char c;
        UINT br = 0;
        if (f_read(fp, &c, 1, &br) != FR_OK || br == 0) {
            buf[n] = '\0';
            return n > 0;
        }
        if (c == '\n') { buf[n] = '\0'; return true; }
        if (c == '\r') continue;
        if (n + 1 < size) buf[n++] = c;
    }
}

// --- Processor -------------------------------------------------------------

int cpuGetValue()
{
    if (!g_emulation || !g_emulation->getSpecialist())
        return 0;
    return static_cast<int>(g_emulation->getSpecialist()->getCpuType());
}

void cpuSetValue(int value)
{
    SpecialistCore* core = g_emulation ? g_emulation->getSpecialist() : nullptr;
    if (!core)
        return;
    core->setCpuType(static_cast<SpecCpuType>(value));
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
    2000000, 3000000, 4000000, 6000000, 8000000
};

int cpuClockGetValue()
{
    SpecialistCore* core = g_emulation ? g_emulation->getSpecialist() : nullptr;
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
    SpecialistCore* core = g_emulation ? g_emulation->getSpecialist() : nullptr;
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

static const char* cpuClockStatus()
{
    static char cpuClockStatusBuffer[17];
#if PICO_RP2040
    char* dst = appendText(cpuClockStatusBuffer, "RP2040: ");
#else
    char* dst = appendText(cpuClockStatusBuffer, "RP2350: ");
#endif
    dst = appendUnsigned(dst, palGetSystemClockMHz());
    dst = appendText(dst, " MHz");
    *dst = '\0';
    return cpuClockStatusBuffer;
}

static const MenuItem cpuClockItems[] = {
    {"2 MHz"},
    {"3 MHz"},
    {"4 MHz"},
    {"6 MHz"},
    {"8 MHz"},
    {nullptr, cpuFpsStatus, nullptr, nullptr, menuItemDisabled, nullptr},
    {nullptr, cpuLoadStatus, nullptr, nullptr, menuItemDisabled, nullptr},
    {nullptr, cpuClockStatus, nullptr, nullptr, menuItemDisabled, nullptr},
};

static const MenuPage cpuClockPage {
    "CPU-Clock",
    nullptr,
    cpuClockItems,
    static_cast<int>(sizeof(cpuClockItems) / sizeof(cpuClockItems[0])),
    cpuClockGetValue,
    cpuClockSetValue
};

void invokeSysReq(SysReq request)
{
    if (g_emulation && g_emulation->getSpecialist())
        g_emulation->getSpecialist()->sysReq(request);
}

void programLoad() { invokeSysReq(SR_LOAD); }
void programLoadAndRun() { invokeSysReq(SR_LOADRUN); }
static const MenuItem programItems[] = {
    {"Load [Alt+L]...", nullptr, nullptr, programLoad, nullptr, nullptr},
    {"Load and run [Alt+F3]...", nullptr, nullptr, programLoadAndRun, nullptr, nullptr},
};
static const MenuPage programPage {"Programs (.RKS)", nullptr, programItems, static_cast<int>(sizeof(programItems) / sizeof(programItems[0])), nullptr, nullptr};

const char* soundTitle()
{
    return palAudioIsI2S() ? "Sound (I2S)" : "Sound (PWM)";
}

const char* soundOutputTitle()
{
    return palAudioIsI2S() ? "Output: I2S (to PWM)" : "Output: PWM (to I2S)";
}

bool soundOutputEnabled()
{
    return palAudioOutputCanSwitch();
}

void toggleSoundOutput()
{
    palSetAudioOutputI2S(!palAudioIsI2S());
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

static const MenuItem soundItems[] = {
    {nullptr, soundOutputTitle, nullptr, toggleSoundOutput, soundOutputEnabled, nullptr, true},
    {"Volume", nullptr, &volumePage, nullptr, nullptr, nullptr},
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
    SpecialistCore* core = g_emulation ? g_emulation->getSpecialist() : nullptr;
    return tapeStatus(tapeInputStatusBuffer, "Input: ",
                      core ? core->getTapeInputFileName() : std::string());
}

const char* tapeOutputStatus()
{
    SpecialistCore* core = g_emulation ? g_emulation->getSpecialist() : nullptr;
    return tapeStatus(tapeOutputStatusBuffer, "Output: ",
                      core ? core->getTapeOutputFileName() : std::string());
}

bool tapeHooksChecked()
{
    SpecialistCore* core = g_emulation ? g_emulation->getSpecialist() : nullptr;
    return core && core->tapeHooksEnabled();
}

void toggleTapeHooks()
{
    SpecialistCore* core = g_emulation ? g_emulation->getSpecialist() : nullptr;
    if (core)
        core->setTapeHooksEnabled(!core->tapeHooksEnabled());
}

void tapeLoad()
{
    SpecialistCore* core = g_emulation ? g_emulation->getSpecialist() : nullptr;
    if (core)
        core->chooseTapeInput();
}

void tapeCreate()
{
    SpecialistCore* core = g_emulation ? g_emulation->getSpecialist() : nullptr;
    if (core)
        core->chooseTapeOutput();
}

bool tapeEjectEnabled()
{
    SpecialistCore* core = g_emulation ? g_emulation->getSpecialist() : nullptr;
    return core && core->tapeFilePresent();
}

void tapeEject()
{
    SpecialistCore* core = g_emulation ? g_emulation->getSpecialist() : nullptr;
    if (core)
        core->ejectTapeFiles();
}

bool textMenuMode();
static void drawTextDialog(const char* title, const char* const* lines, int lineCount);
static const MenuItem tapeItems[] = {
    {"Redirect tape I/O [Alt+T]", nullptr, nullptr, toggleTapeHooks, nullptr, tapeHooksChecked, true},
    {"Load tape image...", nullptr, nullptr, tapeLoad, nullptr, nullptr},
    {"Create new tape...", nullptr, nullptr, tapeCreate, nullptr, nullptr},
    {"Eject", nullptr, nullptr, tapeEject, tapeEjectEnabled, nullptr, true},
};
static const MenuPage tapePage {
    "Tape", nullptr, tapeItems,
    static_cast<int>(sizeof(tapeItems) / sizeof(tapeItems[0])),
    nullptr, nullptr, tapeInputStatus, tapeOutputStatus
};

char videoShiftStatusBuffer[40];

const char* videoShiftStatus()
{
    char* dst = appendText(videoShiftStatusBuffer, "Offset: X=");
    const int x = graphics_get_picture_shift_x();
    if (x < 0) {
        *dst++ = '-';
        dst = appendUnsigned(dst, static_cast<unsigned>(-x));
    } else {
        *dst++ = '+';
        dst = appendUnsigned(dst, static_cast<unsigned>(x));
    }

    dst = appendText(dst, " Y=");
    const int y = graphics_get_picture_shift_y();
    if (y < 0) {
        *dst++ = '-';
        dst = appendUnsigned(dst, static_cast<unsigned>(-y));
    } else {
        *dst++ = '+';
        dst = appendUnsigned(dst, static_cast<unsigned>(y));
    }
    *dst = '\0';
    return videoShiftStatusBuffer;
}

void videoMoveLeft()  { graphics_dec_x(); }
void videoMoveRight() { graphics_inc_x(); }
void videoMoveUp()    { graphics_dec_y(); }
void videoMoveDown()  { graphics_inc_y(); }
void videoCenter()    { graphics_set_offset(0, 0); }

int videoColorGetValue()
{
    auto* core = g_emulation ? g_emulation->getSpecialist() : nullptr;
    return core ? static_cast<int>(core->getColorMode()) : 2;
}

void videoColorSetValue(int value)
{
    if (g_emulation && g_emulation->getSpecialist())
        g_emulation->getSpecialist()->setColorMode(static_cast<SpecColorMode>(value));
}

static const MenuItem colorItems[] = {
    {"Monochrome"},
    {"4 colors"},
    {"8 colors"},
};
static const MenuPage colorPage {"Colors", nullptr, colorItems, 3, videoColorGetValue, videoColorSetValue};

static const MenuItem videoItems[] = {
    {"Colors [Alt+C]", nullptr, &colorPage, nullptr, nullptr, nullptr},
    {"Move left",  nullptr, nullptr, videoMoveLeft,  nullptr, nullptr, true},
    {"Move right", nullptr, nullptr, videoMoveRight, nullptr, nullptr, true},
    {"Move up",    nullptr, nullptr, videoMoveUp,    nullptr, nullptr, true},
    {"Move down",  nullptr, nullptr, videoMoveDown,  nullptr, nullptr, true},
    {"Center",     nullptr, nullptr, videoCenter,    nullptr, nullptr, true},
};
static const MenuPage videoPage {
    "Video", nullptr, videoItems,
    static_cast<int>(sizeof(videoItems) / sizeof(videoItems[0])),
    nullptr, nullptr, videoShiftStatus
};

#if defined(RGB_TV)
static constexpr uint16_t coreVoltageValues[] = {1500, 1600, 1650};
#else
static constexpr uint16_t coreVoltageValues[] = {1300, 1400, 1500, 1600, 1650};
#endif

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
#if defined(HDMI_DVI)
    {"400 MHz", nullptr, nullptr, nullptr, menuItemDisabled, nullptr},
#elif defined(RGB_TV)
    {"448 MHz", nullptr, nullptr, nullptr, menuItemDisabled, nullptr},
#elif defined(PICO_RP2040)
    {"400 MHz", nullptr, nullptr, nullptr, systemClockItemEnabled, nullptr},
    {"402 MHz", nullptr, nullptr, nullptr, systemClockItemEnabled, nullptr},
    {"404 MHz", nullptr, nullptr, nullptr, systemClockItemEnabled, nullptr},
    {"408 MHz", nullptr, nullptr, nullptr, systemClockItemEnabled, nullptr},
    {"412 MHz", nullptr, nullptr, nullptr, systemClockItemEnabled, nullptr},
#else
    {"400 MHz", nullptr, nullptr, nullptr, systemClockItemEnabled, nullptr},
    {"440 MHz", nullptr, nullptr, nullptr, systemClockItemEnabled, nullptr},
    {"480 MHz", nullptr, nullptr, nullptr, systemClockItemEnabled, nullptr},
    {"520 MHz", nullptr, nullptr, nullptr, systemClockItemEnabled, nullptr},
    {"540 MHz", nullptr, nullptr, nullptr, systemClockItemEnabled, nullptr},
#endif
};
static const MenuPage systemClockPage {
    "System clock", nullptr, systemClockItems,
    static_cast<int>(sizeof(systemClockItems) / sizeof(systemClockItems[0])),
    systemClockGetValue, systemClockSetValue
};

static const MenuItem coreVoltageItems[] = {
#if defined(PICO_RP2040)
    {"1.30 V", nullptr, nullptr, nullptr, menuItemDisabled, nullptr},
#elif defined(RGB_TV)
    {"1.50 V", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"1.60 V", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"1.65 V", nullptr, nullptr, nullptr, nullptr, nullptr},
#else
    {"1.30 V", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"1.40 V", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"1.50 V", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"1.60 V", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"1.65 V", nullptr, nullptr, nullptr, nullptr, nullptr},
#endif
};
static const MenuPage coreVoltagePage {
    "Core voltage", nullptr, coreVoltageItems,
    static_cast<int>(sizeof(coreVoltageItems) / sizeof(coreVoltageItems[0])),
    coreVoltageGetValue, coreVoltageSetValue
};

void machineReset() { invokeSysReq(SR_RESET); }
void rebootDevice() { palReboot(); }

static const MenuItem systemItems[] = {
    {"System clock", nullptr, &systemClockPage, nullptr, nullptr, nullptr},
    {"Core voltage", nullptr, &coreVoltagePage, nullptr, nullptr, nullptr},
    {"Reset [Alt+F11]", nullptr, nullptr, machineReset, nullptr, nullptr},
    {"Reboot device [Ctrl+Alt+Del]", nullptr, nullptr, rebootDevice, nullptr, nullptr},
};
static const MenuPage systemPage {
    "System", nullptr, systemItems,
    static_cast<int>(sizeof(systemItems) / sizeof(systemItems[0])), nullptr, nullptr
};


void saveMenuStateImpl()
{
    if (!g_emulation || !g_emulation->getSpecialist() || !palEnsureSdMounted())
        return;
    f_mkdir("/.config");

    SpecialistCore* core = g_emulation->getSpecialist();
    SoundMixer* mixer = g_emulation->getSoundMixer();
    const char* output = palAudioIsI2S() ? "i2s" : "pwm";
    const char* cpu = core->getCpuType() == SpecCpuType::Z80 ? "z80" : "i8080";
    static const char* const keyboardLayouts[] = {"qwerty", "jcuken", "smart"};
    const int keyboardLayout = core->getKbdLayoutModeIndex();
    const int volume = s_userMuted ? 0 : (mixer ? mixer->getVolume() : 5);

    // Пишем прямо в файл, без промежуточного буфера на весь конфиг.
    if (f_open(&g_file, c_stateFileName, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return;

    cfgPut("# Specialist emulator settings. Plain text configuration.\n"
           "# Edit it on a PC while the emulator is not running.\n"
           "# Unknown keys are ignored; invalid values keep the current setting.\n"
           "version = 1\n\n"
           "processor = ");
    cfgPut(cpu);
    cfgPut("                 # i8080 | z80\ncpu_clock_hz = ");
    cfgPutUnsigned(core->getCpuFrequency());
    cfgPut("\nkeyboard_layout = ");
    cfgPut(keyboardLayouts[keyboardLayout >= 0 && keyboardLayout < 3 ? keyboardLayout : 0]);
    cfgPut("          # qwerty | jcuken | smart");

    for (unsigned joystick = 0; joystick < 2; ++joystick) {
        cfgPut(joystick == 0 ? "\njoystick_1_map = " : "\njoystick_2_map = ");
        for (unsigned control = 0; control < PAL_JOY_CONTROL_COUNT; ++control) {
            if (control) cfgPut(",");
            cfgPutUnsigned(static_cast<unsigned>(palGetJoystickMapping(joystick, control)));
        }
    }
    cfgPut("        # up,down,left,right,start,select,A,B");

    cfgPut("\n\nsound_output = ");
    cfgPut(output);
    cfgPut("              # pwm | i2s\nvolume = ");
    cfgPutSigned(volume);
    cfgPut("                    # 0..7; 0 is mute\ncolor_mode = ");
    cfgPutUnsigned(static_cast<unsigned>(core->getColorMode()));
    cfgPut("                # 0 mono | 1 four colors | 2 eight colors\ntape_redirect = ");
    cfgPutBool(core->tapeHooksEnabled());

    cfgPut("\n\n" VIDEO_OFFSET_X_KEY " = ");
    cfgPutSigned(graphics_get_picture_shift_x());
    cfgPut("\n" VIDEO_OFFSET_Y_KEY " = ");
    cfgPutSigned(graphics_get_picture_shift_y());

    cfgPut("\n\nsystem_clock_mhz = ");
    cfgPutUnsigned(static_cast<unsigned>(palGetSystemClockMHz()));
    cfgPut("\ncore_voltage_mv = ");
    cfgPutUnsigned(static_cast<unsigned>(palGetCoreVoltageMv()));
    cfgPut("\n");

    f_sync(&g_file);
    f_close(&g_file);
}

void loadMenuStateImpl()
{
    if (!g_emulation || !g_emulation->getSpecialist() || !palEnsureSdMounted())
        return;
    f_mkdir("/.config");

    if (f_open(&g_file, c_stateFileName, FA_READ) != FR_OK) {
        saveMenuStateImpl();
        return;
    }

    SpecialistCore* core = g_emulation->getSpecialist();
    SoundMixer* mixer = g_emulation->getSoundMixer();
    int videoX = graphics_get_picture_shift_x();
    int videoY = graphics_get_picture_shift_y();
    unsigned systemClock = palGetSystemClockMHz();
    unsigned coreVoltage = palGetCoreVoltageMv();
    char soundOutput[8] = {0};       // копия значения, а не указатель в буфер строки
    bool haveSoundOutput = false;

    // Строки конфига короткие (key = value [# comment]); читаем по одной в
    // маленький стековый буфер и сразу разбираем. Буфера на весь файл нет.
    char line[128];
    while (readConfigLine(&g_file, line, sizeof(line))) {
        char* comment = std::strchr(line, '#');
        if (comment)
            *comment = '\0';
        char* key = trimText(line);
        char* equals = std::strchr(key, '=');
        if (equals) {
            *equals++ = '\0';
            key = trimText(key);
            char* value = trimText(equals);
            bool boolean = false;
            unsigned number = 0;
            int signedNumber = 0;

            if (textEquals(key, "processor")) {
                if (textEquals(value, "z80")) core->setCpuType(SpecCpuType::Z80);
                else if (textEquals(value, "i8080")) core->setCpuType(SpecCpuType::I8080);
            } else if (textEquals(key, "cpu_clock_hz") && parseUnsignedValue(value, number)) {
                for (unsigned frequency : cpuClockValues)
                    if (frequency == number) core->setCpuFrequency(number);
            } else if (textEquals(key, "keyboard_layout")) {
                if (textEquals(value, "jcuken")) core->sysReq(SR_JCUKEN);
                else if (textEquals(value, "smart")) core->sysReq(SR_SMART);
                else if (textEquals(value, "qwerty")) core->sysReq(SR_QUERTY);
            } else if (textEquals(key, "joystick_1_map") || textEquals(key, "joystick_2_map")) {
                PalKeyCode mapping[PAL_JOY_CONTROL_COUNT];
                if (parseJoystickMap(value, mapping)) {
                    const unsigned joystick = textEquals(key, "joystick_2_map") ? 1u : 0u;
                    for (unsigned control = 0; control < PAL_JOY_CONTROL_COUNT; ++control)
                        palSetJoystickMapping(joystick, control, mapping[control]);
                }
            } else if (textEquals(key, "sound_output")) {
                if (textEquals(value, "pwm") || textEquals(value, "i2s")) {
                    std::strncpy(soundOutput, value, sizeof(soundOutput) - 1);
                    soundOutput[sizeof(soundOutput) - 1] = '\0';
                    haveSoundOutput = true;
                }
            } else if (textEquals(key, "volume") && parseUnsignedValue(value, number) && number <= 7) {
                s_userMuted = number == 0;
                if (mixer) {
                    if (number != 0) mixer->setVolume(static_cast<int>(number));
                    mixer->setMuted(s_userMuted);
                }
            } else if (textEquals(key, "color_mode") && parseUnsignedValue(value, number) && number <= 2) {
                core->setColorMode(static_cast<SpecColorMode>(number));
            } else if (textEquals(key, "tape_redirect") && parseBoolValue(value, boolean)) {
                core->setTapeHooksEnabled(boolean);
            } else if (textEquals(key, VIDEO_OFFSET_X_KEY) && parseSignedValue(value, signedNumber)) {
                videoX = signedNumber;
            } else if (textEquals(key, VIDEO_OFFSET_Y_KEY) && parseSignedValue(value, signedNumber)) {
                videoY = signedNumber;
            } else if (textEquals(key, "system_clock_mhz") && parseUnsignedValue(value, number)) {
                systemClock = number;
            } else if (textEquals(key, "core_voltage_mv") && parseUnsignedValue(value, number)) {
                coreVoltage = number;
            }
        }
    }
    f_close(&g_file);

    for (uint16_t voltage : coreVoltageValues)
        if (voltage == coreVoltage) palSetCoreVoltageMv(voltage);

    if (graphics_system_clock_can_change()) {
        uint32_t clockCount = 0;
        const uint32_t* clocks = graphics_get_supported_system_clocks(&clockCount);
        for (uint32_t i = 0; i < clockCount; ++i)
            if (clocks[i] == systemClock) palSetSystemClockMHz(systemClock);
    }

    graphics_set_offset(std::clamp(videoX, -128, 128), std::clamp(videoY, -128, 128));

    if (haveSoundOutput)
        palSetAudioOutputI2S(textEquals(soundOutput, "i2s"));
}

// Общий модальный текстовый диалог (About, Help): рамка, заголовок и список
// строк по центру. Раньше вся геометрия жила в drawAboutDialog(); вынесена,
// чтобы Help переиспользовал ту же отрисовку.
static void drawTextDialog(const char* title, const char* const* lines, int lineCount)
{
    const int screenW = graphics_get_width();
    const int screenH = graphics_get_height();
    const int visibleH = static_cast<int>(graphics_get_visible_height());
    const int fontW = graphics_get_font_width();
    const int fontH = graphics_get_font_height();
    const bool textMode =
#ifdef SCANLINE_TEXT_MENU
        graphics_get_menu_text_mode();
#else
        false;
#endif
    const int rowH = textMode ? fontH : fontH + 2;

    size_t longest = std::strlen(title);
    for (int i = 0; i < lineCount; ++i)
        longest = std::max(longest, std::strlen(lines[i]));

    int w;
    int h;
    if (textMode) {
        w = static_cast<int>(longest + 4) * fontW;
        h = (lineCount + 1) * fontH;
    } else {
        w = static_cast<int>(longest + 4) * fontW;
        h = fontH + 7 + lineCount * rowH + 4;
    }
    w = std::min(w, screenW - (textMode ? 2 * fontW : 8));
    h = std::min(h, visibleH - (textMode ? 2 * fontH : 8));

    int x = (screenW - w) / 2 - graphics_get_picture_shift_x();
    int y = (visibleH - h) / 2 - graphics_get_picture_shift_y();
    x = std::max(0, std::min(x, screenW - w));
    y = std::max(0, std::min(y, screenH - h));

    if (textMode) {
        graphics_fill(x + fontW, y, w, h + fontH, RGB888(32, 32, 32));
        graphics_fill(x, y, w, h, RGB888(232, 232, 232));
        graphics_fill(x, y, w, fontH, RGB888(0, 48, 128));
        graphics_fill(x, y + fontH, w,
                      h - fontH, RGB888(232, 232, 232));

        graphics_type(x, y, RGB888(255, 255, 255),
                      title, std::strlen(title));

        int lineY = y + fontH;
        for (int i = 0; i < lineCount; ++i) {
            const int len = static_cast<int>(std::strlen(lines[i]));
            const int usableW = w - 3 * fontW;
            const int lineX = x + fontW
                            + std::max(0, (usableW - len * fontW) / 2);
            graphics_type(lineX, lineY, RGB888(0, 0, 0), lines[i], len);
            lineY += fontH;
        }
        return;
    }

    graphics_fill(x + 4, y + 4, w, h, RGB888(32, 32, 32));
    graphics_fill(x, y, w, h, RGB888(232, 232, 232));
    graphics_rect(x, y, w, h, RGB888(0, 0, 0));
    graphics_fill(x + 1, y + 1, w - 2, fontH + 4, RGB888(0, 48, 128));

    graphics_type(x + 5, y + 3, RGB888(255, 255, 255), title,
                  static_cast<int>(std::strlen(title)));

    int lineY = y + fontH + 7;
    for (int i = 0; i < lineCount; ++i) {
        const int len = static_cast<int>(std::strlen(lines[i]));
        const int lineX = x + std::max(5, (w - len * fontW) / 2);
        graphics_type(lineX, lineY, RGB888(0, 0, 0), lines[i], len);
        lineY += rowH;
    }
}

static void showTextDialog(const char* title, const char* const* lines, int lineCount)
{
    drawTextDialog(title, lines, lineCount);
    while (true) {
        sleep_ms(100);
        palInputTick();
        const PalKeyCodeAction key = getKey();
        if (key.pressed && (key.vk == PK_ESC || key.vk == PK_ENTER
                         || key.vk == PK_KP_ENTER || key.vk == PK_SPACE))
            return;
    }
}

void showAboutDialog()
{
    static const char* const lines[] = {
        "Specialist emulator for",
        "Murmulator 1.x / Murmulator 2.0",
        "and Raspberry Pi Pico 2 (RP2350)",
        "",
        "This firmware is a port of the",
        "Specialist platform from the",
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
    showTextDialog("About", lines, static_cast<int>(sizeof(lines) / sizeof(lines[0])));
}

void showHelpDialog()
{
    static const char* const lines[] = {
        "Alt - open / close menu",
        "Ctrl+Alt+Del - hard reset (reboot)",
        "Alt+F11 - reset the machine",
        "Alt+L / Alt+F3 - load / load and run",
        "Alt+T - tape redirect on / off",
        "Alt+C - cycle color mode",
        "Alt+Q / Alt+J / Alt+K - kbd layouts",
        "",
        "Numpad * / move image horizontally",
        "Numpad + - move image vertically",
        "With menu open: same keys move menu",
        "",
        "Gamepad:",
        "Start = menu, Select = Tab",
        "A = Space, B = Enter, D-pad = arrows",
        "",
        "Enter / Esc - close"
    };
    showTextDialog("Help", lines, static_cast<int>(sizeof(lines) / sizeof(lines[0])));
}

int kbdLayoutGetValue()
{
    SpecialistCore* core = g_emulation ? g_emulation->getSpecialist() : nullptr;
    return core ? core->getKbdLayoutModeIndex() : 0;
}

void kbdLayoutSetValue(int value)
{
    switch (value) {
        case 1:  invokeSysReq(SR_JCUKEN); break;
        case 2:  invokeSysReq(SR_SMART);  break;
        default: invokeSysReq(SR_QUERTY); break;
    }
}

static const MenuItem keyboardItems[] = {
    {"QWERTY [Alt+Q]", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"JCUKEN [Alt+J]", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"Smart [Alt+K]",  nullptr, nullptr, nullptr, nullptr, nullptr},
};
static const MenuPage keyboardPage {
    "Keyboard layout", nullptr, keyboardItems,
    static_cast<int>(sizeof(keyboardItems) / sizeof(keyboardItems[0])),
    kbdLayoutGetValue, kbdLayoutSetValue
};

void redrawMenu();
void relayoutMenu();

void captureJoystickMapping(unsigned joystick, unsigned control, const char* title)
{
    static constexpr const char* prompt = "Press desired key to map";
    palMessageBox(title, prompt);
    while (true) {
        sleep_ms(20);
        palInputTick();
        const PalKeyCodeAction key = getKey();
        if (!key.pressed || key.vk == PK_NONE)
            continue;
        if (key.vk != PK_ESC)
            palSetJoystickMapping(joystick, control, key.vk);
        g_emulation->resetKeys();
        relayoutMenu();
        return;
    }
}

#define JOY_ACTION(name, joy, control, title) \
    void name() { captureJoystickMapping(joy, control, title); }

JOY_ACTION(mapJ1Up,     0, PAL_JOY_UP,     "Joystick #1: Up")
JOY_ACTION(mapJ1Down,   0, PAL_JOY_DOWN,   "Joystick #1: Down")
JOY_ACTION(mapJ1Left,   0, PAL_JOY_LEFT,   "Joystick #1: Left")
JOY_ACTION(mapJ1Right,  0, PAL_JOY_RIGHT,  "Joystick #1: Right")
JOY_ACTION(mapJ1Start,  0, PAL_JOY_START,  "Joystick #1: Start")
JOY_ACTION(mapJ1Select, 0, PAL_JOY_SELECT, "Joystick #1: Select")
JOY_ACTION(mapJ1A,      0, PAL_JOY_A,      "Joystick #1: A")
JOY_ACTION(mapJ1B,      0, PAL_JOY_B,      "Joystick #1: B")
JOY_ACTION(mapJ2Up,     1, PAL_JOY_UP,     "Joystick #2: Up")
JOY_ACTION(mapJ2Down,   1, PAL_JOY_DOWN,   "Joystick #2: Down")
JOY_ACTION(mapJ2Left,   1, PAL_JOY_LEFT,   "Joystick #2: Left")
JOY_ACTION(mapJ2Right,  1, PAL_JOY_RIGHT,  "Joystick #2: Right")
JOY_ACTION(mapJ2Start,  1, PAL_JOY_START,  "Joystick #2: Start")
JOY_ACTION(mapJ2Select, 1, PAL_JOY_SELECT, "Joystick #2: Select")
JOY_ACTION(mapJ2A,      1, PAL_JOY_A,      "Joystick #2: A")
JOY_ACTION(mapJ2B,      1, PAL_JOY_B,      "Joystick #2: B")
#undef JOY_ACTION

static const MenuItem joystick1Items[] = {
    {"Up", nullptr, nullptr, mapJ1Up, nullptr, nullptr, true},
    {"Down", nullptr, nullptr, mapJ1Down, nullptr, nullptr, true},
    {"Left", nullptr, nullptr, mapJ1Left, nullptr, nullptr, true},
    {"Right", nullptr, nullptr, mapJ1Right, nullptr, nullptr, true},
    {"Start", nullptr, nullptr, mapJ1Start, nullptr, nullptr, true},
    {"Select", nullptr, nullptr, mapJ1Select, nullptr, nullptr, true},
    {"A", nullptr, nullptr, mapJ1A, nullptr, nullptr, true},
    {"B", nullptr, nullptr, mapJ1B, nullptr, nullptr, true},
};
static const MenuPage joystick1Page {
    "Joystick #1", nullptr, joystick1Items,
    static_cast<int>(sizeof(joystick1Items) / sizeof(joystick1Items[0])), nullptr, nullptr
};

static const MenuItem joystick2Items[] = {
    {"Up", nullptr, nullptr, mapJ2Up, nullptr, nullptr, true},
    {"Down", nullptr, nullptr, mapJ2Down, nullptr, nullptr, true},
    {"Left", nullptr, nullptr, mapJ2Left, nullptr, nullptr, true},
    {"Right", nullptr, nullptr, mapJ2Right, nullptr, nullptr, true},
    {"Start", nullptr, nullptr, mapJ2Start, nullptr, nullptr, true},
    {"Select", nullptr, nullptr, mapJ2Select, nullptr, nullptr, true},
    {"A", nullptr, nullptr, mapJ2A, nullptr, nullptr, true},
    {"B", nullptr, nullptr, mapJ2B, nullptr, nullptr, true},
};
static const MenuPage joystick2Page {
    "Joystick #2", nullptr, joystick2Items,
    static_cast<int>(sizeof(joystick2Items) / sizeof(joystick2Items[0])), nullptr, nullptr
};

static const MenuItem joysticksItems[] = {
    {"Joystick #1", nullptr, &joystick1Page, nullptr, nullptr, nullptr},
    {"Joystick #2", nullptr, &joystick2Page, nullptr, nullptr, nullptr},
};
static const MenuPage joysticksPage {
    "Joysticks", nullptr, joysticksItems,
    static_cast<int>(sizeof(joysticksItems) / sizeof(joysticksItems[0])), nullptr, nullptr
};

static const MenuItem rootItems[] = {
    {"Processor", nullptr, &processorPage, nullptr, nullptr, nullptr},
    {"CPU-Clock", nullptr, &cpuClockPage, nullptr, nullptr, nullptr},
    {"Keyboard", nullptr, &keyboardPage, nullptr, nullptr, nullptr},
    {"Joysticks", nullptr, &joysticksPage, nullptr, nullptr, nullptr},
    {"Programs (.RKS)", nullptr, &programPage, nullptr, nullptr, nullptr},
    {"Sound", soundTitle, &soundPage, nullptr, nullptr, nullptr},
    {"Tape", nullptr, &tapePage, nullptr, nullptr, nullptr},
    {"Video", nullptr, &videoPage, nullptr, nullptr, nullptr},
    {"System", nullptr, &systemPage, nullptr, nullptr, nullptr},
    {"Help", nullptr, nullptr, showHelpDialog, nullptr, nullptr},
    {"About", nullptr, nullptr, showAboutDialog, nullptr, nullptr},
};

// Заголовок корневой страницы отражает установленное ядро
const char* rootTitle()
{
    if (g_emulation && g_emulation->getSpecialist()
        && g_emulation->getSpecialist()->getCpuType() == SpecCpuType::Z80)
        return "Specialist Z80";
    return "Specialist 80A";
}

static const MenuPage rootPage {
    "Specialist 80A",
    rootTitle,
    rootItems,
    static_cast<int>(sizeof(rootItems) / sizeof(rootItems[0])),
    nullptr,
    nullptr
};

// ---------------------------------------------------------------------------
// Задержка до начала автоповтора и период повторов
static constexpr uint64_t c_repeatDelayUs = 400000;
static constexpr uint64_t c_repeatRateUs  = 60000;

static constexpr int c_menuMaxDepth = 8;

bool textMenuMode()
{
#ifdef SCANLINE_TEXT_MENU
    return graphics_get_menu_text_mode();
#else
    return false;
#endif
}

int menuRowHeight()
{
    const int fontH = graphics_get_font_height();
    return textMenuMode() ? fontH : fontH + 3;
}

int pageHeight(const MenuPage& page)
{
    const int rowH = menuRowHeight();
    const int rows = page.itemCount > 0 ? page.itemCount : 1;
    const int statusRows = (page.getStatusLine1 ? 1 : 0)
                         + (page.getStatusLine2 ? 1 : 0);
    if (textMenuMode())
        return (rows + statusRows + 1) * rowH;

    const int fontH = graphics_get_font_height();
    return fontH + 6 + (rows + statusRows) * rowH + 2;
}

// Геометрия строки списка внутри страницы
int rowTop(int y, int index)
{
    if (textMenuMode())
        return y + menuRowHeight() + index * menuRowHeight();

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
    const int rowH = menuRowHeight();
    const int rowY = rowTop(y, index);
    const bool current = index == selected;
    const bool enabled = !page.items[index].isEnabled
                      || page.items[index].isEnabled();
    const uint8_t fg = !enabled ? RGB888(128, 128, 128)
                                : RGB888(0, 0, 0);
    const int insetX = textMenuMode() ? fontW : 2;
    const int fillW = textMenuMode()
                    ? w - insetX
                    : w - 2 * insetX;

    graphics_fill(x + insetX, rowY, fillW, rowH - 1,
                  current ? RGB888(64, 192, 255)
                          : RGB888(232, 232, 232));

    const char* itemTitle = page.items[index].getTitle
                          ? page.items[index].getTitle()
                          : page.items[index].title;
    graphics_type(x + (textMenuMode() ? fontW : 6),
                  rowY + (textMenuMode() ? 0 : 1),
                  fg, itemTitle, std::strlen(itemTitle));

    if ((page.getValue && page.getValue() == index)
        || (page.items[index].isChecked && page.items[index].isChecked())) {
        const char mark[] = "*";
        graphics_type(x + w - fontW - 6,
                      rowY + (textMenuMode() ? 0 : 1), fg, mark, 1);
    }
    if (page.items[index].submenu) {
        const char arrow[] = ">";
        graphics_type(x + w - fontW - 6,
                      rowY + (textMenuMode() ? 0 : 1), fg, arrow, 1);
    }
}

void drawPage(const MenuPage& page, int selected, int x, int y, int w, int h)
{
    const int fontW = graphics_get_font_width();
    const int fontH = graphics_get_font_height();
    if (textMenuMode())
        graphics_fill(x + fontW, y, w, h + fontH, RGB888(32, 32, 32));
    else
        graphics_fill(x + 4, y + 4, w, h, RGB888(32, 32, 32));

    graphics_fill(x, y, w, h, RGB888(232, 232, 232));
    if (!textMenuMode())
        graphics_rect(x, y, w, h, RGB888(0, 0, 0));

    if (textMenuMode()) {
        graphics_fill(x, y, w, fontH, RGB888(0, 48, 128));
    } else {
        graphics_fill(x + 1, y + 1, w - 2, fontH + 4,
                      RGB888(0, 48, 128));
    }

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
        const int marginX = textMenuMode() ? graphics_get_font_width() : 0;
        const int marginY = textMenuMode() ? graphics_get_font_height() : 0;
        nx = marginX + palMainMenuOriginX();
        ny = marginY + palMainMenuOriginY();
    } else {
        const int fontW = graphics_get_font_width();
        const int fontH = graphics_get_font_height();
        nx = menu.pageX[d - 1] + menu.pageW[d - 1]
           + (textMenuMode() ? fontW : 2);
        ny = textMenuMode()
           ? rowTop(menu.pageY[d - 1], menu.selected[d - 1])
           : menu.pageY[d - 1] + fontH + 5
             + menu.selected[d - 1] * (fontH + 3) - 2;
    }

    const int shadowX = textMenuMode() ? graphics_get_font_width() : 4;
    const int shadowY = textMenuMode() ? graphics_get_font_height() : 4;
    if (nx + nw + shadowX > screenW)
        nx = std::max(0, screenW - nw - shadowX);
    if (ny + nh + shadowY > menu.screenH)
        ny = std::max(0, menu.screenH - nh - shadowY);
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;

    menu.pageX[d] = nx;
    menu.pageY[d] = ny;
    menu.pageW[d] = nw;
    menu.pageH[d] = nh;
}

void relayoutMenu()
{
    if (textMenuMode())
        graphics_clear_menu_text();
    else
        g_emulation->getSpecialist()->redrawVideo();

    for (int d = 0; d <= menu.depth; ++d) {
        layoutLevel(d);
        drawPage(*menu.stack[d], menu.selected[d],
                 menu.pageX[d], menu.pageY[d], menu.pageW[d], menu.pageH[d]);
    }
}

} // namespace

void palLoadMenuState()
{
    loadMenuStateImpl();
}

void palSaveMenuState()
{
    saveMenuStateImpl();
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

#ifdef SCANLINE_TEXT_MENU
    graphics_set_video_content_mode(GRAPHICS_VIDEO_TEXT);
#endif

    const int screenW = graphics_get_width();
    menu.screenH = graphics_get_height();

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

    menu.open = false;
#ifdef SCANLINE_TEXT_MENU
    graphics_set_video_content_mode(GRAPHICS_VIDEO_FRAMEBUFFER);
#endif
    if (g_emulation && g_emulation->getSpecialist())
        g_emulation->getSpecialist()->redrawVideo();
    saveMenuStateImpl();
    if (menu.mixer)
        menu.mixer->setMuted(s_userMuted);
    if (g_emulation) {
        g_emulation->resetKeys();
        g_emulation->setPaused(menu.wasPaused);
    }
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
        --menu.depth;
        relayoutMenu();
        return true;
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
                if (item.action == toggleSoundOutput || item.action == tapeEject)
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


namespace {

void messageBoxRect(const char* title, const char* text,
                    int& x, int& y, int& w, int& h)
{
    const int screenW = graphics_get_width();
    const int screenH = graphics_get_height();
    const int fontW = graphics_get_font_width();
    const int fontH = graphics_get_font_height();
    const int textLen = static_cast<int>(std::strlen(text));
    const int titleLen = static_cast<int>(std::strlen(title));

    w = (std::max(textLen, titleLen) + 3) * fontW;
    if (w > screenW - 8)
        w = screenW - 8;
    h = fontH * 2 + 12;
    x = (screenW - w) / 2;
    y = (screenH - h) / 2;
}

} // namespace

void palMessageBox(const char* title, const char* text)
{
    const int fontH = graphics_get_font_height();
    const int textLen = static_cast<int>(std::strlen(text));
    const int titleLen = static_cast<int>(std::strlen(title));
    int x, y, w, h;
    messageBoxRect(title, text, x, y, w, h);

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

#ifdef SCANLINE_TEXT_MENU
    if (graphics_get_menu_text_mode()) {
        dx *= graphics_get_font_width();
        dy *= graphics_get_font_height();
    }
#endif
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
int palMainMenuOriginX() {return s_menuScreenX - (textMenuMode() ? 0 : graphics_get_picture_shift_x());}
int palMainMenuOriginY() {return s_menuScreenY - (textMenuMode() ? 0 : graphics_get_picture_shift_y());}
