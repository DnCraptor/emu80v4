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

#include <sstream>
#include <iomanip>
#include <string.h>
#include <cmath>

#include "Globals.h"
#include "EmuWindow.h"
#include "Emulation.h"
#include "Platform.h"

using namespace std;


EmuWindow::EmuWindow()
{
    m_windowType = EWT_EMULATION;

    m_curWindowWidth = m_defWindowWidth;
    m_curWindowHeight = m_defWindowHeight;

    m_params.style = PWS_FIXED;
    m_params.visible = false;
    m_params.width = m_defWindowWidth;
    m_params.height = m_defWindowHeight;
    m_params.title = "";
    m_params.vsync = g_emulation->getVsync();
    m_params.smoothing = ST_SHARP;

    applyParams();
}

EmuWindow::~EmuWindow()
{
    if (m_interlacedImage)
        delete m_interlacedImage;
}


void EmuWindow::init()
{
    initPalWindow();
}


string EmuWindow::getPlatformObjectName()
{
    if (m_platform)
        return m_platform->getName();
    else
        return "";
}


void EmuWindow::setDefaultWindowSize(int width, int height)
{
    m_defWindowWidth = width;
    m_defWindowHeight = height;
    m_params.width = m_defWindowWidth;
    m_params.height = m_defWindowHeight;
    applyParams();

    m_curWindowWidth = m_defWindowWidth;
    m_curWindowHeight = m_defWindowHeight;
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


void EmuWindow::setWindowStyle(WindowStyle ws)
{
    if (ws == WS_FIXED || ws == WS_AUTOSIZE)
        m_params.style = PWS_FIXED;
    else // if (ws == WS_RESIZABLE)
        m_params.style = PWS_RESIZABLE;

    if (ws == WS_FIXED) {
        m_params.width = m_defWindowWidth;
        m_params.height = m_defWindowHeight;
    }

    applyParams();

    m_windowStyle = ws;
}


void EmuWindow::setFieldsMixing(FieldsMixing fm)
{
    m_fieldsMixing = fm;
}


void EmuWindow::setSmoothing(SmoothingType smoothing)
{
    m_smoothing = smoothing;
    m_params.smoothing = smoothing;
    applyParams();
}


void EmuWindow::setAspectCorrection(bool aspectCorrection)
{
    m_aspectCorrection = aspectCorrection;
}


void EmuWindow::setSquarePixels(bool squarePixels)
{
    m_squarePixels = squarePixels;
}


void EmuWindow::setWideScreen(bool wideScreen)
{
    if (!m_useCustomScreenFormat)
        m_wideScreen = wideScreen;
}


void EmuWindow::setCustomScreenFormat(bool custom)
{
    m_useCustomScreenFormat = custom;
}


void EmuWindow::setCustomScreenFormatValue(double format)
{
    m_customScreenFormat = format;
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


double EmuWindow::calcBestAspectRatio(double srcAspectRatio, double scaleY)
{
    /*if (m_overlay)
        return m_primaryFrameAspectRatio * srcAspectRatio;*/

    if (m_squarePixels)
        return 1.0;

    double ar = srcAspectRatio * scaleY;
    if (ar < 1.4142) // sqrt(1*2)
        ar = 1;
    else if (ar < 2.4495) // sqrt(2*3)
        ar = 2;
    else if (ar < 3.4641) // sqrt(3*4)
        ar = 3;
    else
        ar = round(ar);

    ar /= scaleY;

    /*if (!m_overlay)
        m_primaryFrameAspectRatio = ar;*/

    return ar;
}


void EmuWindow::calcDstRect(int srcWidth, int srcHeight,  double srcAspectRatio, int wndWidth, int wndHeight, int& dstWidth, int& dstHeight, int& dstX, int& dstY)
{
    if (m_fieldsMixing == FM_INTERLACE || m_fieldsMixing == FM_SCANLINE)
        srcHeight /= 2;

    double aspectRatio = m_aspectCorrection
        ? (m_useCustomScreenFormat ? srcAspectRatio * 0.75 * m_customScreenFormat : m_wideScreen ? srcAspectRatio / 0.75 : srcAspectRatio)
        : calcBestAspectRatio(srcAspectRatio, 2.0);

    dstWidth = srcWidth * 2.0 * aspectRatio + .5;
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


void EmuWindow::interlaceFields(EmuPixelData frame)
{
    int requiredSize = frame.height * 2 * frame.width;
    if (requiredSize > m_interlacedImageSize) {
        if (m_interlacedImage)
            delete m_interlacedImage;
        m_interlacedImage = new uint32_t[requiredSize];
        m_interlacedImageSize = requiredSize;
    }

    for (int i = 0; i < frame.height; i++) {
        memcpy(m_interlacedImage + frame.width * i * 2, frame.pixelData + i * frame.width, frame.width * 4);
///        memcpy(m_interlacedImage + frame.width * (i * 2 + 1), frame.prevPixelData + i * frame.width, frame.width * 4);
    }
}


void EmuWindow::prepareScanline(EmuPixelData frame)
{
    int requiredSize = frame.height * 2 * frame.width;
    if (requiredSize > m_interlacedImageSize) {
        if (m_interlacedImage)
            delete m_interlacedImage;
        m_interlacedImage = new uint32_t[requiredSize];
        m_interlacedImageSize = requiredSize;
    }

    for (int i = 0; i < frame.height; i++) {
        memcpy(m_interlacedImage + frame.width * i * 2, frame.pixelData + i * frame.width, frame.width * 4);
        memcpy(m_interlacedImage + frame.width * (i * 2 + 1), frame.pixelData + i * frame.width, frame.width * 4);
        uint8_t* secondLine = (uint8_t*)(m_interlacedImage + frame.width * (i * 2 + 1));
        for (int x = 0; x < frame.width * 4; x += 4) {
            int r = secondLine[x]; secondLine[x] = r >> 2;
            int g = secondLine[x + 1]; secondLine[x + 1] = g >> 2;
            int b = secondLine[x + 2]; secondLine[x + 2] = b >> 2;
        }
    }
}


void EmuWindow::drawFrame(EmuPixelData frame)
{
    /**
    if (frame.frameNo == m_curFrameNo)
        return;
    m_curFrameNo = frame.frameNo;
    m_frameDrawn = true;

    if (frame.width == 0 || frame.height == 0 || !frame.pixelData) {
        drawFill(0x303050);
        return;
    }

    //m_overlay = false;

    m_curImgWidth = frame.width;
    m_curImgHeight = frame.height;
    getSize(m_curWindowWidth, m_curWindowHeight);

    if (m_fieldsMixing != FM_INTERLACE && m_fieldsMixing != FM_SCANLINE)
        calcDstRect(frame.width, frame.height, frame.aspectRatio, m_curWindowWidth, m_curWindowHeight, m_dstWidth, m_dstHeight, m_dstX, m_dstY);
    else
        calcDstRect(frame.width, frame.height * 2, frame.aspectRatio, m_curWindowWidth, m_curWindowHeight, m_dstWidth, m_dstHeight, m_dstX, m_dstY);


    if ((m_windowStyle == WS_AUTOSIZE)
          && (m_curWindowWidth != m_dstWidth || m_curWindowHeight != m_dstHeight)) {
        m_curWindowHeight = m_dstHeight;
        m_curWindowWidth = m_dstWidth;

        m_params.width = m_dstWidth;
        m_params.height = m_dstHeight;
        applyParams();
    }

    drawFill(0x282828);

    if (m_fieldsMixing != FM_INTERLACE && m_fieldsMixing != FM_SCANLINE) {
        drawImage((uint32_t*)frame.pixelData, frame.width, frame.height, frame.aspectRatio, false, false);
///        if (m_fieldsMixing == FM_MIX && frame.prevPixelData && frame.prevHeight == frame.height && frame.prevWidth == frame.width) {
///            drawImage((uint32_t*)frame.prevPixelData, frame.prevWidth, frame.prevHeight, frame.prevAspectRatio, true, false);
///        }
///    } else if (m_fieldsMixing == FM_INTERLACE && frame.prevPixelData && frame.prevHeight == frame.height && frame.prevWidth == frame.width) { // FM_INTERLACE
///        if (frame.frameNo & 1)
///            interlaceFields(frame);
///        if (m_interlacedImage)
///            drawImage(m_interlacedImage, frame.width, frame.height * 2, frame.aspectRatio, false, false);
    } else { // if (m_fieldsMixing == FM_SCANLINE)
        prepareScanline(frame);
        drawImage(m_interlacedImage, frame.width, frame.height * 2, frame.aspectRatio, false, false);
    }
    */
}


void EmuWindow::drawOverlay(EmuPixelData frame)
{
    if (!m_frameDrawn)
        return;

    if (frame.width == 0 || frame.height == 0 || !frame.pixelData)
        return;

    //m_overlay = true;

///    drawImage((uint32_t*)frame.pixelData, frame.width, frame.height, frame.aspectRatio, true, true);

/*    if (m_fieldsMixing == FM_MIX && frame.prevPixelData) {
        drawImage((uint32_t*)frame.prevPixelData, frame.prevWidth, frame.prevHeight, m_dstX, m_dstY, m_dstWidth, m_dstHeight, true, true);
    }*/
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
        case SR_ASPECTCORRECTION:
            m_aspectCorrection = !m_aspectCorrection;
            m_platform->updateScreenOnce();
            break;
        case SR_WIDESCREEN:
            setWideScreen(!m_wideScreen);
            m_platform->updateScreenOnce();
            break;
        case SR_SMOOTHING:
            switch (m_smoothing) {
                case ST_NEAREST:
                    setSmoothing(ST_SHARP);
                    break;
                case ST_SHARP:
                    setSmoothing(ST_BILINEAR);
                    break;
                case ST_BILINEAR:
                    setSmoothing(ST_NEAREST);
            }
            break;
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
        m_platform->mouseDrag(x, y);
}


bool EmuWindow::setProperty(const string& propertyName, const EmuValuesList& values)
{
    if (EmuObject::setProperty(propertyName, values))
        return true;

    if (propertyName == "caption") {
        setCaption(values[0].asString());
        return true;
    } else if (propertyName == "windowStyle") {
        if (values[0].asString() == "autosize") {
            setWindowStyle(WS_AUTOSIZE);
            return true;
        } else if (values[0].asString() == "fixed") {
            setWindowStyle(WS_FIXED);
            return true;
        } else if (values[0].asString() == "resizable" || values[0].asString() == "sizable" /*for compatibility*/) {
            setWindowStyle(WS_RESIZABLE);
            return true;
        } else
            return false;
    } else if (propertyName == "fieldsMixing") {
        if (values[0].asString() == "none") {
            setFieldsMixing(FM_NONE);
            return true;
        } else if (values[0].asString() == "mix") {
            setFieldsMixing(FM_MIX);
            return true;
        } else if (values[0].asString() == "interlace") {
            setFieldsMixing(FM_INTERLACE);
            return true;
        } else if (values[0].asString() == "scanline") {
            setFieldsMixing(FM_SCANLINE);
            return true;
        }
    } else if (propertyName == "defaultWindowSize") {
        if (values[0].isInt() && values[1].isInt()) {
            setDefaultWindowSize(values[0].asInt(), values[1].asInt());
            return true;
        }
    } else if (propertyName == "defaultWindowWidth") { // !!!
        if (values[0].isInt()) {
            setDefaultWindowSize(values[0].asInt(), m_defWindowHeight);
            return true;
        }
    } else if (propertyName == "defaultWindowHeight") { // !!!
        if (values[0].isInt()) {
            setDefaultWindowSize(m_defWindowWidth, values[0].asInt());
            return true;
        }
    } else if (propertyName == "smoothing") {
        if (values[0].asString() == "nearest") {
            setSmoothing(ST_NEAREST);
            return true;
        } else if (values[0].asString() == "bilinear") {
            setSmoothing(ST_BILINEAR);
            return true;
        } else if (values[0].asString() == "sharp") {
            setSmoothing(ST_SHARP);
            return true;
        }
    } else if (propertyName == "aspectCorrection") {
        if (values[0].asString() == "no") {
            setAspectCorrection(false);
            return true;
        } else if (values[0].asString() == "yes") {
            setAspectCorrection(true);
            return true;
        }
    } else if (propertyName == "wideScreen") {
        if (values[0].asString() == "no") {
            setCustomScreenFormat(false);
            setWideScreen(false);
            return true;
        } else if (values[0].asString() == "yes") {
            setCustomScreenFormat(false);
            setWideScreen(true);
            return true;
        } else if (values[0].asString() == "custom") {
            setCustomScreenFormat(true);
            return true;
        }
    } else if (propertyName == "customScreenFormat") {
        if (values[0].isFloat()) {
            setCustomScreenFormatValue(values[0].asFloat());
            return true;
        }
    } else if (propertyName == "squarePixels") {
        if (values[0].asString() == "no") {
            setSquarePixels(false);
            return true;
        } else if (values[0].asString() == "yes") {
            setSquarePixels(true);
            return true;
        }
    }

    return false;
}


string EmuWindow::getPropertyStringValue(const string& propertyName)
{
    string res;

    res = EmuObject::getPropertyStringValue(propertyName);
    if (res != "")
        return res;

    if (propertyName == "caption")
        return m_caption;
    else if (propertyName == "windowStyle") {
        switch (m_windowStyle) {
            case WS_AUTOSIZE:
                return "autosize";
            case WS_FIXED:
                return "fixed";
            case WS_RESIZABLE:
                return "resizable";
        }
    } else if (propertyName == "fieldsMixing") {
        switch (m_fieldsMixing) {
            case FM_NONE:
                return "none";
            case FM_MIX:
                return "mix";
            case FM_INTERLACE:
                return "interlace";
            case FM_SCANLINE:
                return "scanline";
        }
    } else if (propertyName == "smoothing") {
        switch (m_smoothing) {
            case ST_NEAREST:
                return "nearest";
            case ST_BILINEAR:
                return "bilinear";
            case ST_SHARP:
                return "sharp";
        }
    } else if (propertyName == "aspectCorrection") {
        return m_aspectCorrection ? "yes" : "no";
    } else if (propertyName == "squarePixels") {
        return m_squarePixels ? "yes" : "no";
    } else if (propertyName == "wideScreen") {
        return m_useCustomScreenFormat ? "custom" : m_wideScreen ? "yes" : "no";
    } else if (propertyName == "defaultWindowWidth") {
        string res;
        stringstream stringStream;
        stringStream << m_defWindowWidth;
        stringStream >> res;
        return res;
    } else if (propertyName == "defaultWindowHeight") {
        string res;
        stringstream stringStream;
        stringStream << m_defWindowHeight;
        stringStream >> res;
        return res;
    } else if (propertyName == "customScreenFormat") {
        string res;
        stringstream stringStream;
        stringStream << setprecision(4) << m_customScreenFormat;
        stringStream >> res;
        return res;
    }

    return "";
}
