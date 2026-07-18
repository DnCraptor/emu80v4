/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2019-2024
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

#include <algorithm>
#include <cstring>

#include "Globals.h"
#include "EmuCalls.h"
#include "Vector.h"
#include "EmuWindow.h"
#include "Cpu.h"
#include "Cpu8080.h"
#include "AddrSpace.h"
#include "Emulation.h"
#include "Memory.h"
#include "CrtRenderer.h"
#include "DiskImage.h"
#include "Keyboard.h"
#include "KbdLayout.h"
#include "Fdc1793.h"
#include "SoundMixer.h"
#include "WavReader.h"
#include "Covox.h"
#include "PrnWriter.h"
#include "AtaDrive.h"
#include "KbdTapper.h"
#include "Ppi8255.h"
#include "Pit8253.h"
#include "Pit8253Sound.h"
#include "Psg3910.h"
#include "TapeRedirector.h"
#include "RkTapeHooks.h"
#include "CloseFileHook.h"
#include "CpuHook.h"
#include "RamDisk.h"

using namespace std;


void VectorAddrSpace::reset() {
    m_romEnabled = true;
    m_inRamPagesMask = 0;
    m_stackDiskEnabled = false;
    m_inRamDiskPage = 0;
    m_stackDiskPage = 0;
    m_inRamPagesMask2 = 0;
    m_stackDiskEnabled2 = false;
    m_inRamDiskPage2 = 0;
    m_stackDiskPage2 = 0;
    m_eramSegment = 0;
    m_eramPageStartAddr = 0xA000;
    m_eramPageEndAddr = 0xDFFF;
}


void VectorAddrSpace::writeByte(int addr, uint8_t value)
{
    if (m_eram) {
        // ERAM
        if (m_stackDiskEnabled && m_cpu->checkForStackOperation())
            m_ramDisk->writeByte(m_eramSegment * 0x40000 + m_stackDiskPage * 0x10000 + addr, value);
        else if (m_inRamPagesMask & 2 && addr >= m_eramPageStartAddr && addr <= m_eramPageEndAddr)
            m_ramDisk->writeByte(m_eramSegment * 0x40000 + m_inRamDiskPage * 0x10000 + addr, value);
        else {
            if (addr >= 0x8000 && m_crtRenderer)
                m_crtRenderer->vidMemWriteNotify();
            m_mainMemory->writeByte(addr, value);
        }
        return;
    }

    // Barkar
    if (m_stackDiskEnabled && m_cpu->checkForStackOperation())
        m_ramDisk->writeByte(m_stackDiskPage * 0x10000 + addr, value);
    else if (m_stackDiskEnabled2 && m_cpu->checkForStackOperation())
        m_ramDisk2->writeByte(m_stackDiskPage2 * 0x10000 + addr, value);
    else if (m_inRamPagesMask && (addr >= 0x8000) && m_inRamPagesMask & (1 << ((addr & 0x6000) >> 13)))
        m_ramDisk->writeByte(m_inRamDiskPage * 0x10000 + addr, value);
    else if (m_inRamPagesMask2 && (addr >= 0x8000) && m_inRamPagesMask2 & (1 << ((addr & 0x6000) >> 13)))
        m_ramDisk2->writeByte(m_inRamDiskPage2 * 0x10000 + addr, value);
    else {
        if (addr >= 0x8000 && m_crtRenderer)
            m_crtRenderer->vidMemWriteNotify();
        m_mainMemory->writeByte(addr, value);
    }
}


uint8_t VectorAddrSpace::readByte(int addr)
{
    if (m_eram) {
        // ERAM
        if (m_stackDiskEnabled && m_cpu->checkForStackOperation())
            return m_ramDisk->readByte(m_eramSegment * 0x40000 + m_stackDiskPage * 0x10000 + addr);
        if (m_inRamPagesMask & 2 && addr >= m_eramPageStartAddr && addr <= m_eramPageEndAddr)
            return m_ramDisk->readByte(m_eramSegment * 0x40000 + m_inRamDiskPage * 0x10000 + addr);
        if (m_romEnabled && addr < m_rom->getSize())
            return m_rom->readByte(addr); // add rom check
        else
            return m_mainMemory->readByte(addr);
    }

    // Barkar
    if (m_stackDiskEnabled && m_cpu->checkForStackOperation())
        return m_ramDisk->readByte(m_stackDiskPage * 0x10000 + addr);
    if (m_stackDiskEnabled2 && m_cpu->checkForStackOperation())
        return m_ramDisk2->readByte(m_stackDiskPage2 * 0x10000 + addr);
    if (m_inRamPagesMask && (addr >= 0x8000) && m_inRamPagesMask & (1 << ((addr & 0x6000) >> 13)))
        return m_ramDisk->readByte(m_inRamDiskPage * 0x10000 + addr);
    if (m_inRamPagesMask2 && (addr >= 0x8000) && m_inRamPagesMask2 & (1 << ((addr & 0x6000) >> 13)))
        return m_ramDisk2->readByte(m_inRamDiskPage2 * 0x10000 + addr);
    if (m_romEnabled && addr < m_rom->getSize())
        return m_rom->readByte(addr); // add rom check
    else
        return m_mainMemory->readByte(addr);
}


void VectorAddrSpace::attachRamDisk(int diskNum, AddressableDevice* ramDisk)
{
    if (diskNum == 0)
        m_ramDisk = ramDisk;
    else // if (diskNum == 1)
        m_ramDisk2 = ramDisk;
}

