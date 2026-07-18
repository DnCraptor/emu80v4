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

#include "Globals.h"
#include "AddrSpace.h"

using namespace std;

AddrSpace::AddrSpace(uint8_t nullByte)
{
    m_nullByte = nullByte;
}


void AddrSpace::addRange(int firstAddr, int lastAddr, AddressableDevice* addrDevice, int devFirstAddr)
{
    addReadRange(firstAddr, lastAddr, addrDevice, devFirstAddr);
    addWriteRange(firstAddr, lastAddr, addrDevice, devFirstAddr);
}


void AddrSpace::addReadRange(int firstAddr, int lastAddr, AddressableDevice* addrDevice, int devFirstAddr)
{
    if (m_itemCountR >= MAX_RANGES)
        return;

    int pos = 0;
    while (pos < m_itemCountR && m_readRanges[pos].firstAddress <= firstAddr)
        pos++;
    for (int i = m_itemCountR; i > pos; i--)
        m_readRanges[i] = m_readRanges[i - 1];
    m_readRanges[pos] = {addrDevice, firstAddr, lastAddr - firstAddr + 1, devFirstAddr};
    m_itemCountR++;
}


void AddrSpace::addWriteRange(int firstAddr, int lastAddr, AddressableDevice* addrDevice, int devFirstAddr)
{
    if (m_itemCountW >= MAX_RANGES)
        return;

    int pos = 0;
    while (pos < m_itemCountW && m_writeRanges[pos].firstAddress <= firstAddr)
        pos++;
    for (int i = m_itemCountW; i > pos; i--)
        m_writeRanges[i] = m_writeRanges[i - 1];
    m_writeRanges[pos] = {addrDevice, firstAddr, lastAddr - firstAddr + 1, devFirstAddr};
    m_itemCountW++;
}


uint8_t __not_in_flash_func(AddrSpace::readByte)(int addr)
{
    int i = 0;
    while (i < m_itemCountR && m_readRanges[i].firstAddress <= addr)
        i++;
    if (i == 0)
        return m_nullByte;
    const Range& range = m_readRanges[i - 1];
    return addr - range.firstAddress < range.itemSize
        ? range.device->readByte(addr - range.firstAddress + range.devFirstAddress)
        : m_nullByte;
}


void __not_in_flash_func(AddrSpace::writeByte)(int addr, uint8_t value)
{
    int i = 0;
    while (i < m_itemCountW && m_writeRanges[i].firstAddress <= addr)
        i++;
    if (i == 0)
        return;
    const Range& range = m_writeRanges[i - 1];
    if (addr - range.firstAddress < range.itemSize)
        range.device->writeByte(addr - range.firstAddress + range.devFirstAddress, value);
}



AddrSpaceInverter::AddrSpaceInverter(AddressableDevice* as)
{
    m_as = as;
}


uint8_t __not_in_flash_func(AddrSpaceInverter::readByte)(int addr)
{
    return m_as->readByte(~addr);
}


void __not_in_flash_func(AddrSpaceInverter::writeByte)(int addr, uint8_t value)
{
    m_as->writeByte(~addr, value);
}
