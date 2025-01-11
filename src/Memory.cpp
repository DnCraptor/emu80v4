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

#include <string.h>

#include "Memory.h"
#include "Pal.h"
#include "psram_spi.h"

using namespace std;

// Ram implementation

static size_t sram_used = 0;

SRam::SRam(unsigned memSize) : m_size(memSize), m_offset(sram_used)
{
    m_supportsTags = true;
/// TODO:    memset(m_buf, 0, memSize);
    sram_used += m_size;
}

SRam::~SRam() {
    sram_used -= m_size; /// TODO: ensure order
}

void SRam::writeByte(int addr, uint8_t value) {
    write8psram(addr, value);
}

uint8_t SRam::readByte(int addr) {
    return read8psram(addr);
}

Ram::Ram(unsigned memSize)
{
    m_supportsTags = true;
    m_extBuf = nullptr;
    m_buf = new uint8_t [memSize];
    memset(m_buf, 0, memSize);
    m_size = memSize;
}

Ram::Ram(uint8_t* buf, unsigned memSize)
{
    m_supportsTags = true;
    m_extBuf = buf;
    m_buf = buf;
    m_size = memSize;
}

Ram::~Ram()
{
    if (!m_extBuf)
        delete[] m_buf;
}



void Ram::writeByte(int addr, uint8_t value)
{
    m_lastTag = m_tag;
    if (m_addrMask)
        addr &= m_addrMask;
    if (m_buf && addr < m_size)
        m_buf[addr] = value;
}



uint8_t Ram::readByte(int addr)
{
    m_lastTag = m_tag;
    if (m_addrMask)
        addr &= m_addrMask;
    if (m_buf && addr < m_size)
        return m_buf[addr];
    else
        return 0xFF;
}

// Rom implementation
#include "pico/vector_loader.rom.h"
#include "pico/apogey.rom.h"

Rom::Rom(unsigned memSize, string fileName)
{
    if (fileName == "vector/loader.rom") {
        m_buf = vector_loader_rom;
        m_size = sizeof(vector_loader_rom);
        return;
    }
    if (fileName == "apogey/apogey.rom") {
        m_buf = apogey_rom;
        m_size = sizeof(apogey_rom);
        return;
    }
    m_buf = new uint8_t [memSize];
    memset((uint8_t*)m_buf, 0xFF, memSize);
    m_size = memSize;
    if (palReadFromFile(fileName, 0, memSize, (uint8_t*)m_buf) == 0/*!= memSize*/) {
        delete[] m_buf;
        m_buf = nullptr;
        return;
    }
    b_ram = true;
}



Rom::~Rom()
{
    if (b_ram)
       delete[] m_buf;
}



uint8_t Rom::readByte(int addr)
{
    if (m_addrMask)
        addr &= m_addrMask;
    if (m_buf && addr < m_size)
        return m_buf[addr];
    else
        return 0xFF;
}
