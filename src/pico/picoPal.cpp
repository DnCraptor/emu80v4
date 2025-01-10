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

    emuLog << "fullFileName: '" << fullFileName << "'\n";
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

std::string palOpenFileDialog(std::string title, std::string filter, bool write, PalWindow* window) {
    /// TODO:
    return "emu80.dmp";
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
