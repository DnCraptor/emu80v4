/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2016-2024
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

// emulation.h

#ifndef EMULATION_H
#define EMULATION_H

#include <cstdint>
#include <string>

#include "PalKeys.h"
#include "EmuTypes.h"
#include "EmuObjects.h"

class Cpu;
class SoundMixer;
class WavReader;
class PrnWriter;
class VectorCore;


/*struct DevListItem
{
    uint64_t clock;
    ActiveDevice* device;
    DevListItem* next;
};*/

class Emulation
{
    public:
        Emulation(); //: EmuObject();

        // Создание и инициализация машины. Вынесено из конструктора, чтобы тот
        // не обращался к ещё не готовым глобальным объектам.
        void init();

        // Однократный сбор всех созданных активных устройств
        void registerActiveDevices();
        bool activeDevicesRegistered() const {return m_activeDevicesRegistered;}

        ~Emulation();



        void registerActiveDevice(IActive* device);
        void unregisterActiveDevice(IActive* device);

        inline void debugRequest(Cpu* cpu) {m_debugReqCpu = cpu;}
        inline bool isDebuggerActive() {return m_debugReqCpu;}

        void processKey(PalKeyCode keyCode, bool isPressed, unsigned unicodeKey = 0);
        void machineKey(PalKeyCode keyCode, bool isPressed, unsigned unicodeKey = 0);
        void resetKeys();

        void mainLoopCycle();
        void exec(uint64_t ticks, bool forced = false);

        inline uint64_t getCurClock() {return m_curClock;}
        inline SoundMixer* getSoundMixer() {return m_mixer;}
        inline WavReader* getWavReader() {return m_wavReader;}
        inline PrnWriter* getPrnWriter() {return m_prnWriter;}

        void setFrequency(int64_t freq);
        int64_t getFrequency() {return m_frequency;}
        void setSampleRate(int sampleRate);             // установка частоты дискретизации звуковой карты
        int getSampleRate() {return m_sampleRate;}
        void setVsync(bool vsync);                      // установка vsync
        void setTemporarySpeedUpFactor(unsigned speed);
        void updateFrequency();

        double getSpeedUpFactor() {return m_currentSpeedUpFactor;}
        bool getPausedState() {return m_isPaused;}
        bool getFullThrottleState() {return m_fullThrottle;}


    private:
        static constexpr int MAX_ACTIVE_DEVICES = 32;
        IActive* m_activeDevices[MAX_ACTIVE_DEVICES] = {};
        int nDevices = 0;
        IActive* m_cpuDev = nullptr;
        bool m_activeDevicesRegistered = false;
        uint64_t m_clockOffset = 0;
        uint64_t m_sysClock;
        uint64_t m_prevSysClock = 0;
        Cpu* m_debugReqCpu = nullptr;
        bool m_fullThrottle = false;

        bool m_isPaused = false;
        double m_speedUpFactor = 1.0;
        double m_currentSpeedUpFactor = 1.0;

        uint64_t m_frequency;
        uint64_t m_curFrequency;
        bool m_vsync = true;
        unsigned m_sampleRate = 48000;

        uint64_t m_curClock = 0;

        SoundMixer* m_mixer = nullptr;
        WavReader* m_wavReader = nullptr;
        PrnWriter* m_prnWriter = nullptr;
        VectorCore* m_vector = nullptr;

};


#endif  // EMULATION_H

