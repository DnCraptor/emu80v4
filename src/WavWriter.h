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

#ifndef WAVWRITER_H
#define WAVWRITER_H

#include <string>

#include "EmuObjects.h"
#include "PalFile.h"

class VectorCore;

class WavWriter : public ActiveDevice
{
    public:
        WavWriter();
        ~WavWriter();

        // Объект существует всё время работы прошивки и зарегистрирован как
        // активное устройство однократно, при старте. Пока файл не открыт, он
        // находится в состоянии pause() и планировщиком не рассматривается.
        // Прежде WavWriter создавался и удалялся по ходу эмуляции, из
        // RkTapeOutHook::hookProc(), то есть регистрация активного устройства
        // происходила прямо внутри обхода их массива в Emulation::exec().
        bool open(VectorCore* core, const std::string& fileName, bool cswFormat);
        void close();

        // derived from EmuObject

        // derived from ActiveDevice
        void operate() override;

        bool isOpen() {return m_open;}

    private:
        static constexpr uint8_t c_wavHeader[44] = {
            0x52, 0x49, 0x46, 0x46, // RIFF
            0x24, 0x00, 0x00, 0x00, // file size - 8 = chunk size
            0x57, 0x41, 0x56, 0x45, // WAVE
            0x66, 0x6D, 0x74, 0x20, // fmt
            0x10, 0x00, 0x00, 0x00, // 16 - subchunk size
            0x01, 0x00,             // PCM = 1
            0x01, 0x00,             // Mono = 1
            0x44, 0xAC, 0x00, 0x00, // 0xAC44 = 44100 Hz
            0x44, 0xAC, 0x00, 0x00, // 44100 = bytes per second
            0x01, 0x00,             // 1 - bytes per sample
            0x08, 0x00,             // bits per sample
            0x64, 0x61, 0x74, 0x61, // DATA
            0x00, 0x00, 0x00, 0x00  // data size
            };

        static constexpr uint8_t c_cswHeader[32] = {
            0x43, 0x6F, 0x6D, 0x70, 0x72, 0x65, 0x73, 0x73, 0x65, 0x64, 0x20, // Compressed
            0x53, 0x71, 0x75, 0x61, 0x72, 0x65, 0x20,                         // Square
            0x57, 0x61, 0x76, 0x65, 0x1A,                                     // Wave
            0x01, 0x01,      // version
            0x44, 0xAC,      // sample rate
            0x01,            // RLE compression
            0x00,            // initial value
            0x00, 0x00, 0x00 // reserved
        };

        unsigned m_ticksPerSample = 1;  // тактов на сэмпл
        PalFile m_file;
        std::string m_fileName;
        bool m_open = false;
        VectorCore* m_core = nullptr;
        unsigned m_size = 0;
        bool m_initialValue = false;
        bool m_cswFormat = false;

        unsigned m_cswRleCounter = 0;
        bool m_cswCurValue = false;

        void writeCswSequence();
};


#endif // WAVWRITER_H
