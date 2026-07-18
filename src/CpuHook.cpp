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

#include "Globals.h"
#include "Emulation.h"
#include "CpuHook.h"
#include "TapeRedirector.h"


CpuHook::CpuHook(int addr)
{
    m_hookAddr = addr;
}


CpuHook::~CpuHook()
{
    //dtor
}


void CpuHook::setSignature(const char* signature)
{
    m_signatureLen = 0;

    for (unsigned i = 0; signature[i] && signature[i + 1] && m_signatureLen < MAX_SIGNATURE_LEN; i += 2) {
        auto hexValue = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            return 0;
        };

        m_signature[m_signatureLen++] = (hexValue(signature[i]) << 4) | hexValue(signature[i + 1]);
    }

    m_hasSignature = m_signatureLen != 0;
}


bool CpuHook::checkSignature()
{
    AddressableDevice* as = m_cpu->getAddrSpace();
    for (unsigned i = 0; i < m_signatureLen; i++)
        if (m_signature[i] != as->readByte(m_hookAddr + i))
            return false;
    return true;
}




bool Ret8080Hook::hookProc()
{
    if (!m_isEnabled || (m_hasSignature && !checkSignature()))
        return false;

    m_cpu->ret();

    return true;
}
