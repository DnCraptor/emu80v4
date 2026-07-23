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

// Fdc1793.cpp
// Реализация контроллера НГМД КР580ВГ93 (FDC1793)


#include "Globals.h"
#include "Fdc1793.h"
#include "DiskImage.h"
#include "Emulation.h"
#include "Vector.h"

using namespace std;

Fdc1793::Fdc1793()
{
    m_accessMode = FAM_WAITING;
    m_disk = 0; // номер дисковода
    m_head = 0; // номер головки
    m_track = 0; // регистр дорожки
    m_sector = 1; // регистр сектора
    m_data = 0; // регистр данных
    m_status = 0; // регистр статусаs
    m_directionIn = true; // направление движения true=in, false=out

    m_cmdTime = 0;
    m_irq = false;
    m_lastCommand = 0;
    m_addressIdCnt = 0;
    for (unsigned i = 0; i < sizeof(m_addressId); ++i)
        m_addressId[i] = 0;
    m_writeTrackCnt = 0;
    m_wrTrackState = WTS_NO_WR;
    m_indexDataValid = false;
    for (unsigned i = 0; i < sizeof(m_indexData); ++i)
        m_indexData[i] = 0;
    m_indexDataCnt = 0;
    m_sectorDataCnt = 0;

    for (int i = 0; i < MAX_DRIVES; i++)
        m_images[i] = nullptr;
}


Fdc1793::~Fdc1793()
{
    // dtor
}


void Fdc1793::attachFdImage(int driveNum, FdImage* image)
{
    if (driveNum < MAX_DRIVES)
        m_images[driveNum] = image;
}



void Fdc1793::reset()
{
    m_accessMode = FAM_WAITING;
    m_track = 0; // регистр дорожки
    m_sector = 1; // регистр сектора
    m_data = 0; // регистр данных
    m_directionIn = true; // направление движения true=in, false=out
    m_cmdTime = 0;
    m_irq = false;
    m_lastCommand = 0;
    m_addressIdCnt = 0;
    m_writeTrackCnt = 0;
    m_wrTrackState = WTS_NO_WR;
    m_indexDataValid = false;
    m_indexDataCnt = 0;
    m_sectorDataCnt = 0;
}


void Fdc1793::setDrive(int drive)
{
    m_disk = drive;
}


void Fdc1793::setHead(int head)
{
    m_head = head;
}


