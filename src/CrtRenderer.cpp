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

#include "Globals.h"
#include "Emulation.h"
#include "CrtRenderer.h"

using namespace std;

CrtRenderer::~CrtRenderer()
{
    delete[] m_pixelData;
}


EmuPixelData CrtRenderer::getPixelData()
{
    EmuPixelData pd;

    // Проверка "синего экрана"
    if (!isRasterPresent()) {
        pd.pixelData = nullptr;
        return pd;
    }


    pd.width = m_sizeX;
    pd.height = m_sizeY;
    pd.pixelData = m_pixelData;
    pd.frameNo = m_frameNo;
    return pd;
}


void CrtRenderer::swapBuffers()
{
    // don't update if in debug or paused mode
    if (!reqForSwapBuffers && (g_emulation->getPausedState() || g_emulation->isDebuggerActive()))
        return;
    reqForSwapBuffers = false;
/**
    int w,h, bs;
    uint32_t* buf;

    w = m_prevSizeX;
    h = m_prevSizeY;
    buf = m_prevPixelData;
    bs = m_prevBufSize;

    m_prevSizeX = m_sizeX;
    m_prevSizeY = m_sizeY;
    m_prevPixelData = m_pixelData;
    m_prevBufSize = m_bufSize;
    m_prevAspectRatio = m_aspectRatio;

    m_sizeX = w;
    m_sizeY = h;
    m_pixelData = buf;
    m_bufSize = bs;
*/
    ++m_frameNo;
}


void CrtRenderer::updateScreenOnce()
{
    if (!g_emulation->getPausedState() && !g_emulation->isDebuggerActive())
        return;

       ++m_frameNo;
}


void CrtRenderer::prepareDebugScreen()
{
    if (m_defaultDebugRendering) {
        enableSwapBuffersOnce();
        renderFrame();
    }
}