void VectorAddrSpace::ramDiskControl(int diskNum, int inRamPagesMask, bool stackEnabled, int inRamPage, int stackPage)
{
    if (diskNum == 0) {
        m_inRamPagesMask = inRamPagesMask;
        m_stackDiskEnabled = stackEnabled;
        m_inRamDiskPage = inRamPage;
        m_stackDiskPage = stackPage;
    } else { // if (diskNum == 1)
        m_inRamPagesMask2 = inRamPagesMask;
        m_stackDiskEnabled2 = stackEnabled;
        m_inRamDiskPage2 = inRamPage;
        m_stackDiskPage2 = stackPage;
    }
}


void VectorAddrSpace::eramControl(int eramSegment, int eramPageStartAddr, int eramPageEndAddr)
{
    m_eramSegment = eramSegment;
    m_eramPageStartAddr = eramPageStartAddr;
    m_eramPageEndAddr = eramPageEndAddr;
}

void VectorCore::inte(bool isActive)
{
    m_intsEnabled = isActive;
    if (!isActive)
        m_intReq = false;
    else if (m_intReq) {
        Cpu8080Compatible* cpu = getCpu();
        if (cpu->getInte()) {
            cpu->intRst(7);
            cpu->hrq(cpu->getKDiv() * 5); // add waits to RST
        }
    }
}


void VectorCore::vrtc(bool isActive)
{
    if (isActive && m_intsEnabled) {
        m_intReq = true;
        Cpu8080Compatible* cpu = getCpu();
        if (cpu->getInte()) {
            cpu->intRst(7);
            cpu->hrq(cpu->getKDiv() * 5); // add waits to RST
        }
    }
}



VectorRenderer::VectorRenderer()
{
    const int pixelFreq = 12; // MHz
    const int maxBufSize = 626 * 288; // 626 = 704 / 13.5 * pixelFreq

    m_sizeX = m_prevSizeX = 512;
    m_sizeY = m_prevSizeY = 256;
    m_bufSize = m_sizeX * m_sizeY;
    m_pixelData = new uint8_t[maxBufSize];

    m_ticksPerPixel = g_emulation->getFrequency() / 12000000;
    m_curScanlineClock = m_curClock;

    m_curFramePixel = 0;
    m_curFrameClock = m_curClock;

    m_frameBuf = m_pixelData; ///new uint8_t[maxBufSize];

    memset(m_colorPalette, 0, 16);
    memset(m_bwPalette, 0, 16);
    m_palette = m_colorPalette;

    prepareFrame(); // prepare 1st frame dimensions
}


VectorRenderer::~VectorRenderer()
{
///    if (m_frameBuf)
///        delete[] m_frameBuf;
}


void VectorRenderer::operate()
{
    advanceTo(m_curClock);
    m_curFrameClock = m_curClock;
    m_curFramePixel = 0;
    m_curClock += m_ticksPerPixel * 768 * 312;
    m_lineOffsetIsLatched = false;
    renderFrame();
    m_machine->vrtc(true);
    m_mode512pxLatched = m_mode512px;
    m_lastColor = 0;
}


void VectorRenderer::advanceTo(uint64_t clock)
{
    const int bias = 189;

    if (clock <= m_curFrameClock)
        return;

    int toPixel = (int64_t(clock) - int64_t(m_curFrameClock)) / m_ticksPerPixel + bias;

    if (toPixel <= m_curFramePixel) {
        if (toPixel < 40 * 768 || toPixel >= 296 * 768)
            m_lastColor = m_borderColor;
        return;
    }

    if (toPixel >= 312 * 768)
        toPixel = 312 * 768 - 1;

    if (!m_lineOffsetIsLatched && toPixel > 768 * 40 + 180) {
        m_lineOffsetIsLatched = true;
        m_latchedLineOffset = m_lineOffset;
    }

    int firstLine = m_curFramePixel / 768;
    int firstPixel = m_curFramePixel % 768;
    int lastLine = toPixel / 768;
    int lastPixel = toPixel % 768;
    m_curFramePixel = toPixel;
    renderLine(firstLine, firstPixel, firstLine == lastLine ? lastPixel : 768);
    for (int line = firstLine + 1; line < lastLine; line++)
        renderLine(line, 0, 768);
    if (firstLine != lastLine)
        renderLine(lastLine, 0, lastPixel);
}


void VectorRenderer::setBorderColor(uint8_t color)
{
    advanceTo(g_emulation->getCurClock() + m_ticksPerPixel * 48);
    m_borderColor = color;
}


void VectorRenderer::set512pxMode(bool mode512)
{
    advanceTo(g_emulation->getCurClock() + m_ticksPerPixel * 34);
    m_mode512px = mode512;
}


void VectorRenderer::setLineOffset(uint8_t lineOffset)
{
    advanceTo(g_emulation->getCurClock() + m_ticksPerPixel * 48);
    m_lineOffset = lineOffset;
}


void VectorRenderer::setPaletteColor(uint8_t color)
{
    advanceTo(g_emulation->getCurClock() + m_ticksPerPixel * 27);
    register uint32_t c = ((color & 0x7) << 21) | ((color & 0x7) << 18) | ((color & 0x6) << 15) |
                          ((color & 0x38) << 10) | ((color & 0x38) << 7) | ((color & 0x30) << 4) |
                          (color & 0xC0) | ((color & 0xC0) >> 2) | ((color & 0xC0) >> 4) | ((color & 0xC0) >> 6);
    m_colorPalette[m_lastColor] = RGB888(((c >> 16) & 0xFF), ((c >> 8) & 0xFF), (c & 0xFF));
    register uint8_t bw = c_bwMap[color];
    m_bwPalette[m_lastColor] = RGB888((bw << 16), (bw << 8), bw);
}


void VectorRenderer::vidMemWriteNotify()
{
    advanceTo(g_emulation->getCurClock() + m_ticksPerPixel * 40);
}


