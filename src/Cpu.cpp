/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2016-2022
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

#include <cstring>

#include "Globals.h"
#include "Cpu.h"
#include "CpuHook.h"
#include "CpuWaits.h"
#include "Emulation.h"

using namespace std;

Cpu::Cpu()
{
    m_addrSpace = nullptr;
    m_ioAddrSpace = nullptr;
}


Cpu::~Cpu() = default;


void Cpu::attachAddrSpace(AddressableDevice* as)
{
    m_addrSpace = as;
}



void Cpu::attachIoAddrSpace(AddressableDevice* as)
{
    m_ioAddrSpace = as;
}



void Cpu::attachCore(VectorCore* core)
{
    m_core = core;
}


int Cpu::as_input(int addr)
{
    return m_addrSpace->readByte(addr);
}


void Cpu::as_output(int addr, int value)
{
    m_addrSpace->writeByte(addr, value);
}

Cpu8080Compatible::Cpu8080Compatible()
{
}


Cpu8080Compatible::~Cpu8080Compatible()
{
    for (int i = 0; i < m_hookCount; i++)
        m_hooks[i]->setCpu(nullptr);
}


void Cpu8080Compatible::addHook(CpuHook* hook)
{
    if (m_hookCount >= MAX_HOOKS)
        return;
    m_hooks[m_hookCount++] = hook;
    hook->setCpu(this);
}


void Cpu8080Compatible::removeHook(CpuHook* hook)
{
    int index = 0;
    while (index < m_hookCount && m_hooks[index] != hook)
        index++;
    if (index == m_hookCount)
        return;
    hook->setCpu(nullptr);
    for (int i = index + 1; i < m_hookCount; i++)
        m_hooks[i - 1] = m_hooks[i];
    m_hooks[--m_hookCount] = nullptr;
}


bool Cpu8080Compatible::processHooks(uint16_t addr)
{
    bool handled = false;
    for (int i = 0; i < m_hookCount; i++)
        if (m_hooks[i]->getHookAddr() == addr)
            handled = handled || m_hooks[i]->hookProc();
    return handled;
}


void Cpu8080Compatible::hrq(int ticks) {
    m_curClock += ticks;
}


int Cpu8080Compatible::io_input(int port)
{
    if (m_ioAddrSpace)
        return m_ioAddrSpace->readByte(port);
    else
        return m_addrSpace->readByte((port & 0xff) << 8 | (port & 0xff));
}


void Cpu8080Compatible::io_output(int port, int value)
{
    if (m_ioAddrSpace)
        m_ioAddrSpace->writeByte(port, value);
    else
        m_addrSpace->writeByte((port & 0xff) <<8 | (port & 0xff), value);
}
