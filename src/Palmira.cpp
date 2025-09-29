/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2022-2024
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

#include "Globals.h"
#include "Palmira.h"
#include "Emulation.h"
#include "EmuWindow.h"
#include "Memory.h"
#include "AddrSpace.h"
#include "Crt8275.h"

using namespace std;


void PalmiraCore::vrtc(bool isActive)
{
    static uint8_t cnt = 0;
    if (isActive) {
        if (cnt == 0)
            m_crtRenderer->renderFrame();
        cnt = (cnt + 1) & 1;
    }

    if (isActive)
        g_emulation->screenUpdateReq();
}


void PalmiraCore::draw()
{
    m_window->drawFrame(m_crtRenderer->getPixelData());
    m_window->endDraw();
}


void PalmiraCore::attachCrtRenderer(Crt8275Renderer* crtRenderer)
{
    m_crtRenderer = crtRenderer;
}


bool PalmiraCore::setProperty(const string& propertyName, const EmuValuesList& values)
{
    if (PlatformCore::setProperty(propertyName, values))
        return true;

    if (propertyName == "crtRenderer") {
        attachCrtRenderer(static_cast<Crt8275Renderer*>(g_emulation->findObject(values[0].asString())));
        return true;
    }
    return false;
}


PalmiraRenderer::PalmiraRenderer()
{
    m_useAltFont = false;

    m_fntCharWidth = 8;
    m_fntCharHeight = 16;
    m_fntLcMask = 0xF;

    graphics_set_duplicateLines(false);
    graphics_set_mode(GMODE_640_480);
}

uint8_t PalmiraRenderer::getCurFgColor(bool gpa0, bool gpa1, bool hglt)
{
    uint8_t res = (gpa1 ? 0b001 : 0) | (gpa0 ? 0b010 : 0) | (hglt ? 0b100 : 0);
    return res == 0 ? 0b111 : res;
}

void PalmiraRenderer::primaryRenderFrame() {
    calcAspectRatio(m_fntCharWidth);

    const Frame* frame = m_crt->getFrame();

    int nRows = frame->nRows;
    int nLines = frame->nLines;
    int nChars = frame->nCharsPerRow;

    m_sizeX = nChars * m_fntCharWidth;
    if (m_sizeX & 7) m_sizeX += 8 - (m_sizeX & 7); // adjust X to be divisionable to 8
    m_sizeY = nRows * nLines;

    m_dataSize = (m_sizeY * m_sizeX) >> 1;
    if (m_dataSize > m_bufSize) {
#if LOG
    emuLog << "m_dataSize: " << to_string(m_dataSize) << "; m_sizeX: " << to_string(m_sizeX) << "; m_sizeY: " << to_string(m_sizeY) << "\n";
#endif
        if (m_pixelData)
            delete[] m_pixelData;
        m_pixelData = new uint8_t [m_dataSize];
        memset(m_pixelData, 0, m_dataSize);
        m_bufSize = m_dataSize;
    }
    Crt4Bit rowPtr(m_pixelData);

    for (int row = 0; row < nRows; row++) {
        Crt4Bit chrPtr = rowPtr;
        bool curLten[16];
        memset(curLten, 0, sizeof(curLten));
        for (int chr = 0; chr < nChars; chr++) {
            Symbol symbol = frame->symbols[row][chr];
            Crt4Bit linePtr = chrPtr;
            bool hglt;
            if (!m_hgltOffset || (chr == nChars - 1))
                hglt = symbol.symbolAttributes.hglt();
            else
                hglt = frame->symbols[row][chr+1].symbolAttributes.hglt();
            bool gpa0, gpa1;
            if (!m_gpaOffset || (chr == nChars - 1)) {
                gpa0 = symbol.symbolAttributes.gpa0();
                gpa1 = symbol.symbolAttributes.gpa1();
            }
            else {
                gpa0 = frame->symbols[row][chr+1].symbolAttributes.gpa0();
                gpa1 = frame->symbols[row][chr+1].symbolAttributes.gpa1();
            }
            bool rvv;
            if (!m_rvvOffset || (chr == nChars - 1))
                rvv = symbol.symbolAttributes.rvv();
            else
                rvv = frame->symbols[row][chr+1].symbolAttributes.rvv();
            const uint8_t* fntPtr = getCurFontPtr(gpa0, gpa1, hglt);
            uint8_t fgColor = getCurFgColor(gpa0, gpa1, hglt);
            for (int ln = 0; ln < nLines; ln++) {
                int lc;
                if (!frame->isOffsetLineMode)
                    lc = ln;
                else
                    lc = ln != 0 ? ln - 1 : nLines - 1;
                bool vsp = symbol.vsp(ln);
                bool lten;
                if (!m_ltenOffset || (chr == nChars - 1))
                    lten = symbol.lten(ln);
                else
                    lten = frame->symbols[row][chr+1].lten(ln);
                curLten[ln] = m_dashedLten ? lten && !curLten[ln] : lten;
                uint16_t fntLine = fntPtr[symbol.chr * m_fntCharHeight + (lc & m_fntLcMask)] << (8 - m_fntCharWidth);
                for (int pt = 0; pt < m_fntCharWidth; pt++) {
                    bool v = curLten[ln] || !(vsp || (fntLine & 0x80));
                    if (rvv && m_useRvv)
                        v = !v;
                    linePtr.set4Bit(pt, v ? fgColor : 0);
                    fntLine <<= 1;
                }
                if (m_dashedLten && (curLten[ln] || !(vsp || (fntLine & 0x400))))
                    curLten[ln] = true;
                linePtr += m_sizeX;
            }
            chrPtr += m_fntCharWidth;
        }
        rowPtr += nLines * m_sizeX;
    }
    graphics_set_4bit_buffer(m_pixelData, m_sizeX, m_sizeY);
}

