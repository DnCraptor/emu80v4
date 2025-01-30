#include "picoPal.h"
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
    f_open(&pl, "/emu80.log", FA_WRITE | FA_OPEN_APPEND);
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

uint64_t palGetCounterFreq()
{
    return 1000000;
}

uint64_t palGetCounter()
{
    return time_us_64();
}

string palMakeFullFileName(string fileName)
{
    if (fileName[0] == '\0' || fileName[0] == '/' || fileName[0] == '\\')
        return fileName;
    string fullFileName("/emu80/");
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
extern PalKeyCode pressed_key[256];
#include "ps2kbd_mrmltr.h"
#include <algorithm>
static std::string fdir = "/emu80";
std::string palOpenFileDialog(std::string title, std::string filter, bool write, PalWindow* window) {
    uint32_t sw = graphics_get_width();
    uint32_t sh = graphics_get_height();
    uint32_t w = sw - 10;
    uint32_t h = sh - 6;
    int32_t x = (sw - w) / 2;
    int32_t y = (sh - h) / 2;
    uint32_t fntw = graphics_get_font_width();
    uint32_t fnth = graphics_get_font_height();
    graphics_rect(x, y, w, h, RGB888(0, 0, 0)); // outer rect

    DIR f_dir;
    if (f_opendir(&f_dir, fdir.c_str()) != FR_OK) {
        fdir = "/";
    } else {
        f_closedir(&f_dir);
    }
    if (f_opendir(&f_dir, fdir.c_str()) != FR_OK) {
        return "";
    }

    vector<PalFileInfo*> fileList;
    int selected_file_n = 0;
    int shift_j = 0;
    uint32_t height_in_j = 0;
    PalFileInfo* selected_fi = nullptr;
    uint32_t yt = y+2+1;
    const char back[] = "..";
    string t2;
    if (write) t2 = "Type a name of ext: " + filter;
again:
    graphics_fill(x+1, y+1, w-2, fnth+2, 0b000101); // Title background
    graphics_rect(x, y, w, fnth+4, RGB888(0, 0, 0));
    string t = title + ": " + fdir; // include dir into title
    uint32_t xt = (w-2 - t.length() * fntw) / 2; // center title
    graphics_type(xt, yt, 0b101010, t.c_str(), t.length()); // print title
    for (auto i = fileList.begin(); i != fileList.end(); ++i) { // cleanup files list
        delete *i;
    }
    fileList.clear();
    // add virtual dir for back action
    if (fdir.length() > 1) {
        PalFileInfo* newFile = new PalFileInfo;
        newFile->fileName = back;
        newFile->isDir = 1;
        fileList.push_back(newFile);
    }
    palGetDirContent(fdir, fileList);
    sort(fileList.begin(), fileList.end(), [](const PPalFileInfo& a, const PPalFileInfo& b) {
        if ((a->isDir && b->isDir) || (!a->isDir && !b->isDir)) return a->fileName < b->fileName;
        return b->isDir < a->isDir;
    });
    int j = 0;
    uint32_t xb = x + 2;
    uint32_t yb = y + fnth + 5;
    if (write) {
        graphics_fill(x+1, yb+1, w-2, yb+fnth+1, RGB888(207, 255, 255)); // Title2 background
        xt = x+2;
        graphics_type(xt, yb+1, RGB888(0, 0, 0), t2.c_str(), t2.length()); // print title2
        yb += fnth+1;
    }
    graphics_fill(x + 1, yb, w-2, h-2-fnth-2-2, RGB888(0xFF, 0xFF, 0xFF)); // cleanup (prpepare background) in the rect
    uint32_t msi = fnth + 1; // height of one file line
    height_in_j = 0;
    for (auto i = fileList.begin(); i != fileList.end(); ++i, ++j) {
        if (j < shift_j) continue;
        PalFileInfo* fi = *i;
        uint32_t ybj = yb + (j - shift_j) * msi;
        if (ybj > y + h - fnth) break;
        height_in_j++;
        string name = fi->isDir ? "<" + fi->fileName + ">" : fi->fileName;
        if (selected_file_n == j) {
            selected_fi = fi;
            graphics_fill(xb-1, ybj, w-2, fnth, RGB888(114, 114, 224));
            graphics_type(xb, ybj, RGB888(0xFF, 0xFF, 0xFF), name.c_str(), name.length());
        } else {
            graphics_type(xb, ybj, RGB888(0, 0, 0), name.c_str(), name.length());
        }
    }
    string res = "";
    while(1) {
        sleep_ms(100);
        PalKeyCodeAction pk = getKey();
        if (write && pk.pressed) {
            if (pk.vk >= PK_1 && pk.vk <= PK_0) { t2 += '1' + pk.vk - PK_1; goto again; }
            if (pk.vk >= PK_A && pk.vk <= PK_Z) { t2 += 'a' + pk.vk - PK_A; goto again; } // TODO: shift, caps lock
            if (pk.vk == PK_SPACE) { t2 += ' '; goto again; }
            if (pk.vk == PK_PERIOD) { t2 += '.'; goto again; }
            if (pk.vk == PK_BSP) { t2 = ""; goto again; } /// TODO:
        }
        if (pressed_key[HID_KEY_ARROW_UP] || pressed_key[HID_KEY_KEYPAD_8]) {
            selected_file_n--;
            if (selected_file_n < 0) {
                selected_file_n = fileList.size() - 1;
                while (selected_file_n >= shift_j + height_in_j) {
                    shift_j += 10;
                }
            }
            while (selected_file_n < shift_j) {
                shift_j--;
            }
            if (shift_j < 0) shift_j = fileList.size() - 1;
            goto again;
        }
        if (pressed_key[HID_KEY_PAGE_UP] || pressed_key[HID_KEY_KEYPAD_9]) {
            selected_file_n -= 10;
            if (selected_file_n < 0) {
                selected_file_n = 0;
            }
            while (selected_file_n < shift_j) {
                shift_j -= 10;
            }
            if (shift_j < 0) shift_j = 0;
            goto again;
        }
        if (pressed_key[HID_KEY_ARROW_DOWN] || pressed_key[HID_KEY_KEYPAD_2]) {
            selected_file_n++;
            if (selected_file_n >= fileList.size()) {
                selected_file_n = 0;
                shift_j = 0;
            }
            while (selected_file_n >= shift_j + height_in_j) {
                shift_j++;
            }
            goto again;
        }
            if (pressed_key[HID_KEY_PAGE_DOWN] || pressed_key[HID_KEY_KEYPAD_3]) {
            selected_file_n += 10;
            if (selected_file_n >= fileList.size()) {
                selected_file_n = fileList.size() - 1;
            }
            while (selected_file_n >= shift_j + height_in_j) {
                shift_j += 10;
            }
            goto again;
        }
        if ((pk.vk == PK_HOME || pk.vk == PK_KP_7) && pk.pressed) {
            selected_file_n = 0;
            shift_j = 0;
            goto again;
        }
        if (pressed_key[HID_KEY_END] || pressed_key[HID_KEY_KEYPAD_1]) {
            selected_file_n = fileList.size() - 1;
            while (selected_file_n >= shift_j + height_in_j) {
                shift_j += 10;
            }
            goto again;
        }
        if ((pk.vk == PK_ENTER || pk.vk == PK_KP_ENTER) && pk.pressed && selected_fi) {
            if (write) { // TODO: 
                res = fdir + "/" + t2;
                break;
            }
            selected_file_n = 0;
            shift_j = 0;
            if (selected_fi->isDir) {
                if (selected_fi->fileName == back) {
                    fdir = fdir.substr(0, fdir.find_last_of("/"));
                    if (fdir.empty()) fdir = "/";
                } else {
                    if (fdir.length() == 1) {
                        fdir = fdir + "/" + selected_fi->fileName;
                    } else {
                        fdir = fdir + "/" + selected_fi->fileName;
                    }
                }
                goto again;
            }
            res = fdir + "/" + selected_fi->fileName;
            break;
        }
        if (pk.vk == PK_ESC && pk.pressed) {
            res = "";
            break;
        };
    }
    for (auto i = fileList.begin(); i != fileList.end(); ++i) {
        delete *i;
    }
    return res;
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

#ifdef I2S_SOUND
extern i2s_config_t i2s_config;
#endif
#ifdef AUDIO_PWM_PIN
#include "hardware/pwm.h"
#endif

void palPlaySample(int16_t left, int16_t right) {
#ifdef I2S_SOUND
    uint16_t s32[2] = { 0 };
    s32[0] = left; s32[0] = right;
    i2s_dma_write(&i2s_config, s32);
#else
    pwm_set_gpio_level(PWM_PIN0, left >> 7); // Лево
    pwm_set_gpio_level(PWM_PIN1, right >> 7); // Право
#endif
}

bool isRunning = true;
int sampleRate = 48000;

bool palSetSampleRate(int sampleRate)
{
    if (isRunning)
        return false;
    ::sampleRate = sampleRate;
    return true;
}

int palGetSampleRate()
{
    return ::sampleRate;
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
