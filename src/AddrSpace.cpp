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

#include "AddrSpace.h"

using namespace std;

AddrSpace::AddrSpace(uint8_t nullByte)
{
    m_nullByte = nullByte;
    m_itemCountR = m_itemCountW = 0;
}


void AddrSpace::addRange(int firstAddr, int lastAddr, AddressableDevice* addrDevice, int devFirstAddr)
{
    addReadRange(firstAddr, lastAddr, addrDevice, devFirstAddr);
    addWriteRange(firstAddr, lastAddr, addrDevice, devFirstAddr);
}


void AddrSpace::addReadRange(int firstAddr, int lastAddr, AddressableDevice* addrDevice, int devFirstAddr)
{
    auto devIt = m_devicesRVector.begin();
    auto firstIt = m_firstAddressesRVector.begin();
    auto sizeIt = m_itemSizesRVector.begin();
    auto devFirstIt = m_devFirstAddressesRVector.begin();
    for (int i = 0; i < m_itemCountR && m_firstAddressesR[i] <= firstAddr; i++, devIt++, firstIt++, sizeIt++, devFirstIt++);

    m_devicesRVector.insert(devIt, addrDevice);
    m_firstAddressesRVector.insert(firstIt, firstAddr);
    m_itemSizesRVector.insert(sizeIt, lastAddr - firstAddr + 1);
    m_devFirstAddressesRVector.insert(devFirstIt, devFirstAddr);

    m_itemCountR++;

    m_devicesRVector.resize(m_itemCountR);
    m_firstAddressesRVector.resize(m_itemCountR);
    m_itemSizesRVector.resize(m_itemCountR);
    m_devFirstAddressesRVector.resize(m_itemCountR);

    m_devicesR = m_devicesRVector.data();
    m_firstAddressesR = m_firstAddressesRVector.data();
    m_itemSizesR = m_itemSizesRVector.data();
    m_devFirstAddressesR = m_devFirstAddressesRVector.data();
}


void AddrSpace::addWriteRange(int firstAddr, int lastAddr, AddressableDevice* addrDevice, int devFirstAddr)
{
    auto devIt = m_devicesWVector.begin();
    auto firstIt = m_firstAddressesWVector.begin();
    auto sizeIt = m_itemSizesWVector.begin();
    auto devFirstIt = m_devFirstAddressesWVector.begin();
    for (int i = 0; i < m_itemCountW && m_firstAddressesW[i] <= firstAddr; i++, devIt++, firstIt++, sizeIt++, devFirstIt++);

    m_devicesWVector.insert(devIt, addrDevice);
    m_firstAddressesWVector.insert(firstIt, firstAddr);
    m_itemSizesWVector.insert(sizeIt, lastAddr - firstAddr + 1);
    m_devFirstAddressesWVector.insert(devFirstIt, devFirstAddr);

    m_itemCountW++;

    m_devicesWVector.resize(m_itemCountW);
    m_firstAddressesWVector.resize(m_itemCountW);
    m_itemSizesWVector.resize(m_itemCountW);
    m_devFirstAddressesWVector.resize(m_itemCountW);

    m_devicesW = m_devicesWVector.data();
    m_firstAddressesW = m_firstAddressesWVector.data();
    m_itemSizesW = m_itemSizesWVector.data();
    m_devFirstAddressesW = m_devFirstAddressesWVector.data();
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