const uint8_t* PalmiraRenderer::getCurFontPtr(bool, bool, bool)
{
    return m_useExtFont ? m_extFontPtr : (m_font + (m_fontNumber * 2048));
}


void PalmiraRenderer::attachFontRam(Ram* fontRam)
{
    m_extFontPtr = fontRam->getDataPtr();
}


void PalmiraRenderer::setColorMode(Rk86ColorMode)
{
    m_colorMode = RCM_COLOR1;
    m_dashedLten = true;
}


bool PalmiraRenderer::setProperty(const string& propertyName, const EmuValuesList& values)
{
    if (Rk86Renderer::setProperty(propertyName, values))
        return true;

    if (propertyName == "fontRam") {
        attachFontRam(static_cast<Ram*>(g_emulation->findObject(values[0].asString())));
        return true;
    }

    return false;
}


string PalmiraRenderer::getPropertyStringValue(const string& propertyName)
{
    if (propertyName == "altRenderer") {
        return "";
    }

    return Rk86Renderer::getPropertyStringValue(propertyName);
}


bool PalmiraConfigRegister::setProperty(const string& propertyName, const EmuValuesList& values)
{
    if (AddressableDevice::setProperty(propertyName, values))
        return true;

    if (propertyName == "crtRenderer") {
        m_renderer = static_cast<PalmiraRenderer*>(g_emulation->findObject(values[0].asString()));
        return true;
    } else if (propertyName == "lowerMemMapper") {
        m_lowerMemMapper = static_cast<AddrSpaceMapper*>(g_emulation->findObject(values[0].asString()));
        return true;
    } else if (propertyName == "upperMemMapper") {
        m_upperMemMapper = static_cast<AddrSpaceMapper*>(g_emulation->findObject(values[0].asString()));
        return true;
    } else if (propertyName == "fontMemMapper") {
        m_fontMemMapper = static_cast<AddrSpaceMapper*>(g_emulation->findObject(values[0].asString()));
        return true;
    }
    return false;
}


void PalmiraConfigRegister::writeByte(int, uint8_t value)
{
    m_renderer->setExtFontMode(value & 0x40);
    m_fontMemMapper->setCurPage(value & 0x40 ? 1 : 0);
    m_renderer->setFontSetNum((value & 0x0E) >> 1);

    m_lowerMemMapper->setCurPage(value & 1);
    m_upperMemMapper->setCurPage(value & 0x80 ? 0 : 1);
}
