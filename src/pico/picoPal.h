// Platform Abstraction Layer (pico version)

#ifndef PICOPAL_H
#define PICOPAL_H

#include <string>
#include <vector>
#include <list>
#include <map>

#include <cstring>

#include "../EmuTypes.h"
#include "../PalKeys.h"

#include "graphics.h"

using namespace std;

class PalWindow;

void palRequestForQuit();
void palExecute();
int palReadFromFile(const std::string& fileName, int first, int size, uint8_t* buffer, bool useBasePath = true);
uint8_t* palReadFile(const string& fileName, int &fileSize, bool useBasePath = true);
string palMakeFullFileName(string fileName);
#define palGetCounter time_us_64
#define palGetCounterFreq() 1000000
void palDelay(uint64_t time);
std::string palGetDefaultPlatform();
bool palSetVsync(bool vsync);
bool palSetSampleRate(int sampleRate);
void palCopyTextToClipboard(const char* text);
std::string palGetTextFromClipboard();
void palPlaySample(int16_t left, int16_t right); // stereo

#ifndef PAL_WASM
std::string palOpenFileDialog(std::string title, std::string filter, bool write, PalWindow* window = nullptr);
void palUpdateConfig();
#endif //!PAL_WASM

bool palChoosePlatform(std::vector<PlatformInfo>& pi, int& pos, bool& newWnd, bool setDef = false, PalWindow* wnd = nullptr);
bool palChooseConfiguration(std::string platformName, PalWindow* wnd);
void palSetRunFileName(std::string runFileName);
void palShowConfigWindow(int curTabId = 0);
void palGetPalDefines(std::list<std::string>& difineList);
void palGetPlatformDefines(std::string platformName, std::map<std::string, std::string>& definesMap);

void palAddTabToConfigWindow(int tabId, std::string tabName);
void palRemoveTabFromConfigWindow(int tabId);
void palAddRadioSelectorToTab(int tabId, int column, std::string caption, std::string object, std::string property, SelectItem* items, int nItems);
void palSetTabOptFileName(int tabId, std::string optFileName);

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

typedef PalFileInfo* PPalFileInfo;

void palGetDirContent(const std::string& dir, std::list<PalFileInfo*>& fileList);

#endif // PICOPAL_H