void VectorRenderer::renderLine(int nLine, int firstPx, int lastPx)
{
    // Render scan line #nLine
    // Vertical: 0-22 - invisible, 23-39 - border, 40-295 - visible, 296-311 - border) from firstPx to lastPx
    // Horizonlal: 0-123 - invisible, 124-180 - border, 181-692 - active area, 693-749 - border, 750-767 - invisible
    // (lastPx is non-inclusive)

    if (nLine < 24) {
        m_lastColor = m_borderColor;
        return;
    }

    uint8_t* linePtr = m_frameBuf + (nLine - 24) * 626;
    uint8_t* ptr;

    if (nLine < 40 || nLine >= 296) {
        // upper and lower borders
        if (firstPx < 124) firstPx = 124;
        ptr = linePtr + firstPx - 124;
        m_lastColor = m_borderColor;
        for (int px = firstPx; px < lastPx && px < 750; px++) {
            *ptr++ = m_palette[m_mode512px ? (px & 1 ? m_borderColor & 0x0c : m_borderColor & 0x03) : m_borderColor];
        }
    } else {
        // left border
        if (firstPx < 124) firstPx = 124;
        ptr = linePtr + firstPx - 124;
        for (int px = firstPx; px < lastPx && px < 181; px++)
            *ptr++ = m_palette[m_mode512px ? (px & 1 ? m_borderColor & 0x0c : m_borderColor & 0x03) : m_borderColor];

        // active area
        if (firstPx < 181) firstPx = 181;
        ptr = linePtr + firstPx - 124;
        uint8_t rollOff = uint8_t(m_latchedLineOffset - nLine + 40);
        for (int px = firstPx - 181; px < lastPx - 181 && px < 693 - 181; px++) {
            int dot = (px & 0x0E) >> 1;
            int offset = ((px & 0x1F0) << 4) | rollOff;
            uint8_t btY = m_screenMemory[0x8000 + offset] << dot;
            uint8_t btR = m_screenMemory[0xA000 + offset] << dot;
            uint8_t btG = m_screenMemory[0xC000 + offset] << dot;
            uint8_t btB = m_screenMemory[0xE000 + offset] << dot;
            int logBGcolor = ((btG & 0x80) >> 6) | ((btB & 0x80) >> 7);
            int logYRcolor = ((btY & 0x80) >> 4) | ((btR & 0x80) >> 5);
            m_lastColor = logYRcolor | logBGcolor;
            if (m_mode512px) {
                *ptr++ = m_palette[px & 1 ? m_lastColor & 0x0c : m_lastColor & 0x03];
            } else {
                *ptr++ = m_palette[m_lastColor];
            }
        }

        // right border
        if (firstPx < 693) firstPx = 693;
        ptr = linePtr + firstPx - 124;
        for (int px = firstPx; px < lastPx && px < 750; px++)
            *ptr++ = m_palette[m_mode512px ? (px & 1 ? m_borderColor & 0x0c : m_borderColor & 0x03) : m_borderColor];

        if (lastPx < 182 || lastPx >= 694)
            m_lastColor = m_borderColor;
    }
}


void VectorRenderer::renderFrame()
{
    /**
    if (m_showBorder)
        memcpy(m_pixelData, m_frameBuf, m_sizeX * m_sizeY);
    else {
        uint8_t* ptr = m_frameBuf + 626 * 16 + 57;
        for (int i = 0; i < 256 * 512; i += 512) {
            memcpy(m_pixelData + i, ptr, 512);
            ptr += 626;
        }
    }
*/
    swapBuffers();
    prepareFrame();
    graphics_set_buffer(m_frameBuf, m_sizeX, m_sizeY);
}


void VectorRenderer::prepareFrame()
{
    if (!m_showBorder) {
        m_sizeX = 512;
        m_sizeY = 256;
///        m_aspectRatio = 576.0 * 9 / 704 / 12;
    } else {
        m_sizeX = 626;
        m_sizeY = 288;
///        m_aspectRatio = double(m_sizeY) * 4 / 3 / m_sizeX;
    }
}


void VectorRenderer::prepareDebugScreen()
{
    advanceTo(g_emulation->getCurClock());
    enableSwapBuffersOnce();
    renderFrame();
}


void VectorRenderer::setColorMode(bool colorMode)
{
    m_colorMode = colorMode;
    m_palette = m_colorMode ? m_colorPalette : m_bwPalette;
}


void VectorRenderer::toggleColorMode()
{
    setColorMode(!m_colorMode);
}


void VectorRenderer::toggleCropping()
{
    m_showBorder = !m_showBorder;
}


void VectorRenderer::attachMemory(Ram* memory)
{
    m_screenMemory = memory->getDataPtr();
}

bool VectorFileLoader::chooseAndLoadFile(bool run)
{
    string fileName = palOpenFileDialog("Open file", m_filter, false, m_machine->getWindow());
    g_emulation->restoreFocus();
    if (fileName.empty())
        return true;
    if (!loadFile(fileName, run)) {
        emuLog << "Error loading file: " << fileName << "\n";
        return false;
    }
    return true;
}


