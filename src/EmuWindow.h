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

#ifndef EMUWINDOW_H
#define EMUWINDOW_H

#include <string>

#include "Pal.h"
#include "PalWindow.h"

#include "EmuTypes.h"
#include "EmuObjects.h"


class Emulation;

class PalWindow;

class EmuWindow : public EmuObject, public PalWindow
{
    public:
        EmuWindow();
        virtual ~EmuWindow() = default;


        void init() override;

        void calcDstRect(int srcWidth, int srcHeight, double srcAspectRatio, int wndWidth, int wndHeight, int& dstWidth, int& dstHeight, int& dstX, int& dstY) override;


        virtual void processKey(PalKeyCode, bool) {}
        virtual void closeRequest() {}

        virtual bool translateCoords(int& x, int& y);

        void sysReq(SysReq sr);

        void show();
        void hide();

        void drawFrame(EmuPixelData frame);
        void drawOverlay(EmuPixelData frame);
        void endDraw();

        bool isVisible() {return m_params.visible;}


    private:

        int m_curImgWidth = 0;
        int m_curImgHeight = 0;

        int m_dstX;
        int m_dstY;
        int m_dstWidth;
        int m_dstHeight;

        unsigned m_curFrameNo = unsigned(-1);
        bool m_frameDrawn = false;

};

#endif // EMUWINDOW_H
