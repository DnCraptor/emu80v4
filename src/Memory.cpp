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
static unsigned sram_count = 0;
static FIL f;
static bool pagefileOpen = false;
static const char PAGEFILE[] = "/emu80/.orion.pagefile";

SRam::SRam(unsigned memSize) : m_size(memSize), m_offset(sram_used)
{
    m_supportsTags = true;
    sram_used += m_size;
    ++sram_count;
    if (butter_psram_size() >= sram_used) {
        for (size_t i = m_offset; i < sram_used; ++i)
            PSRAM_DATA[i] = 0;
    } else if (psram_size() >= sram_used) {
        for (size_t i = m_offset; i < sram_used; ++i)
            write8psram(i, 0);
    } else {
        if (!pagefileOpen)
            pagefileOpen = f_open(&f, PAGEFILE, FA_READ | FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
        if (pagefileOpen) {
            static const uint8_t zero[512] = {};
            UINT bw;
            if (f_lseek(&f, m_offset) == FR_OK) {
                for (unsigned i = 0; i < memSize; i += sizeof(zero)) {
                    unsigned n = memSize - i < sizeof(zero) ? memSize - i : sizeof(zero);
                    if (f_write(&f, zero, n, &bw) != FR_OK || bw != n) break;
                }
                f_sync(&f);
            }
        }
    }
}

void SRam::shutdown() {
    if (pagefileOpen) f_sync(&f);
}

SRam::~SRam() {
    if (--sram_count == 0) {
        if (pagefileOpen) f_close(&f);
        pagefileOpen = false;
        sram_used = 0;
    }
}

void __not_in_flash_func(SRam::writeByte)(int addr, uint8_t value) {
    if (addr < 0 || addr >= m_size) return;
    size_t off = m_offset + addr;
    m_lastTag = m_tag;
    if (butter_psram_size() > off) { PSRAM_DATA[off] = value; return; }
    if (psram_size() > off) { write8psram(off, value); return; }
    UINT bw;
    if (pagefileOpen && (f_tell(&f) == off || f_lseek(&f, off) == FR_OK))
        f_write(&f, &value, 1, &bw);
}

uint8_t __not_in_flash_func(SRam::readByte)(int addr) {
    if (addr < 0 || addr >= m_size) return 0xFF;
    size_t off = m_offset + addr;
    m_lastTag = m_tag;
    if (butter_psram_size() > off) return PSRAM_DATA[off];
    if (psram_size() > off) return read8psram(off);
    uint8_t value;
    UINT br;
    if (pagefileOpen && (f_tell(&f) == off || f_lseek(&f, off) == FR_OK) &&
        f_read(&f, &value, 1, &br) == FR_OK && br == 1) return value;
    return 0xFF;
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



void __not_in_flash_func(Ram::writeByte)(int addr, uint8_t value)
{
    m_lastTag = m_tag;
    if (m_addrMask)
        addr &= m_addrMask;
    if (m_buf && addr < m_size)
        m_buf[addr] = value;
}



uint8_t __not_in_flash_func(Ram::readByte)(int addr)
{
    m_lastTag = m_tag;
    if (m_addrMask)
        addr &= m_addrMask;
    if (m_buf && addr < m_size)
        return m_buf[addr];
    else
        return 0xFF;
}

// Orion monitor ROMs
#include "pico/orion_m1rk_bin.h"
#include "pico/orion_m2rk_bin.h"
#include "pico/orion_m31rk_bin.h"
#include "pico/orion_m32zrk_bin.h"

Rom::Rom(unsigned memSize, string fileName)
{
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