bool VectorFileLoader::loadFile(const std::string& fileName, bool run)
{
    auto periodPos = fileName.find_last_of(".");
    string ext = periodPos != string::npos ? fileName.substr(periodPos) : fileName;
    transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".fdd") {
        if (!m_machine->assignDiskAFileName(fileName))
            return false;

        Cpu8080Compatible* cpu = m_machine->getCpu();
        static_cast<VectorAddrSpace*>(m_machine->getCpu()->getAddrSpace())->enableRom();
        m_machine->getCpu()->setPC(0);
        g_emulation->exec((int64_t)cpu->getKDiv() * 25000000, true);

        if (run) {
            m_machine->reset();
            static_cast<VectorAddrSpace*>(m_machine->getCpu()->getAddrSpace())->disableRom();
        }
        return true;
    }

    FIL f;
    if (f_open(&f, fileName.c_str(), FA_READ) != FR_OK)
        return false;
    bool basFile = false;
    uint16_t begAddr = 0x100;

    // check for "r0m"
    if (fileName.size() >= 4) {
        string ext = fileName.substr(fileName.size() - 4, 4);
        if (ext == ".r0m" || ext == ".R0M")
            begAddr = 0;
        else if (ext == ".bas" || ext == ".BAS" || ext == ".cas" || ext == ".CAS")
            basFile = true;
    }

    Cpu8080Compatible* cpu = m_machine->getCpu();
    VectorAddrSpace* as = static_cast<VectorAddrSpace*>(cpu->getAddrSpace());
    m_machine->reset();
    as->enableRom();
    cpu->disableHooks();
    g_emulation->exec(int64_t(cpu->getKDiv()) * m_skipTicks, true);
    cpu->enableHooks();

    for (unsigned i = 0; i < 0x100; i++)
        m_addrSpace->writeByte(i, 0x00);

    UINT br;
    if (!basFile)
        for (int i = 0; i < f_size(&f); ++i) {
            uint16_t addr = begAddr + i;
            uint8_t v;
            f_read(&f, &v, 1, &br);
            m_addrSpace->writeByte(addr, v);
            if (!run && (addr & 0xFF) == 0) {
                // paint block
                int block = addr >> 8;
                uint16_t blockAddr = 0xC018 + (block % 32) * 0x100 + (block / 32) * 0x18;
                for (int i = 0; i < 8; i++)
                    m_addrSpace->writeByte(blockAddr + i, 0x7E);
            }
        }
    else {
        int fileSize = f_size(&f);
        uint32_t v;
        f_read(&f, &v, 4, &br);
        // check for CAS
        if (fileSize >= 14 && v == 0xD3D3D3D3) {
            // Cas file
            while (fileSize) {
                uint8_t v;
                f_read(&f, &v, 1, &br);
                if (v == 0xE6) break;
                fileSize--;
            }

            if (fileSize < 7) {
                f_close(&f);
                return false;
            }
            f_lseek(&f, f_tell(&f) + 5);
            fileSize -= 5;
        }

        for (int i = 0; i < 0x39c6; i++)
            m_addrSpace->writeByte(0x0100 + i, as->readByte(0x08C5 + i));
        as->disableRom();
        cpu->setPC(begAddr);
        cpu->setIFF(false);
        cpu->disableHooks();
        g_emulation->exec(int64_t(cpu->getKDiv()) * 4000000, true);
        cpu->enableHooks();
        m_addrSpace->writeByte(0x4300, 0);

        uint16_t addr, nextAddr;
        addr = nextAddr = 0x4301;
        for(;;) {
            if (addr == nextAddr + 1) {
                f_lseek(&f, f_tell(&f) - 1);
                f_read(&f, &nextAddr, 2, &br); // TODO: ensure order of bytes
                ///nextAddr = (ptr[0] << 8) | ptr[-1];
                f_lseek(&f, f_tell(&f) - 1);
            }
            uint8_t v;
            f_read(&f, &v, 1, &br);
            m_addrSpace->writeByte(addr++, v);
            fileSize--;
            if (nextAddr == 0 || fileSize == 0 || addr >= 0x7EFF)
                break;
        }
        m_addrSpace->writeByte(0x4045, addr & 0xFF);
        m_addrSpace->writeByte(0x4046, addr >> 8);
        m_addrSpace->writeByte(0x4047, addr & 0xFF);
        m_addrSpace->writeByte(0x4048, addr >> 8);
        m_addrSpace->writeByte(0x4049, addr & 0xFF);
        m_addrSpace->writeByte(0x404A, addr >> 8);
        f_close(&f);

        if (run) {
            m_addrSpace->writeByte(0x3DBF, 'R');
            m_addrSpace->writeByte(0x3DC0, 'U');
            m_addrSpace->writeByte(0x3DC1, 'N');
            m_addrSpace->writeByte(0x3DC2, '\r');
            m_addrSpace->writeByte(0x3DB8, 4);
            m_addrSpace->writeByte(0x3DB9, 4);
            m_addrSpace->writeByte(0x3DBA, 0);
        }

        return true;
    }


    f_close(&f);

    if (run) {
        as->disableRom();
        cpu->setPC(begAddr);
        cpu->setIFF(false);
    } else {
        as->enableRom();
        cpu->setPC(0xDF);
        //cpu->setSP(0xDCF0);
    }

    return true;
}


// Port 01
void VectorPpi8255Circuit::setPortC(uint8_t value)
{
    m_tapeSoundSource->setValue(value & 1);
    m_machine->tapeOut(value & 1);
}


// Port 02
void VectorPpi8255Circuit::setPortB(uint8_t value)
{
    // order is important!
    m_renderer->set512pxMode(value & 0x10);
    m_renderer->setBorderColor(value & 0x0f);
}


// Port 03
void VectorPpi8255Circuit::setPortA(uint8_t value)
{
    m_renderer->setLineOffset(value);
    m_kbd->setMatrixMask(value);
}


uint8_t VectorPpi8255Circuit::getPortB()
{
    return m_kbd->getMatrixData();
}


uint8_t VectorPpi8255Circuit::getPortC()
{
    return (m_kbd->getCtrlKeys() & 0xEF) | (g_emulation->getWavReader()->getCurValue() ? 0x10 : 0x00);
}



