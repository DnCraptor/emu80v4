/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2017-2024
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

#include "Pal.h"
#include "Globals.h"
#include "Emulation.h"
#include "Vector.h"
#include "Cpu.h"
#include "EmuWindow.h"
#include "TapeRedirector.h"
#include "FileLoader.h"


using namespace std;


void FileLoader::attachAddrSpace(AddressableDevice* as)
{
    m_as = as;
}


void FileLoader::attachTapeRedirector(TapeRedirector* tapeRedirector)
{
    m_tapeRedirector = tapeRedirector;
}


void FileLoader::setFilter(const std::string& filter)
{
    m_filter = filter;
}


bool FileLoader::chooseAndLoadFile(bool run)
{
    string fileName = palOpenFileDialog("Open file", m_filter, false, m_machine->getWindow());
    g_emulation->restoreFocus();
    if (fileName == "")
        return true;
    if (!loadFile(fileName, run)) {
        emuLog << "Error loading file: " << fileName << "\n";
        return false;
    }
    m_lastFile = fileName;
    return true;
}




bool RkFileLoader::loadFile(const std::string& fileName, bool run)
{
    int fileSize;
    uint8_t* buf = palReadFile(fileName, fileSize, false);
    if (!buf)
        return false;

    int fullSize = fileSize;

    if (fileSize < 9) {
        delete[] buf;
        return false;
    }

    uint8_t* ptr = buf;

    if ((*ptr) == 0xE6) {
        ptr++;
        fileSize--;
    }

    uint16_t begAddr = (ptr[0] << 8) | ptr[1];
    uint16_t endAddr = (ptr[2] << 8) | ptr[3];
    ptr += 4;
    fileSize -= 4;

    uint16_t progLen = endAddr - begAddr + 1;

    if (begAddr == 0xE6E6 || begAddr == 0xD3D3 || fileSize < progLen + 2) {
        // Basic or EDM File
        delete[] buf;
        return false;
    }

    Cpu* bc = m_machine->getCpu();
    Cpu8080Compatible* cpu = nullptr;
    if (bc) {
        cpu = bc->asCpu8080Compatible();
        if (run && cpu) {
            m_machine->reset();
            bc = m_machine->getCpu();
            if (bc) {
                if (cpu = bc->asCpu8080Compatible()) {
                    cpu->disableHooks();
                    g_emulation->exec((int64_t)cpu->getKDiv() * m_skipTicks, true);
                }
            }
            afterReset();
        }
    }

    for (unsigned addr = begAddr; addr <= endAddr; addr++)
        m_as->writeByte(addr, *ptr++);

    fileSize -= (endAddr - begAddr + 1);

    // skip CS
    while (fileSize >0 && (*ptr) != 0xE6) {
        ++ptr;
        --fileSize;
    }
    if (fileSize > 3) {
        fileSize -= 3;
        ptr += 3;
    } else
        fileSize = 0;

    // Find next block
    if (m_allowMultiblock && m_tapeRedirector && fileSize > 0)
        while (fileSize > 0 && (*ptr) != 0xE6) {
            ++ptr;
            --fileSize;
        }
    //if (fileSize > 0)
    //    --fileSize;

    delete[] buf;

    if (run && cpu) {
        cpu->enableHooks();
        cpu->setPC(begAddr);
        if (m_allowMultiblock && m_tapeRedirector && fileSize > 0) {
            m_tapeRedirector->assignFile(fileName, "r");
            m_tapeRedirector->openFile();
            m_tapeRedirector->assignFile("", "r");
            m_tapeRedirector->setFilePos(fullSize - fileSize);
        }
    }

    return true;
}


static const uint8_t casHeader[8] = {0x1F, 0xA6, 0xDE, 0xBA, 0xCC, 0x13, 0x7D, 0x74};
static const uint8_t tsxHeader[8] = {0x5A, 0x58, 0x54, 0x61, 0x70, 0x65, 0x21, 0x1A};


MsxFileParser::MsxFileParser(uint8_t* data, int size) :
    m_data(data), m_size(size)
{
    if (m_size < 8)
        m_format = Format::MF_UNKNOWN;
    else if (!memcmp(data, casHeader, 8))
        m_format = Format::MF_CAS;
    else if (!memcmp(data, tsxHeader, 8))
        m_format = Format::MF_TSX;
    else
        m_format = Format::MF_UNKNOWN;
}


bool MsxFileParser::getNextBlock(int& pos, int& size)
{
    // returns false if no block found
    // pos - block offset
    // size - maximum block size, for cas may be greater than real block size

    // default return values if no next block
    pos = m_size;
    size = 0;

    switch (m_format) {

    case Format::MF_UNKNOWN:
        pos = m_curPos;
        size = m_size - m_curPos;
        return size != 0;

    case Format::MF_CAS: {
        m_curPos = (m_curPos + 7) & ~7;
        if (m_size - m_curPos < 8)
            return false;
        while (memcmp(m_data + m_curPos, casHeader, 8)) {
            m_curPos += 8;
            if (m_size - m_curPos < 8)
                return false;
        }
        m_curPos += 8;
        pos = m_curPos;

        int nextPos = pos;
        while (memcmp(m_data + nextPos, casHeader, 8)) {
            nextPos += 8;
            if (m_size - nextPos < 8) {
                size = m_size - m_curPos;
                m_curPos = m_size;
                return true;
            }
        }

        size = nextPos - pos;
        m_curPos = nextPos;
        return true; }

    case Format::MF_TSX:
        int curPos = m_nextTsxBlockPos;
        while(true) {
            if (curPos >= m_size)
                return false;
            uint8_t blockId = m_data[curPos];
            switch (blockId) {
            case 0x5A: {
                // TZX Header or Glue block
                if (m_size - curPos < 10)
                    return false;
                if (memcmp(m_data + curPos, tsxHeader, 8))
                    return false;
                curPos += 10;
                break;
            }
            case 0x32: {
                // Archive info block
                if (m_size - curPos < 3)
                    return false;
                uint16_t blockSize = m_data[curPos + 1] + (m_data[curPos + 2] << 8);
                curPos += 3;
                if (m_size - curPos < blockSize)
                    return false;
                curPos += blockSize;
                break;
            }
            case 0x35: {
                // Custom info block
                if (m_size - curPos < 21)
                    return false;
                uint32_t blockSize = m_data[curPos + 17] + (m_data[curPos + 18] << 8) + (m_data[curPos + 19] << 16) + (m_data[curPos + 20] << 24);
                curPos += 21;
                if (m_size - curPos < blockSize)
                    return false;
                curPos += blockSize;
                break;
            }
            case 0x30: {
                // Text description
                if (m_size - curPos < 2)
                    return false;
                uint32_t blockSize = m_data[curPos + 1];
                curPos += 2;
                if (m_size - curPos < blockSize)
                    return false;
                curPos += blockSize;
                break;
            }
            case 0x4B: {
                // Kansas City (MSX) block
                if (m_size - curPos < 5)
                    return false;
                uint32_t blockSize = m_data[curPos + 1] + (m_data[curPos + 2] << 8) + (m_data[curPos + 3] << 16) + (m_data[curPos + 4] << 24);
                curPos += 5;
                if (m_size - curPos < blockSize)
                    return false;
                pos = curPos + 12;
                size = blockSize - 12;
                m_nextTsxBlockPos = curPos + blockSize;
                return true;
            }
            }
        }
        return false; // this should never occur
    }
}
