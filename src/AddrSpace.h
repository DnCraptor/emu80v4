/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2016-2018
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

#ifndef ADDRSPACE_H
#define ADDRSPACE_H

#include "EmuObjects.h"

class AddrSpace : public AddressableDevice
{
    public:
        AddrSpace(uint8_t nullByte = 0xFF);


        uint8_t readByte(int addr) override;
        void writeByte(int addr, uint8_t value) override;

        void addRange(int firstAddr, int lastAddr, AddressableDevice* addrDevice, int devFirstAddr = 0);
        void addReadRange(int firstAddr, int lastAddr, AddressableDevice* addrDevice, int devFirstAddr = 0);
        void addWriteRange(int firstAddr, int lastAddr, AddressableDevice* addrDevice, int devFirstAddr = 0);


private:
        static constexpr int MAX_RANGES = 32;

        struct Range {
            AddressableDevice* device = nullptr;
            int firstAddress = 0;
            int itemSize = 0;
            int devFirstAddress = 0;
        };

        uint8_t m_nullByte;
        Range m_readRanges[MAX_RANGES] = {};
        Range m_writeRanges[MAX_RANGES] = {};
        int m_itemCountR = 0;
        int m_itemCountW = 0;

};



class AddrSpaceInverter : public AddressableDevice
{
    public:
        AddrSpaceInverter(AddressableDevice* as);

        void writeByte(int addr, uint8_t value) override;
        uint8_t readByte(int addr) override;


private:
        AddressableDevice* m_as;
};



#endif // ADDRSPACE_H