void VectorColorRegister::writeByte(int, uint8_t value)
{
    m_renderer->setPaletteColor(value);
}



VectorKeyboard::VectorKeyboard()
{
    VectorKeyboard::resetKeys();
}


void VectorKeyboard::resetKeys()
{
    for (int i = 0; i < 8; i++)
        m_keys[i] = 0;
    m_mask = 0;
    m_ctrlKeys = 0;
}


void VectorKeyboard::processKey(EmuKey key, bool isPressed)
{
    if (key == EK_NONE)
        return;

    int i, j;

    // Основная матрица
    for (i = 0; i < 8; i++)
        for (j = 0; j < 8; j++)
            if (key == m_keyMatrix[i][j])
                goto found;

    // Управляющие клавиши
    for (i = 0; i < 8; i++)
        if (m_ctrlKeyMatrix[i] == key) {
            if (isPressed)
                m_ctrlKeys |= (1 << i);
            else
                m_ctrlKeys &= ~(1 << i);
        }
    return;

found:
    if (isPressed)
        m_keys[i] |= (1 << j);
    else
        m_keys[i] &= ~(1 << j);
}


uint8_t VectorKeyboard::getMatrixData()
{
    uint8_t val = 0;
    uint8_t mask = m_mask;
    for (int i=0; i<8; i++) {
        if (mask & 1)
            val |= m_keys[i];
        mask >>= 1;
    }
    return ~val;
}


int VectorCpuWaits::getCpuWaitStates(int, int normalClocks)
{
    static const int waits[19] = {0, 0, 0, 0, 0, 3, 0, 1, 0, 0, 2, 5, 0, 3, 0, 0, 4, 7, 6};
    return waits[normalClocks];
}


int VectorZ80CpuWaits::getCpuWaitStates(int opcode, int normalClocks)
{
    static const int waits[24] = {0, 0, 0, 0, 0, 3, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1, 4, 3, 2, 1, 4, 3, 0, 5};
    // 8, 11, 12, 13, 15 should be revised
    switch (normalClocks) {
    case 8:
        if ((opcode & 0xFF) == 0x10) // DJNZ, if B==0
            return 4;
        break;
    case 11:
        if ((opcode & 0xCF) == 0xC5 ||   // PUSH qq
            (opcode & 0xC7) == 0xC0 ||   // RET cc if cc = true
            (opcode & 0xC7) == 0xC7)     // RST p
            return 5;
        break;
    case 14:
        if ((opcode & 0xFFDF) == 0xE1DD) // POP ix/iy
            return 6;
        break;
    case 15:
        if ((opcode & 0xFFDF) == 0xE5DD) // PUSH ix/iy
            return 5;
        break;
    case 19:
        if ((opcode & 0xFF) == 0xE3 ||   // EX (SP),HL
            (opcode & 0xFFDF) == 0x36DD) // LD (ix+d),n
            return 5;
        break;
    case 23:
        if ((opcode & 0xFEDF) == 0x34DD) // INC/DEC (ix/iy+d)
            return 1;
        break;
    default:
        break;
    }
    return waits[normalClocks];
}


bool VectorKbdLayout::processSpecialKeys(PalKeyCode keyCode)
{
    if (keyCode == PK_F11) {
        //m_machine->getKeyboard()->disableKeysReset();
        //m_machine->reset();
        //m_machine->getKeyboard()->enableKeysReset();
        static_cast<VectorAddrSpace*>(m_machine->getCpu()->getAddrSpace())->enableRom();
        m_machine->getCpu()->setPC(0);
        return true;
    } else if (keyCode == PK_F12) {
        m_machine->getKeyboard()->disableKeysReset();
        m_machine->reset();
        m_machine->getKeyboard()->enableKeysReset();
        static_cast<VectorAddrSpace*>(m_machine->getCpu()->getAddrSpace())->disableRom();
        //m_machine->getCpu()->setPC(0);
        return true;
    }
    return false;
}



void VectorRamDiskSelector::writeByte(int, uint8_t value)
{
    if (m_vectorAddrSpace)
        m_vectorAddrSpace->ramDiskControl(m_diskNum, ((value & 0x40) >> 6) | ((value & 0x20) >> 4) | ((value & 0x20) >> 3) | ((value & 0x80) >> 4), value & 0x10, value & 0x3, (value >> 2) & 0x3);
}



void VectorEramSelector::writeByte(int, uint8_t value)
{
    int start, end, segment;
    if (value & 4) {
        start = 0x0000;
        end = 0xFFFF;
    } else switch (value & 3) {
    case 0:
        start = 0xA000;
        end = 0xDFFF;
        break;
    case 1:
        start = 0x8000;
        end = 0xDFFF;
        break;
    case 2:
        start = 0x8000;
        end = 0xFFFF;
        break;
    default: // case 3
        start = 0x0100;
        end = 0x7FFF;
    }
    segment = (value >> 3) & 7;

    if (m_vectorAddrSpace)
        m_vectorAddrSpace->eramControl(segment, start, end);
}


void VectorFddControlRegister::writeByte(int, uint8_t value)
{
    m_fdc->setDrive(value & 1);
    m_fdc->setHead(((value & 0x4) >> 2) ^ 1);
}




void VectorPpi8255Circuit2::setPortA(uint8_t value)
{
    if (m_covox) {
        m_covox->setValue(value >> 1);
    }

    m_printerData = value;
}


void VectorPpi8255Circuit2::setPortC(uint8_t value)
{
    bool newStrobe = value & 0x10;
    if (m_printerStrobe && !newStrobe) {
        g_emulation->getPrnWriter()->printByte(m_printerData);
    }
    m_printerStrobe = newStrobe;
}



