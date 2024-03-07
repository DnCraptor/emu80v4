/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2018
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <sstream>
#include <iostream>

#include <time.h>
#include <pico/time.h>
#include <hardware/structs/vreg_and_chip_reset.h>
#include <hardware/clocks.h>
#include <hardware/pwm.h>
#include <pico/multicore.h>
#include <pico/bootrom.h>
#include <pico/stdlib.h>
#include "psram_spi.h"

extern "C" {
    #include "ps2.h"
    #include "util_Wii_Joy.h"
    #include "nespad.h"
    #include "vga.h"
    #include "graphics.h"
}

#include "picoPal.h"
#include "ff.h"

#include "../EmuCalls.h"

static volatile bool upPressed = false;
static volatile bool downPressed = false;
static volatile bool pageUpPressed = false;
static volatile bool pageDownPressed = false;
static volatile bool enterPressed = false;

using namespace std;

std::string palOpenFileDialog(std::string, std::string, bool, PalWindow*) {
    return "";
}

void palGetDirContent(const string& dir, list<PalFileInfo*>& fileList) {
	DIR d;
	FILINFO fileInfo;
    string utf8Dir = dir;

    if (utf8Dir[utf8Dir.size() - 1] != '/' && utf8Dir[utf8Dir.size() - 1] != '\\')
        utf8Dir += "\\";

	if (f_opendir(&d, utf8Dir.c_str()) == FR_OK) {
    	while (f_readdir(&d, &fileInfo) == FR_OK && fileInfo.fname[0] != '\0') {
            PalFileInfo* newFile = new PalFileInfo;
            newFile->fileName = fileInfo.fname;
            if (newFile->fileName == "." || newFile->fileName == "..")
                continue;
            string fullPath = utf8Dir + newFile->fileName;
            newFile->isDir = fileInfo.fattrib | AM_DIR;
            newFile->size = (uint32_t)fileInfo.fsize;
            struct tm *fileDateTime;
        //    fileDateTime = gmtime(&(fileInfo.ftime));
/// TODO:
            newFile->year = 2030; // fileDateTime->tm_year + 1900;
            newFile->month = 1; // fileDateTime->tm_mon + 1;
            newFile->day = 1; // fileDateTime->tm_mday;
            newFile->hour = 1; // fileDateTime->tm_hour;
            newFile->minute = 1; // fileDateTime->tm_min;
            newFile->second = 1; ///fileDateTime->tm_sec;
            fileList.push_back(newFile);
        }
    }
}
/***
const static unsigned char cp12512koi8r[0x80] = { // not in use
   0xB1, 0xB2, 0x82, 0xA2, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F,
   0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F,
   0xA0, 0xA1, 0x83, 0xB8, 0xA4, 0xA5, 0xA6, 0xA7, 0xB3, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
   0xB0, 0x80, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xA3, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF,
   0xE1, 0xE2, 0xF7, 0xE7, 0xE4, 0xE5, 0xF6, 0xFA, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF0,
   0xF2, 0xF3, 0xF4, 0xF5, 0xE6, 0xE8, 0xE3, 0xFE, 0xFB, 0xFD, 0xFF, 0xF9, 0xF8, 0xFC, 0xE0, 0xF1,
   0xC1, 0xC2, 0xD7, 0xC7, 0xC4, 0xC5, 0xD6, 0xDA, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF, 0xD0,
   0xD2, 0xD3, 0xD4, 0xD5, 0xC6, 0xC8, 0xC3, 0xDE, 0xDB, 0xDD, 0xDF, 0xD9, 0xD8, 0xDC, 0xC0, 0xD1
};
*/
inline static unsigned char to_cp866(wchar_t c) {
    if (c < 0x0080) return (u_char)c & 127;
    c = c - 0x0410 /* A == 0 */ + 0x0080 /* A == 0x80*/;
    if (c < 0x00B0) return (u_char)c & 0xFF;
    c += 0x0030;
    return (u_char)c & 0xFF;
}

#include <locale>
#include <codecvt>

