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

#include <string.h>

#include "Globals.h"
#include "EmuWindow.h"
#include "Emulation.h"
#include "Vector.h"

using namespace std;


EmuWindow::EmuWindow()
{
    m_params.visible = false;

    applyParams();
}


void EmuWindow::init()
{
    initPalWindow();
}




void EmuWindow::show()
{
    m_params.visible = true;
    applyParams();
}


void EmuWindow::hide()
{
    m_params.visible = false;
    applyParams();
}


void EmuWindow::calcDstRect(int srcWidth, int srcHeight,  double srcAspectRatio, int wndWidth, int wndHeight, int& dstWidth, int& dstHeight, int& dstX, int& dstY)
{
    (void)srcAspectRatio;

    dstWidth = srcWidth * 2;
    dstHeight = srcHeight * 2;
    dstX = (wndWidth - dstWidth) / 2;
    dstY = (wndHeight - dstHeight) / 2;
}


void EmuWindow::drawFrame(EmuPixelData frame)
{
    (void)frame;
}


void EmuWindow::drawOverlay(EmuPixelData frame)
{
    (void)frame;
}


void EmuWindow::endDraw()
{
    if (!m_frameDrawn)
        return;

    m_frameDrawn = false;

    drawEnd();
}


void EmuWindow::sysReq(SysReq sr)
{
    switch (sr) {
        case SR_SCREENSHOT:
            screenshotRequest(palOpenFileDialog("Save screenshot",
#ifdef PAL_QT
                "PNG files (*.png)|*.png|"
#endif
                "BMP files (*.bmp)|*.bmp"
                , true, this));
            break;
        default:
            break;
    }
}
