#include "picoPal.h"
#include "../OrionMenu.h"
#include "../Emulation.h"
#include "../Globals.h"
#include "ffPalFile.h"

#include <sstream>
#include <iostream>

#include <pico/stdlib.h>
#include <hardware/pio.h>

std::string palGetDefaultPlatform() {
    return "";
}

uint8_t* palReadFile(const string& fileName, int &fileSize, bool useBasePath)
{
    string fullFileName;
    if (useBasePath)
        fullFileName = palMakeFullFileName(fileName);
    else
        fullFileName = fileName;
#if LOG
    emuLog << "fullFileName: '" << fullFileName << "'\n";
#endif
    FIL file;
    if (f_open(&file, fullFileName.c_str(), FA_READ) == FR_OK) {
        fileSize = f_size(&file);
        uint8_t* buf = new uint8_t[fileSize];
        UINT br;
        f_read(&file, buf, fileSize, &br);
        f_close(&file);
        return buf;
    }
    return nullptr;
}

int palReadFromFile(const string& fileName, int offset, int sizeToRead, uint8_t* buffer, bool useBasePath)
{
#if LOG
    emuLog << "palReadFromFile: " << fileName << "\n";
#endif
    string fullFileName;
    if (useBasePath)
        fullFileName = palMakeFullFileName(fileName);
    else
        fullFileName = fileName;

    FIL file;
    if (f_open(&file, fullFileName.c_str(), FA_READ) == FR_OK) {
        f_lseek(&file, offset);
        UINT nBytesRead;
        f_read(&file, buffer, sizeToRead, &nBytesRead);
        f_close(&file);
        return nBytesRead;
    }
    return 0;
}

void palLog(std::string s) {
#if LOG
    static FIL pl;
    gpio_put(PICO_DEFAULT_LED_PIN, true);
    f_open(&pl, "/orion.log", FA_WRITE | FA_OPEN_APPEND);
    UINT bw;
    f_write(&pl, s.c_str(), s.length(), &bw);
    f_close(&pl);
    gpio_put(PICO_DEFAULT_LED_PIN, false);
#endif
}

EmuLog& EmuLog::operator<<(string s)
{
#if LOG
    palLog(s);
#endif
    return *this;
}


EmuLog& EmuLog::operator<<(const char* sz)
{
#if LOG
    string s = sz;
    palLog(s);
#endif
    return *this;
}


EmuLog& EmuLog::operator<<(int n)
{
#if LOG
    ostringstream oss;
    oss << n;
    string s = oss.str();
    palLog(s);
#endif
    return *this;
}

EmuLog emuLog;

string palMakeFullFileName(string fileName)
{
    if (fileName[0] == '\0' || fileName[0] == '/' || fileName[0] == '\\')
        return fileName;
    string fullFileName("/orion/");
    fullFileName += fileName;
    return fullFileName;
}

static std::string clipboard;
std::string palGetTextFromClipboard() {
    return clipboard;
}

void palCopyTextToClipboard(const char* text) {
    clipboard = text;
}

void palGetDirContent(const string& d, vector<PalFileInfo*>& fileList)
{
    DIR dir;
    FILINFO entry;
    if (f_opendir(&dir, d.c_str()) != FR_OK) {
        return;
    }
    while (f_readdir(&dir, &entry) == FR_OK && entry.fname[0] != '\0') {
        PalFileInfo* newFile = new PalFileInfo;
        newFile->fileName = entry.fname;
        newFile->isDir = (entry.fattrib & AM_DIR) != 0;
        newFile->size = entry.fsize;
        newFile->year = 1980 + (entry.fdate >> 9);
        newFile->month = (entry.fdate >> 5) & 0b111;
        newFile->day = entry.fdate & 0b11111;
        newFile->hour = entry.ftime >> 11;
        newFile->minute =  (entry.ftime >> 5) & 0b1111111;
        newFile->second = entry.ftime & 0b11111;
        fileList.push_back(newFile);
    }
    f_closedir(&dir);
}

