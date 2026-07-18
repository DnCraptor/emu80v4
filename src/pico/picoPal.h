// Platform Abstraction Layer (pico version)

#ifndef PICOPAL_H
#define PICOPAL_H

#include <string>
#include <cstring>

#include "../EmuTypes.h"
#include "../PalKeys.h"

#include "graphics.h"

using namespace std;

class PalWindow;

void palRequestForQuit();
void palExecute();
int palReadFromFile(const std::string& fileName, int first, int size, uint8_t* buffer, bool useBasePath = true);
string palMakeFullFileName(string fileName);
#define palGetCounter time_us_64
#define palGetCounterFreq() 1000000
void palDelay(uint64_t time);
bool palSetVsync(bool vsync);
bool palSetSampleRate(int sampleRate);
void palPlaySample(int16_t left, int16_t right); // stereo

#ifndef PAL_WASM
std::string palOpenFileDialog(std::string title, std::string filter, bool write, PalWindow* window = nullptr);
void palUpdateConfig();
#endif //!PAL_WASM


void palWxProcessMessages();

void palLog(std::string s);

void palMsgBox(std::string msg, bool critical = false);

class EmuLog
{
    public:
        EmuLog& operator<<(std::string s);
        EmuLog& operator<<(const char* sz);
        EmuLog& operator<<(int n);
};

extern EmuLog emuLog;

struct PalFileInfo {
    std::string fileName;
    //char shortLatinFileName[11];
    bool isDir;
    unsigned size;
    int second;
    int minute;
    int hour;
    int day;
    int month;
    int year;
};

#endif // PICOPAL_H
