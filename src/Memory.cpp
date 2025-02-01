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
#include "ff.h"

using namespace std;

// Ram implementation

static size_t sram_used = 0;

static FIL f;
static const char PAGEFILE[] = "/emu80/m1p2-emu80.pagefile";

SRam::SRam(unsigned memSize) : m_size(memSize), m_offset(sram_used)
{
    m_supportsTags = true;
/// TODO:    memset(m_buf, 0, memSize);
    sram_used += m_size;
    f_open(&f, PAGEFILE, FA_READ | FA_WRITE | FA_CREATE_ALWAYS);
}

SRam::~SRam() {
    sram_used -= m_size; /// TODO: ensure order
    f_close(&f);
}

void SRam::writeByte(int addr, uint8_t value) {
    if (psram_size() > m_offset + addr) {
        write8psram(m_offset + addr, value);
        return;
    }
    UINT br;
    FSIZE_t lba = m_offset;
    f_lseek(&f, lba + addr);
    f_write(&f, &value, 1, &br);
}

uint8_t SRam::readByte(int addr) {
    if (psram_size() > m_offset + addr) {
        return read8psram(m_offset + addr);
    }
    UINT br;
    FSIZE_t lba = m_offset;
    f_lseek(&f, lba + addr);
    uint8_t value;
    f_read(&f, &value, 1, &br);
    return value;
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
#include "pico/bashkiria_bios1_rom.h"
#include "pico/korvet_rom1_bin.h"
#include "pico/korvet_rom2_bin.h"
#include "pico/korvet_rom3_bin.h"
#if ORION
#include "pico/orion_m1rk_bin.h"
#include "pico/orion_m2rk_bin.h"
#include "pico/orion_m31rk_bin.h"
#include "pico/orion_m32zrk_bin.h"
#endif
#if PK8000
#include "pico/pk8000_fdc.rom.h"
#include "pico/pk8000_hdd.rom.h"
#include "pico/pk8000_v12.rom.h"
#endif
#include "pico/ut88.rom.h"
#if PK86
#include "pico/rk86_dos29_bin.h"
#include "pico/rk86_rom.h"
#endif
#include "pico/partner_romp1_bin.h"
#include "pico/partner_romp2_bin.h"
#include "pico/partner_mcpgrom_bin.h"
#include "pico/partner_fddrom_bin.h"
#include "pico/partner_sdrom_bin.h"

#include "pico/kr04_rom0.bin.h"
#include "pico/kr04_rom1.bin.h"
#include "pico/kr04_rom2.bin.h"

Rom::Rom(unsigned memSize, string fileName)
{
    if (fileName == "kr04/rom0.bin") {
        m_buf = kr04_rom0_bin;
        m_size = sizeof(kr04_rom0_bin);
        return;
    }
    if (fileName == "kr04/rom1.bin") {
        m_buf = kr04_rom1_bin;
        m_size = sizeof(kr04_rom1_bin);
        return;
    }
    if (fileName == "kr04/rom2.bin") {
        m_buf = kr04_rom2_bin;
        m_size = sizeof(kr04_rom2_bin);
        return;
    }
    if (fileName == "partner/romp1.bin") {
        m_buf = partner_romp1_bin;
        m_size = sizeof(partner_romp1_bin);
        return;
    }
    if (fileName == "partner/romp2.bin") {
        m_buf = partner_romp2_bin;
        m_size = sizeof(partner_romp2_bin);
        return;
    }
    if (fileName == "partner/mcpgrom.bin") {
        m_buf = partner_mcpgrom_bin;
        m_size = sizeof(partner_mcpgrom_bin);
        return;
    }
    if (fileName == "partner/fddrom.bin") {
        m_buf = partner_fddrom_bin;
        m_size = sizeof(partner_fddrom_bin);
        return;
    }
    if (fileName == "partner/fddrom.bin") {
        m_buf = partner_fddrom_bin;
        m_size = sizeof(partner_fddrom_bin);
        return;
    }
    if (fileName == "partner/sdrom.bin") {
        m_buf = partner_sdrom_bin;
        m_size = sizeof(partner_sdrom_bin);
        return;
    }
    if (fileName == "apogey/apogey.rom") {
        m_buf = apogey_rom;
        m_size = sizeof(apogey_rom);
        return;
    }
    if (fileName == "bashkiria/bios1.rom") {
        m_buf = bashkiria_bios1_rom;
        m_size = sizeof(bashkiria_bios1_rom);
        return;
    }
    if (fileName == "korvet/rom1.bin") {
        m_buf = korvet_rom1_bin;
        m_size = sizeof(korvet_rom1_bin);
        return;
    }
    if (fileName == "korvet/rom2.bin") {
        m_buf = korvet_rom2_bin;
        m_size = sizeof(korvet_rom2_bin);
        return;
    }
    if (fileName == "korvet/rom3.bin") {
        m_buf = korvet_rom3_bin;
        m_size = sizeof(korvet_rom3_bin);
        return;
    }
    if (fileName == "ut88/ut88.rom") {
        m_buf = ut88_rom;
        m_size = sizeof(ut88_rom);
        return;
    }
    #if ORION
    if (fileName == "orion/rom/m1rk.bin") {
        m_buf = orion_m1rk_bin;
        m_size = sizeof(orion_m1rk_bin);
        return;
    }
    if (fileName == "orion/rom/m2rk.bin") {
        m_buf = orion_m2rk_bin;
        m_size = sizeof(orion_m2rk_bin);
        return;
    }
    if (fileName == "orion/rom/m31rk.bin") {
        m_buf = orion_m31rk_bin;
        m_size = sizeof(orion_m31rk_bin);
        return;
    }
    if (fileName == "orion/rom/m32zrk.bin") {
        m_buf = orion_m32zrk_bin;
        m_size = sizeof(orion_m32zrk_bin);
        return;
    }
    #endif
    #if PK8000
    if (fileName == "pk8000/pk8000_v12.rom") {
        m_buf = pk8000_v12_rom;
        m_size = sizeof(pk8000_v12_rom);
        return;
    }
    if (fileName == "pk8000/pk8000_fdc.rom") {
        m_buf = pk8000_fdc_rom;
        m_size = sizeof(pk8000_fdc_rom);
        return;
    }
    if (fileName == "pk8000/pk8000_hdd.rom") {
        m_buf = pk8000_hdd_rom;
        m_size = sizeof(pk8000_hdd_rom);
        return;
    }
    #endif
    #if PK86
    if (fileName == "rk86/rk86.rom") {
        m_buf = rk86_rom;
        m_size = sizeof(rk86_rom);
        return;
    }
    if (fileName == "rk86/dos29.bin") {
        m_buf = dos29_bin;
        m_size = sizeof(dos29_bin);
        return;
    }
    #endif
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