/// TODO: .h
extern PalKeyCodeAction getKey();
#include <algorithm>
// File browser: the text-mode layout, scrollbar and incremental row redraw
// follow the Specialist/Vector PAL file dialog. Keep Orion's PalWindow API.
namespace {
struct BrowserEntry { std::string name; bool dir; };

char foldAscii(char ch) { return ch >= 'A' && ch <= 'Z' ? char(ch + ('a'-'A')) : ch; }
bool matchGlob(const char* pattern, const char* value)
{
    const char* star = nullptr;
    const char* retry = nullptr;
    while (*value) {
        if (*pattern == '?' || (*pattern && foldAscii(*pattern) == foldAscii(*value))) {
            ++pattern; ++value;
        } else if (*pattern == '*') {
            star = pattern++; retry = value;
        } else if (star) {
            pattern = star + 1; value = ++retry;
        } else return false;
    }
    while (*pattern == '*') ++pattern;
    return !*pattern;
}

std::vector<std::string> filePatterns(const std::string& filter)
{
    // Emu80 filters are "label|*.ext;*.ext|All files|*". The caller may also
    // supply a plain mask or a suffix (e.g. ".rk" for a tape output file).
    const size_t bar = filter.find('|');
    const size_t end = bar == std::string::npos ? std::string::npos : filter.find('|',bar+1);
    std::string masks = bar == std::string::npos ? filter : filter.substr(bar+1,end == std::string::npos ? end : end-bar-1);
    std::vector<std::string> result;
    size_t start = 0;
    while (start < masks.size()) {
        const size_t split = masks.find(';',start);
        auto mask = masks.substr(start,split == std::string::npos ? split : split-start);
        if (!mask.empty() && mask[0] == '.') mask = '*' + mask;
        if (!mask.empty()) result.push_back(mask);
        if (split == std::string::npos) break;
        start = split+1;
    }
    return result;
}

bool browserCloseKey(PalKeyCode k) {
    return k == PK_ESC || k == PK_F12 || k == PK_LALT || k == PK_RALT;
}
}

