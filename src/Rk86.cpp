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

#include "Rk86.h"
#include "Emulation.h"
#include "Globals.h"
#include "EmuWindow.h"
#include "SoundMixer.h"
#include "Crt8275.h"

using namespace std;


void Rk86Core::vrtc(bool isActive)
{
    if (isActive) {
        m_crtRenderer->renderFrame();
    }

    if (isActive)
        g_emulation->screenUpdateReq();
}


void Rk86Core::draw()
{
    m_window->drawFrame(m_crtRenderer->getPixelData());
    m_window->endDraw();
}


void Rk86Core::inte(bool isActive)
{
    m_beepSoundSource->setValue(isActive ? 1 : 0);
}


void Rk86Core::attachCrtRenderer(Crt8275Renderer* crtRenderer)
{
    m_crtRenderer = crtRenderer;
}


bool Rk86Core::setProperty(const string& propertyName, const EmuValuesList& values)
{
    if (PlatformCore::setProperty(propertyName, values))
        return true;

    if (propertyName == "crtRenderer") {
        attachCrtRenderer(static_cast<Crt8275Renderer*>(g_emulation->findObject(values[0].asString())));
        return true;
    } else if (propertyName == "beepSoundSource") {
        m_beepSoundSource = static_cast<GeneralSoundSource*>(g_emulation->findObject(values[0].asString()));
        return true;
    }
    return false;
}


Rk86Renderer::Rk86Renderer()
{
    m_fntCharWidth = 6;
    m_fntCharHeight = 8;
    m_fntLcMask = 0x7;

    m_ltenOffset = false;
    m_rvvOffset  = false;
    m_hgltOffset = true;
    m_gpaOffset  = true;

    m_useRvv     = true;

    m_customDraw = false;
}

uint8_t Rk86Renderer::getCurFgColor(bool gpa0, bool gpa1, bool hglt)
{
    switch (m_colorMode) {
        case RCM_COLOR1:
             {
                uint8_t res = (gpa1 ? 0b100 : 0) | (gpa0 ? 0b010 : 0) | (hglt ? 0b001 : 0);
                if (res == 0)
                    res = 0b111;
                return res;
             }
        case RCM_COLOR2:
            return (gpa0 ? 0 : 0b001) | (gpa1 ? 0 : 0b010) | (hglt ? 0 : 0b100);
        case RCM_COLOR3:
            return (gpa0 ? 0 : 0b100) | (gpa1 ? 0 : 0b010) | (hglt ? 0 : 0b001);
        //case RCM_MONO_SIMPLE:
        //case RCM_MONO:
        default:
            return 1;
    }
}


uint8_t Rk86Renderer::getCurBgColor(bool, bool, bool)
{
    return 0;
}

