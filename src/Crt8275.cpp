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

// Crt8275.cpp

// Реализация контроллера CRT КР580ВГ75

#include <sstream>
#include <stdint.h>
#include <stdlib.h>

#include "Globals.h"
#include "Crt8275.h"
#include "Cpu.h"
#include "Dma8257.h"
#include "Platform.h"
#include "PlatformCore.h"
#include "Emulation.h"

using namespace std;

////////////////////////////
// Crt8275 Implementation //
////////////////////////////


// Common Methods Implementation


void Crt8275::attachCore(PlatformCore* core)
{
    m_core = core;
}


void Crt8275::init()
{
    m_cpuKDiv = m_platform->getCpu()->getKDiv();
}


void Crt8275::reset()
{
    //m_curClock = -1;
    m_crtCmd = CC_LOADCURSOR;
    m_nRows = 1;
    m_nLines = 1;
    m_isSpacedRows = false;
    m_nCharsPerRow = 1;
    m_undLine = 0;
    m_isOffsetLine = false;
    m_isTransparentAttr = false;
    m_cursorBlinking = true;
    m_cursorUnderline = false;
    m_nVrRows = 1;
    m_nHrChars = 2;
    m_cursorPos = 0;
    m_cursorRow = 0;
    m_isIntsEnabled = false;
    m_statusReg = 0;
    m_cmdReg = 0;
    m_isCompleteCommand = true;
    m_isRasterStarted = false;
    for (int i=0; i<4; i++)
        m_resetParam[i] = 0;
}



void Crt8275::attachDMA(Dma8257* dma, int channel)
{
    m_dma = dma;
    m_dmaChannel = channel;
}



// Buffer Display Related Methods

void Crt8275::prepareFrame()
{
    m_curUnderline = false;
    m_curReverse = false;
    m_curBlink = false;
    m_curHighlight = false;
    m_curGpa1 = false;
    m_curGpa0 = false;
    m_isBlankedToTheEndOfScreen = false;

    m_frameCount++;

    // alt renderer fields
    m_frame.cursorRow = m_cursorRow;
    m_frame.cursorPos = m_cursorPos;
    m_frame.frameCount = m_frameCount;
    m_frame.cursorBlinking = m_cursorBlinking;
    m_frame.cursorUnderline = m_cursorUnderline;
}