static std::string fdir = "/orion";
std::string palOpenFileDialog(std::string title, std::string filter, bool write, PalWindow* window) {
    (void)window;
    struct RestoreVideoMode {
        graphics_video_content_mode_t mode = graphics_get_video_content_mode();
        ~RestoreVideoMode() {
            graphics_set_video_content_mode(mode);
            if (g_emulation) g_emulation->resetOrionKeys();
        }
    } restore;
    graphics_set_menu_text_mode(true);
    const int fw = graphics_get_font_width(), fh = graphics_get_font_height();
    const int cols = graphics_get_width()/fw, rows = graphics_get_visible_height()/fh;
    if (cols < 12 || rows < 8) return "";
    const int w = cols-2, h = rows-2, x = 1, y = 1;
    const int top = y+1, bottom = y+h-1;
    const int listTop = top+(write ? 1 : 0), visibleRows = bottom-listTop;
    const int listW = w-1, scrollX = x+listW;
    const uint8_t white = RGB(0xffffff), black = RGB(0), titleBg = RGB(0x003080);
    const uint8_t selectedBg = RGB(0x40c0ff), gray = RGB(0x808080);
    const auto masks = filePatterns(filter);
    bool allFiles = masks.empty(), truncated = false;
    bool inputFocused = write;
    std::string filename;
    // Bounded memory usage on RP2040, as in the reference dialog. Do not
    // allocate one full PalFileInfo object for every entry of a large SD card.
    constexpr size_t maxEntries = 256, maxNameBytes = 16*1024;
    std::vector<BrowserEntry> entries;
    entries.reserve(32);
    int selected = 0, scroll = 0;

    auto fill = [&](int cx,int cy,int cw,int ch,uint8_t color) {
        graphics_fill(cx*fw,cy*fh,cw*fw,ch*fh,color);
    };
    auto text = [&](int cx,int cy,const std::string& value,int width,uint8_t color) {
        graphics_type(cx*fw,cy*fh,color,value.c_str(),std::min(value.size(),size_t(std::max(0,width))));
    };
    auto pathFor = [&](const std::string& name) { return fdir == "/" ? "/"+name : fdir+"/"+name; };
    auto loadDirectory = [&]() -> bool {
        DIR dir;
        if (f_opendir(&dir,fdir.c_str()) != FR_OK) return false;
        entries.clear(); truncated = false;
        size_t nameBytes = 0;
        if (fdir != "/") entries.push_back({"..",true});
        const size_t first = entries.size();
        FILINFO entry;
        FRESULT result;
        while ((result=f_readdir(&dir,&entry)) == FR_OK && entry.fname[0]) {
            if (!strcmp(entry.fname,".") || !strcmp(entry.fname,"..")) continue;
            const bool isDir = (entry.fattrib & AM_DIR) != 0;
            bool matches = isDir || allFiles;
            for (const auto& pattern : masks)
                if (matchGlob(pattern.c_str(),entry.fname)) { matches = true; break; }
            if (!matches) continue;
            const size_t bytes = strlen(entry.fname)+1;
            if (entries.size() == maxEntries || nameBytes+bytes > maxNameBytes) { truncated = true; break; }
            nameBytes += bytes;
            entries.push_back({entry.fname,isDir});
        }
        f_closedir(&dir);
        std::sort(entries.begin()+first,entries.end(),[](const BrowserEntry& a,const BrowserEntry& b) {
            if (a.dir != b.dir) return a.dir > b.dir;
            return std::lexicographical_compare(a.name.begin(),a.name.end(),b.name.begin(),b.name.end(),[](char a,char b){return foldAscii(a)<foldAscii(b);});
        });
        selected = scroll = 0;
        return result == FR_OK;
    };
    if (!loadDirectory()) {
        fdir = "/";
        if (!loadDirectory()) { orionConfirm("File manager","Cannot read SD card"); return ""; }
    }
    auto drawTitle = [&]() {
        fill(x,y,w,1,titleBg);
        std::string prefix = title+": ";
        std::string path = fdir;
        const int room = w-2-int(prefix.size());
        if (room >= 4 && int(path.size()) > room) path = "..."+path.substr(path.size()-room+3);
        text(x+1,y,prefix+path,w-2,white);
    };
    auto drawInput = [&]() {
        if (!write) return;
        fill(x,top,w,1,inputFocused ? RGB(0xcfffff) : RGB(0xd0d0d0));
        const size_t space = size_t(std::max(1,w-8));
        const auto shown = filename.size() >= space ? filename.substr(filename.size()-space+1) : filename;
        text(x+1,top,"Name: "+shown+(inputFocused ? "_" : ""),w-2,black);
    };
    auto drawRow = [&](int index) {
        if (index < scroll || index >= scroll+visibleRows) return;
        const int yy = listTop+index-scroll;
        fill(x,yy,listW,1,index == selected && !entries.empty() ? (inputFocused ? RGB(0xd0d0d0) : selectedBg) : white);
        if (index < int(entries.size())) {
            const auto& item = entries[index];
            text(x+1,yy,item.dir ? "<"+item.name+">" : item.name,listW-2,black);
        }
    };
    auto drawScrollbar = [&]() {
        fill(scrollX,listTop,1,visibleRows,gray);
        if (entries.size() <= size_t(visibleRows)) return;
        const int thumb = std::max(1,visibleRows*visibleRows/int(entries.size()));
        const int offset = (visibleRows-thumb)*scroll/(int(entries.size())-visibleRows);
        fill(scrollX,listTop+offset,1,thumb,white);
    };
    auto drawList = [&]() {
        for (int i=0;i<visibleRows;++i) drawRow(scroll+i);
        if (entries.empty()) text(x+1,listTop,"(No matching files)",listW-2,gray);
        drawScrollbar();
    };
    auto drawFooter = [&]() {
        fill(x,bottom,w,1,black);
        std::string footer = write ? "Tab: name/list  Enter: save" : (allFiles ? "F4: filter" : "F4: all files");
        footer += "  Esc: cancel";
        if (truncated) footer = "List truncated; narrow filter";
        text(x+1,bottom,footer,w-2,white);
    };
    auto redraw = [&]() { fill(x,y,w,h,white); drawTitle(); drawInput(); drawList(); drawFooter(); };
    redraw();
    while (true) {
        const auto key = getKey();
        if (!key.pressed) { sleep_ms(2); continue; }
        if (browserCloseKey(key.vk)) return "";
        if (key.vk == PK_F4 && !write) {
            allFiles = !allFiles;
            if (!loadDirectory()) { orionConfirm("File manager","Cannot read directory"); return ""; }
            redraw(); continue;
        }
        if (write && key.vk == PK_TAB) {
            inputFocused = !inputFocused; drawInput(); drawRow(selected); continue;
        }
        const bool enter = key.vk == PK_ENTER || key.vk == PK_KP_ENTER;
        if (inputFocused) {
            if (enter && !filename.empty()) {
                const auto path = pathFor(filename);
                FILINFO existing;
                if (f_stat(path.c_str(),&existing) == FR_OK) {
                    if (existing.fattrib & AM_DIR) { orionConfirm("Cannot save","A directory has this name"); redraw(); continue; }
                    if (!orionConfirm("Overwrite file","Replace existing file?",true)) { redraw(); continue; }
                }
                return path;
            }
            char ch = 0;
            if (key.vk >= PK_1 && key.vk <= PK_0) ch = key.vk == PK_0 ? '0' : '1'+key.vk-PK_1;
            else if (key.vk >= PK_A && key.vk <= PK_Z) ch = 'a'+key.vk-PK_A;
            else if (key.vk == PK_SPACE) ch = ' ';
            else if (key.vk == PK_PERIOD) ch = '.';
            else if (key.vk == PK_MINUS) ch = '-';
            else if (key.vk == PK_BSP && !filename.empty()) filename.pop_back();
            if (ch && filename.size() < 127) filename += ch;
            drawInput(); continue;
        }
        if (entries.empty()) continue;
        const int previous = selected, previousScroll = scroll, count = int(entries.size());
        if (key.vk == PK_UP || key.vk == PK_KP_8) selected = (selected+count-1)%count;
        else if (key.vk == PK_DOWN || key.vk == PK_KP_2) selected = (selected+1)%count;
        else if (key.vk == PK_PGUP || key.vk == PK_KP_9) selected = std::max(0,selected-visibleRows);
        else if (key.vk == PK_PGDN || key.vk == PK_KP_3) selected = std::min(count-1,selected+visibleRows);
        else if (key.vk == PK_HOME || key.vk == PK_KP_7) selected = 0;
        else if (key.vk == PK_END || key.vk == PK_KP_1) selected = count-1;
        else if (enter) {
            const auto item = entries[selected];
            if (item.dir) {
                const auto oldDir = fdir;
                if (item.name == "..") {
                    fdir = fdir.substr(0,fdir.find_last_of('/'));
                    if (fdir.empty()) fdir = "/";
                } else fdir = pathFor(item.name);
                if (!loadDirectory()) {
                    orionConfirm("File manager","Cannot read directory");
                    fdir = oldDir;
                    if (!loadDirectory()) return "";
                }
                redraw(); continue;
            }
            if (!write) return pathFor(item.name);
            filename = item.name; inputFocused = true; drawInput(); drawRow(selected); continue;
        } else continue;
        if (selected < scroll) scroll = selected;
        if (selected >= scroll+visibleRows) scroll = selected-visibleRows+1;
        if (scroll != previousScroll) drawList();
        else if (selected != previous) { drawRow(previous); drawRow(selected); }
    }
}

