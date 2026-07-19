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
class VectorCore;
class Cpu8080Compatible;
class VectorRenderer;

class Cpu : public ActiveDevice
{
    public:
        Cpu();
        virtual ~Cpu();


        void attachAddrSpace(AddressableDevice* as);
        void attachIoAddrSpace(AddressableDevice* as);
        void attachCore(VectorCore* core);
        void setStartAddr(unsigned addr) {m_startAddr = addr;}

        virtual void interrupt(int) {}
        virtual void hrq(int) {}

        void disableHooks() {m_hooksDisabled = true;}
        void enableHooks() {m_hooksDisabled = false;}

        AddressableDevice* getAddrSpace() {return m_addrSpace;}
        AddressableDevice* getIoAddrSpace() {return m_ioAddrSpace;}

        // Быстрая карта гостевой памяти: 256 страниц по 256 байт.
        // Значение — база блока, в котором адрес лежит по своему собственному
        // смещению, то есть доступ выполняется как base[addr] без маскирования
        // (у Вектора и ОЗУ, и ПЗУ отображаются с адреса 0). nullptr означает
        // «идти прежним путём через m_addrSpace».
        // Карту строит VectorAddrSpace::rebuildPageMap().
        void clearPageMap();
        void setReadPage(int page, const uint8_t* base) {m_rdPage[page & 0xFF] = base;}
        void setWritePage(int page, uint8_t* base) {m_wrPage[page & 0xFF] = base;}
        void attachCrtRenderer(VectorRenderer* crt) {m_crt = crt;}

    protected:
        // Обращение к гостевой памяти. Прежде это была цепочка из трёх
        // виртуальных вызовов (m_addrSpace->readByte -> VectorAddrSpace::readByte
        // -> Ram::readByte) с проверками страниц и границ на каждый байт.
        // Теперь в типовом случае это загрузка указателя страницы и загрузка
        // байта; всё, что не ложится в статическую карту, уходит на прежний путь.
        inline int as_input(int addr)
        {
            const uint8_t* p = m_rdPage[(addr >> 8) & 0xFF];
            if (__builtin_expect(p != nullptr, 1))
                return p[addr & 0xFFFF];
            return m_addrSpace->readByte(addr);
        }

        inline void as_output(int addr, int value)
        {
            uint8_t* p = m_wrPage[(addr >> 8) & 0xFF];
            if (__builtin_expect(p != nullptr, 1)) {
                // Вся верхняя половина адресного пространства Вектора — экран
                if (addr >= 0x8000)
                    vidWriteNotify();
                p[addr & 0xFFFF] = uint8_t(value);
                return;
            }
            m_addrSpace->writeByte(addr, value);
        }

        // Определён в Cpu.cpp: заголовку не нужен полный тип VectorRenderer
        void vidWriteNotify();

        const uint8_t* m_rdPage[256] = {};
        uint8_t* m_wrPage[256] = {};
        VectorRenderer* m_crt = nullptr;

        AddressableDevice* m_addrSpace = nullptr;
        AddressableDevice* m_ioAddrSpace = nullptr;
        VectorCore* m_core = nullptr;
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

        static constexpr int MAX_HOOKS = 32;
        CpuHook* m_hooks[MAX_HOOKS] = {};
        int m_hookCount = 0;
};

#endif // CPU_H
