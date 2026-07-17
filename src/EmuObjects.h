/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2016-2023
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

#ifndef EMUOBJECTS_H
#define EMUOBJECTS_H

#include <cstdint>



class VectorCore;
class EmuWindow;
class Cpu;
class KbdLayout;
class CrtRenderer;
class DiskImage;
class FileLoader;
class RamDisk;
class Keyboard;
class KbdTapper;

class EmuObject
{
    public:
        virtual ~EmuObject() = default;

        int getKDiv() {return m_kDiv;}

        virtual void setFrequency(int64_t freq); // лучше бы в одном из производных классов, но пусть пока будет здесь
        virtual void init() {}
        virtual void shutdown() {}

        virtual void reset() {}

        void setMachine(VectorCore* machine) {m_machine = machine;}

    protected:
        int m_kDiv = 1;
        VectorCore* m_machine = nullptr;
};

class Ram;
class SRam;

class AddressableDevice : public EmuObject
{
    public:
        virtual Ram* asRam() { return nullptr; }
        virtual SRam* asSRam() { return nullptr; }
        ~AddressableDevice() override = default;

        virtual void writeByte(int addr, uint8_t value) = 0;
        virtual uint8_t readByte(int) {return 0xFF;}

        void setAddrMask(int mask) {m_addrMask = mask;}

    protected:
        int m_addrMask = 0;


};


class IActive
{
    public:
        IActive();
        virtual ~IActive();
        uint64_t getClock() {return m_curClock;}
        //void setClock(uint64_t clock) {m_curClock = clock;}
        void pause() {m_isPaused = true; m_curClock = -1;}
        void resume() {m_isPaused = false;}
        void syncronize(uint64_t curClock) {m_curClock = curClock;}
        void syncronize();
        inline bool isPaused() {return m_isPaused;}
        virtual void operate() = 0;

    protected:
        //int m_kDiv = 1;
        uint64_t m_curClock = 0;
        bool m_isPaused = false;
};


class ActiveDevice : public EmuObject, public IActive
{

};


#endif // EMUOBJECTS_H