void Rk86Renderer::primaryRenderFrame() {
    calcAspectRatio(m_fntCharWidth);

    const Frame* frame = m_crt->getFrame();

    int nRows = frame->nRows;
    int nLines = frame->nLines;
    int nChars = frame->nCharsPerRow;

    m_sizeX = nChars * m_fntCharWidth;
    if (m_sizeX & 7) m_sizeX += 8 - (m_sizeX & 7); // adjust X to be divisionable to 8
    m_sizeY = nRows * nLines;

    m_dataSize = (m_sizeY * m_sizeX) >> 3;
    if (m_dataSize > m_bufSize) {
        if (m_pixelData)
            delete[] m_pixelData;
        if (m_pixelData2)
            delete[] m_pixelData2;
        if (m_pixelData3)
            delete[] m_pixelData3;
        m_pixelData = new uint8_t [m_dataSize];
        m_pixelData2 = new uint8_t [m_dataSize];
        m_pixelData3 = new uint8_t [m_dataSize];
        memset(m_pixelData, 0, m_dataSize);
        memset(m_pixelData2, 0, m_dataSize);
        memset(m_pixelData3, 0, m_dataSize);
        m_bufSize = m_dataSize;
    }
    if (m_colorMode == RCM_COLOR1 || m_colorMode == RCM_COLOR2 || m_colorMode == RCM_COLOR3) {
        Crt3Bit rowPtr = { m_pixelData, m_pixelData2, m_pixelData3, 0 };
        for (int row = 0; row < nRows; row++) {
            Crt3Bit chrPtr = rowPtr;
            bool curLten[16];
            memset(curLten, 0, sizeof(curLten));
            for (int chr = 0; chr < nChars; chr++) {
                Symbol symbol = frame->symbols[row][chr];
                Crt3Bit linePtr = chrPtr;
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
                uint8_t bgColor = getCurBgColor(gpa0, gpa1, hglt);
                for (int ln = 0; ln < nLines; ln++) {
                    int lc;
                    if (!frame->isOffsetLineMode)
                        lc = ln;
                    else
                        lc = ln != 0 ? ln - 1 : nLines - 1;
                    bool vsp = symbol.symbolLineAttributes[ln].vsp();
                    bool lten;
                    if (!m_ltenOffset || (chr == nChars - 1))
                        lten = symbol.symbolLineAttributes[ln].lten();
                    else
                        lten = frame->symbols[row][chr+1].symbolLineAttributes[ln].lten();
                    curLten[ln] = m_dashedLten ? lten && !curLten[ln] : lten;
            ///    if (!m_customDraw) {
                    uint16_t fntLine = fntPtr[symbol.chr * m_fntCharHeight + (lc & m_fntLcMask)] << (8 - m_fntCharWidth);

                    for (int pt = 0; pt < m_fntCharWidth; pt++) {
                        bool v = curLten[ln] || !(vsp || (fntLine & 0x80));
                        if (rvv && m_useRvv)
                            v = !v;
                        linePtr.set3Bit(pt, v ? fgColor : bgColor);
                        fntLine <<= 1;
                    }
                    if (m_dashedLten && (curLten[ln] || !(vsp || (fntLine & 0x400))))
                        curLten[ln] = true;
            ///    } else
            ///        customDrawSymbolLine(linePtr, symbol.chr, lc, lten, vsp, rvv, gpa0, gpa1, hglt);
                    linePtr += m_sizeX;
                }
                chrPtr += m_fntCharWidth;
            }
            rowPtr += nLines * m_sizeX;
        }
        graphics_set_1bit_buffer3(m_pixelData, m_pixelData2, m_pixelData3, m_sizeX, m_sizeY);
        return;
    }
    memcpy(m_pixelData2, m_pixelData, m_dataSize);
    memset(m_pixelData, 0, m_dataSize);
    Crt1Bit rowPtr = { m_pixelData, 0 };

    for (int row = 0; row < nRows; row++) {
        Crt1Bit chrPtr = rowPtr;
        bool curLten[16];
        memset(curLten, 0, sizeof(curLten));
        for (int chr = 0; chr < nChars; chr++) {
            Symbol symbol = frame->symbols[row][chr];
            Crt1Bit linePtr = chrPtr;

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
            uint8_t bgColor = getCurBgColor(gpa0, gpa1, hglt);

            for (int ln = 0; ln < nLines; ln++) {
                int lc;
                if (!frame->isOffsetLineMode)
                    lc = ln;
                else
                    lc = ln != 0 ? ln - 1 : nLines - 1;

                bool vsp = symbol.symbolLineAttributes[ln].vsp();

                bool lten;
                if (!m_ltenOffset || (chr == nChars - 1))
                    lten = symbol.symbolLineAttributes[ln].lten();
                else
                    lten = frame->symbols[row][chr+1].symbolLineAttributes[ln].lten();

                curLten[ln] = m_dashedLten ? lten && !curLten[ln] : lten;

                if (!m_customDraw) {
                    uint16_t fntLine = fntPtr[symbol.chr * m_fntCharHeight + (lc & m_fntLcMask)] << (8 - m_fntCharWidth);

                    for (int pt = 0; pt < m_fntCharWidth; pt++) {
                        bool v = curLten[ln] || !(vsp || (fntLine & 0x80));
                        if (rvv && m_useRvv)
                            v = !v;
                        linePtr.setBit(pt, v ? fgColor : bgColor);
                        fntLine <<= 1;
                    }
                    if (m_dashedLten && (curLten[ln] || !(vsp || (fntLine & 0x400))))
                        curLten[ln] = true;
                } else
                    customDrawSymbolLine(linePtr, symbol.chr, lc, lten, vsp, rvv, gpa0, gpa1, hglt);
                linePtr += m_sizeX;
            }
            chrPtr += m_fntCharWidth;
        }
        rowPtr += nLines * m_sizeX;
    }
    graphics_set_1bit_buffer(m_pixelData2, m_sizeX, m_sizeY);
}

