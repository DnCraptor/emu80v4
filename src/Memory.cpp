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
/// TODO:    memset(m_buf, 0, memSize);
    sram_used += m_size;
}


void SRam::init()
{
    // Файловая система готова только к моменту init(), в конструкторе её может
    // ещё не быть
    f_open(&f, PAGEFILE, FA_READ | FA_WRITE | FA_CREATE_ALWAYS);
}

SRam::~SRam() {
    sram_used -= m_size; /// TODO: ensure order
    f_close(&f);
}

void __not_in_flash_func(SRam::writeByte)(int addr, uint8_t value) {
    size_t off = m_offset + addr;
    if (butter_psram_size() > off) {
        PSRAM_DATA[off] = value;
        return;
    }
    if (psram_size() > off) {
        write8psram(off, value);
        return;
    }
    UINT br;
    FSIZE_t lba = m_offset;
    f_lseek(&f, lba + addr);
    f_write(&f, &value, 1, &br);
}

uint8_t __not_in_flash_func(SRam::readByte)(int addr) {
    size_t off = m_offset + addr;
    if (butter_psram_size() > off) {
        return PSRAM_DATA[off];
    }
    if (psram_size() > off) {
        return read8psram(off);
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
    m_extBuf = nullptr;
    m_buf = new uint8_t [memSize];
    memset(m_buf, 0, memSize);
    m_size = memSize;
}

Ram::Ram(uint8_t* buf, unsigned memSize)
{
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
    if (m_buf && addr < m_size)
        m_buf[addr] = value;
}



uint8_t __not_in_flash_func(Ram::readByte)(int addr)
{
    if (m_buf && addr < m_size)
        return m_buf[addr];
    else
        return 0xFF;
}

// Rom implementation
#include "pico/vector_loader.rom.h"

Rom::Rom(unsigned memSize, const string& fileName)
{
    if (fileName == "vector/loader.rom") {
        // Встроенный образ: указатель и размер известны сразу, чтения нет
        m_buf = vector_loader_rom;
        m_size = sizeof(vector_loader_rom);
        return;
    }

    // Размер выставляется здесь, потому что getSize() нужен уже при построении
    // карты страниц в attachRom(). Выделение памяти и чтение файла — в init().
    m_size = memSize;
    m_fileName = fileName;
}


void Rom::init()
{
    if (m_buf || m_fileName.empty())
        return;   // встроенный образ либо уже загружено

    m_buf = new uint8_t [m_size];
    memset((uint8_t*)m_buf, 0xFF, m_size);
    if (palReadFromFile(m_fileName, 0, m_size, (uint8_t*)m_buf) == 0/*!= m_size*/) {
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
    if (m_buf && addr < m_size)
        return m_buf[addr];
    else
        return 0xFF;
}
