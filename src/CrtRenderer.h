﻿/*
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

// CrtRenderer.h

#ifndef CRTRENDERER_H
#define CRTRENDERER_H

#include "EmuTypes.h"
#include "EmuObjects.h"


class CrtRenderer : public EmuObject
{
    public:
        virtual CrtRenderer* asCrtRenderer() override { return this; }
        CrtRenderer() {}
        virtual ~CrtRenderer();

        bool setProperty(const std::string& propertyName, const EmuValuesList& values) override;

        virtual void renderFrame() = 0;
        virtual EmuPixelData getPixelData();
        virtual void prepareDebugScreen();

        virtual void toggleRenderingMethod() {}
        virtual void toggleColorMode() {}
        virtual void toggleCropping() {}
        virtual void setCropping(bool) {}

        virtual const char* getTextScreen() {return nullptr;}

        virtual void mouseDrag(int /*x*/, int /*y*/) {}

        void updateScreenOnce();

        void attachSecondaryRenderer(CrtRenderer* renderer);
        void markSecondary(CrtRenderer* v) { m_primaryRenderer = v; }

        uint8_t* m_pixelData = nullptr;
        uint8_t* m_pixelData2 = nullptr;
        uint8_t* m_pixelData3 = nullptr; /// TODO: optimize (move to only ...)
        int m_sizeX = 0;
        int m_sizeY = 0;
        int m_nativeSizeX = 6;
        int m_dataSize = 0;
    protected:
        CrtRenderer* m_secondaryRenderer = nullptr;
        CrtRenderer* m_primaryRenderer = nullptr;

        int m_bufSize = 0;

        int m_prevSizeX = 0;
        int m_prevSizeY = 0;

        virtual bool isRasterPresent() {return true;}
        void swapBuffers();

        bool m_defaultDebugRendering = true;
        bool reqForSwapBuffers = false;
        void enableSwapBuffersOnce() {reqForSwapBuffers = true;}

        const char* generateTextScreen(wchar_t* wTextArray, int w, int h);

    private:
        unsigned m_frameNo = 0;
        std::string m_textScreen;
};


class TextCrtRenderer : public CrtRenderer
{
    public:
        virtual ~TextCrtRenderer();

        bool setProperty(const std::string& propertyName, const EmuValuesList& values) override;
        std::string getPropertyStringValue(const std::string& propertyName) override;

        void toggleRenderingMethod() override;
        void renderFrame() override;

        void setFontFile(std::string fontFileName);
        void setAltFontFile(std::string fontFileName);
        void setAltRender(bool isAltRender);

    protected:
        const uint8_t* m_font = nullptr;
        const uint8_t* m_altFont = nullptr;
        bool m_font_ram = false;
        bool m_altFont_ram = false;
        bool m_useAltFont = false;
        bool m_isAltRender = false;
        int m_fontSize;
        int m_altFontSize;

        virtual void primaryRenderFrame() = 0;
        virtual void altRenderFrame() = 0;

        const wchar_t* c_rkSymbols =
            L" ▘▝▀▗▚▐▜ ★ ↑  ↣↓▖▌▞▛▄▙▟█   ┃━↢✿ "
            L" !\"#¤%&'()*+,-./0123456789:;<=>?"
            L"@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_"
            L"ЮАБЦДЕФГХИЙКЛМНОПЯРСТУЖВЬЫЗШЭЩЧ▇";

        const wchar_t* c_mikroSymbols =
            L" ▘▝▀▗▚▐▜ ★ ↑  ↣↓▖▌▞▛▄▙▟█   ┃━↢· "
            L" !\"#$%&'()*+,-./0123456789:;<=>?"
            L"@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_"
            L"ЮАБЦДЕФГХИЙКЛМНОПЯРСТУЖВЬЫЗШЭЩЧ▇";
};

#endif // CRTRENDERER_H
