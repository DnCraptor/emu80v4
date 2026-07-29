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

#include <pico.h>
#include <string.h>

#include "Memory.h"
#include "Pal.h"
#if PSRAM
    #include "psram_spi.h"
#endif
#include "ff.h"

using namespace std;

// Ram implementation

static size_t sram_used = 0;
extern uint8_t* PSRAM_DATA;
extern uint32_t butter_psram_size();

static FIL f;
static bool sram_file_open = false;
static unsigned sram_object_count = 0;
static const char PAGEFILE[] = "/tmp/.korvet.pagefile";

SRam::SRam(unsigned memSize) : m_size(memSize), m_offset(sram_used)
{
/// TODO:    memset(m_buf, 0, memSize);
    sram_used += m_size;
    ++sram_object_count;
}


void SRam::init()
{
    // Все SRam используют один pagefile. Повторный init() другого объекта не
    // должен заново открывать и обнулять уже используемый файл.
    if (!sram_file_open) {
        sram_file_open =
            f_open(&f, PAGEFILE,
                   FA_READ | FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
    }
}

SRam::~SRam() {
    sram_used -= m_size; /// TODO: ensure order
    if (sram_object_count != 0)
        --sram_object_count;
    if (sram_object_count == 0 && sram_file_open) {
        f_close(&f);
        sram_file_open = false;
    }
}

void __not_in_flash_func(SRam::writeByte)(int addr, uint8_t value) {
    if (addr < 0 || addr >= m_size)
        return;

    const size_t off = m_offset + static_cast<size_t>(addr);
    if (butter_psram_size() > off) {
        PSRAM_DATA[off] = value;
        return;
    }
#if PSRAM
    if (psram_size() > off) {
        write8psram(off, value);
        return;
    }
#endif
    // Как и в writeBlock: файл подкачки мог не открыться (напр., каталога нет).
    // Без этой проверки f_lseek/f_write идут по неоткрытому FIL -> hardfault.
    if (!sram_file_open)
        init();
    if (!sram_file_open)
        return;
    FSIZE_t lba = m_offset + addr;
    if (lba != f_tell(&f)) f_lseek(&f, lba);
    UINT br;
    f_write(&f, &value, 1, &br);
}

uint8_t __not_in_flash_func(SRam::readByte)(int addr) {
    if (addr < 0 || addr >= m_size)
        return 0xFF;

    const size_t off = m_offset + static_cast<size_t>(addr);
    if (butter_psram_size() > off) {
        return PSRAM_DATA[off];
    }
#if PSRAM
    if (psram_size() > off) {
        return read8psram(off);
    }
#endif
    // Симметрично readBlock: если файл подкачки не открыт, не трогаем FIL.
    if (!sram_file_open)
        init();
    if (!sram_file_open)
        return 0xFF;
    FSIZE_t lba = m_offset + addr;
    if (lba != f_tell(&f)) f_lseek(&f, lba);
    uint8_t value;
    UINT br;
    f_read(&f, &value, 1, &br);
    return value;

}

void __not_in_flash_func(SRam::writeBlock)(
        int addr, const uint8_t* data, int size)
{
    if (!data || size <= 0 || addr < 0 || addr >= m_size)
        return;
    if (size > m_size - addr)
        size = m_size - addr;

    const size_t off = m_offset + (size_t)addr;
    if (butter_psram_size() >= off + (size_t)size) {
        memcpy(PSRAM_DATA + off, data, (size_t)size);
        return;
    }
#if PSRAM
    if (psram_size() >= off + (size_t)size) {
        for (int i = 0; i < size; ++i)
            write8psram(off + (size_t)i, data[i]);
        return;
    }
#endif
    if (!sram_file_open)
        init();
    if (!sram_file_open)
        return;

    UINT written = 0;
    FSIZE_t lba = off;
    if (lba != f_tell(&f)) f_lseek(&f, lba);
    f_write(&f, data, (UINT)size, &written);
}

void __not_in_flash_func(SRam::readBlock)(
        int addr, uint8_t* data, int size)
{
    if (!data || size <= 0 || addr < 0 || addr >= m_size)
        return;
    if (size > m_size - addr)
        size = m_size - addr;

    const size_t off = m_offset + (size_t)addr;
    if (butter_psram_size() >= off + (size_t)size) {
        memcpy(data, PSRAM_DATA + off, (size_t)size);
        return;
    }
#if PSRAM
    if (psram_size() >= off + (size_t)size) {
        for (int i = 0; i < size; ++i)
            data[i] = read8psram(off + (size_t)i);
        return;
    }
#endif
    if (!sram_file_open)
        init();
    if (!sram_file_open) {
        memset(data, 0, (size_t)size);
        return;
    }

    UINT read = 0;
    FSIZE_t lba = off;
    if (lba != f_tell(&f)) f_lseek(&f, lba);
    f_read(&f, data, (UINT)size, &read);
    if (read < (UINT)size)
        memset(data + read, 0, (size_t)size - read);
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
#include "pico/korvet_loader.rom.h"
#include "pico/korvet_rom1.bin.h"
#include "pico/korvet_rom2.bin.h"
#include "pico/korvet_rom3.bin.h"

Rom::Rom(unsigned memSize, const string& fileName)
{
    m_capacity = memSize;
    if (fileName == "vector/loader.rom") {
        // Встроенный образ: указатель и размер известны сразу, чтения нет
        m_buf = korvet_loader_rom;
        m_size = sizeof(korvet_loader_rom);
        return;
    } else if (fileName == "korvet/rom1.bin") {
        m_buf = korvet_rom1_bin;
        m_size = sizeof(korvet_rom1_bin);
        return;
    } else if (fileName == "korvet/rom2.bin") {
        m_buf = korvet_rom2_bin;
        m_size = sizeof(korvet_rom2_bin);
        return;
    } else if (fileName == "korvet/rom3.bin") {
        m_buf = korvet_rom3_bin;
        m_size = sizeof(korvet_rom3_bin);
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



bool Rom::loadFile(const string& fileName)
{
    uint8_t* data = new uint8_t [m_capacity];
    memset(data, 0xFF, m_capacity);
    if (palReadFromFile(fileName, 0, m_capacity, data, false) == 0) {
        delete[] data;
        return false;
    }
    if (b_ram)
        delete[] m_buf;
    m_buf = data;
    m_size = m_capacity;
    m_fileName = fileName;
    b_ram = true;
    return true;
}

void Rom::useBuiltIn()
{
    if (b_ram)
        delete[] m_buf;
    m_buf = korvet_loader_rom;
    m_size = sizeof(korvet_loader_rom);
    m_fileName.clear();
    b_ram = false;
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