void palGetDirContent(const string& d, list<PalFileInfo*>& fileList)
{
    DIR dir;
    FILINFO entry;
    if (f_opendir(&dir, d.c_str()) != FR_OK) {
        return;
    }
    while (f_readdir(&dir, &entry) == FR_OK && entry.fname[0] != '\0') {
        PalFileInfo* newFile = new PalFileInfo;
        newFile->fileName = entry.fname;
        newFile->isDir = (entry.fattrib & AM_DIR) != 0;
        newFile->size = entry.fsize;
        newFile->year = 1980 + (entry.fdate >> 9);
        newFile->month = (entry.fdate >> 5) & 0b111;
        newFile->day = entry.fdate & 0b11111;
        newFile->hour = entry.ftime >> 11;
        newFile->minute =  (entry.ftime >> 5) & 0b1111111;
        newFile->second = entry.ftime & 0b11111;
        fileList.push_back(newFile);
    }
    f_closedir(&dir);
}

#include "../EmuCalls.h"

void palExecute() {
    while(1) {
        emuEmulationCycle();
        sleep_ms(2);
    }
    __unreachable();
}

#include "audio.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"

enum AudioOut : uint8_t { AUDIO_OUT_PWM, AUDIO_OUT_I2S };
static AudioOut s_audioOut = AUDIO_OUT_PWM;
#define s_audioI2S (s_audioOut == AUDIO_OUT_I2S)
static bool s_audioOutputInitialized = false;

static uint32_t s_audioProbe[3] = {0, 0, 0};
uint32_t palAudioProbe(int i) {return (i >= 0 && i < 3) ? s_audioProbe[i] : 0;}

