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

#include "Apogey.h"
#include "Emulation.h"
#include "Globals.h"
#include "EmuWindow.h"
#include "Crt8275.h"

using namespace std;

ApogeyCore::ApogeyCore()
{
    //
}



ApogeyCore::~ApogeyCore()
{
   //
}



void ApogeyCore::vrtc(bool isActive)
{
    if (isActive) {
        m_crtRenderer->renderFrame();
    }

    if (isActive)
        g_emulation->screenUpdateReq();
}


void ApogeyCore::draw()
{
    m_window->drawFrame(m_crtRenderer->getPixelData());
    m_window->endDraw();
}


void ApogeyCore::inte(bool isActive)
{
    m_crtRenderer->setFontSetNum(isActive ? 1 : 0);
}


void ApogeyCore::attachCrtRenderer(Crt8275Renderer* crtRenderer)
{
    m_crtRenderer = crtRenderer;
}


bool ApogeyCore::setProperty(const string& propertyName, const EmuValuesList& values)
{
    if (PlatformCore::setProperty(propertyName, values))
        return true;

    if (propertyName == "crtRenderer") {
        attachCrtRenderer(static_cast<Crt8275Renderer*>(g_emulation->findObject(values[0].asString())));
        return true;
    }
    return false;
}

ApogeyRenderer::~ApogeyRenderer() {
        if (m_pixelData3)
            delete[] m_pixelData3;
}

ApogeyRenderer::ApogeyRenderer()
{
    m_fntCharWidth = 6;
    m_fntCharHeight = 8;
    m_fntLcMask = 0x7;

    m_ltenOffset = false;
    m_rvvOffset  = false;
    m_hgltOffset = false;
    m_gpaOffset  = false;

    m_useRvv     = true;

    m_customDraw = false;
}

uint8_t ApogeyRenderer::getCurFgColor(bool gpa0, bool gpa1, bool hglt)
{
    switch (m_colorMode) {
    case ColorMode::Mono:
        return hglt ? 0b11 : 0b10;
    default:
        return (gpa0 ? 0 : 0b100) | (gpa1 ? 0 : 0b010) | (hglt ? 0 : 0b001);
    }
}


uint8_t ApogeyRenderer::getCurBgColor(bool, bool, bool)
{
    return 0;
}


const uint8_t* ApogeyRenderer::getCurFontPtr(bool, bool, bool)
{
    return m_font + (m_fontNumber ? 1024 : 0);
}


const uint8_t* ApogeyRenderer::getAltFontPtr(bool, bool, bool)
{
    return m_altFont + (m_fontNumber ? (8+12+16)*128 : 0);
}

typedef struct {
    uint8_t* ptr1; // 0b101010
    uint8_t* ptr2; // 0b010101
    uint8_t bit;
    inline void operator +=(int a) {
        uint64_t p8 = (uint32_t)ptr1;
        p8 = (p8 << 3) + bit + a;
        uint8_t* p = (uint8_t*)(p8 >> 3);
        size_t shift = p - ptr1;
        ptr1 = p;
        ptr2 += shift;
        bit = (p8 & 7);
    }
    inline void set2Bits(uint8_t b) { // 0bHL
        bitWrite(*ptr1, bit, b & 1);        // 0b101010
        bitWrite(*ptr2, bit, (b >> 1) & 1); // 0b010101
    }
    inline void set2Bits(int a, uint8_t b) { // b - 0bHighLow
        uint64_t p8 = (uint32_t)ptr1;
        p8 = (p8 << 3) + bit + a;
        uint8_t* p = (uint8_t*)(p8 >> 3);
        uint8_t bi = p8 & 7;
        bitWrite(*p, bi, b & 1); // 0b101010
        size_t shift = p - ptr1;
        p = ptr2 + shift;
        bitWrite(*p, bi, (b >> 1) & 1); // 0b010101
    }
} Crt2BitGS;

