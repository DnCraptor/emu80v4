/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2016-2024
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
#include "WavReader.h"
#include "Specialist.h"
#include <strings.h>
#include <cstring>

#include "TapeRedirector.h"

using namespace std;

TapeRedirector::TapeRedirector()
{
    m_filter = "*";
}


TapeRedirector::~TapeRedirector()
{
    if (m_isOpen)
        closeFile();
}


void TapeRedirector::reset()
{
    closeFile();
}


void TapeRedirector::openFile()
{
    openFile(palOpenFileDialog(
        m_rwMode == "w" ? "Save tape file" : "Open tape file",
        m_filter + "|.wav|.csw", m_rwMode == "w"));
}


bool TapeRedirector::openFile(const std::string& path, int position)
{
    if (m_rwMode == "r" && g_emulation->getWavReader()->isPlaying())
        g_emulation->getWavReader()->stop();
    closeFile();
    m_fileName = path;
    const string ext = path.size() >= 4 ? path.substr(path.size() - 4) : string();
    const bool csw = strcasecmp(ext.c_str(), ".csw") == 0;
    if (csw || strcasecmp(ext.c_str(), ".wav") == 0) {
        m_cancelled = true;
        if (m_rwMode == "r")
            g_emulation->getWavReader()->loadFile(path, this);
        else if (m_machine->getWavWriter())
            m_machine->getWavWriter()->open(m_machine, path, csw);
        return false;
    }

    m_lvt = ext.compare(0, 3, ".lv") == 0 || ext.compare(0, 3, ".LV") == 0;
    m_tsx = strcasecmp(ext.c_str(), ".tsx") == 0;
    m_cas = strcasecmp(ext.c_str(), ".cas") == 0;
    m_bytesLeftInBlock = 0;
    if (!path.empty() && m_file.open(path, m_rwMode)) {
        m_file.seek(position);
        m_isOpen = m_file.getPos() == position;
        if (!m_isOpen)
            m_file.close();
    }
    m_cancelled = !m_isOpen;
    return m_isOpen;
}


void TapeRedirector::ejectFile()
{
    if (m_rwMode == "r" && g_emulation && g_emulation->getWavReader()->isPlaying())
        g_emulation->getWavReader()->stop();
    closeFile();
    m_fileName.clear();
}


void TapeRedirector::closeFile()
{
    if (m_isOpen) {
        m_file.close();
        m_isOpen = false;
    }
    m_cancelled = false;

    if (m_rwMode == "w" && m_machine && m_machine->getWavWriter())
        m_machine->getWavWriter()->close();

}


uint8_t TapeRedirector::readByte()
{
    if (!m_isOpen && !m_cancelled)
        openFile();
    if (m_tsx && m_isOpen && !m_bytesLeftInBlock)
        advanceToNextBlock();
    if (!m_isOpen || isEof())
        return 0;

    uint8_t buf = m_file.read8();
    if (isEof()) {
        closeFile();
        if (m_lvt) {
            switchToNextLvt();
        }
    }

    if (m_tsx && m_bytesLeftInBlock)
        --m_bytesLeftInBlock;


    return buf;
}


uint8_t TapeRedirector::peekByte()
{

    if (!m_isOpen && !m_cancelled)
        openFile();

    if (!m_isOpen)
        return 0;

    int64_t savedPos = m_file.getPos();
    uint8_t buf = m_file.read8();
    m_file.seek(savedPos);
    return buf;
}


void TapeRedirector::writeByte(uint8_t bt)
{
    if (!m_isOpen && !m_cancelled)
        openFile();

    if (m_isOpen) {
        m_file.write8(bt);
    }

}


int TapeRedirector::getPos()
{
    if (!m_isOpen)
        return 0;

    return m_file.getPos();
}


bool TapeRedirector::isEof()
{
    if (!m_isOpen)
        return true;

    return m_file.getPos() == m_file.getSize();
}


bool TapeRedirector::isOpen()
{
    return m_isOpen;
}


bool TapeRedirector::waitForSequence(const uint8_t* seq, int len)
{
    if (!m_isOpen && !m_cancelled)
        openFile();

    if (!m_isOpen)
        return false;

    int i = 0;
    while ((i < len) && !isEof()) {
        uint8_t inByte = readByte();
        if (inByte != seq[i++])
            i = 0;
    }
    return i == len;
}



bool TapeRedirector::isCancelled()
{
    return m_cancelled/* && !m_read*/;
}




void TapeRedirector::switchToNextLvt()
{
    if (m_isOpen)
        closeFile();

    if (m_fileName.size() >= 4) {
        string ext = m_fileName.substr(m_fileName.size() - 4, 4);
        if (ext.substr(0, 3) == ".lv" || ext.substr(0, 3) == ".LV") {
            char letter = ext[3];
            if (letter == 't' || letter == 'T')
                letter = '0';
            else
                letter += 1;
            m_fileName[m_fileName.size() - 1] = letter;
            m_file.open(m_fileName, m_rwMode);
            m_isOpen = m_file.isOpen();
        }
    }
}


bool TapeRedirector::nextTsxDataBlock(PalFile& file, int& end)
{
    uint8_t id;
    while (file.read(&id, 1) == 1) {
        uint8_t length[4] = {};
        unsigned field = 0;
        if (id == 0x4b) field = 4;
        else if (id == 0x30) field = 1;
        else if (id == 0x32) field = 2;
        else if (id == 0x5a) {
            constexpr uint8_t signature[] = {'X','T','a','p','e','!',0x1a};
            uint8_t glue[9];
            if (file.read(glue, sizeof(glue)) != sizeof(glue) ||
                memcmp(glue, signature, sizeof(signature)))
                return false;
            continue;
        } else if (id == 0x35) {
            if (file.getSize() - file.getPos() < 20) return false;
            file.skip(16);
            field = 4;
        } else return false;
        if (file.read(length, field) != field)
            return false;
        const uint32_t n = length[0] | (uint32_t(length[1]) << 8) |
            (uint32_t(length[2]) << 16) | (uint32_t(length[3]) << 24);
        if (n > file.getSize() - file.getPos() || file.getPos() + n > INT32_MAX)
            return false;
        if (id == 0x4b) {
            if (n <= 12) return false;
            end = file.getPos() + n;
            file.skip(12);
            return true;
        }
        file.skip(n);
    }
    return false;
}

void TapeRedirector::advanceToNextBlock()
{
    if (!m_tsx || !m_isOpen)
        return;
    if (m_bytesLeftInBlock) {
        m_file.skip(m_bytesLeftInBlock);
        m_bytesLeftInBlock = 0;
    }
    int end;
    if (nextTsxDataBlock(m_file, end))
        m_bytesLeftInBlock = end - m_file.getPos();
    else if (!isEof())
        closeFile();
}
