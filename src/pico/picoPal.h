// Platform Abstraction Layer (pico version)

#ifndef PICOPAL_H
#define PICOPAL_H

#include <string>
#include <cstring>

#include "../EmuTypes.h"
#include "../PalKeys.h"

#include "graphics.h"

using namespace std;


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
std::string palOpenFileDialog(const std::string& title, const std::string& filter, bool write);
#endif //!PAL_WASM


void palLog(const std::string& s);

void palMsgBox(const std::string& msg, bool critical = false);

class EmuLog
{
    public:
        EmuLog& operator<<(const std::string& s);
        EmuLog& operator<<(const char* sz);
        EmuLog& operator<<(int n);
};

extern EmuLog emuLog;

// Определение типа звукового выхода (ШИМ или I2S) при старте
bool palProbeAudioOutput();
bool palAudioIsI2S();
// Сырые результаты прозвонки: [0],[1] — время нарастания в мкс, [2] — внешняя запитка
uint32_t palAudioProbe(int i);

struct PalFileInfo {
    std::string fileName;   // 24 байта: указатель, длина и буфер коротких строк
    //char shortLatinFileName[11];
    bool isDir;

    // Здесь были ещё size и шесть int под дату и время. Они заполнялись в
    // palGetDirContent(), но нигде не читались: список файлов показывает только
    // имя и признак каталога, сортировка тоже смотрит только на них.
    // 28 байт на запись при 256 записях — 7168 байт впустую.
};

#endif // PICOPAL_H
