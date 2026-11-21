/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2016-2021
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

#ifndef CPU_H
#define CPU_H


#include "EmuObjects.h"


class CpuHook;
class Cpu8080Compatible;

class Cpu : public ActiveDevice
{
    public:
        Cpu();
        virtual ~Cpu();


        void attachAddrSpace(AddressableDevice* as);
        void attachIoAddrSpace(AddressableDevice* as);
        void setStartAddr(unsigned addr) {m_startAddr = addr;}

        virtual void interrupt(int) {}
        virtual void hrq(int) {}

        void disableHooks() {m_hooksDisabled = true;}
        void enableHooks() {m_hooksDisabled = false;}

        AddressableDevice* getAddrSpace() {return m_addrSpace;}
        AddressableDevice* getIoAddrSpace() {return m_ioAddrSpace;}

        // Entries are real pointers to the start of a 256-byte page.
        void setReadPage(unsigned page, const uint8_t* data) {m_rdPage[page] = data;}
        void setWritePage(unsigned page, uint8_t* data) {m_wrPage[page] = data;}

    protected:
        int as_input(int addr)
        {
            const auto* page = m_rdPage[(addr >> 8) & 0xff];
            return page ? page[addr & 0xff] : m_addrSpace->readByte(addr & 0xffff);
        }

        void as_output(int addr, int value)
        {
            auto* page = m_wrPage[(addr >> 8) & 0xff];
            if (page)
                page[addr & 0xff] = uint8_t(value);
            else
                m_addrSpace->writeByte(addr & 0xffff, uint8_t(value));
        }

        const uint8_t* m_rdPage[256] = {};
        uint8_t* m_wrPage[256] = {};
        AddressableDevice* m_addrSpace = nullptr;
        AddressableDevice* m_ioAddrSpace = nullptr;
        unsigned m_startAddr = 0;

        bool m_hooksDisabled = false;

        bool m_debugOnHalt = false;
        bool m_debugOnIllegalCmd = false;

};

class Cpu8080Compatible : public Cpu
{
    public:
        Cpu8080Compatible();
        ~Cpu8080Compatible() override;

        void addHook(CpuHook* hook);

        virtual void intRst(int vect) = 0;
        virtual void intCall(uint16_t addr) = 0;
        virtual void ret() = 0;

        void hrq(int ticks) override;

        virtual uint16_t getPC() = 0;
        virtual uint16_t getBC() = 0;
        virtual uint16_t getDE() = 0;
        virtual uint16_t getHL() = 0;
        virtual uint16_t getSP() = 0;
        virtual uint16_t getAF() = 0;

        /*virtual int getA() = 0;
        virtual int getB() = 0;
        virtual int getC() = 0;
        virtual int getD() = 0;
        virtual int getE() = 0;
        virtual int getH() = 0;
        virtual int getL() = 0;*/

        virtual void setBC(uint16_t value) = 0;
        virtual void setDE(uint16_t value) = 0;
        virtual void setHL(uint16_t value) = 0;
        virtual void setSP(uint16_t value) = 0;
        virtual void setPC(uint16_t value) = 0;
        virtual void setAF(uint16_t value) = 0;
        virtual void setIFF(bool iff) = 0;

        virtual bool getInte() = 0;
        virtual bool checkForStackOperation() = 0;

    protected:
        int io_input(int port);
        void io_output(int port, int value);
        bool processHooks(uint16_t addr);

        // Быстрый отсев по младшему байту адреса. Хуков десяток, срабатывают
        // они раз в миллионы команд, а линейный перебор шёл на каждой. Фильтр
        // на 256 бит стоит 32 байта и пропускает к перебору лишь те адреса,
        // младший байт которых совпал с младшим байтом какого-нибудь хука.
        // Ложное срабатывание безопасно: перебор просто ничего не найдёт.
        inline bool mayHaveHook(uint16_t addr) const
        {
            return (m_hookAddrFilter[(addr & 0xFF) >> 5] >> (addr & 0x1F)) & 1;
        }

        static constexpr int MAX_HOOKS = 32;
        CpuHook* m_hooks[MAX_HOOKS] = {};
        uint32_t m_hookAddrFilter[8] = {};
        int m_hookCount = 0;
};

#endif // CPU_H