static int audioTest0000(unsigned pin0, unsigned pin1, int res)
{
    gpio_init(pin0);
    gpio_set_dir(pin0, GPIO_OUT);
    sleep_ms(33);
    gpio_put(pin0, 1);

    gpio_init(pin1);
    gpio_set_dir(pin1, GPIO_IN);
    gpio_pull_down(pin1);
    sleep_ms(33);
    if (gpio_get(pin1))
        res |= (1 << 5) | 1;

    gpio_deinit(pin0);
    gpio_deinit(pin1);
    return res;
}

static int audioTest0101(unsigned pin0, unsigned pin1, int res)
{
    gpio_init(pin0);
    gpio_set_dir(pin0, GPIO_OUT);
    sleep_ms(33);
    gpio_put(pin0, 1);

    gpio_init(pin1);
    gpio_set_dir(pin1, GPIO_IN);
    gpio_pull_down(pin1);
    sleep_ms(33);
    if (gpio_get(pin1))
        res |= (1 << 5) | 1;

    gpio_deinit(pin0);
    gpio_deinit(pin1);
    return res;
}

static int audioTest1111(unsigned pin0, unsigned pin1, int res)
{
    gpio_init(pin0);
    gpio_set_dir(pin0, GPIO_OUT);
    sleep_ms(33);
    gpio_put(pin0, 0);

    gpio_init(pin1);
    gpio_set_dir(pin1, GPIO_IN);
    gpio_pull_up(pin1);
    sleep_ms(33);
    if (!gpio_get(pin1))
        res |= 1;

    gpio_deinit(pin0);
    gpio_deinit(pin1);
    return res;
}

// Полный testPins из pico-spec. Пассивных уровней недостаточно: после них
// рабочий алгоритм дополнительно возбуждает одну линию и проверяет отклик
// второй. Именно этот активный этап отличает установленный I2S-модуль.
static int audioTestPins(unsigned pin0, unsigned pin1)
{
    gpio_init(pin0);
    gpio_set_dir(pin0, GPIO_IN);
    gpio_pull_down(pin0);
    gpio_init(pin1);
    gpio_set_dir(pin1, GPIO_IN);
    gpio_pull_down(pin1);
    sleep_ms(33);
    const int pin0vPD = gpio_get(pin0);
    const int pin1vPD = gpio_get(pin1);
    gpio_deinit(pin0);
    gpio_deinit(pin1);

    gpio_init(pin0);
    gpio_set_dir(pin0, GPIO_IN);
    gpio_pull_up(pin0);
    gpio_init(pin1);
    gpio_set_dir(pin1, GPIO_IN);
    gpio_pull_up(pin1);
    sleep_ms(33);
    const int pin0vPU = gpio_get(pin0);
    const int pin1vPU = gpio_get(pin1);
    gpio_deinit(pin0);
    gpio_deinit(pin1);

    int res = (pin0vPD << 4) | (pin0vPU << 3)
            | (pin1vPD << 2) | (pin1vPU << 1);
    s_audioProbe[0] = uint32_t(res);

    if (pin0vPD == 1) {
        if (pin0vPU == 1 && pin1vPD == 1 && pin1vPU == 1)
            res = audioTest1111(pin0, pin1, res);
        else if (pin0vPU == 0 && pin1vPD == 1 && pin1vPU == 0)
            res |= (1 << 5) | 1;
    } else if (pin0vPU == 1) {
        if (pin1vPD == 0 && pin1vPU == 1)
            res = audioTest0101(pin0, pin1, res);
    } else if (pin1vPD == 0 && pin1vPU == 0) {
        res = audioTest0000(pin0, pin1, res);
    }

    s_audioProbe[1] = uint32_t(res & 0x21);
    s_audioProbe[2] = uint32_t(res);
    return res;
}

// Возвращает true, если полный тест связи DIN/BCK обнаружил I2S-модуль.
bool palProbeAudioOutput()
{
#if defined(AUDIO_FORCE_PWM)
    s_audioOut = AUDIO_OUT_PWM;
#elif defined(AUDIO_FORCE_I2S)
    s_audioOut = AUDIO_OUT_I2S;
#else
    s_audioOut = audioTestPins(AUDIO_DATA_PIN, AUDIO_CLOCK_PIN) != 0
               ? AUDIO_OUT_I2S : AUDIO_OUT_PWM;
#endif
    return s_audioI2S;
}

