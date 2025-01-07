﻿/*
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

#include <algorithm>
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


Cpu::~Cpu()
{
    for (auto it = m_hookVector.begin(); it != m_hookVector.end(); it++) {
        (*it)->setCpu(nullptr);
        if ((*it)->getName() == "") // breakponts
            delete (*it);
    }
}


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
    m_hookVector.push_back(hook);
    m_nHooks++;
    hook->setCpu(this);
}


void Cpu::removeHook(CpuHook* hook)
{
    m_hookVector.erase(remove(m_hookVector.begin(), m_hookVector.end(), hook), m_hookVector.end());
    m_nHooks--; // добавить проверку на существование!
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


bool Cpu::setProperty(const string& propertyName, const EmuValuesList& values)
{
    if (EmuObject::setProperty(propertyName, values))
        return true;

    if (propertyName == "addrSpace") {
        attachAddrSpace(static_cast<AddressableDevice*>(g_emulation->findObject(values[0].asString())));
        return true;
    } else if (propertyName == "ioAddrSpace") {
        attachIoAddrSpace(static_cast<AddressableDevice*>(g_emulation->findObject(values[0].asString())));
        return true;
    } else if (propertyName == "core") {
        attachCore(static_cast<PlatformCore*>(g_emulation->findObject(values[0].asString())));
        return true;
    } else if (propertyName == "addHook") {
        addHook(static_cast<CpuHook*>(g_emulation->findObject(values[0].asString())));
        return true;
    } else if (propertyName == "startAddr" && values[0].isInt()) {
        setStartAddr(values[0].asInt());
        return true;
    } else if (propertyName == "debugOnHalt") {
        if (values[0].asString() == "yes" || values[0].asString() == "no") {
            m_debugOnHalt = values[0].asString() == "yes";
            return true;
        }
    } else if (propertyName == "debugOnIllegalCmd") {
        if (values[0].asString() == "yes" || values[0].asString() == "no") {
            m_debugOnIllegalCmd = values[0].asString() == "yes";
            return true;
        }
    } else if (propertyName == "cpuWaits") {
        m_waits = (static_cast<CpuWaits*>(g_emulation->findObject(values[0].asString())));
        return true;
    } else if (propertyName == "cpuCycleWaits") {
        m_cycleWaits = (static_cast<CpuCycleWaits*>(g_emulation->findObject(values[0].asString())));
        return true;
    }

    return false;
}


std::string Cpu::getPropertyStringValue(const std::string& propertyName)
{
    string res;

    res = EmuObject::getPropertyStringValue(propertyName);
    if (res != "")
        return res;

    if (propertyName == "debugOnHalt")
        return m_debugOnHalt ? "yes" : "no";
    else if (propertyName == "debugOnIllegalCmd")
        return m_debugOnIllegalCmd ? "yes" : "no";

    return "";
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


void Cpu8080Compatible::addHook(CpuHook* hook)
{
    Cpu::addHook(hook);
    uint16_t addr = hook->getHookAddr();
    if (!m_hooksMap[addr])
        m_hooksMap[addr] = new list<CpuHook*>;
    m_hooksMap[addr]->push_back(hook);
}


void Cpu8080Compatible::removeHook(CpuHook* hook)
{
    Cpu::removeHook(hook);
    uint16_t addr = hook->getHookAddr();
    list<CpuHook*>* hookList = m_hooksMap[addr];
    if (hookList) {
        hookList->remove(hook);
        if (hookList->empty()) {
            delete hookList;
            m_hooksMap.erase(addr);
        }
    }
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
