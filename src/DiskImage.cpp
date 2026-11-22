/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2017-2023
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
#include "DiskImage.h"
#include "Emulation.h"

using namespace std;

DiskImage::DiskImage()
{
    //
}


DiskImage::~DiskImage()
{
    if (m_file.isOpen())
        m_file.close();
}


bool DiskImage::assignFileName(const string& fileName)
{
    if (m_file.isOpen() && fileName == m_fileName)
        return true;

    m_file.close();

    if (!fileName.empty()) {
        m_fileName = palMakeFullFileName(fileName);
        m_file.open(m_fileName.c_str(), m_isWriteProtected ? "r" : "r+");
    }

    if (!m_file.isOpen())
        m_fileName = "";

    reset();

    if (m_owner)
        m_owner->diskImageChanged(this, m_file.isOpen());

    return m_file.isOpen();
}


bool DiskImage::assignFileName(const string& fileName, bool isWriteProtected)
{
    const string fullFileName = fileName.empty() ? string() : palMakeFullFileName(fileName);
    if (m_file.isOpen() && fullFileName == m_fileName) {
        setWriteProtection(isWriteProtected);
        return true;
    }

    m_isWriteProtected = isWriteProtected;
    return assignFileName(fullFileName);
}


string DiskImage::chooseFileName(const string& title, bool* readOnly)
{
    return palOpenFileDialog(title, m_filter, false, readOnly);
}


void DiskImage::chooseFile(const string& title)
{
    string fileName = chooseFileName(title);
    if (fileName != "")
        assignFileName(fileName);
}


void DiskImage::close()
{
    if (m_file.isOpen())
        m_file.close();
}


int64_t DiskImage::getSize()
{
    return m_file.getSize();
}


void DiskImage::setWriteProtection(bool isWriteProtected)
{
    bool reopen = m_file.isOpen() && (m_isWriteProtected != isWriteProtected);

    m_isWriteProtected = isWriteProtected;

    if (reopen) {
        int pos = m_file.getPos();
        m_file.close();
        assignFileName(m_fileName);
        if (m_file.isOpen())
            m_file.seek(pos);
    }
}


bool DiskImage::getWriteProtectStatus()
{
    return m_isWriteProtected;
}


void DiskImage::setCurOffset(int offset)
{
    m_file.seek(offset);
}


bool DiskImage::read(uint8_t* buf, int len)
{
    if (!m_file.isOpen())
        return false;
    return m_file.read(buf, static_cast<UINT>(len)) == static_cast<UINT>(len);
}


bool DiskImage::write(uint8_t* buf, int len)
{
    if (!m_file.isOpen() || m_isWriteProtected)
        return false;
    return m_file.write(buf, static_cast<UINT>(len)) == static_cast<UINT>(len);
}


uint8_t DiskImage::read8()
{
    if (!m_file.isOpen())
        return 0;
    return m_file.read8();
}




FdImage::FdImage(int nTracks, int nHeads, int nSectors, int sectorSize)
{
    m_nTracks = nTracks;
    m_nHeads = nHeads;
    m_nSectors = nSectors;
    m_sectorSize = sectorSize;
    m_isWriteProtected = false;

    m_curTrack = 0;
    m_curHead = 0;
    m_curSector = 0;
    m_curSectorOffset = m_sectorSize; // >= sectorSize
}


void FdImage::reset()
{
    m_curTrack = 0;
    m_curHead = 0;
    m_curSector = 0;
    m_curSectorOffset = m_sectorSize; // >= sectorSize
}


uint8_t FdImage::readNextByte()
{
    if (!getReadyStatus() || (m_curSectorOffset == 0 && !seek(0)))
        return 0;
    ++m_curSectorOffset;
    return m_file.read8();
}


uint8_t FdImage::readByte(int offset)
{
    if (!m_file.isOpen() || !seek(offset))
        return 0;
    return m_file.read8();
}

void FdImage::writeNextByte(uint8_t bt)
{
    if (m_isWriteProtected || !getReadyStatus() || (m_curSectorOffset == 0 && !seek(0)))
        return;
    m_file.write8(bt);
    ++m_curSectorOffset;
}


void FdImage::writeByte(int offset, uint8_t bt)
{
    if (!m_file.isOpen() || m_isWriteProtected || !seek(offset))
        return;
    m_file.write8(bt);
}


bool FdImage::getReadyStatus()
{
    return (m_file.isOpen()) && (m_curSectorOffset < m_sectorSize);
}


void FdImage::setCurTrack(int track)
{
    m_curTrack = track;
}


void FdImage::setCurHead(int head)
{
    m_curHead = head;
}


void FdImage::startSectorAccess(int sector)
{
    m_curSector = sector;
    const bool valid = m_curTrack >= 0 && m_curTrack < m_nTracks &&
        m_curHead >= 0 && m_curHead < m_nHeads && sector >= 0 && sector < m_nSectors;
    m_curSectorOffset = valid ? 0 : m_sectorSize;
}


int FdImage::readSectorAddress()
{
    m_curSector = (m_curSector + 1) % m_nSectors;
    return m_curSector;
}


bool FdImage::seek(int offset)
{
    if (m_curTrack < 0 || m_curTrack >= m_nTracks || m_curHead < 0 ||
        m_curHead >= m_nHeads || m_curSector < 0 || m_curSector >= m_nSectors ||
        offset < 0 || offset >= m_sectorSize)
        return false;
    int ofs = offset + (m_curSector + (m_curHead + m_curTrack * m_nHeads) * m_nSectors) * m_sectorSize;
    m_file.seek(ofs);
    m_curSectorOffset = offset;
    return m_file.getPos() == ofs;
}
