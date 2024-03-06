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
#include "ff.h"

extern "C" {
    #include "psram_spi.h"
}
#include <list>
#include <utility>

using namespace std;

// Ram implementation

Ram::Ram(unsigned memSize) {
    m_supportsTags = true;
    m_off = psram_alloc(memSize);
    m_size = memSize;
}


Ram::~Ram() {
    if (m_off != -1)
        psram_free(m_off);
}


void Ram::writeByte(int addr, uint8_t value)
{
    m_lastTag = m_tag;
    if (m_addrMask)
        addr &= m_addrMask;
    if (m_off != -1 && addr < m_size)
        write8psram(addr, value);
}

uint8_t Ram::readByte(int addr) {
    m_lastTag = m_tag;
    if (m_addrMask)
        addr &= m_addrMask;
    if (m_off != -1 && addr < m_size)
        return read8psram(addr);
    else
        return 0xFF;
}

static int readFromFile(const string& fileName, int offset, int sizeToRead, int psram_off) {
    string fullFileName = palMakeFullFileName(fileName);
    lprintf("readFromFile([%s], offset: %d, sizeToRead: %d, psram_off: %08Xh)", fullFileName.c_str(), offset, sizeToRead, psram_off);
    UINT nBytesRead = 0;
    FIL file;
    if (f_open(&file, fullFileName.c_str(), FA_READ) == FR_OK) {
        f_lseek(&file, offset);
        UINT rdo;
        uint8_t buffer[512];
        do {
            UINT rd = sizeToRead > 512 ? 512 : sizeToRead;
            f_read(&file, buffer, rd, &rdo);
            if (rd != rdo) break;
            for (int i = 0; i < rdo; i += 4) {
                write32psram(psram_off + i, buffer[i]);
            }
            nBytesRead += rdo;
            sizeToRead -= 512;
            psram_off += 512;
        } while (sizeToRead > 0);
        f_close(&file);
        return nBytesRead;
    }
    else
        return 0;
}

// Rom implementation
Rom::Rom(unsigned memSize, string fileName) {
    m_off = psram_alloc(memSize, 0xFFFFFFFF);
    m_size = memSize;
    if (readFromFile(fileName, 0, memSize, m_off) == 0) {
        psram_free(m_off);
        m_off = -1;
    }
}

Rom::~Rom() {
    if (m_off != -1)
        psram_free(m_off);
}

uint8_t Rom::readByte(int addr) {
    if (m_addrMask)
        addr &= m_addrMask;
    if (m_off != -1 && addr < m_size)
        return read8psram(addr);
    else
        return 0xFF;
}

using namespace std;

static list< pair<size_t, size_t> > psram_chunks;

inline static size_t wrap_addr32(size_t _off) {
    if ((_off % 4) != 0) { // use 32-bit wrapped chunks only
        return ((_off >> 2) << 2) + 4;
    }
    return _off;
}

#ifndef MAX_PSRAM_MB
#define MAX_PSRAM_MB 4
#endif

static const size_t psram_end = 0x100000 * MAX_PSRAM_MB;

size_t psram_alloc(size_t sz, uint32_t init_val) {
    size_t off = 0;
    if (psram_chunks.size() == 0) {
        psram_chunks.push_back(pair(off, sz));
    } else {
        pair<size_t, size_t>& t = psram_chunks.back();
        register auto _off = wrap_addr32(t.first + t.second);
        if (_off + sz < psram_end) {
            off = _off;
            psram_chunks.push_back(pair(off, sz));
        } else {
            pair<size_t, size_t> prev = psram_chunks.front();
            _off = wrap_addr32(prev.first + prev.second);
            for (auto it = psram_chunks.begin(); it != psram_chunks.end(); it++) {
                pair<size_t, size_t>& t = *it;
                if (_off + sz <= t.first) {
                    off = _off;
                    psram_chunks.insert(it, pair(off, sz));
                    break;
                }
                prev = t;
                _off = wrap_addr32(prev.first + prev.second);
            }
            lprintf("psram_alloc(%d) FAILED", sz);
            return 0; // TODO: thorow
        }
    }
    for (size_t i = 0; i < sz; i += 4) {
        write32psram(off + i, init_val);
    }
    lprintf("psram_alloc(%d): %08Xh", sz, off);
    return off;
}

void psram_free(size_t off) {
    for (auto it = psram_chunks.rbegin(); it != psram_chunks.rend(); it++) {
        if (it->first == off) {
            size_t sz = it->second;
            psram_chunks.erase(std::next(it).base());
            lprintf("psram_free(%08Xh) released %d", off, sz);
            return;
        }
    }
    lprintf("psram_free(%08Xh) FAILED", off);
}