void Crt8275::displayBuffer()
{
    m_frame.nRows = m_nRows;
    m_frame.nLines = m_nLines;
    m_frame.nCharsPerRow = m_nCharsPerRow;
    m_frame.isOffsetLineMode = m_isOffsetLine;

    // ffame format check fields
    m_frame.nHrChars = m_nHrChars;
    m_frame.nVrRows = m_nVrRows;

    bool isBlankedToTheEndOfRow = false;

    int fifoPos = 0;

    for (int i = 0; i < m_nCharsPerRow; i++) {
        uint8_t chr = m_rowBuf[i % 80];

        if (isBlankedToTheEndOfRow || m_isBlankedToTheEndOfScreen || !m_isDisplayStarted)
            chr = 0;

        if (m_isTransparentAttr && ((chr & 0xC0) == 0x80)) {
            // Transparent Field Attribute Code
            m_curUnderline = chr & 0x20;
            m_curReverse = chr & 0x10;
            m_curBlink = chr & 0x02;
            m_curHighlight = chr & 0x01;
            m_curGpa0 = chr & 0x04;
            m_curGpa1 = chr & 0x08;

            chr = m_fifo[fifoPos++];
            fifoPos &= 0x0f;
        }

        Symbol& si = m_frame.symbols[m_curRow][i];
        si.chr = chr & 0x7F;
        if (!m_isDisplayStarted || isBlankedToTheEndOfRow || m_isBlankedToTheEndOfScreen) {
            si.symbolAttributes.attrs(0);
//            si.symbolAttributes.rvv(false);
//            si.symbolAttributes.hglt(false); // ?
//            si.symbolAttributes.gpa0(false); // ?
//            si.symbolAttributes.gpa1(false); // ?
            for (int j = 0; j < m_nLines; j++) {
                SymbolLineAttributes& sia = si.symbolLineAttributes[j];
                sia.vsp(true);
                sia.lten(false);
            }
        } else if (chr < 0x80) {
            // Ordinary symbol
            si.symbolAttributes.attrs(m_curReverse, m_curHighlight, m_curGpa0, m_curGpa1);
            ///si.symbolAttributes.rvv(m_curReverse);
            ///si.symbolAttributes.hglt(m_curHighlight);
            ///si.symbolAttributes.gpa0(m_curGpa0);
            ///si.symbolAttributes.gpa1(m_curGpa1);
            for (int j = 0; j < m_nLines; j++) {
                SymbolLineAttributes& sia = si.symbolLineAttributes[j];
                sia.vsp(m_curBlink && (m_frameCount & 0x10));
                sia.lten(false);
                if ((m_undLine > 7) && ((j == 0) || (j == m_nLines - 1)))
                    sia.vsp(true);
            }
            if (m_curUnderline) {
                SymbolLineAttributes& sia = si.symbolLineAttributes[m_undLine];
                sia.lten(true);
                if (m_curBlink)
                    sia.lten(!(m_frameCount & 0x10));
            }
        } else if ((chr & 0xC0) == 0x80) {
            // Field Attribute Code
            m_curUnderline = chr & 0x20;
            m_curReverse = chr & 0x10;
            m_curBlink = chr & 0x02;
            m_curHighlight = chr & 0x01;
            m_curGpa0 = chr & 0x04;
            m_curGpa1 = chr & 0x08;
            for (int j = 0; j < m_nLines; j++) {
                SymbolLineAttributes& sia = si.symbolLineAttributes[j];
                sia.vsp(true);
                sia.lten(false);
            }
            si.symbolAttributes.attrs(0, m_curHighlight, m_curGpa0, m_curGpa1);
///            si.symbolAttributes.rvv(false);
   ///         si.symbolAttributes.hglt(m_curHighlight); // ?
      ///      si.symbolAttributes.gpa0(m_curGpa0); //?? уточнить!!!
         ///   si.symbolAttributes.gpa1(m_curGpa1); //?? уточнить!!!
        } else if ((chr & 0xC0) == 0xC0 && (chr & 0x30) != 0x30) {
            // Character Attribute
            int cccc = (chr & 0x3C) >> 2;
            for (int j = 0; j < m_nLines; j++) {
                SymbolLineAttributes& sia = si.symbolLineAttributes[j];
                if (j < m_undLine) {
                    sia.vsp( m_cCharAttrVsp[cccc][0] || ((chr & 0x02) && (m_frameCount & 0x10)) );
                    sia.lten( 0 );
                } else if (j > m_undLine) {
                    sia.vsp ( m_cCharAttrVsp[cccc][1] || ((chr & 0x02) && (m_frameCount & 0x10)) );
                    sia.lten( 0 );
                } else {// j == _undLine
                    sia.vsp ( (chr & 0x02) && (m_frameCount & 0x10) );
                    sia.lten ( m_cCharAttrLten[cccc] && !((chr & 0x02) && (m_frameCount & 0x10)) );
                }
            }
///            si.symbolAttributes.hglt(chr & 0x01);
   ///         si.symbolAttributes.rvv(m_curReverse);
      ///      si.symbolAttributes.gpa0(m_curGpa0);
         ///   si.symbolAttributes.gpa1(m_curGpa1);
            si.symbolAttributes.attrs(m_curReverse, chr & 0x01, m_curGpa0, m_curGpa1);
        } else {
            // Special Control Characters
            if (chr & 0x02) {
                // End of Screen (stop or not stop DMA)
                m_isBlankedToTheEndOfScreen = true;
            } else {
                // End of Row (stop or not stop DMA)
                isBlankedToTheEndOfRow = true;
            }
            si.symbolAttributes.attrs(m_curReverse, m_curHighlight, m_curGpa0, m_curGpa1);
            ///si.symbolAttributes.rvv(m_curReverse);
            ///si.symbolAttributes.hglt(m_curHighlight);
            ///si.symbolAttributes.gpa0(m_curGpa0);
            ///si.symbolAttributes.gpa1(m_curGpa1);
            for (int j = 0; j < m_nLines; j++) {
                SymbolLineAttributes& sia = si.symbolLineAttributes[j];
                sia.vsp  ( true );
                sia.lten ( false);
            }
            if (m_curUnderline)
                si.symbolLineAttributes[m_undLine].lten ( true);
        }
    }
    if (m_isDisplayStarted && (m_curRow == m_cursorRow) && (m_cursorPos < 80)) {
        if (m_cursorUnderline) {
            m_frame.symbols[m_cursorRow][m_cursorPos].symbolLineAttributes[m_undLine].lten ( !m_cursorBlinking || (m_frameCount & 0x08) );
        } else {
            if (!m_cursorBlinking || (m_frameCount & 0x08))
                m_frame.symbols[m_cursorRow][m_cursorPos].symbolAttributes.rvv(
                    !(m_frame.symbols[m_cursorRow][m_cursorPos].symbolAttributes.rvv())
                );
        }
    }
}



