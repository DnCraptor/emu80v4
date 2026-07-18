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

// CrtRenderer.h

#ifndef CRTRENDERER_H
#define CRTRENDERER_H

#include "EmuTypes.h"
#include "EmuObjects.h"


class CrtRenderer : public EmuObject
{
    public:
        CrtRenderer() {}
        virtual ~CrtRenderer();


        virtual void renderFrame() = 0;
        virtual EmuPixelData getPixelData();
        virtual void prepareDebugScreen();

        virtual void toggleColorMode() {}
        virtual void toggleCropping() {}

        void updateScreenOnce();

        uint8_t* m_pixelData = nullptr;
        int m_sizeX = 0;
        int m_sizeY = 0;
    protected:
        virtual bool isRasterPresent() {return true;}
        void swapBuffers();

        bool m_defaultDebugRendering = true;
        bool reqForSwapBuffers = false;
        void enableSwapBuffersOnce() {reqForSwapBuffers = true;}

    private:
        unsigned m_frameNo = 0;
};


#endif // CRTRENDERER_H