bool palChoosePlatform(std::vector<PlatformInfo>& pi, int& pos, bool&, bool, PalWindow*) {
    lprintf("palChoosePlatform");
    clrScr(1);
    draw_window("Select platform", 0, 0, TEXTMODE_COLS, TEXTMODE_ROWS);
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> convert;
    int selected = 0;
    int shift = 0;
    while(1) {
        int selectedYline = 1;
        for (int i = 0; i < TEXTMODE_ROWS - 2; ++i) { 
            int idx = i + shift;
            if (idx >= pi.size()) {
                draw_label(1, i + 1, TEXTMODE_COLS - 2, " ", false, false);
                continue;
            }
            std::wstring wstr = convert.from_bytes(pi[idx].platformName);
            const wchar_t *utf16 = wstr.c_str();
            string str;
            while(*utf16) {
                str += to_cp866(*utf16++);
            }
            bool bSelected = idx == selected;
            draw_label(1, i + 1, TEXTMODE_COLS - 2, str.c_str(), bSelected, false);
            if (bSelected) {
                selectedYline = i + 1;
            }
        }
        if (enterPressed) {
            pos = selected;
            return true;
        }
        sleep_ms(100);
        if (upPressed) {
            if (selectedYline > 1) { if(selected > 0) selected--; }
            else if (shift > 0) shift--;
        }
        if (downPressed) {
            if (selectedYline < TEXTMODE_ROWS - 2) { if(selected < pi.size() - 1) selected++; }
            else shift++;
        }
    }
    return false;
}


bool palChooseConfiguration(std::string platformName, PalWindow* wnd) {
    lprintf("!palChooseConfiguration");
    return false;
}


void palGetPalDefines(std::list<std::string>& defineList)
{
    lprintf("palGetPalDefines PICO");
    defineList.push_back("PICO");
}


void palGetPlatformDefines(std::string platformName, std::map<std::string, std::string>& definesMap)
{
    lprintf("!palGetPlatformDefines [%s]", platformName.c_str());
}


void palSetRunFileName(std::string fileName) {
    lprintf("!palSetRunFileName [%s]", fileName.c_str());
}

void palShowConfigWindow(int d) {
    lprintf("!palShowConfigWindow [%d]", d);
}

void palUpdateConfig() {
    lprintf("!palUpdateConfig");
    //configWnd->updateConfig();
}

void palAddTabToConfigWindow(int d, std::string s) {
    lprintf("!palAddTabToConfigWindow %d [%s]", d, s.c_str());
}

void palRemoveTabFromConfigWindow(int d) {
    lprintf("!palRemoveTabFromConfigWindow [%d]", d);
}

void palAddRadioSelectorToTab(int d, int i, std::string s1, std::string s2, std::string s3, SelectItem*, int) {
    lprintf("!palAddRadioSelectorToTab [%d][%s][%s][%s]", d, s1.c_str(), s2.c_str(), s3.c_str());
}

void palSetTabOptFileName(int d, string s) {
    lprintf("!palSetTabOptFileName [%d][%s]", d, s.c_str());
}

void palWxProcessMessages() {
    lprintf("!palWxProcessMessages");
}

void palLog(std::string s) {
#ifndef PICO_PAL
    cout << s << endl;
#else
    lprintf(s.c_str());
#endif
}

EmuLog& EmuLog::operator<<(string s)
{
    palLog(s);
    return *this;
}


EmuLog& EmuLog::operator<<(const char* sz)
{
    string s = sz;
    palLog(s);
    return *this;
}


EmuLog& EmuLog::operator<<(int n)
{
    ostringstream oss;
    oss << n;
    string s = oss.str();
    palLog(s);
    return *this;
}


void palMsgBox(string msg, bool)
{
    lprintf("palMsgBox(%s)", msg.c_str());
    cout << msg << endl;
}


EmuLog emuLog;

bool PalFile::open(string fileName, string mode) {
    lprintf("bool PalFile::open(string fileName: [%s], string mode: [%s])", fileName.c_str(), mode.c_str());
    if (isOpen()) close();
    int m = mode == "r" ? FA_READ : (FA_READ | FA_WRITE | FA_CREATE_ALWAYS);
    mp_file = malloc(sizeof(FIL));
    bool ok = FR_OK == f_open((FIL*)mp_file, fileName.c_str(), m);
    if (!ok) { free(mp_file); mp_file = 0; }
    return ok;
}