const uint8_t* Rk86Renderer::getCurFontPtr(bool, bool, bool)
{
    return m_font;
}


const uint8_t* Rk86Renderer::getAltFontPtr(bool, bool, bool)
{
    return m_altFont;
}


wchar_t Rk86Renderer::getUnicodeSymbol(uint8_t chr, bool, bool, bool)
{
    return c_rkSymbols[chr];
}


void Rk86Renderer::setColorMode(Rk86ColorMode mode) {
    m_colorMode = mode;
    m_dashedLten = mode == RCM_MONO_ORIG;
    m_useRvv = mode != RCM_MONO_ORIG;
}


void Rk86Renderer::toggleColorMode()
{
    if (m_colorMode == RCM_MONO_ORIG)
        setColorMode(RCM_MONO);
    else if (m_colorMode == RCM_MONO)
        setColorMode(RCM_COLOR1);
    else if (m_colorMode == RCM_COLOR1)
        setColorMode(RCM_COLOR2);
    else if (m_colorMode == RCM_COLOR2)
        setColorMode(RCM_COLOR3);
    else if (m_colorMode == RCM_COLOR3)
        setColorMode(RCM_MONO_ORIG);
}


bool Rk86Renderer::setProperty(const string& propertyName, const EmuValuesList& values)
{
    if (Crt8275Renderer::setProperty(propertyName, values))
        return true;

    if (propertyName == "colorMode") {
        if (values[0].asString() == "original")
            setColorMode(RCM_MONO_ORIG);
        else if (values[0].asString() == "mono")
            setColorMode(RCM_MONO);
        else if (values[0].asString() == "color1")
            setColorMode(RCM_COLOR1);
        else if (values[0].asString() == "color2")
            setColorMode(RCM_COLOR2);
        else if (values[0].asString() == "color3")
            setColorMode(RCM_COLOR3);
        else
            return false;
        return true;
    }

    return false;
}


string Rk86Renderer::getPropertyStringValue(const string& propertyName)
{
    string res;

    res = Crt8275Renderer::getPropertyStringValue(propertyName);
    if (res != "")
        return res;

    if (propertyName == "colorMode") {
        switch (m_colorMode) {
            case RCM_MONO_ORIG:
                return "original";
            case RCM_MONO:
                return "mono";
            case RCM_COLOR1:
                return "color1";
            case RCM_COLOR2:
                return "color2";
        }
    }

    return "";
}


RkPixeltronRenderer::RkPixeltronRenderer()
{
    m_fntCharWidth = 6;

    m_customDraw = true;

    m_ltenOffset = false;
    m_gpaOffset  = false;
}

void RkPixeltronRenderer::customDrawSymbolLine(Crt1Bit linePtr, uint8_t symbol, int line, bool lten, bool vsp, bool /*rvv*/, bool gpa0, bool /*gpa1*/, bool /*hglt*/)
{
    int offset = (gpa0 ? 0x400 : 0) + symbol * 8 + (line & 7);
    uint8_t bt = ~m_font[offset];
    if (lten)
        bt = 0x3f;
    else if (vsp)
        bt = 0x00;

    uint8_t fgColor = (bt & 0x40) ? RGB888(0xC0, 0xC0, 0xC0) : RGB888(0xFF, 0xFF, 0xFF);
    uint8_t bgColor = (bt & 0x80) ? RGB888(0x40, 0x40, 0x40) : RGB888(0, 0, 0);

    for (int i = 0; i < 6; i++) {
        linePtr.setBit(i, (bt & 0x20) ? fgColor : bgColor); // TODO: 2-bits?
        bt <<= 1;
    }
}