void VectorHddRegisters::writeByte(int addr, uint8_t value)
{
    if (!m_ataDrive)
        return;

    if (addr == 8) {
        m_highW = value;
        return;
    }

    addr &= 7;

    if (addr == 0)
        m_ataDrive->writeReg(addr, value | m_highW << 8);
    else
        m_ataDrive->writeReg(addr, value);
}


uint8_t VectorHddRegisters::readByte(int addr)
{
    if (!m_ataDrive)
        return 0xFF;

    if (addr == 8)
        return m_highR;

    addr &= 7;

    uint16_t read = m_ataDrive->readReg(addr);
    if (addr == 0)
        m_highR = read >> 8;
    return read & 0x00FF;
}

VectorCore::VectorCore()
{

    EmuWindow* window = addDevice<EmuWindow>();
    window->setCaption("Вектор-06Ц");
    m_window = window;

    Ram* ram = addDevice<Ram>(0x10000);

    Rom* rom = addDevice<Rom>(0x8000, "vector/loader.rom");

    Cpu8080* cpu = addDevice<Cpu8080>();
    cpu->setFrequency(3000000);
    cpu->setStartAddr(0x0000);
    m_cpu = cpu;

    VectorAddrSpace* addrSpace = addDevice<VectorAddrSpace>();
    addrSpace->attachRam(ram);
    addrSpace->attachRom(rom);
    addrSpace->attachCpu(cpu);

    AddrSpace* ioAddrSpace = addDevice<AddrSpace>();

    cpu->attachAddrSpace(addrSpace);
    cpu->attachIoAddrSpace(ioAddrSpace);

    VectorRenderer* crtRenderer = addDevice<VectorRenderer>();
    crtRenderer->attachMemory(ram);
    crtRenderer->setVisibleArea(true);
    m_renderer = crtRenderer;

    addrSpace->attachCrtRenderer(crtRenderer);

    cpu->attachCore(this);

    VectorKeyboard* keyboard = addDevice<VectorKeyboard>();
    m_keyboard = keyboard;

    VectorKbdLayout* kbdLayout = addDevice<VectorKbdLayout>();
    kbdLayout->setQwertyMode();
    m_kbdLayout = kbdLayout;

    KbdTapper* kbdTapper = addDevice<KbdTapper>();
    kbdTapper->setPressTime(20);
    kbdTapper->setReleaseTime(20);
    kbdTapper->setCrDelay(100);
    m_kbdTapper = kbdTapper;

    VectorPpi8255Circuit* ppiCircuit = addDevice<VectorPpi8255Circuit>();
    ppiCircuit->attachRenderer(crtRenderer);
    ppiCircuit->attachKeyboard(keyboard);

    GeneralSoundSource* tapeSoundSource = addDevice<GeneralSoundSource>();
    ppiCircuit->attachTapeSoundSource(tapeSoundSource);

    Ppi8255* ppi = addDevice<Ppi8255>();
    ppi->setNoReset(true);
    ppi->attachPpi8255Circuit(ppiCircuit);

    AddrSpaceInverter* invertedPpi = addDevice<AddrSpaceInverter>(ppi);
    ioAddrSpace->addRange(0x00, 0x03, invertedPpi);

    VectorColorRegister* colorReg = addDevice<VectorColorRegister>();
    colorReg->attachRenderer(crtRenderer);
    ioAddrSpace->addRange(0x0C, 0x0F, colorReg);

    Covox* covox = addDevice<Covox>(7);
    covox->setNegative(true);

    VectorPpi8255Circuit2* covoxCircuit = addDevice<VectorPpi8255Circuit2>();
    covoxCircuit->attachCovox(covox);

    Ppi8255* ppi2 = addDevice<Ppi8255>();
    ppi2->attachPpi8255Circuit(covoxCircuit);

    AddrSpaceInverter* invertedPpi2 = addDevice<AddrSpaceInverter>(ppi2);
    ioAddrSpace->addRange(0x04, 0x07, invertedPpi2);

    Pit8253* pit = addDevice<Pit8253>();
    pit->setFrequency(1500000);

    Pit8253SoundSource* sndSource = addDevice<Pit8253SoundSource>();
    sndSource->attachPit(pit);
    sndSource->setNegative(true);

    AddrSpaceInverter* invertedPit = addDevice<AddrSpaceInverter>(pit);
    ioAddrSpace->addRange(0x08, 0x0B, invertedPit);

    Psg3910* ay = addDevice<Psg3910>();
    ay->setFrequency(1750000);
    ioAddrSpace->addRange(0x14, 0x15, ay);

    Psg3910SoundSource* psgSoundSource = addDevice<Psg3910SoundSource>();
    psgSoundSource->attachPsg(ay);

    Fdc1793* fdc = addDevice<Fdc1793>();

    AddrSpaceInverter* invertedFdc = addDevice<AddrSpaceInverter>(fdc);
    ioAddrSpace->addRange(0x18, 0x1B, invertedFdc);

    VectorFddControlRegister* fddReg = addDevice<VectorFddControlRegister>();
    fddReg->attachFdc1793(fdc);
    ioAddrSpace->addRange(0x1C, 0x1C, fddReg);

    AtaDrive* ataDrive = addDevice<AtaDrive>();
    ataDrive->setVectorGeometry();

    VectorHddRegisters* hddRegisters = addDevice<VectorHddRegisters>();
    hddRegisters->attachAtaDrive(ataDrive);
    ioAddrSpace->addRange(0x50, 0x5F, hddRegisters);

    FdImage* diskA = addDevice<FdImage>(80, 2, 5, 1024);
    diskA->setLabel("A");
    diskA->setFilter("Образы дисков Вектора (*.fdd)|*.fdd;*.FDD|Все файлы (*.*)|*");
    m_diskA = diskA;
    fdc->attachFdImage(0, diskA);

    FdImage* diskB = addDevice<FdImage>(80, 2, 5, 1024);
    diskB->setLabel("B");
    diskB->setFilter("Образы дисков Вектора (*.fdd)|*.fdd;*.FDD|Все файлы (*.*)|*");
    m_diskB = diskB;
    fdc->attachFdImage(1, diskB);

    DiskImage* hdd = addDevice<DiskImage>();
    hdd->setLabel("HDD");
    hdd->setFilter("Образы HDD Вектора (*.hdd;*.img)|*.hdd;*.HDD;*.img;*.IMG|Все файлы (*.*)|*");
    m_hdd = hdd;
    ataDrive->assignDiskImage(hdd);

    VectorFileLoader* loader = addDevice<VectorFileLoader>();
    loader->attachAddrSpace(ram);
    loader->setFilter("Файлы Вектора (*.rom;*.r0m;*.vec;*.cas;*.bas;*fdd)|*.rom;*.ROM;*.rom;*.R0M;*.vec;*.VEC;*.cas;*.CAS;*.bas;*.BAS;*.fdd;*.FDD|Все файлы (*.*)|*");
    m_loader = loader;

    TapeRedirector* tapeInFile = addDevice<TapeRedirector>();
    tapeInFile->setMode("r");
    tapeInFile->setFilter("Файлы RK-совместимых ПК (*.rk?)|*.rk;*.rk?;*.RK;*.RK?|Файлы Бейсика (*.cas)|*.cas;*.CAS|Все файлы (*.*)|*");

    TapeRedirector* tapeOutFile = addDevice<TapeRedirector>();
    tapeOutFile->setMode("w");
    tapeOutFile->setFilter(".rk|.cas");

    RkTapeInHook* tapeInHookBas = addDevice<RkTapeInHook>(0x2B05);
    tapeInHookBas->setSignature("C5D50E0057DB");
    tapeInHookBas->setTapeRedirector(tapeInFile);
    cpu->addHook(tapeInHookBas);

    RkTapeOutHook* tapeOutHookBas = addDevice<RkTapeOutHook>(0x2B60);
    tapeOutHookBas->setOutputRegisterA(true);
    tapeOutHookBas->setSignature("C5D5F5570E08");
    tapeOutHookBas->setTapeRedirector(tapeOutFile);
    cpu->addHook(tapeOutHookBas);

    CloseFileHook* closeFileHookBas = addDevice<CloseFileHook>(0x2B8E);
    closeFileHookBas->setSignature("C506003A203C");
    closeFileHookBas->addTapeRedirector(tapeInFile);
    closeFileHookBas->addTapeRedirector(tapeOutFile);
    cpu->addHook(closeFileHookBas);

    RkTapeInHook* tapeInHookMon = addDevice<RkTapeInHook>(0xF840);
    tapeInHookMon->setSignature("C5D50E0057DB");
    tapeInHookMon->setTapeRedirector(tapeInFile);
    cpu->addHook(tapeInHookMon);

    RkTapeOutHook* tapeOutHookMon = addDevice<RkTapeOutHook>(0xF89B);
    tapeOutHookMon->setOutputRegisterA(true);
    tapeOutHookMon->setSignature("C5D5F5573E02");
    tapeOutHookMon->setTapeRedirector(tapeOutFile);
    cpu->addHook(tapeOutHookMon);

    Ret8080Hook* skipHookMon = addDevice<Ret8080Hook>(0xEDDC);
    skipHookMon->setSignature("CD1097FB76F3");
    cpu->addHook(skipHookMon);

    CloseFileHook* closeFileHookMon = addDevice<CloseFileHook>(0xFEFF);
    closeFileHookMon->setSignature("3AFDFFE604CD");
    closeFileHookMon->addTapeRedirector(tapeInFile);
    closeFileHookMon->addTapeRedirector(tapeOutFile);
    cpu->addHook(closeFileHookMon);

    RkTapeInHook* tapeInHookEmuRk = addDevice<RkTapeInHook>(0xFC31);
    tapeInHookEmuRk->setSignature("F3C5D50E0057");
    tapeInHookEmuRk->setTapeRedirector(tapeInFile);
    cpu->addHook(tapeInHookEmuRk);

    RkTapeOutHook* tapeOutHookEmuRk = addDevice<RkTapeOutHook>(0xFC7D);
    tapeOutHookEmuRk->setOutputRegisterA(true);
    tapeOutHookEmuRk->setSignature("F3C5D5F51608");
    tapeOutHookEmuRk->setTapeRedirector(tapeOutFile);
    cpu->addHook(tapeOutHookEmuRk);

    CloseFileHook* closeFileHookEmuRk = addDevice<CloseFileHook>(0xFF18);
    closeFileHookEmuRk->setSignature("FB3A61F6E604");
    closeFileHookEmuRk->addTapeRedirector(tapeInFile);
    closeFileHookEmuRk->addTapeRedirector(tapeOutFile);
    cpu->addHook(closeFileHookEmuRk);

    SRam* ramDiskMem = addDevice<SRam>(0x40000);
    addrSpace->attachRamDisk(0, ramDiskMem);

    RamDisk* ramDisk = addDevice<RamDisk>(1, 0x40000);
    ramDisk->setFilter("Файлы RAM-диска Вектора (*.edd)|*.edd;*.EDD|Все файлы (*.*)|*");
    ramDisk->attachPage(0, ramDiskMem);
    m_ramDisk = ramDisk;

    VectorRamDiskSelector* ramDiskSelector = addDevice<VectorRamDiskSelector>();
    ramDiskSelector->attachVectorAddrSpace(addrSpace);
    ramDiskSelector->setDiskNum(0);
    ioAddrSpace->addRange(0x10, 0x10, ramDiskSelector);

    SRam* ramDiskMem2 = addDevice<SRam>(0x40000);
    addrSpace->attachRamDisk(1, ramDiskMem2);

    RamDisk* ramDisk2 = addDevice<RamDisk>(1, 0x40000);
    ramDisk2->setLabel("EDD2");
    ramDisk2->setFilter("Файлы RAM-диска Вектора (*.edd)|*.edd;*.EDD|Все файлы (*.*)|*");
    ramDisk2->attachPage(0, ramDiskMem2);
    m_ramDisk2 = ramDisk2;

    VectorRamDiskSelector* ramDiskSelector2 = addDevice<VectorRamDiskSelector>();
    ramDiskSelector2->attachVectorAddrSpace(addrSpace);
    ramDiskSelector2->setDiskNum(1);
    ioAddrSpace->addRange(0x11, 0x11, ramDiskSelector2);

    VectorCpuWaits* cpuWaits = addDevice<VectorCpuWaits>();
    cpu->attachCpuWaits(cpuWaits);

    m_tapeHooks = {
        tapeOutHookBas, tapeInHookBas, closeFileHookBas,
        tapeOutHookMon, tapeInHookMon, closeFileHookMon,
        tapeOutHookEmuRk, tapeInHookEmuRk, closeFileHookEmuRk,
        skipHookMon
    };

    init();

    reset();

    if (m_window)
        m_window->show();
}


