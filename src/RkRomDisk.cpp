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

#include <string.h>

#include "Pal.h"
#include "Globals.h"
#include "Emulation.h"
#include "RkRomDisk.h"

using namespace std;


RkRomDisk::RkRomDisk(string romDiskName)
{
    m_romDisk = new FIL();
    romDiskName = palMakeFullFileName(romDiskName);
    f_open(m_romDisk, romDiskName.c_str(), FA_READ);
    m_fileSize = f_size(m_romDisk);
}


RkRomDisk::~RkRomDisk()
{
    f_close(m_romDisk);
    delete m_romDisk;
}


uint8_t RkRomDisk::getPortA()
{
    if (m_curAddr < unsigned(m_fileSize)) {
        f_lseek(m_romDisk, m_curAddr);
        uint8_t res;
        UINT br;
        f_read(m_romDisk, &res, 1, &br);
        return res;
    }
    return 0xFF;
}


void RkRomDisk::setPortB(uint8_t value)
{
    m_curAddr = (m_curAddr & ~0xff) | value;
}


void RkRomDisk::setPortC(uint8_t value)
{
    m_curAddr = (m_curAddr & ~0xff00) | (value << 8);
}


void ExtRkRomDisk::setPage(int page)
{
    m_curAddr = (m_curAddr & 0xFFFF) | (page << 16);
}


void ExtRkRomDisk::reset()
{
    m_curAddr &= 0xFFFF;
}


void RomDiskPageSelector::writeByte(int, uint8_t value)
{
    m_romDisk->setPage(value & m_mask);
};


bool RomDiskPageSelector::setProperty(const string& propertyName, const EmuValuesList& values)
{
    if (AddressableDevice::setProperty(propertyName, values))
        return true;

    if (propertyName == "romDisk") {
        m_romDisk = static_cast<ExtRkRomDisk*>(g_emulation->findObject(values[0].asString()));
        return true;
    } else  if (propertyName == "bits") {
        int bits = values[0].asInt();
        m_mask = (((1 << bits) - 1) & 0xFF);
        return true;
    }

    return false;
}
