#include "picoPal.h"
#include "ffPalFile.h"

#include <sstream>
#include <iostream>

#include <pico/stdlib.h>
#include <hardware/pio.h>

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

string palMakeFullFileName(string fileName)
{
    if (fileName[0] == '\0' || fileName[0] == '/' || fileName[0] == '\\')
        return fileName;
    string fullFileName("/vector06c/");
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

static constexpr int MAX_FILE_DIALOG_ITEMS = 256;

static int palGetDirContent(const string& d, PalFileInfo* fileList, int maxItems)
{
    DIR dir;
    FILINFO entry;
    if (f_opendir(&dir, d.c_str()) != FR_OK)
        return 0;

    int count = 0;
    while (count < maxItems && f_readdir(&dir, &entry) == FR_OK && entry.fname[0] != '\0') {
        PalFileInfo& file = fileList[count++];
        file.fileName = entry.fname;
        file.isDir = (entry.fattrib & AM_DIR) != 0;
        file.size = entry.fsize;
        file.year = 1980 + (entry.fdate >> 9);
        file.month = (entry.fdate >> 5) & 0b111;
        file.day = entry.fdate & 0b11111;
        file.hour = entry.ftime >> 11;
        file.minute = (entry.ftime >> 5) & 0b1111111;
        file.second = entry.ftime & 0b11111;
    }
    f_closedir(&dir);
    return count;
}

/// TODO: .h
extern PalKeyCodeAction getKey();
extern PalKeyCode pressed_key[256];
#include "ps2kbd_mrmltr.h"
#include <algorithm>
static std::string fdir = "/vector06c";
std::string palOpenFileDialog(std::string title, std::string filter, bool write, PalWindow* window) {
    uint32_t sw = graphics_get_width();
    uint32_t sh = graphics_get_height();
    uint32_t w = sw - 10;
    uint32_t h = sh - 6;
    int32_t x = (sw - w) / 2;
    int32_t y = (sh - h) / 2;
    uint32_t fntw = graphics_get_font_width();
    uint32_t fnth = graphics_get_font_height();
    uint32_t msi = fnth + 1;
    uint32_t xb = x + 2;
    uint32_t yb = y + fnth + 5;
    const uint32_t scrollW = 4;
    const uint32_t listW = w - 2 - scrollW;
    const uint32_t scrollX = x + w - 1 - scrollW;
    const char back[] = "..";

    graphics_rect(x, y, w, h, RGB888(0, 0, 0));
    graphics_fill(x + 1, y + 1, w - 2, fnth + 2, 0b000101);
    graphics_rect(x, y, w, fnth + 4, RGB888(0, 0, 0));

    string t2;
    if (write) {
        t2 = "Type a name of ext: " + filter;
        graphics_fill(x + 1, yb, w - 2, fnth + 1, RGB888(207, 255, 255));
        yb += fnth + 1;
    }

    DIR f_dir;
    if (f_opendir(&f_dir, fdir.c_str()) != FR_OK)
        fdir = "/";
    else
        f_closedir(&f_dir);
    if (f_opendir(&f_dir, fdir.c_str()) != FR_OK)
        return "";
    f_closedir(&f_dir);

    static PalFileInfo fileList[MAX_FILE_DIALOG_ITEMS];
    int fileCount = 0;
    int selected_file_n = 0;
    int shift_j = 0;
    int visibleRows = (int)((y + h - fnth - yb) / msi) + 1;
    if (visibleRows < 1) visibleRows = 1;
    PalFileInfo* selected_fi = nullptr;

    auto loadDirectory = [&]() {
        fileCount = 0;
        if (fdir.length() > 1) {
            fileList[fileCount].fileName = back;
            fileList[fileCount].isDir = true;
            fileList[fileCount].size = 0;
            fileCount++;
        }
        fileCount += palGetDirContent(fdir, fileList + fileCount, MAX_FILE_DIALOG_ITEMS - fileCount);
        sort(fileList, fileList + fileCount, [](const PalFileInfo& a, const PalFileInfo& b) {
            if (a.isDir == b.isDir) return a.fileName < b.fileName;
            return a.isDir > b.isDir;
        });
    };

    auto drawTitle = [&]() {
        graphics_fill(x + 1, y + 1, w - 2, fnth + 2, 0b000101);
        string t = title + ": " + fdir;
        uint32_t xt = x + 1;
        if (t.length() * fntw < w - 2)
            xt = x + 1 + (w - 2 - t.length() * fntw) / 2;
        graphics_type(xt, y + 3, 0b101010, t.c_str(), t.length());
    };

    auto drawInput = [&]() {
        if (!write) return;
        graphics_fill(x + 1, y + fnth + 5, w - 2, fnth + 1, RGB888(207, 255, 255));
        graphics_type(x + 2, y + fnth + 6, RGB888(0, 0, 0), t2.c_str(), t2.length());
    };

    auto drawRow = [&](int itemIndex) {
        if (itemIndex < shift_j || itemIndex >= shift_j + visibleRows) return;
        int row = itemIndex - shift_j;
        uint32_t rowY = yb + row * msi;
        bool selected = itemIndex == selected_file_n;
        uint32_t bg = selected ? RGB888(114, 114, 224) : RGB888(255, 255, 255);
        uint32_t fg = selected ? RGB888(255, 255, 255) : RGB888(0, 0, 0);
        graphics_fill(x + 1, rowY, listW, fnth, bg);
        if (itemIndex >= 0 && itemIndex < fileCount) {
            const PalFileInfo& fi = fileList[itemIndex];
            string name = fi.isDir ? "<" + fi.fileName + ">" : fi.fileName;
            size_t maxChars = listW > 2 ? (listW - 2) / fntw : 0;
            if (name.length() > maxChars) name.resize(maxChars);
            graphics_type(xb, rowY, fg, name.c_str(), name.length());
        }
    };

    auto drawScrollbar = [&]() {
        uint32_t trackY = yb;
        uint32_t trackH = y + h - 1 - trackY;
        graphics_fill(scrollX, trackY, scrollW, trackH, RGB888(224, 224, 224));
        int total = fileCount;
        if (total <= visibleRows || trackH == 0) return;
        uint32_t thumbH = (uint32_t)((uint64_t)trackH * visibleRows / total);
        if (thumbH < 4) thumbH = 4;
        if (thumbH > trackH) thumbH = trackH;
        int maxShift = total - visibleRows;
        uint32_t thumbY = trackY + (uint32_t)((uint64_t)(trackH - thumbH) * shift_j / maxShift);
        graphics_fill(scrollX, thumbY, scrollW, thumbH, RGB888(96, 96, 96));
    };

    auto drawWindow = [&]() {
        for (int row = 0; row < visibleRows; ++row)
            drawRow(shift_j + row);
        drawScrollbar();
    };

    loadDirectory();
    drawTitle();
    drawInput();
    drawWindow();

    string res;
    while (1) {
        sleep_ms(100);
        PalKeyCodeAction pk = getKey();

        if (write && pk.pressed) {
            bool changed = true;
            if (pk.vk >= PK_1 && pk.vk <= PK_0) t2 += '1' + pk.vk - PK_1;
            else if (pk.vk >= PK_A && pk.vk <= PK_Z) t2 += 'a' + pk.vk - PK_A;
            else if (pk.vk == PK_SPACE) t2 += ' ';
            else if (pk.vk == PK_PERIOD) t2 += '.';
            else if (pk.vk == PK_BSP) t2.clear();
            else changed = false;
            if (changed) drawInput();
        }

        int oldSelected = selected_file_n;
        int oldShift = shift_j;
        int count = fileCount;
        if (count == 0) {
            if (pk.vk == PK_ESC && pk.pressed) break;
            continue;
        }

        if (pressed_key[HID_KEY_ARROW_UP] || pressed_key[HID_KEY_KEYPAD_8]) {
            selected_file_n = selected_file_n > 0 ? selected_file_n - 1 : count - 1;
        } else if (pressed_key[HID_KEY_PAGE_UP] || pressed_key[HID_KEY_KEYPAD_9]) {
            selected_file_n = std::max(0, selected_file_n - visibleRows);
        } else if (pressed_key[HID_KEY_ARROW_DOWN] || pressed_key[HID_KEY_KEYPAD_2]) {
            selected_file_n = selected_file_n + 1 < count ? selected_file_n + 1 : 0;
        } else if (pressed_key[HID_KEY_PAGE_DOWN] || pressed_key[HID_KEY_KEYPAD_3]) {
            selected_file_n = std::min(count - 1, selected_file_n + visibleRows);
        } else if ((pk.vk == PK_HOME || pk.vk == PK_KP_7) && pk.pressed) {
            selected_file_n = 0;
        } else if (pressed_key[HID_KEY_END] || pressed_key[HID_KEY_KEYPAD_1]) {
            selected_file_n = count - 1;
        } else if ((pk.vk == PK_ENTER || pk.vk == PK_KP_ENTER) && pk.pressed) {
            selected_fi = &fileList[selected_file_n];
            if (write) {
                res = fdir + "/" + t2;
                break;
            }
            if (selected_fi->isDir) {
                if (selected_fi->fileName == back) {
                    fdir = fdir.substr(0, fdir.find_last_of('/'));
                    if (fdir.empty()) fdir = "/";
                } else {
                    if (fdir.length() > 1) fdir += "/";
                    fdir += selected_fi->fileName;
                }
                selected_file_n = 0;
                shift_j = 0;
                loadDirectory();
                drawTitle();
                drawWindow();
                continue;
            }
            res = fdir + "/" + selected_fi->fileName;
            break;
        } else if (pk.vk == PK_ESC && pk.pressed) {
            break;
        } else {
            continue;
        }

        if (selected_file_n < shift_j) shift_j = selected_file_n;
        if (selected_file_n >= shift_j + visibleRows) shift_j = selected_file_n - visibleRows + 1;
        int maxShift = std::max(0, count - visibleRows);
        if (shift_j > maxShift) shift_j = maxShift;

        if (shift_j != oldShift) {
            drawWindow();
        } else if (selected_file_n != oldSelected) {
            drawRow(oldSelected);
            drawRow(selected_file_n);
        }
    }

    return res;
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

void palUpdateConfig() {
}

void palRequestForQuit() {  while(true); } /// TODO:
