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
#define bitRead(value, bit) (((value) >> (bit)) & 0x01)

class Crt1Bit {
    uint64_t p8;
public:
    inline Crt1Bit(uint8_t* p) : p8((uint64_t)p << 3) {}
    inline void operator +=(int a) {
        p8 += a;
    }
    inline void setBit(uint8_t b) { /// TODO: b - bool
        uint8_t* p = (uint8_t*)(p8 >> 3);
        uint8_t bi = p8 & 7;
        bitWrite(*p, bi, b);
    }
    inline bool getBit(int a) {
        uint64_t p82 = p8 + a;
        uint8_t* p = (uint8_t*)(p82 >> 3);
        uint8_t bi = p82 & 7;
        return bitRead(*p, bi);
    }
    inline void setBit(int a, uint8_t b) { /// TODO: b - bool
        uint64_t p82 = p8 + a;
        uint8_t* p = (uint8_t*)(p82 >> 3);
        uint8_t bi = p82 & 7;
        bitWrite(*p, bi, b);
    }
};


typedef struct {
    uint8_t* ptr1; // R
    uint8_t* ptr2; // G
    uint8_t* ptr3; // B
    uint8_t bit;
    inline void operator +=(int a) {
        uint64_t p8 = (uint32_t)ptr1;
        p8 = (p8 << 3) + bit + a;
        uint8_t* p = (uint8_t*)(p8 >> 3);
        size_t shift = p - ptr1;
        ptr1 = p;
        ptr2 += shift;
        ptr3 += shift;
        bit = (p8 & 7);
    }
    inline void set3Bit(uint8_t b) { // 0bRGB
        bitWrite(*ptr1, bit, b & 1);        // R
        bitWrite(*ptr2, bit, (b >> 1) & 1); // G
        bitWrite(*ptr3, bit, (b >> 2) & 1); // B
    }
    inline void set3Bit(int a, uint8_t b) { // b - 0bRGB
        uint64_t p8 = (uint32_t)ptr1;
        p8 = (p8 << 3) + bit + a;
        uint8_t* p = (uint8_t*)(p8 >> 3);
        uint8_t bi = p8 & 7;
        bitWrite(*p, bi, b & 1); // R
        size_t shift = p - ptr1;
        p = ptr2 + shift;
        bitWrite(*p, bi, (b >> 1) & 1); // G
        p = ptr3 + shift;
        bitWrite(*p, bi, (b >> 2) & 1); // B
    }
} Crt3Bit;

class Crt4Bit {
    uint32_t p8;
public:
    inline Crt4Bit(uint8_t* p) : p8((uint32_t)p << 1) {}
    inline void operator +=(int a) {
        p8 += a;
    }
    inline void set4Bit(uint8_t b) { /// b - 0xARGB
        register uint8_t* p = (uint8_t*)(p8 >> 1);
        if (p8 & 1) { // high 4-bits
            *p = (*p & 0b00001111) | (b << 4);
        } else { // low 4-bits
            *p = (*p & 0b11110000) | (b & 0b1111);
        }
    }
    inline void set4Bit(int a, uint8_t b) { /// b - 0xARGB
        register uint32_t p82 = p8 + a;
        register uint8_t* p = (uint8_t*)(p82 >> 1);
        if (p82 & 1) { // high 4-bits
            *p = (*p & 0b00001111) | (b << 4);
        } else { // low 4-bits
            *p = (*p & 0b11110000) | b;
        }
    }
};

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

        Crt8275* m_crt = nullptr;
        bool m_ltenOffset;
        bool m_hgltOffset;
        bool m_rvvOffset;
        bool m_gpaOffset;
        bool m_dashedLten = false;

    protected:
        void toggleCropping() override;
        void setCropping(bool cropping) override;

        virtual const uint8_t* getCurFontPtr(bool, bool, bool) {return nullptr;}
        virtual const uint8_t* getAltFontPtr(bool, bool, bool) {return nullptr;}
        virtual uint8_t getCurFgColor(bool, bool, bool) {return 0xFF;}
        virtual uint8_t getCurBgColor(bool, bool, bool) {return 0x00;}
        virtual void customDrawSymbolLine(Crt1Bit, uint8_t, int, bool, bool, bool, bool, bool, bool) {}
        virtual wchar_t getUnicodeSymbol(uint8_t chr, bool gpa0, bool gpa1, bool hglt);

        int m_fontNumber = 0;

        int m_fntCharHeight;
        int m_fntCharWidth;
        unsigned m_fntLcMask;

        bool m_customDraw = false;

        bool m_useRvv;

        int m_visibleOffsetX = 0;

        bool isRasterPresent() override;

        void primaryRenderFrame() override;
        void altRenderFrame() override;

        void calcAspectRatio(int charWidth);
    private:
        double m_freqMHz;
        double m_frameRate;
        bool m_cropping = false;

        int m_cropX = 0;
        int m_cropY = 0;

        std::string getCrtMode();
        void trimImage(int charWidth, int charHeight);
};


#endif // CRT8275RENDERER_H