void Fdc1793::writeByte(int addr, uint8_t value)
{
    addr &= 0x3;

    switch(addr) {
        case 0:
            // command register
            m_lastCommand = (value & 0xF0) >> 4;
            switch (m_lastCommand) {
                case 0:
                    // restore
                    m_track = 0;
                    m_directionIn = false;
                    m_accessMode = FAM_WAITING;
                    m_status = 0x01;
                    generateInt();
                    break;
                case 1:
                    // seek
                    m_track = m_data;
                    if (m_track > 79)
                        m_track = 79;
                    m_status = 0x01;
                    generateInt();
                    break;
                case 2:
                case 3:
                    // step
                    if (m_directionIn && m_track < 79)
                        ++m_track;
                    else if (!m_directionIn && m_track > 0)
                        --m_track;
                    m_status = 0x01;
                    generateInt();
                    break;
                case 4:
                case 5:
                    // step in
                    m_directionIn = true;
                    if (m_track < 79)
                        ++m_track;
                    m_status = 0x01;
                    generateInt();
                    break;
                case 6:
                case 7:
                    // step out
                    m_directionIn = false;
                    if (m_track > 0)
                        --m_track;
                    m_status = 0x01;
                    generateInt();
                    break;
                case 8:
                case 9:
                    // read
                    if (!m_images[m_disk])
                        break;
                    m_images[m_disk]->setCurHead(m_head);
                    m_images[m_disk]->setCurTrack(m_track);
                    if (m_sector <= m_images[m_disk]->getSectors()) {
                        m_images[m_disk]->startSectorAccess(m_sector - 1);
                        m_accessMode = FAM_READING;
                        m_status = 0x03;
                    } else {
                        m_accessMode = FAM_WAITING;
                        m_status = 0x10;
                    }
                    break;
                case 0xA:
                case 0xB:
                    // write
                    if (!m_images[m_disk])
                        break;
                    if (m_images[m_disk]->getWriteProtectStatus()) {
                        m_status = 0x40;
                        break;
                    }
                    m_images[m_disk]->setCurHead(m_head);
                    m_images[m_disk]->setCurTrack(m_track);
                    if (m_sector <= m_images[m_disk]->getSectors()) {
                        m_images[m_disk]->startSectorAccess(m_sector - 1);
                        m_accessMode = FAM_WRITING;
                        m_status = 0x03;
                    } else {
                        m_accessMode = FAM_WAITING;
                        m_status = 0x80;
                    }
                    break;
                case 0xC:
                    // read address
                    if (!m_images[m_disk])
                        break;
                    m_addressId[0] = m_track;
                    m_addressId[1] = m_head;
                    m_addressId[2] = m_images[m_disk]->readSectorAddress() + 1;
                    m_addressId[3] = 3;
                    m_addressId[4] = 0;
                    m_addressId[5] = 0;
                    m_addressIdCnt = 6;
                    m_accessMode = FAM_READING;
                    m_status = 0x03;
                    m_cmdTime = g_emulation->getCurClock();
                break;
                case 0xD:
                    // force interrupt
                    m_accessMode = FAM_WAITING;
                    m_addressIdCnt = 0;
                    m_status = 0x01;
                    break;
                case 0xF:
                    // write track
                    m_accessMode = FAM_WRITING;
                    m_wrTrackState = WTS_NO_WR;
                    m_writeTrackCnt = 0;
                    m_indexDataValid = false;
                    for (int i = 0; i < 4; i++)
                        m_indexData[i] = 0;
                    m_indexDataCnt = 0;
                    m_writeTrackCnt = 0;
                    m_status = 0x03;
                    break;
            }
            break;
        case 1:
            // track register
            m_track = value;
            break;
        case 2:
            // sector register
            m_sector = value;
            break;
        case 3:
            // data register
            m_data = value;
            if (m_images[m_disk] && m_accessMode == FAM_WRITING) {
                if (m_lastCommand == 0xF) {
                    // write track
                    if (writeTrackByte(m_data))
                        m_status = 0x03;
                    else {
                        m_accessMode = FAM_WAITING;
                        m_status = 0x00;
                        generateInt();
                    }
                } else {
                    m_images[m_disk]->writeNextByte(m_data);
                    if (!m_images[m_disk]->getReadyStatus()) {
                        if (m_lastCommand == 0xB) {
                            m_images[m_disk]->startSectorAccess(m_sector++);
                            m_status = 0x03;
                        }
                        else {
                            m_accessMode = FAM_WAITING;
                            m_status = 0x00;
                        }
                    }
                }
            }
            break;
    }
}