void VectorCore::init()
{
    for (const auto& device : m_devices)
        device->init();
}


void VectorCore::shutdown()
{
    for (const auto& device : m_devices)
        device->shutdown();
}


void VectorCore::reset()
{
    for (const auto& device : m_devices)
        device->reset();
    m_intReq = false;
    m_intsEnabled = false;
}


VectorCore::~VectorCore()
{
    shutdown();
    for (auto& device : m_devices)
        device.reset();
}


void VectorCore::sysReq(SysReq sr)
{
    switch (sr) {
        case SR_RESET:
            reset();
            break;
        case SR_QUERTY:
            if (m_kbdLayout) {
                m_kbdLayout->setQwertyMode();
            }
            break;
        case SR_JCUKEN:
            if (m_kbdLayout) {
                m_kbdLayout->setJcukenMode();
            }
            break;
        case SR_SMART:
            if (m_kbdLayout) {
                m_kbdLayout->setSmartMode();
            }
            break;
        case SR_FONT:
            if (m_renderer) {
                m_renderer->toggleRenderingMethod();
            }
            break;
        case SR_CROPTOVISIBLE:
            if (m_renderer) {
                m_renderer->toggleCropping();
            }
            break;
        case SR_COLOR:
            if (m_renderer) {
                m_renderer->toggleColorMode();
            }
            break;
        case SR_COPYTXT:
            if (m_renderer) {
                const char* text = m_renderer->getTextScreen();
                if (text)
                    palCopyTextToClipboard(text);
            }
            break;
        case SR_PASTE:
            if (m_kbdTapper && m_kbdLayout->getMode() == KbdLayout::KLM_SMART) {
                m_kbdTapper->typeText(palGetTextFromClipboard());
            }
            break;
        case SR_DISKA:
            // open disk A image
            if (m_diskA)
                m_diskA->chooseFile();
            break;
        case SR_DISKB:
            // open disk B image
            if (m_diskB)
                m_diskB->chooseFile();
            break;
        case SR_HDD:
            // open HDD/CF image
            if (m_hdd)
                m_hdd->chooseFile();
        break;
        case SR_LOAD:
            if (m_loader) {
                m_loader->chooseAndLoadFile();
            }
            break;
        case SR_LOADRUN:
            if (m_loader) {
                m_loader->chooseAndLoadFile(true);
            }
            break;
        case SR_DEBUG:
            // show debugger
            g_emulation->debugRequest(m_cpu);
            break;
        case SR_OPENRAMDISK:
            if (m_ramDisk)
                m_ramDisk->openFile();
            break;
        case SR_SAVERAMDISKAS:
            if (m_ramDisk)
                m_ramDisk->saveFileAs();
            break;
        case SR_OPENRAMDISK2:
            if (m_ramDisk2)
                m_ramDisk2->openFile();
            break;
        case SR_SAVERAMDISK2AS:
            if (m_ramDisk2)
                m_ramDisk2->saveFileAs();
            break;
        case SR_TAPEHOOK:
            if (!m_tapeHooks.empty()) {
                bool enabled = !m_tapeHooks.front()->getEnabled();
                for (CpuHook* hook : m_tapeHooks)
                    hook->setEnabled(enabled);
            }
            break;
        default:
            break;
    }
    g_emulation->resetKeys(nullptr);
}


void VectorCore::processKey(PalKeyCode keyCode, bool isPressed, unsigned unicodeKey)
{
    emuLog << "VectorCore::processKey " << to_string(keyCode) << " / " << isPressed << "\n";
    if (m_kbdLayout)
        m_kbdLayout->processKey(keyCode, isPressed, unicodeKey);
}


void VectorCore::resetKeys()
{
    if (m_kbdLayout)
        m_kbdLayout->resetKeys();
}


void VectorCore::mouseDrag(int x, int y)
{
    if (m_renderer)
        m_renderer->mouseDrag(x, y);
}


bool VectorCore::loadFile(string fileName, bool run)
{
    if (m_loader) {
        m_loader->loadFile(fileName, run);
        return true;
    }
    return false;
}


bool VectorCore::assignDiskAFileName(const std::string& fileName)
{
    return m_diskA && m_diskA->assignFileName(fileName);
}