// AddressableDevice Methods Implemantation

void Crt8275::writeByte(int addr, uint8_t value)
{
    addr &= 1;
    switch (addr) {
        case 0:
            // Writing Parameter Register
            switch (m_crtCmd) {
                case CC_RESET:
                    switch (m_parameterNum++) {
                        case 0:
                            // SHHHHHHH
                            m_isSpacedRows = (value & 0x80) != 0;
                            m_nCharsPerRow = (value & 0x7f) + 1;
                            m_isCompleteCommand = false;
                            m_statusReg |= 0x08; // reset IC flag
                            break;
                        case 1:
                            // VVRRRRRR
                            m_nVrRows = ((value & 0xc0) >> 6) + 1;
                            m_nRows = (value & 0x3f) + 1;
                            break;
                        case 2:
                            // UUUULLLL
                            m_undLine = ((value & 0xf0) >> 4);
                            m_nLines = (value & 0xf) + 1;
                            break;
                        case 3:
                            // MFCCZZZZ
                            m_isOffsetLine = (value & 0x80) != 0;
                            m_isTransparentAttr = (value & 0x40) == 0;
                            m_cursorBlinking = !(value & 0x20);
                            m_cursorUnderline = value & 0x10;
                            m_nHrChars = ((value & 0x0f) + 1) * 2;
                            m_isCompleteCommand = true;
                            m_statusReg &= ~0x08; // reset IC flag
                            m_parameterNum = 0;
                            break;
                        default:
                        ;
                        //! change status
                    }
                    break;
                case CC_LOADCURSOR:
                    switch (m_parameterNum) {
                        case 0:
                            // Char number
                            m_cursorPos = value & 0x7f;
                            m_parameterNum = 1;
                            m_isCompleteCommand = false;
                            m_statusReg |= 0x08; // set IC flag
                            break;
                        case 1:
                            // Row number
                            m_cursorRow = value & 0x3f;
                            m_parameterNum = 0;
                            m_isCompleteCommand = true;
                            m_statusReg &= ~0x08; // reset IC flag
                            break;
                    }
                    break;
                default:
                    break;
            }
            break;
        case 1:
            // Writing Command Register
            m_cmdReg = value;
            //m_statusReg |= m_isCompleteCommand ? 0 : 0x08;
            m_isCompleteCommand = true;
            m_statusReg &= ~0x08; // reset IC flag
            switch ((value & 0xE0) >> 5) {
                case 0:
                    // Reset
                    m_crtCmd = CC_RESET;
                    m_parameterNum = 0;
                    m_isCompleteCommand = false;
                    m_isDisplayStarted = false;
                    m_statusReg &= 0xBB; // reset IE and VE flags
                    m_statusReg |= 0x08; // set IC flag
                    m_isRasterStarted = true;
                    break;
                case 1:
                    // Start Display
                    m_statusReg |= 0x44; // VE & IE
                    m_isIntsEnabled = true;
                    m_isDisplayStarted = true;
                    m_isRasterStarted = true;
                    break;
                case 2:
                    // Stop Display
                    m_statusReg &= 0xFb;
                    //m_raster->stopRaster();
                    m_isDisplayStarted = false;
                    m_isRasterStarted = true;
                    break;
                case 3:
                    // Read Light Pen
                    m_crtCmd = CC_READLPEN;
                    m_parameterNum = 0;
                    m_isRasterStarted = true;
                    break;
                case 4:
                    // Load Cursor Position
                    m_crtCmd = CC_LOADCURSOR;
                    m_parameterNum = 0;
                    m_isCompleteCommand = false;
                    m_statusReg |= 0x08; // set IC flag
                    m_isRasterStarted = true;
                    break;
                case 5:
                    // Enable Interrupts
                    m_isIntsEnabled = true;
                    m_statusReg |= 0x40; // IE
                    m_isRasterStarted = true;
                    break;
                case 6:
                    // Disable Interrupts
                    m_isIntsEnabled = false;
                    m_statusReg &= ~0x40;
                    m_isRasterStarted = true;
                    break;
                case 7:
                    // Preset Counetrs
                    presetCounters();
                    m_isRasterStarted = false;
                    break;
                default:
                    break;
            }
            break;
        default:
            // Normally this never occurs
            break;
    }
}