// ---------------------------------------------------------------------------
//  Кольцевой буфер и вывод по таймеру.
//
//  Эмулятор считает отсчёты в ВИРТУАЛЬНОМ времени: за один проход главного
//  цикла их появляется столько, сколько виртуального времени прошло, и все они
//  выдаются подряд за единицы микросекунд. Прежняя реализация писала каждый
//  отсчёт прямо в регистр PWM, поэтому промежуточные уровни держались доли
//  микросекунды, а последний — до следующего прохода цикла.
//
//  Теперь отсчёты складываются в кольцевой буфер, а достаёт их обработчик
//  повторяющегося таймера — ровно по одному за период. Способ взят из
//  pico-spec (pwm_audio.cpp): он проверен на этом железе.
//
//  Период задаётся как 1000000 / частота дискретизации в микросекундах, то есть
//  частота обязана быть делителем миллиона. Поэтому в Emulation выставлено
//  50000 Гц: 20 мкс ровно. При 48000 период округлился бы до тех же 20 мкс,
//  что дало бы воспроизведение на 4.17% быстрее — почти три четверти полутона.
// ---------------------------------------------------------------------------
static constexpr unsigned c_audioRingSize = 1024;
static constexpr unsigned c_audioRingMask = c_audioRingSize - 1;

// Оба знаковых 16-битных канала в одном слове: младшая половина — левый,
// старшая — правый. Квантование до 12 бит выполняется только в PWM callback.
static uint32_t s_audioRing[c_audioRingSize];
static volatile unsigned s_audioWrite = 0;
static volatile unsigned s_audioRead = 0;
static uint32_t s_audioLast = 0;
static bool s_audioPaced = false;
static repeating_timer_t s_audioTimer;

// Остаток квантования, переносимый в следующий отсчёт (только для ШИМ)
static int s_audioErrL = 0;
static int s_audioErrR = 0;

extern i2s_config_t i2s_config;

static void audioWritePwm(uint32_t sample)
{
    if (s_audioOut != AUDIO_OUT_PWM)   // чужой режим — ноги не наши
        return;
    int xL = int(int16_t(sample & 0xFFFF)) + 32768 + s_audioErrL;
    if (xL < 0) xL = 0; else if (xL > 0xFFFF) xL = 0xFFFF;
    const uint16_t outL = uint16_t(unsigned(xL) >> 4);
    s_audioErrL = xL - (int(outL) << 4);

    int xR = int(int16_t(sample >> 16)) + 32768 + s_audioErrR;
    if (xR < 0) xR = 0; else if (xR > 0xFFFF) xR = 0xFFFF;
    const uint16_t outR = uint16_t(unsigned(xR) >> 4);
    s_audioErrR = xR - (int(outR) << 4);

    pwm_set_gpio_level(PWM_PIN0, outL);
    pwm_set_gpio_level(PWM_PIN1, outR);
}

static bool __not_in_flash_func(audioTimerCb)(repeating_timer_t*)
{
    if (s_audioRead != s_audioWrite) {
        s_audioLast = s_audioRing[s_audioRead];
        s_audioRead = (s_audioRead + 1) & c_audioRingMask;
    }
    #if defined(HDMI_DVI) && defined(PICO_RP2350)
    hdmi_dvi_push_audio_sample(
        int16_t(s_audioLast & 0xffffu),
        int16_t(s_audioLast >> 16));
    #endif
    if (s_audioI2S) {
        if (!pio_sm_is_tx_fifo_full(i2s_config.pio, i2s_config.sm))
            pio_sm_put(i2s_config.pio, i2s_config.sm, s_audioLast);
    } else {
        audioWritePwm(s_audioLast);
    }
    return true;
}

static void audioStopPacedOutput()
{
    if (s_audioPaced) {
        cancel_repeating_timer(&s_audioTimer);
        s_audioPaced = false;
    }
}

