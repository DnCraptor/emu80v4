/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2016-2023
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

// Crt8275Renderer.h

#ifndef CRT8275RENDERER_H
#define CRT8275RENDERER_H

#include "CrtRenderer.h"

class Crt8275;

#define bitSet(value, bit) ((value) |= (1UL << (bit)))
#define bitClear(value, bit) ((value) &= ~(1UL << (bit)))
#define bitWrite(value, bit, bitvalue) ((bitvalue) ? bitSet(value, bit) : bitClear(value, bit))

typedef struct {
    uint8_t* ptr;
    uint8_t bit;
    inline void operator +=(int a) {
        int nv = a + bit;
        ptr += nv >> 3;
        bit = nv & 7;
    }
    inline void setBit(int a, uint8_t b) { /// TODO: b - bool
        int nv = a + bit;
        uint8_t* p = ptr + (nv >> 3);
        uint8_t bit = nv & 7;
        bitWrite(*p, bit, b);
    }
} Crt1Bit;

class Crt8275Renderer : public TextCrtRenderer
{

    public:
        Crt8275Renderer();

        bool setProperty(const std::string& propertyName, const EmuValuesList& values) override;
        std::string getPropertyStringValue(const std::string& propertyName) override;

        const char* getTextScreen() override;
        void prepareDebugScreen() override;

        void mouseDrag(int x, int y) override;

        void attachCrt(Crt8275* crt);

        void setFontSetNum(int fontNum) {m_fontNumber = fontNum;}

    protected:
        void toggleCropping() override;
        void setCropping(bool cropping) override;

        virtual const uint8_t* getCurFontPtr(bool, bool, bool) {return nullptr;}
        virtual const uint8_t* getAltFontPtr(bool, bool, bool) {return nullptr;}
        virtual uint8_t getCurFgColor(bool, bool, bool) {return 0xFF;}
        virtual uint8_t getCurBgColor(bool, bool, bool) {return 0x00;}
        virtual void customDrawSymbolLine(Crt1Bit&, uint8_t, int, bool, bool, bool, bool, bool, bool) {}
        virtual wchar_t getUnicodeSymbol(uint8_t chr, bool gpa0, bool gpa1, bool hglt);

        Crt8275* m_crt = nullptr;

        int m_fontNumber = 0;

        int m_fntCharHeight;
        int m_fntCharWidth;
        unsigned m_fntLcMask;

        bool m_customDraw = false;

        bool m_ltenOffset;
        bool m_rvvOffset;
        bool m_hgltOffset;
        bool m_gpaOffset;

        bool m_useRvv;
        bool m_dashedLten = false;

        int m_visibleOffsetX = 0;

        int m_dataSize = 0;

        bool isRasterPresent() override;

        void primaryRenderFrame() override;
        void altRenderFrame() override;

    private:
        double m_freqMHz;
        double m_frameRate;
        bool m_cropping = false;

        int m_cropX = 0;
        int m_cropY = 0;

        std::string getCrtMode();
        void calcAspectRatio(int charWidth);
        void trimImage(int charWidth, int charHeight);
};


#endif // CRT8275RENDERER_H