bool Fdc1793::writeTrackByte(uint8_t val)
{
    if (val == 0xFE) {
        // ID Address Mark
        m_wrTrackState = WTS_WR_ID;
    }
    else if (val == 0xFB) {
        // Data Address Mark
        m_wrTrackState = WTS_WR_DATA;
    }
    else if (val == 0xF7)
        m_writeTrackCnt++;
    else {
        switch (m_wrTrackState) {
        case WTS_WR_ID:
            m_indexData[m_indexDataCnt++] = val;
            if (m_indexDataCnt >= 4) {
                m_indexDataCnt = 0;
                int track = m_indexData[0];
                int side = m_indexData[1];
                int sector = m_indexData[2];
                int sectorSize = 128 << (m_indexData[3] & 3);
                m_indexDataValid = m_images[m_disk] &&
                        (track == m_track) &&
                        (side == m_head) &&
                        (sectorSize == m_images[m_disk]->getSectorSize()) &&
                        (sector <= m_images[m_disk]->getSectors());
                if (m_indexDataValid) {
                    m_images[m_disk]->setCurHead(m_head);
                    m_images[m_disk]->setCurTrack(m_track);
                    m_images[m_disk]->startSectorAccess(sector - 1);
                }
                m_wrTrackState = WTS_NO_WR;
                m_sectorDataCnt = 0;
            }
            break;
        case WTS_WR_DATA:
            if (m_images[m_disk] && m_indexDataValid) {
                m_images[m_disk]->writeNextByte(val);
                if (++m_sectorDataCnt == m_images[m_disk]->getSectorSize()) {
                    m_sectorDataCnt = 0;
                    m_indexDataValid = false;
                    m_wrTrackState = WTS_NO_WR;
                }
            }
            break;
        default:
            break;
        }
    }

    if (++m_writeTrackCnt >= 6125/*5208*/)
        return false;

    return true;
}


uint8_t Fdc1793::readByte(int addr)
{
    addr &= 0x3;

    switch(addr) {
        case 0: {
            // status register
            m_irq = false;

            if (!(m_images[m_disk]) || !m_images[m_disk]->getImagePresent())
                m_status |= 0x80;
            else
                m_status &= ~0x80;

            if (m_lastCommand <= 7 || m_lastCommand == 0x0D) {
                // index
                int trackPosMks = g_emulation->getCurClock() * 1000000 / g_emulation->getFrequency() % 200000;
                if (trackPosMks < 3000) // 3 ms
                    m_status |= 0x02;
                else
                    m_status &= ~0x02;
            }

            if (m_lastCommand == 0x0C) {
                // read address
                if ((g_emulation->getCurClock() - m_cmdTime)  * 1000000 / g_emulation->getFrequency() > 300000)
                    m_status &= ~0x01;
            }

            uint8_t res = m_status;
            switch (m_lastCommand) {
                case 0:
                    m_status = 0x24;
                    break;
                case 1:
                case 2:
                case 3:
                case 4:
                case 5:
                case 6:
                case 7:
                case 0xD:
                    m_status = m_track == 0 ? 0x24 : 0x20;
                    break;
                case 8:
                case 9:
                    break;
                case 0xA:
                case 0xB:
                    break;
                case 0xC:
                    break;
                case 0xF:
                    break;
            }
            return res;
        }
        case 1:
            // track register
            return m_track;
        case 2:
            // sector register
            return m_sector;
        case 3:
            // data register
            if (m_accessMode == FAM_WAITING)
                m_status &= ~2;
            if (m_accessMode == FAM_READING) {
                if (m_addressIdCnt > 0) {
                    m_data = m_addressId[6 - m_addressIdCnt--];
                    if (m_addressIdCnt == 0) {
                        m_accessMode = FAM_WAITING;
                        m_status = 0x00;
                    }
                } else if (m_images[m_disk]) {
                    m_data = m_images[m_disk]->readNextByte();
                    if (!m_images[m_disk]->getReadyStatus()) {
                        if (m_lastCommand == 9) {
                            m_images[m_disk]->startSectorAccess(m_sector++);
                            m_status = 0x01;
                        }
                        else {
                            m_accessMode = FAM_WAITING;
                            m_status = 0x00;
                        }
                    }
                }

            }
            return m_data;
    }
    return 0xFF; // normally this not occurs
}


void Fdc1793::generateInt()
{
    m_irq = true;
    // call core.inte()
}

bool Fdc1793::getDrq()
{
    return m_accessMode != FAM_WAITING;
}