void ApogeyRenderer::primaryRenderFrame()
{
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
        if (m_pixelData3) {
            delete[] m_pixelData3;
        }
        m_pixelData = new uint8_t [m_dataSize];
        m_pixelData2 = new uint8_t [m_dataSize];
        m_pixelData3 = new uint8_t [m_dataSize];
        memset(m_pixelData, 0, m_dataSize);
        memset(m_pixelData2, 0, m_dataSize);
        memset(m_pixelData3, 0, m_dataSize);
        m_bufSize = m_dataSize;
    }

    if(m_colorMode == ColorMode::Mono) {

    Crt2BitGS rowPtr = { m_pixelData, m_pixelData2, 0 };

    for (int row = 0; row < nRows; row++) {
        Crt2BitGS chrPtr = rowPtr;
        bool curLten[16];
        memset(curLten, 0, sizeof(curLten));
        for (int chr = 0; chr < nChars; chr++) {
            Symbol symbol = frame->symbols[row][chr];
            Crt2BitGS linePtr = chrPtr;

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
                        linePtr.set2Bits(pt, v ? fgColor : bgColor);
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

///    trimImage(m_fntCharWidth, nLines);
    graphics_set_1bit_buffer2(m_pixelData, m_pixelData2, m_sizeX, m_sizeY);
    return;
    }

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

///    trimImage(m_fntCharWidth, nLines);
    graphics_set_1bit_buffer3(m_pixelData, m_pixelData2, m_pixelData3, m_sizeX, m_sizeY);
}


wchar_t ApogeyRenderer::getUnicodeSymbol(uint8_t chr, bool, bool, bool)
{
    if (m_fontNumber == 0)
        return c_apogeySymbols[chr];
    else
        return chr ? L'·' : L' ';
}


void ApogeyRenderer::setColorMode(ColorMode colorMode)
{
    m_colorMode = colorMode;
    switch (colorMode) {
    case ColorMode::Color:
    case ColorMode::Grayscale:
        m_rvvOffset  = false;
        m_hgltOffset = false;
        m_gpaOffset  = false;
        break;
    case ColorMode::Mono:
        m_rvvOffset  = true;
        m_hgltOffset = true;
        m_gpaOffset  = true;
    }
}


void ApogeyRenderer::toggleColorMode()
{
    switch (m_colorMode) {
    case ColorMode::Mono:
        setColorMode(ColorMode::Color);
        break;
    case ColorMode::Color:
        setColorMode(ColorMode::Grayscale);
        break;
    case ColorMode::Grayscale:
        setColorMode(ColorMode::Mono);
    }
}


bool ApogeyRenderer::setProperty(const string& propertyName, const EmuValuesList& values)
{
    if (Crt8275Renderer::setProperty(propertyName, values))
        return true;

    if (propertyName == "colorMode") {
        if (values[0].asString() == "mono")
            setColorMode(ColorMode::Mono);
        else if (values[0].asString() == "color")
            setColorMode(ColorMode::Color);
        else if (values[0].asString() == "grayscale")
            setColorMode(ColorMode::Grayscale);
        else
            return false;
        return true;
    }

    return false;
}


string ApogeyRenderer::getPropertyStringValue(const string& propertyName)
{
    string res;

    res = Crt8275Renderer::getPropertyStringValue(propertyName);
    if (res != "")
        return res;

    if (propertyName == "colorMode") {
        switch (m_colorMode) {
        case ColorMode::Mono:
            return "mono";
        case ColorMode::Color:
            return "color";
        case ColorMode::Grayscale:
            return "grayscale";
        }
    }
    return "";
}


void ApogeyRomDisk::setPortC(uint8_t value)
{
    bool newA15 = value & 0x80;
    value &= 0x7f;
    m_curAddr = (m_curAddr & ~0x7f00) | (value << 8);
    if (newA15 && !m_oldA15) // перед переключением банка нужно сбросить бит 7 порта B
        m_curAddr = (m_curAddr & 0x7fff) | ((m_curAddr & m_mask) << 15);
    m_oldA15 = newA15;
}


bool ApogeyRomDisk::setProperty(const std::string& propertyName, const EmuValuesList& values)
{
    if (RkRomDisk::setProperty(propertyName, values))
        return true;

    if (propertyName == "extBits") {
        int bits = values[0].asInt();
        m_mask = (1 << bits) - 1;
        return true;
    } else if (propertyName == "sizeMB") {
        int mb = values[0].asInt();
        if (mb < 0)
            mb = 0;
        else if (mb > 2 && mb <= 4)
            mb = 4;
        else if (mb > 4)
            mb = 8;
        switch(mb)
        {
           case 0: m_mask = 0x0f; break; // 512KB
           case 1: m_mask = 0x1f; break; // 1MB
           case 2: m_mask = 0x3f; break; // 2MB
           case 4: m_mask = 0x7f; break; // 4MB
           case 8: m_mask = 0xff; break; // 8MB
        }
        return true;
    }

    return false;
}