void PalFile::close() {
    if (mp_file == nullptr) return;
    f_close((FIL*)mp_file);
    free(mp_file);
    mp_file = nullptr;
}

bool PalFile::isOpen() {
    return mp_file != nullptr && ((FIL*)mp_file)->obj.fs;
}

bool PalFile::eof() {
    if (mp_file == nullptr) return true;
    return f_eof((FIL*)mp_file);
}

uint8_t PalFile::read8() {
    if (mp_file == nullptr) return 0;
    UINT br;
    uint8_t res;
    if (FR_OK == f_read((FIL*)mp_file, &res, 1, &br))
        return res;
    return 0;
}

uint16_t PalFile::read16() {
    if (mp_file == nullptr) return 0;
    UINT br;
    uint16_t res;
    if (FR_OK == f_read((FIL*)mp_file, &res, 2, &br))
        return res;
    return 0;
}

uint32_t PalFile::read32() {
    if (mp_file == nullptr) return 0;
    UINT br;
    uint32_t res;
    if (FR_OK == f_read((FIL*)mp_file, &res, 4, &br))
        return res;
    return 0;
}

void PalFile::write8(uint8_t c) {
    if (mp_file == nullptr) return;
    UINT bw;
    f_write((FIL*)mp_file, &c, 1, &bw);
}

void PalFile::write16(uint16_t c) {
    if (mp_file == nullptr) return;
    UINT bw;
    f_write((FIL*)mp_file, &c, 2, &bw);
}

void PalFile::write32(uint32_t c) {
    if (mp_file == nullptr) return;
    UINT bw;
    f_write((FIL*)mp_file, &c, 4, &bw);
}

int64_t PalFile::getSize() {
    if (mp_file == nullptr) return 0;
    return f_size((FIL*)mp_file);
}

int64_t PalFile::getPos() {
    lprintf("PalFile::getPos() mp_file: %08Xh", mp_file);
    if (mp_file == nullptr) return 0;
    int64_t res = f_tell((FIL*)mp_file); // TODO: ensure
    lprintf("PalFile::getPos() res: %d", res);
    return res;
}

void PalFile::seek(int position) {
    if (mp_file == nullptr) return;
    f_lseek((FIL*)mp_file, position);
}

void PalFile::skip(int len) {
    seek(getPos() + len);
}

static const string basePath = "\\emu80\\";

string palMakeFullFileName(string fileName) {
    if (fileName[0] == '\0' || fileName[0] == '/' || fileName[0] == '\\' || (fileName.size() > 1 && fileName[1] == ':'))
        return fileName;
    string fullFileName(::basePath);
    fullFileName += fileName;
    return fullFileName;
}

uint8_t* palReadFile(const std::string& fileName, int &fileSize, bool useBasePath) {
    lprintf("palReadFile(%s, .. %d)", fileName.c_str(), useBasePath);
    string fullFileName;
    if (useBasePath)
        fullFileName = palMakeFullFileName(fileName);
    else
        fullFileName = fileName;
    FIL file;
    if (f_open(&file, fullFileName.c_str(), FA_READ) == FR_OK) {
        fileSize = f_size(&file);
        if (fileSize < 0) {
            fileSize = 0;
            lprintf("palReadFile(%s, .. %d) returns null", fileName.c_str(), useBasePath);
            return nullptr;
        }
        uint8_t* buf = new uint8_t[fileSize];
        UINT nBytesRead;
        f_read(&file, buf, fileSize, &nBytesRead);
        f_close(&file);
        fileSize = nBytesRead;
        lprintf("palReadFile(%s, .. %d) returns %08Xh (fileSize: %d)", fileName.c_str(), useBasePath, buf, fileSize);
        return buf;
    }
    else {
        lprintf("palReadFile(%s, .. %d) returns null", fileName.c_str(), useBasePath);
        return nullptr;
    }
}

int palReadFromFile(const string& fileName, int offset, int sizeToRead, uint8_t* buffer, bool useBasePath) {
    string fullFileName;
    if (useBasePath)
        fullFileName = palMakeFullFileName(fileName);
    else
        fullFileName = fileName;
    UINT nBytesRead;
    FIL file;
    if (f_open(&file, fullFileName.c_str(), FA_READ) == FR_OK) {
        f_lseek(&file, offset);
        f_read(&file, buffer, sizeToRead, &nBytesRead);
        f_close(&file);
        return nBytesRead;
    }
    else
        return 0;
}

