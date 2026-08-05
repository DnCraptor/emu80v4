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
#include "PlatformCore.h"

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



void Cpu::attachCore(PlatformCore* core)
{
    m_core = core;
}


void Cpu::addHook(CpuHook* hook)
{
    if (!hook || !m_hooks || m_nHooks >= m_hookCapacity)
        return;

    m_hooks[m_nHooks++] = hook;
    hook->setCpu(this);
}


void Cpu::removeHook(CpuHook* hook)
{
    for (int i = 0; i < m_nHooks; ++i) {
        if (m_hooks[i] != hook)
            continue;

        for (int j = i + 1; j < m_nHooks; ++j)
            m_hooks[j - 1] = m_hooks[j];

        m_hooks[--m_nHooks] = nullptr;
        hook->setCpu(nullptr);
        return;
    }
}


int Cpu::as_input(int addr)
{
    if (!m_cycleWaits)
        return m_addrSpace->readByte(addr);
    else {
        int tag;
        int read = m_addrSpace->readByteEx(addr, tag);
        m_curClock += m_kDiv * m_cycleWaits->getCpuCycleWaitStates(tag, false);
        return read;
    }
}


void Cpu::as_output(int addr, int value)
{
    if (!m_cycleWaits)
        m_addrSpace->writeByte(addr, value);
    else {
        int tag;
        m_addrSpace->writeByteEx(addr, value, tag);
        m_curClock += m_kDiv * m_cycleWaits->getCpuCycleWaitStates(tag, true);
    }
}


/*std::string Cpu::getDebugInfo()
{
    stringstream ss;
    ss << "CPU:" << "\n" << m_curClock / m_kDiv;
    return ss.str();
}*/

Cpu8080Compatible::Cpu8080Compatible()
{
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