uint8_t Crt8275::readByte(int nAddr)
{
    uint8_t value;
    nAddr &= 1;
    switch (nAddr) {
        case 0:
            // Reading Parameter Register
            switch (m_crtCmd) {
                case CC_RESET:
                    value = m_resetParam[m_parameterNum++];
                    if (m_parameterNum == 4) {
                            m_isCompleteCommand = false;
                            m_statusReg |= 0x08; // set IC flag
                            m_parameterNum = 0;
                    }
                    break;
                case CC_LOADCURSOR:
                    switch (m_parameterNum) {
                        case 0:
                            // Char number
                            value = m_cursorPos;
                            m_parameterNum = 1;
                            break;
                        default: //case 1:
                            // Row number
                            value = m_cursorRow;
                            m_parameterNum = 0;
                            m_isCompleteCommand = false;
                            m_statusReg |= 0x08; // set IC flag
                            break;
                    }
                    break;
                case CC_READLPEN:
                    switch (m_parameterNum) {
                        case 0:
                            // Char number
                            value = m_lpenX;
                            m_parameterNum = 1;
                            break;
                        default: //case 1:
                            // Row number
                            value = m_lpenY;
                            m_parameterNum = 0;
                            m_isCompleteCommand = false;
                            break;
                    }
                    break;
                default:
                    value = m_cmdReg & 0x7f; // umdocumented;
            }
            break;
        default: //case 1:
            // Reading Status Register
            value = m_statusReg;
            m_statusReg &= 0xc4;
            break;
    }
    return value;
}



// ActiveDevice Related Methods Implemantation

void Crt8275::operate()
{
    if (!m_isRasterStarted) {
        m_curClock += m_kDiv * 860; // some reasonable value as for RK ordinary mode
        return;
    }

    if (m_curRow < m_nRows) {
        while (!m_isDmaStoppedForFrame && !m_isDmaStoppedForRow) {
            uint8_t byte = 0;
            m_dma->dmaRequest(m_dmaChannel, byte, 0);
            if (putCharToBuffer(byte))
                break;
        }

        displayBuffer();

        m_curBufPos = 0;
        m_curFifoPos = 0;
        m_isNextCharToFifo = false;
        m_isDmaStoppedForRow = false;
    }

    if (m_curRow == m_nRows) {
        if (m_isIntsEnabled)
            m_statusReg |= 0x20; // actually should be at the beginning of the last display row
        m_core->vrtc(true);
    }

    if (++m_curRow >= m_nRows + m_nVrRows) {
        prepareFrame();
        presetCounters();
        m_core->vrtc(false);
    }

    m_curClock += m_kDiv * (m_nCharsPerRow + m_nHrChars) * m_nLines;
}