void palPlaySample(int16_t sample) {
    // TODO:
}

static bool palProcessEvents();

void palExecute() {
    lprintf("void palExecute()");
    while (!palProcessEvents()) {
        emuEmulationCycle();
    }
}

#include <pico/time.h>

uint64_t palGetCounter() {
    return to_us_since_boot(get_absolute_time());
}

uint64_t palGetCounterFreq() {
    return 1000000ul;
}

void palDelay(uint64_t time) {
    sleep_ms((uint32_t)time);
}

static int sampleRate = 48000;
static bool isRunning = false;

bool palSetSampleRate(int sampleRate) {
    if (isRunning)
        return false;
    ::sampleRate = sampleRate;
    return true;
}

int palGetSampleRate() {
    return ::sampleRate;
}

void palRequestForQuit() { /// TODO: what should we do?
    lprintf("palRequestForQuit()");
    ///SDL_Event ev;
    ///ev.type = SDL_QUIT;
    ///SDL_PushEvent(&ev);
}

string palGetDefaultPlatform() {
    return ""; // May be Радио-86РК
}

static unsigned unicodeKey = 0;

static bool palProcessEvents() {
   // lprintf("void palProcessEvents()");
    /*** TODO:
    palIdle();
    // workaround for wxWidgets events processing
    for (int i = 0; i < 10; i++)
        SDL_PumpEvents();

    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
            case SDL_QUIT:
                return true;

            case SDL_KEYDOWN:
            case SDL_KEYUP:
                {
                    if (!SDL_GetWindowFromID(event.key.windowID))
                        break; // могут остаться события, относящиеся к уже уделенному окну
                    PalKeyCode key = TranslateScanCode(event.key.keysym.scancode);
                    SysReq sr = TranslateKeyToSysReq(key, event.type == SDL_KEYDOWN, SDL_GetModState() & (KMOD_ALT | KMOD_GUI), SDL_GetModState() & KMOD_SHIFT);
                    if (sr)
                        emuSysReq(PalWindow::windowById(event.key.windowID), sr);
                    else {
                        if (unicodeKey && event.type == SDL_KEYUP) {
                            emuKeyboard(PalWindow::windowById(event.text.windowID), PK_NONE, false, unicodeKey);
                            unicodeKey = 0;
                        }
                        emuKeyboard(PalWindow::windowById(event.key.windowID), key, event.type == SDL_KEYDOWN);
                    }
                    break;
                }
            case SDL_MOUSEBUTTONDOWN:
                    if (!SDL_GetWindowFromID(event.button.windowID))
                        break; // могут остаться события, относящиеся к уже уделенному окну
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        PalWindow::windowById(event.button.windowID)->mouseClick(event.button.x, event.button.y,
                                                                                 event.button.clicks < 2 ? PM_LEFT_CLICK : PM_LEFT_DBLCLICK);
                        PalWindow::windowById(event.button.windowID)->mouseDrag(event.button.x, event.button.y);
                    }
                    break;
            case SDL_MOUSEMOTION:
                    if (!SDL_GetWindowFromID(event.button.windowID))
                        break; // могут остаться события, относящиеся к уже уделенному окну
                    if (event.motion.state & SDL_BUTTON_LMASK)
                        PalWindow::windowById(event.button.windowID)->mouseDrag(event.motion.x, event.motion.y);
                    break;
            case SDL_MOUSEWHEEL:
                    if (!SDL_GetWindowFromID(event.wheel.windowID))
                        break; // могут остаться события, относящиеся к уже уделенному окну
                    PalWindow::windowById(event.button.windowID)->mouseClick(0, 0, event.wheel.y > 0 ? PM_WHEEL_UP : PM_WHEEL_DOWN);
                    break;
            case SDL_TEXTINPUT:
                {
                    if (unicodeKey)
                        break;
                    unicodeKey = simpleUtfDecode(event.text.text);
                    emuKeyboard(PalWindow::windowById(event.text.windowID), PK_NONE, true, unicodeKey);
                    break;

                }
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED && SDL_GetWindowFromID(event.window.windowID))
                    emuFocusWnd(PalWindow::windowById(event.window.windowID));
                else if (event.window.event == SDL_WINDOWEVENT_CLOSE  && SDL_GetWindowFromID(event.window.windowID))
                    emuSysReq(PalWindow::windowById(event.window.windowID), SR_CLOSE);
                break;
            case SDL_DROPFILE:
                if (SDL_GetWindowFromID(event.drop.windowID))
                    emuDropFile(PalWindow::windowById(event.drop.windowID), event.drop.file);
                break;
        }
    }
    */
    return false;
}

