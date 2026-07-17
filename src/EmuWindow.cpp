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
    m_windowType = EWT_EMULATION;

    m_params.style = PWS_FIXED;
    m_params.visible = false;
    m_params.width = 800;
    m_params.height = 600;
    m_params.title = "";
    m_params.vsync = g_emulation->getVsync();
    m_params.smoothing = ST_SHARP;

    applyParams();
}


void EmuWindow::init()
{
    initPalWindow();
}


string EmuWindow::getPlatformObjectName()
{
    return m_machine ? "vector" : "";
}



void EmuWindow::setCaption(string caption)
{
    m_caption = caption;
    string ver = VERSION;
    m_params.title = "Emu80 " + ver + ": " + caption;
    applyParams();
}

string EmuWindow::getCaption()
{
    return m_caption;
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


bool EmuWindow::translateCoords(int& x, int& y)
{
    if ((x < m_dstX) || (x >= m_dstX + m_dstWidth) || (y < m_dstY) || (y >= m_dstY + m_dstHeight))
        return false;

    x = (x - m_dstX) * m_curImgWidth / m_dstWidth;
    y = (y - m_dstY) * m_curImgHeight / m_dstHeight;

    return true;
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


void EmuWindow::mouseDrag(int x, int y)
{
    if (translateCoords(x, y))
        m_machine->mouseDrag(x, y);
}