namespace {

#pragma pack(push, 1)
struct Fdc1793SnapshotStateV1 {
    uint8_t accessMode;
    int32_t disk;
    int32_t head;
    uint8_t track;
    uint8_t sector;
    uint8_t data;
    uint8_t status;
    uint8_t directionIn;
    uint64_t cmdTime;
    uint8_t irq;
    int32_t lastCommand;
    int32_t addressIdCnt;
    uint8_t addressId[6];
    int32_t writeTrackCnt;
    uint8_t writeTrackState;
    uint8_t indexDataValid;
    uint8_t indexData[4];
    int32_t indexDataCnt;
    int32_t sectorDataCnt;
};
#pragma pack(pop)

}

uint32_t Fdc1793::snapshotSectionId() const
{
    return makeSnapshotSectionId('F', 'D', 'C', ' ');
}

uint16_t Fdc1793::snapshotSectionVersion() const
{
    return 1;
}

bool Fdc1793::saveState(SnapshotWriter& writer) const
{
    Fdc1793SnapshotStateV1 state{};
    state.accessMode = static_cast<uint8_t>(m_accessMode);
    state.disk = m_disk;
    state.head = m_head;
    state.track = m_track;
    state.sector = m_sector;
    state.data = m_data;
    state.status = m_status;
    state.directionIn = m_directionIn ? 1 : 0;
    state.cmdTime = m_cmdTime;
    state.irq = m_irq ? 1 : 0;
    state.lastCommand = m_lastCommand;
    state.addressIdCnt = m_addressIdCnt;
    for (unsigned i = 0; i < sizeof(state.addressId); ++i)
        state.addressId[i] = m_addressId[i];
    state.writeTrackCnt = m_writeTrackCnt;
    state.writeTrackState = static_cast<uint8_t>(m_wrTrackState);
    state.indexDataValid = m_indexDataValid ? 1 : 0;
    for (unsigned i = 0; i < sizeof(state.indexData); ++i)
        state.indexData[i] = m_indexData[i];
    state.indexDataCnt = m_indexDataCnt;
    state.sectorDataCnt = m_sectorDataCnt;
    return writer.writeValue(state);
}

bool Fdc1793::loadState(SnapshotReader& reader, uint16_t version)
{
    if (version != snapshotSectionVersion() ||
        reader.remaining() != sizeof(Fdc1793SnapshotStateV1))
        return false;

    Fdc1793SnapshotStateV1 state{};
    if (!reader.readValue(state) ||
        state.accessMode > FAM_WRITING ||
        state.disk < 0 || state.disk >= MAX_DRIVES ||
        state.head < 0 || state.head > 1 ||
        state.directionIn > 1 || state.irq > 1 ||
        state.lastCommand < 0 || state.lastCommand > 0x0F ||
        state.addressIdCnt < 0 || state.addressIdCnt > 6 ||
        state.writeTrackState > WTS_WR_DATA ||
        state.indexDataValid > 1 ||
        state.indexDataCnt < 0 || state.indexDataCnt > 4 ||
        state.writeTrackCnt < 0 || state.writeTrackCnt > 6125 ||
        state.sectorDataCnt < 0)
        return false;

    m_accessMode = static_cast<FdcAccessMode>(state.accessMode);
    m_disk = state.disk;
    m_head = state.head;
    m_track = state.track;
    m_sector = state.sector;
    m_data = state.data;
    m_status = state.status;
    m_directionIn = state.directionIn != 0;
    m_cmdTime = state.cmdTime;
    m_irq = state.irq != 0;
    m_lastCommand = state.lastCommand;
    m_addressIdCnt = state.addressIdCnt;
    for (unsigned i = 0; i < sizeof(m_addressId); ++i)
        m_addressId[i] = state.addressId[i];
    m_writeTrackCnt = state.writeTrackCnt;
    m_wrTrackState = static_cast<WriteTrackState>(state.writeTrackState);
    m_indexDataValid = state.indexDataValid != 0;
    for (unsigned i = 0; i < sizeof(m_indexData); ++i)
        m_indexData[i] = state.indexData[i];
    m_indexDataCnt = state.indexDataCnt;
    m_sectorDataCnt = state.sectorDataCnt;
    return true;
}