bool palSetFrameRate(int) {
    // nothing to do in pico version
    return true;
}

bool palSetVsync(bool) {
    // nothing to do in pico version
    return true;
}

PalWindow::PalWindow() {
    m_params.style = /* m_prevParams.style = */PWS_FIXED;
    m_params.smoothing = /*m_prevParams.smoothing = */ST_SHARP;
    m_params.width = /*m_prevParams.width = */800;
    m_params.height = /*m_prevParams.height = */600;
    m_params.visible = /*m_prevParams.visible = */false;
    m_params.title = /*m_prevParams.title = */"";
    //m_lastX = SDL_WINDOWPOS_UNDEFINED;
    //m_lastY = SDL_WINDOWPOS_UNDEFINED;
}

PalWindow::~PalWindow() {
   // if (m_renderer)
   //     SDL_DestroyRenderer(m_renderer);
   // if (m_window) {
   //     PalWindow::m_windowsMap.erase(SDL_GetWindowID(m_window));
   //     SDL_DestroyWindow(m_window);
   // }
}

void PalWindow::getSize(int& width, int& height) {
    width = m_params.width;
    height = m_params.height;
}

void PalWindow::bringToFront() {
    ///SDL_RaiseWindow(m_window);
}

void PalWindow::maximize() {
   /// if (m_params.style == PWS_RESIZABLE)
   ///     SDL_MaximizeWindow(m_window);
}

void PalWindow::applyParams() {

}

void PalWindow::drawFill(uint32_t color) {
    /*** TODO: ??
    if (m_glAvailable) {
        drawFillGl(color);
        return;
    }
    uint8_t red = (color & 0xFF0000) >> 16;
    uint8_t green = (color & 0xFF00) >> 8;
    uint8_t blue = color & 0xFF;
    SDL_SetRenderDrawColor(m_renderer, red, green, blue, 0xff);
    SDL_RenderClear(m_renderer);
    */
}

void PalWindow::drawImage(uint32_t* pixels, int imageWidth, int imageHeight, double aspectRatio, bool blend, bool useAlpha) {
/// TODO:
}

void PalWindow::drawEnd() {

}

static FATFS fs;
semaphore vga_start_semaphore;

pwm_config config = pwm_get_default_config();
void PWM_init_pin(uint8_t pinN, uint16_t max_lvl) {
    gpio_set_function(pinN, GPIO_FUNC_PWM);
    pwm_config_set_clkdiv(&config, 1.0);
    pwm_config_set_wrap(&config, max_lvl); // MAX PWM value
    pwm_init(pwm_gpio_to_slice_num(pinN), &config, true);
}

void inInit(uint gpio) {
    gpio_init(gpio);
    gpio_set_dir(gpio, GPIO_IN);
    gpio_pull_up(gpio);
}

extern "C" bool __time_critical_func(handleScancode)(const uint32_t ps2scancode) {
    switch((uint8_t)ps2scancode & 0xFF) {
        case 0x48:
            upPressed = true;
            break;
        case 0xC8:
            upPressed = false;
            break;
        case 0x50:
            downPressed = true;
            break;
        case 0xD0:
            downPressed = false;
            break;
        case 0x49:
            pageUpPressed = true;
            break;
        case 0xC9:
            pageUpPressed = false;
            break;
        case 0x51:
            pageDownPressed = true;
            break;
        case 0xD1:
            pageDownPressed = false;
            break;
        case 0x1C:
            enterPressed = true;
            break;
        case 0x9C:
            enterPressed = false;
            break;
    }
    return true;
}

void nespad_update() {
    // TODO:
}

static uint16_t SCREEN[TEXTMODE_COLS][TEXTMODE_ROWS] = { 0 };

