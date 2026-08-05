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
#include "Emulation.h"

using namespace std;

AddrSpace::AddrSpace(uint8_t nullByte)
    : m_nullByte(nullByte)
{
}


void AddrSpace::addRange(int firstAddr, int lastAddr, AddressableDevice* addrDevice, int devFirstAddr)
{
    addReadRange(firstAddr, lastAddr, addrDevice, devFirstAddr);
    addWriteRange(firstAddr, lastAddr, addrDevice, devFirstAddr);
}


void AddrSpace::addReadRange(int firstAddr, int lastAddr, AddressableDevice* addrDevice, int devFirstAddr)
{
    if (!addrDevice || m_itemCountR >= MAX_RANGES || lastAddr < firstAddr)
        return;

    int pos = 0;
    while (pos < m_itemCountR && m_firstAddressesR[pos] <= firstAddr)
        ++pos;

    for (int i = m_itemCountR; i > pos; --i) {
        m_devicesR[i] = m_devicesR[i - 1];
        m_firstAddressesR[i] = m_firstAddressesR[i - 1];
        m_itemSizesR[i] = m_itemSizesR[i - 1];
        m_devFirstAddressesR[i] = m_devFirstAddressesR[i - 1];
    }

    m_devicesR[pos] = addrDevice;
    m_firstAddressesR[pos] = firstAddr;
    m_itemSizesR[pos] = lastAddr - firstAddr + 1;
    m_devFirstAddressesR[pos] = devFirstAddr;
    ++m_itemCountR;
}


void AddrSpace::addWriteRange(int firstAddr, int lastAddr, AddressableDevice* addrDevice, int devFirstAddr)
{
    if (!addrDevice || m_itemCountW >= MAX_RANGES || lastAddr < firstAddr)
        return;

    int pos = 0;
    while (pos < m_itemCountW && m_firstAddressesW[pos] <= firstAddr)
        ++pos;

    for (int i = m_itemCountW; i > pos; --i) {
        m_devicesW[i] = m_devicesW[i - 1];
        m_firstAddressesW[i] = m_firstAddressesW[i - 1];
        m_itemSizesW[i] = m_itemSizesW[i - 1];
        m_devFirstAddressesW[i] = m_devFirstAddressesW[i - 1];
    }

    m_devicesW[pos] = addrDevice;
    m_firstAddressesW[pos] = firstAddr;
    m_itemSizesW[pos] = lastAddr - firstAddr + 1;
    m_devFirstAddressesW[pos] = devFirstAddr;
    ++m_itemCountW;
}


uint8_t AddrSpace::readByte(int addr)
{
    if (m_addrMask)
        addr &= m_addrMask;
    int i;
    for (i = 0; i < m_itemCountR && m_firstAddressesR[i] <= addr; i++);
    if (i == 0)
        return m_nullByte;
    i--;
    return addr - m_firstAddressesR[i] < m_itemSizesR[i] ? m_devicesR[i]->readByte(addr - m_firstAddressesR[i] + m_devFirstAddressesR[i]) : m_nullByte;
}


void AddrSpace::writeByte(int addr, uint8_t value)
{
    if (m_addrMask)
        addr &= m_addrMask;
    int i;
    for (i = 0; i < m_itemCountW && m_firstAddressesW[i] <= addr; i++);
    if (i == 0)
        return;
    i--;
    if (addr - m_firstAddressesW[i] < m_itemSizesW[i])
        m_devicesW[i]->writeByte(addr - m_firstAddressesW[i] + m_devFirstAddressesW[i], value);
}


AddrSpaceMapper::AddrSpaceMapper(int nPages)
    : m_nPages(nPages > MAX_PAGES ? MAX_PAGES : nPages)
{
}


void AddrSpaceMapper::attachPage(int page, AddressableDevice* as)
{
    if (page < m_nPages)
        m_pages[page] = as;
}


void AddrSpaceMapper::setCurPage(int page)
{
    if (page < m_nPages)
        m_curPage = page;
}


uint8_t AddrSpaceMapper::readByte(int addr)
{
    if (m_pages[m_curPage])
        return m_pages[m_curPage]->readByte(addr);
    else
        return 0xFF;
}


void AddrSpaceMapper::writeByte(int addr, uint8_t value)
{
    if (m_pages[m_curPage])
        m_pages[m_curPage]->writeByte(addr, value);
}


AddrSpaceShifter::AddrSpaceShifter(AddressableDevice* as, int shift)
{
    m_as = as;
    m_shift = shift;
}


uint8_t AddrSpaceShifter::readByte(int addr)
{
    return m_as->readByte(addr >> m_shift);
}


void AddrSpaceShifter::writeByte(int addr, uint8_t value)
{
    m_as->writeByte(addr >> m_shift, value);
}



AddrSpaceInverter::AddrSpaceInverter(AddressableDevice* as)
{
    m_as = as;
}


uint8_t AddrSpaceInverter::readByte(int addr)
{
    return m_as->readByte(~addr);
}


void AddrSpaceInverter::writeByte(int addr, uint8_t value)
{
    m_as->writeByte(~addr, value);
}


void AddrSpaceWriteSplitter::writeByte(int addr, uint8_t value)
{
    m_as1->writeByte(addr, value);
    m_as2->writeByte(addr, value);
}


uint8_t AddrSpaceWriteSplitter::readByte(int addr)
{
    return m_as1->readByte(addr);
}
