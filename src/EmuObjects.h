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
class Cpu;
class KbdLayout;
class CrtRenderer;
class DiskImage;
class RamDisk;
class Keyboard;
class KbdTapper;
class SnapshotWriter;
class SnapshotReader;

class SnapshotSerializable
{
    public:
        virtual ~SnapshotSerializable() = default;

        virtual uint32_t snapshotSectionId() const = 0;
        virtual uint16_t snapshotSectionVersion() const = 0;
        virtual bool saveState(SnapshotWriter& writer) const = 0;
        virtual bool loadState(SnapshotReader& reader, uint16_t version) = 0;
        virtual void postLoad() {}
};

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

class AddressableDevice : public EmuObject
{
    public:
        ~AddressableDevice() override = default;

        virtual void writeByte(int addr, uint8_t value) = 0;
        virtual uint8_t readByte(int) {return 0xFF;}
};


class IActive
{
    public:
        IActive();
        virtual ~IActive();

        // Список всех созданных активных устройств. Строится конструкторами и
        // не обращается к g_emulation, поэтому безопасен на этапе статической
        // инициализации. Добавление в хвост сохраняет порядок создания, а он
        // же был прежним порядком регистрации — это важно, потому что
        // планировщик разрешает совпадение тактов по позиции в массиве.
        static IActive* firstActive() {return s_firstActive;}
        IActive* nextActive() const {return m_nextActive;}
        inline uint64_t getClock() {return m_curClock;}
        inline void pause() {m_isPaused = true; m_curClock = -1;}
        inline void resume() {m_isPaused = false;}
        inline void syncronize(uint64_t curClock) {m_curClock = curClock;}
        void syncronize();
        inline bool isPaused() {return m_isPaused;}
        virtual void operate() = 0;

    protected:
        uint64_t m_curClock = 0;
        bool m_isPaused = false;

    private:
        static IActive* s_firstActive;
        static IActive* s_lastActive;
        IActive* m_nextActive = nullptr;
};


class ActiveDevice : public EmuObject, public IActive
{

};


#endif // EMUOBJECTS_H