static bool audioInitOutput(int sampleRate)
{
    if (s_audioI2S) {
        i2s_config.sample_freq = sampleRate;
        i2s_config.dma_trans_count = 0;
        if (!i2s_init(&i2s_config))
            return false;
    } else {
        pwm_config config = pwm_get_default_config();
        pwm_config_set_clkdiv(&config, 1.0f);
        pwm_config_set_wrap(&config, (1 << 12) - 1);
        gpio_set_function(PWM_PIN0, GPIO_FUNC_PWM);
        gpio_set_function(PWM_PIN1, GPIO_FUNC_PWM);
        pwm_init(pwm_gpio_to_slice_num(PWM_PIN0), &config, true);
        pwm_init(pwm_gpio_to_slice_num(PWM_PIN1), &config, true);
    }
    s_audioOutputInitialized = true;
    return true;
}

static void audioDeinitOutput()
{
    if (!s_audioOutputInitialized)
        return;
    if (s_audioI2S) {
        i2s_deinit(&i2s_config);
    } else {
        const uint slice0 = pwm_gpio_to_slice_num(PWM_PIN0);
        const uint slice1 = pwm_gpio_to_slice_num(PWM_PIN1);
        pwm_set_enabled(slice0, false);
        if (slice1 != slice0)
            pwm_set_enabled(slice1, false);
        gpio_deinit(PWM_PIN0);
        gpio_deinit(PWM_PIN1);
    }
    s_audioOutputInitialized = false;
}

static bool audioStartPacedOutput(int sampleRate)
{
    if (sampleRate <= 0)
        return false;

    const int periodUs = 1000000 / sampleRate;
    if (periodUs <= 0)
        return false;

    s_audioRead = 0;
    s_audioWrite = 0;
    s_audioLast = 0;
    s_audioErrL = 0;
    s_audioErrR = 0;
    for (unsigned i = 0; i < c_audioRingSize; i++)
        s_audioRing[i] = 0;

    if (!add_repeating_timer_us(-periodUs, audioTimerCb, nullptr, &s_audioTimer))
        return false;

    s_audioPaced = true;
    return true;
}

void __not_in_flash_func(palPlaySample)(int16_t left, int16_t right) {
    const uint32_t sample = uint16_t(left) | (uint32_t(uint16_t(right)) << 16);
    if (!s_audioPaced) {
        if (s_audioI2S) {
            if (s_audioOutputInitialized
                && !pio_sm_is_tx_fifo_full(i2s_config.pio, i2s_config.sm))
                pio_sm_put(i2s_config.pio, i2s_config.sm, sample);
        } else if (s_audioOutputInitialized) {
            audioWritePwm(sample);
        }
        return;
    }

    const unsigned next = (s_audioWrite + 1) & c_audioRingMask;
    if (next == s_audioRead)
        return;

    s_audioRing[s_audioWrite] = sample;
    __dmb();
    s_audioWrite = next;
}

int sampleRate = 50000;

bool palSetSampleRate(int newSampleRate)
{
    // The DVI resampler and paced output share this fixed input rate.
    if (newSampleRate != 50000)
        return false;

    if (s_audioOutputInitialized && newSampleRate == sampleRate)
        return true;

    audioStopPacedOutput();
    audioDeinitOutput();
    sampleRate = newSampleRate;
    if (!audioInitOutput(sampleRate))
        return false;
    if (!audioStartPacedOutput(sampleRate)) {
        audioDeinitOutput();
        return false;
    }
    return true;
}

int palGetSampleRate()
{
    return sampleRate;
}

bool palSetVsync(bool)
{
    return true;
}

void palMsgBox(string msg, bool)
{
#if LOG
    /// TODO:
    palLog(msg + "\n");
#endif
}

void palSetRunFileName(std::string) {
}

void palAddTabToConfigWindow(int tabId, string tabName)
{
}

void palShowConfigWindow(int) { orionMenu(); }

void palUpdateConfig() {
}

bool palChoosePlatform(std::vector<PlatformInfo>&, int&, bool&, bool, PalWindow*) {
    return false;
}

bool palChooseConfiguration(std::string platformName, PalWindow* wnd) {
    return false;
}

void palSetTabOptFileName(int, string) {
}

void palRemoveTabFromConfigWindow(int) {
}

void palGetPlatformDefines(std::string platformName, std::map<std::string, std::string>& definesMap)
{

}

void palGetPalDefines(std::list<std::string>& defineList)
{
    /// TODO:
    defineList.push_back("SDL");
}

void palRequestForQuit() {  while(true); } /// TODO:

void palAddRadioSelectorToTab(int, int, std::string, std::string, std::string, SelectItem*, int) {
}