bool Crt8275::putCharToBuffer(uint8_t byte)
{
    if (m_isNextCharToFifo) {
        m_fifo[m_curFifoPos++] = byte & 0x7f;
        m_curFifoPos %= 16;
        if (m_curFifoPos == 0)
            m_statusReg |= 0x01; // FIFO Overrun
        m_isNextCharToFifo = false;
        if (m_curBufPos == m_nCharsPerRow) {
            // end of row
            return true;
        }
    } else {
        if (m_curBufPos < m_nCharsPerRow)
            m_rowBuf[m_curBufPos++ % 80] = byte;
        else {
            return true;
        }

        if ((byte & 0xf1) == 0xF1) {
            // stop DMA
            if (byte & 0x2)
                // end of screen - stop DMA
                m_isDmaStoppedForFrame = true;
            else
                // end of row - stop DMA
                m_isDmaStoppedForRow = true;
        } else if (m_isTransparentAttr && (byte & 0xc0) == 0x80)
            // field attribute code and transparent attribute is active
            m_isNextCharToFifo = true;
        else if (m_curBufPos == m_nCharsPerRow) {
            return true;
        }
    }

    return false;
}


void Crt8275::presetCounters()
{
    m_curRow = 0;
    m_isDmaStoppedForFrame = false;

    m_curBufPos = 0;
    m_curFifoPos = 0;
    m_isNextCharToFifo = false;
    m_isDmaStoppedForRow = false;
}


double Crt8275::getFrameRate()
{
    int chars = (m_nRows + m_nVrRows) * m_nLines * (m_nCharsPerRow + m_nHrChars);
    if (chars == 0)
        return 0.0;
    return double(g_emulation->getFrequency() / m_kDiv) / chars;
}


void Crt8275::setLpenPosition(int x, int y)
{
    m_lpenX = x + m_lpenCorrection;
    m_lpenY = y;
    m_statusReg |= 0x10;
}


bool Crt8275::setProperty(const string& propertyName, const EmuValuesList& values)
{
    if (AddressableDevice::setProperty(propertyName, values))
        return true;

    if (propertyName == "dma") {
        if (values[1].isInt()) {
            attachDMA(static_cast<Dma8257*>(g_emulation->findObject(values[0].asString())), values[1].asInt());
            return true;
        }
    } else if (propertyName == "core") {
        attachCore(static_cast<PlatformCore*>(g_emulation->findObject(values[0].asString())));
        return true;
    } else if (propertyName == "lpenCorrection") {
        m_lpenCorrection = values[0].asInt();
        return true;
    }

    return false;
}


string Crt8275::getDebugInfo()
{
    stringstream ss;
    ss << "CRT i8275:" << "\n";
    ss << "Row:" << m_nRows << " ";
    ss << "Ch:" << m_nCharsPerRow << "\n";
    ss << "Lc:" << m_nLines << " ";
    ss << "Ul:" << m_undLine << "\n";
    ss << "Vrc:" << m_nVrRows << " ";
    ss << "Hrc:" << m_nHrChars << "\n";
    ss << "CX:" << m_cursorPos << " ";
    ss << "CY:" << m_cursorRow << "\n";
    ss << "Trp" << (m_isTransparentAttr ? "+" : "-") << " ";
    ss << "OfsL" << (m_isOffsetLine ? "+" : "-") << "\n";
    ss << "Cur:" << (m_cursorUnderline ? "Ul" : "Bl");
    ss << (m_cursorBlinking ? "Bln" : "") << " ";
    ss << "R" << (m_isRasterStarted ? "+" : "-") << "\n";
    ss << "CurRow: ";
    if (m_curRow < m_nRows)
          ss << m_curRow;
    else
          ss << "VRTC";
    ss << "\n";
    return ss.str();
}