void __time_critical_func(render_core)() {
    multicore_lockout_victim_init();
    graphics_init();

    const auto buffer = (uint8_t *)SCREEN;
    graphics_set_buffer(buffer, TEXTMODE_COLS, TEXTMODE_ROWS);
    graphics_set_textbuffer(buffer);
    graphics_set_bgcolor(0x000000);
    graphics_set_offset(0, 0);
    graphics_set_mode(TEXTMODE_DEFAULT);
    clrScr(1);

    sem_acquire_blocking(&vga_start_semaphore);
    // 60 FPS loop
#define frame_tick (16666)
    uint64_t tick = time_us_64();
#ifdef TFT
    uint64_t last_renderer_tick = tick;
#endif
    uint64_t last_input_tick = tick;
    while (true) {
#ifdef TFT
        if (tick >= last_renderer_tick + frame_tick) {
            refresh_lcd();
            last_renderer_tick = tick;
        }
#endif
        // Every 5th frame
        if (tick >= last_input_tick + frame_tick * 5) {
            nespad_read();
            last_input_tick = tick;
            nespad_update();
        }
        tick = time_us_64();
        tight_loop_contents();
    }
    __unreachable();
}

#ifdef SOUND
static repeating_timer_t timer;
static int snd_channels = 2;
static bool __not_in_flash_func(snd_timer_callback)(repeating_timer_t *rt) {
    static uint16_t outL = 0;  
    static uint16_t outR = 0;

    pwm_set_gpio_level(PWM_PIN0, outR); // Право
    pwm_set_gpio_level(PWM_PIN1, outL); // Лево
    outL = outR = 0;

    pwm_set_gpio_level(BEEPER_PIN, 0);
    return true;
}
#endif

#include "f_util.h"
static FATFS fatfs;
bool SD_CARD_AVAILABLE = false;
static void init_fs() {
    FRESULT result = f_mount(&fatfs, "", 1);
    if (FR_OK != result) {
        lprintf("Unable to mount SD-card: %s (%d)", FRESULT_str(result), result);
    } else {
        SD_CARD_AVAILABLE = true;
    }
}

inline static void init_wii() {
    if (Init_Wii_Joystick()) {
        Wii_decode_joy();
        lprintf("Found WII joystick");
    }
}

// Перенести в palExecute
void palStart() {
}

void palPicoInit() {
    hw_set_bits(&vreg_and_chip_reset_hw->vreg, VREG_AND_CHIP_RESET_VREG_VSEL_BITS);
    sleep_ms(10);
    set_sys_clock_khz(378 * KHZ, true);
    stdio_init_all();
    keyboard_init();
    keyboard_send(0xFF);
    nespad_begin(clock_get_hz(clk_sys) / 1000, NES_GPIO_CLK, NES_GPIO_DATA, NES_GPIO_LAT);

    nespad_read();
    sleep_ms(50);

    // F12 Boot to USB FIRMWARE UPDATE mode
    if (nespad_state & DPAD_START /*|| input_map.keycode == 0x58*/) { // F12
        reset_usb_boot(0, 0);
    }

    sem_init(&vga_start_semaphore, 0, 1);
    multicore_launch_core1(render_core);
    sem_release(&vga_start_semaphore);

    draw_text("LOADING...", 0, 0, 15, 0);

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    for (int i = 0; i < 6; i++) {
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }

    init_fs(); // TODO: psram replacement (pagefile)
    init_psram();
    
#ifdef SOUND
    PWM_init_pin(BEEPER_PIN, (1 << 8) - 1);
    PWM_init_pin(PWM_PIN0, (1 << 8) - 1);
    PWM_init_pin(PWM_PIN1, (1 << 8) - 1);
#endif
#if LOAD_WAV_PIO
    //пин ввода звука
    inInit(LOAD_WAV_PIO);
#endif

#ifdef SOUND
    int hz = 48000; // TODO:
    snd_channels = 1; // TODO:
    // negative timeout means exact delay (rather than delay between callbacks)
    if (!add_repeating_timer_us(-1000000 / hz, snd_timer_callback, NULL, &timer)) {
        lprintf("Failed to add timer");
    } else {
        lprintf("snd_timer_callback timer is registered");
    }
#endif

    isRunning = true;
}
