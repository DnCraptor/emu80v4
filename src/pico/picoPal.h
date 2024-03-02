/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2017-2018
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

// Platform Abstraction Layer (lite version)

#ifndef LITEPAL_H
#define LITEPAL_H

#include <string>
#include <vector>
#include <list>
#include <map>

#include "../EmuTypes.h"
#include "../PalKeys.h"
#include "../debug.h"

void palPicoInit();

class PalWindow;

std::string palOpenFileDialog(std::string title, std::string filter, bool write, PalWindow* window = nullptr);

bool palChoosePlatform(std::vector<PlatformInfo>& pi, int& pos, bool& newWnd, bool setDef = false, PalWindow* wnd = nullptr);
bool palChooseConfiguration(std::string platformName, PalWindow* wnd);
void palSetRunFileName(std::string runFileName);
void palShowConfigWindow(int curTabId = 0);
void palUpdateConfig();
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

void palGetDirContent(const std::string& dir, std::list<PalFileInfo*>& fileList);

uint8_t* palReadFile(const std::string& fileName, int &fileSize, bool useBasePath = true);

class PalWindow
{
    public:

    enum PalWindowStyle {
        PWS_FIXED,
        PWS_RESIZABLE,
        PWS_FULLSCREEN
    };

    struct PalWindowParams {
        PalWindowStyle style;
        SmoothingType smoothing;
        bool vsync;
        bool visible;
        int width;
        int height;
        std::string title;
    };

        PalWindow();
        virtual ~PalWindow();
        void initPalWindow() {}

        static PalWindow* windowById(uint32_t id);
        void bringToFront();
        void maximize();
        void focusChanged(bool isFocused);

        virtual void mouseClick(int x, int y, PalMouseKey key) {}
        virtual void mouseDrag(int x, int y) = 0;

        virtual std::string getPlatformObjectName() = 0;
        EmuWindowType getWindowType() {return m_windowType;}
        virtual void calcDstRect(int srcWidth, int srcHeight,  double srcAspectRatio, int wndWidth, int wndHeight, int& dstWidth, int& dstHeight, int& dstX, int& dstY) = 0;
    protected:
        PalWindowParams m_params;
        EmuWindowType m_windowType = EWT_UNDEFINED;
        void setTitle(const std::string& title);
        void getSize(int& width, int& height);
        void applyParams();

        void drawFill(uint32_t color);
        void drawImage(uint32_t* pixels, int imageWidth, int imageHeight, double aspectratio,
                       bool blend = false, bool useAlpha = false);
        void drawEnd();
        void screenshotRequest(const std::string& ssFileName);
};

class PalFile
{
    public:
        bool open(std::string fileName, std::string mode = "r");
        void close();
        bool isOpen();
        bool eof();
        uint8_t read8();
        uint16_t read16();
        uint32_t read32();
        void write8(uint8_t value);
        void write16(uint16_t value);
        void write32(uint32_t value);
        int64_t getSize();
        int64_t getPos();
        void seek(int position);
        void skip(int len);

        static bool create(std::string fileName) {return false;}
        static bool del(std::string fileName) {return false;}
        static bool mkDir(std::string dirName) {return false;}
        static bool moveRename(std::string src, std::string dst) {return false;}
    private:
        void* mp_file;
};

#include <string>
using namespace std;

string palGetDefaultPlatform();

string palMakeFullFileName(string fileName);
int palReadFromFile(const std::string& fileName, int first, int size, uint8_t* buffer, bool useBasePath = true);

void palStart();
void palExecute();

uint64_t palGetCounter();
uint64_t palGetCounterFreq();
void palDelay(uint64_t time);

bool palSetSampleRate(int sampleRate);
int palGetSampleRate();

bool palSetFrameRate(int frameRate);
bool palSetVsync(bool vsync);

void palRequestForQuit();

void palCopyTextToClipboard(const char* text);
string palGetTextFromClipboard();

void palPlaySample(int16_t sample);

#endif // LITEPAL_H
