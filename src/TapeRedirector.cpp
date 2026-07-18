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
#include "Vector.h"
#include "EmuWindow.h"

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
    if (m_isOpen)
        closeFile();

    m_fileName = palOpenFileDialog(
        m_rwMode == "w" ? "Save tape file" : "Open tape file",
        m_filter + "|.wav|.csw",
        m_rwMode == "w",
        m_machine->getWindow()
    );

    string ext;
    if (m_fileName.size() >= 4)
        ext = m_fileName.substr(m_fileName.size() - 4, 4);
    if (ext == ".wav" || ext == ".WAV" || ext == ".csw" || ext == ".CSW") {
        m_cancelled = true;
        if (m_rwMode == "r")
            g_emulation->getWavReader()->loadFile(m_fileName, this);
        else if (m_rwMode == "w") {
            m_wavWriter = new WavWriter(m_machine, m_fileName, ext == ".csw" || ext == ".CSW");
        }
        return;
    }

    ext = ext.substr(0, 4);
    m_lvt = (ext == ".lv" || ext == ".LV");
    m_tsx = (ext == ".tsx" || ext == ".TSX");

    if (m_fileName == "") {
        m_cancelled = true;
        return;
    }

    m_file.open(m_fileName, m_rwMode);
    m_isOpen = m_file.isOpen();

    m_cancelled = !m_isOpen;
    //m_read = false;

    m_bytesLeftInBlock = 0;

}


void TapeRedirector::closeFile()
{
    if (m_isOpen) {
        m_file.close();
        m_isOpen = false;
    }
    m_cancelled = false;
    //m_read = false;

    if (m_wavWriter) {
        delete m_wavWriter;
        m_wavWriter = nullptr;
    }

}


uint8_t TapeRedirector::readByte()
{
    if (m_tsx && m_isOpen && !m_bytesLeftInBlock)
        advanceToNextBlock();

    if (!m_isOpen && !m_cancelled)
    openFile();

    if (!m_isOpen)
        return 0;

    uint8_t buf = m_file.read8();
    if (isEof()) {
        closeFile();
        if (m_lvt) {
            switchToNextLvt();
        }
    }

    if (m_tsx)
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
    return isEof();
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
            //m_cancelled = false;
        }
    }
}


bool TapeRedirector::bytesAvailable(int n)
{
    if (!m_isOpen)
        return false;

    return ((m_file.getSize() - m_file.getPos()) >= n);
}


void TapeRedirector::skipBytes(int n)
{
    for (int i = 0; i < n; i++)
        m_file.read8();
}


void TapeRedirector::readBuffer(uint8_t* buf, int n)
{
    for (int i = 0; i < n; i++)
        *buf++ = m_file.read8();
}


static const uint8_t tsxHeader[8] = {0x5A, 0x58, 0x54, 0x61, 0x70, 0x65, 0x21, 0x1A};

void TapeRedirector::advanceToNextBlock()
{
    if (!m_tsx)
        return;

    if (m_bytesLeftInBlock) {
        skipBytes(m_bytesLeftInBlock);
        m_bytesLeftInBlock = 0;
    }

    if (isEof())
        return;

    while (!m_bytesLeftInBlock) {
        uint8_t blockId = m_file.read8();
        switch (blockId) {
        case 0x5A: {
            // TZX Header or Glue block
            if (!bytesAvailable(9)) {
                closeFile();
                return;
            }
            uint8_t buf[7];

            readBuffer(buf, 7);

            if (memcmp(buf, tsxHeader + 1, 6)) {
                closeFile();
                return;
            }
            skipBytes(2); // version: 2 bytes
            break;
        }
        case 0x32: {
            // Archive info block
            if (!bytesAvailable(2)) {
                closeFile();
                return;
            }
            uint16_t blockSize = m_file.read16();
            if (!bytesAvailable(blockSize)) {
                closeFile();
                return;
            }
            skipBytes(blockSize);
            break;
        }
        case 0x35: {
            // Custom info block
            if (!bytesAvailable(20)) {
                closeFile();
                return;
            }
            skipBytes(16);
            uint32_t blockSize = m_file.read32();
            if (!bytesAvailable(blockSize)) {
                closeFile();
                return;
            }
            skipBytes(blockSize);
            break;
        }
        case 0x30: {
            // Text description
            if (!bytesAvailable(1)) {
                closeFile();
                return;
            }
            uint8_t blockSize = m_file.read8();
            if (!bytesAvailable(blockSize)) {
                closeFile();
                return;
            }
            skipBytes(blockSize);
            break;
        }
        case 0x4B: {
            // Kansas City (MSX) block
            if (!bytesAvailable(4)) {
                closeFile();
                return;
            }
            uint32_t blockSize = m_file.read32();
            if (!bytesAvailable(blockSize)) {
                closeFile();
                return;
            }

            skipBytes(12);

            // todo: check last 2 bytes for bitmapped options
            //uint8_t buf[12];
            //readBuffer(buf, 12);

            m_bytesLeftInBlock = blockSize - 12;

            break;
        }
        default:
            // unknown block, close file
            closeFile();
            return;
        }
    }
}
