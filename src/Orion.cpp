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
#include <pico.h>

#include "Orion.h"
#include "Emulation.h"
#include "Platform.h"
#include "Globals.h"
#include "EmuWindow.h"
#include "SoundMixer.h"
#include "Memory.h"
#include "AddrSpace.h"
#include "Fdc1793.h"
#include "Cpu.h"

using namespace std;


void OrionCore::draw()
{
    //m_crtRenderer->renderFrame();
    m_window->drawFrame(m_crtRenderer->getPixelData());
    m_window->endDraw();
}


void OrionCore::inte(bool isActive)
{
    m_beepSoundSource->setValue(isActive ? 1 : 0);
}


void OrionCore::attachCrtRenderer(OrionRenderer* crtRenderer)
{
    m_crtRenderer = crtRenderer;
}


bool OrionCore::setProperty(const string& propertyName, const EmuValuesList& values)
{
    if (PlatformCore::setProperty(propertyName, values))
        return true;

    if (propertyName == "crtRenderer") {
        attachCrtRenderer(static_cast<OrionRenderer*>(g_emulation->findObject(values[0].asString())));
        return true;
    } else if (propertyName == "beepSoundSource") {
        m_beepSoundSource = static_cast<GeneralSoundSource*>(g_emulation->findObject(values[0].asString()));
        return true;
    }
    return false;
}


OrionRenderer::OrionRenderer()
{
    m_sizeX = m_prevSizeX = 384;
    m_sizeY = m_prevSizeY = 256;
    m_bufSize = m_sizeX * m_sizeY / 2;
    m_pixelData = new uint8_t[m_bufSize]();
}


void OrionRenderer::attachScreenMemory(Ram* screenMemory)
{
    m_screenMemory = screenMemory->getDataPtr();
}


void OrionRenderer::attachColorMemory(Ram* colorMemory)
{
    m_colorMemory = colorMemory->getDataPtr();
}


void OrionRenderer::setScreenBase(uint16_t base)
{
    m_screenBase = base;
}


void OrionRenderer::setColorModeByte(uint8_t modeByte)
{
    m_colorMode = (OrionColorMode)(modeByte >> 1);
    m_palette = modeByte & 1;
}

void __not_in_flash_func(OrionRenderer::renderFrame)()
{
    swapBuffers();
    // One byte in each plane describes eight horizontal pixels. Store two
    // 4-bit palette indices per output byte, low nibble first (VGA backend).
    static const uint8_t four[2][5] = {{0, 4, 2, 1, 7}, {3, 7, 6, 5, 7}};
    if (m_colorMode == OCM_BLANK) {
        for (int i = 0; i < m_bufSize; ++i) m_pixelData[i] = 0;
    } else {
        for (int row = 0; row < 256; ++row) {
            uint8_t* out = m_pixelData + row * 192;
            for (int col = 0; col < 48; ++col) {
                const unsigned addr = m_screenBase + col * 256 + row;
                uint8_t bits = m_screenMemory[addr];
                if (m_colorMode == OCM_4COLOR) {
                    uint8_t attr = m_colorMemory[addr];
                    for (int pair = 0; pair < 4; ++pair) {
                        unsigned a = ((bits >> 6) & 2) | (attr >> 7);
                        unsigned b = ((bits >> 5) & 2) | ((attr >> 6) & 1);
                        if (!m_isColorMode) { a = a & 2 ? 4 : 0; b = b & 2 ? 4 : 0; }
                        *out++ = four[m_palette][a] | (four[m_palette][b] << 4);
                        bits <<= 2; attr <<= 2;
                    }
                } else {
                    unsigned fg = m_palette ? 6 : 2;
                    unsigned bg = m_palette ? 3 : 0;
                    if (m_colorMode == OCM_16COLOR) {
                        const uint8_t attr = m_colorMemory[addr];
                        fg = attr & 15; bg = attr >> 4;
                    }
                    if (!m_isColorMode) { fg = fg & 2 ? 7 : 0; bg = bg & 2 ? 7 : 0; }
                    const uint8_t pairs[4] = {uint8_t(bg | (bg << 4)), uint8_t(bg | (fg << 4)),
                                              uint8_t(fg | (bg << 4)), uint8_t(fg | (fg << 4))};
                    *out++ = pairs[bits >> 6];
                    *out++ = pairs[(bits >> 4) & 3];
                    *out++ = pairs[(bits >> 2) & 3];
                    *out++ = pairs[bits & 3];
                }
            }
        }
    }
    if (graphics_get_frame() != m_pixelData)
        graphics_set_4bit_buffer(m_pixelData, 384, 256);
}

void OrionRenderer::operate()
{
    renderFrame();
    m_curClock += g_emulation->getFrequency() * 640 * 312 / 10000000; // 10 MHz pixelclock, 312 scanlines, 640 pixels wide
    g_emulation->screenUpdateReq(); // transfer to Core
}


void OrionRenderer::toggleCropping()
{
    // VGA supplies the border; keep the compact framebuffer dimensions fixed.
}


bool OrionRenderer::setProperty(const string& propertyName, const EmuValuesList& values)
{
    if (CrtRenderer::setProperty(propertyName, values))
        return true;

    if (propertyName == "screenMemory") {
        attachScreenMemory(static_cast<Ram*>(g_emulation->findObject(values[0].asString())));
        return true;
    } else if (propertyName == "colorMemory") {
        attachColorMemory(static_cast<Ram*>(g_emulation->findObject(values[0].asString())));
        return true;
    } else if (propertyName == "colorMode") {
        if (values[0].asString() == "mono")
            m_isColorMode = false;
        else if (values[0].asString() == "color")
            m_isColorMode = true;
        else
            return false;
        return true;
    } else if (propertyName == "visibleArea") {
        if (values[0].asString() == "yes" || values[0].asString() == "no") {
            m_showBorder = false;
            return true;
        }
    }
    return false;
}


string OrionRenderer::getPropertyStringValue(const string& propertyName)
{
    string res;

    res = EmuObject::getPropertyStringValue(propertyName);
    if (res != "")
        return res;

    if (propertyName == "colorMode")
        return m_isColorMode ? "color" : "mono";
    else if (propertyName == "visibleArea")
        return m_showBorder ? "yes" : "no";
    else if (propertyName == "crtMode")
        switch (m_colorMode) {
        case OCM_MONO:
            return u8"2-color 384\u00D7256@50.08Hz";
        case OCM_4COLOR:
            return u8"4-color 384\u00D7256@50.08Hz";
        case OCM_16COLOR:
            return u8"16-color 384\u00D7256@50.08Hz";
        default:
            return "384\u00D7256@50.08Hz";
        }

    return "";
}


void OrionMemPageSelector::writeByte(int, uint8_t value)
{
    m_addrSpaceMapper->setCurPage(value & m_mask);
};


bool OrionMemPageSelector::setProperty(const string& propertyName, const EmuValuesList& values)
{
    if (AddressableDevice::setProperty(propertyName, values))
        return true;

    if (propertyName == "mapper") {
        attachAddrSpaceMapper(static_cast<AddrSpaceMapper*>(g_emulation->findObject(values[0].asString())));
        return true;
    } else if (propertyName == "bits") {
        int bits = values[0].asInt();
        m_mask = (((1 << bits) - 1) & 0xFF);
        return true;
    }

    return false;
}


void OrionScreenSelector::writeByte(int, uint8_t value)
{
    if (m_renderer)
        m_renderer->setScreenBase((~value & 0x3) << 14);
};


bool OrionScreenSelector::setProperty(const string& propertyName, const EmuValuesList& values)
{
    if (AddressableDevice::setProperty(propertyName, values))
        return true;

    if (propertyName == "crtRenderer") {
        attachCrtRenderer(static_cast<OrionRenderer*>(g_emulation->findObject(values[0].asString())));
        return true;
    }

    return false;
}


void OrionColorModeSelector::writeByte(int, uint8_t value)
{
    if (m_renderer)
        m_renderer->setColorModeByte(value & 0x7);
};


bool OrionColorModeSelector::setProperty(const string& propertyName, const EmuValuesList& values)
{
    if (AddressableDevice::setProperty(propertyName, values))
        return true;

    if (propertyName == "crtRenderer") {
        attachCrtRenderer(static_cast<OrionRenderer*>(g_emulation->findObject(values[0].asString())));
        return true;
    }

    return false;
}


void OrionFddControlRegister::writeByte(int, uint8_t value)
{
    if (m_type == OFT_STANDARD) {
        m_fdc->setDrive(value & 1);
        m_fdc->setHead(((value & 0x10) >> 4) ^ 1);
    } else { // m_type == OFT_SPDOS
        m_fdc->setDrive((value & 0x2) >> 1);
        m_fdc->setHead(value & 1);
    }
}


bool OrionFddControlRegister::setProperty(const string& propertyName, const EmuValuesList& values)
{
    if (AddressableDevice::setProperty(propertyName, values))
        return true;

    if (propertyName == "type") {
        if (values[0].asString() == "standart") {
            m_type = OFT_STANDARD;
            return true;
        } else if (values[0].asString() == "spdos") {
            m_type = OFT_SPDOS;
            return true;
        }
        return true;
    } else if (propertyName == "fdc") {
        attachFdc1793(static_cast<Fdc1793*>(g_emulation->findObject(values[0].asString())));
        return true;
    }

    return false;
}


uint8_t OrionFddQueryRegister::readByte(int)
{
    return (m_fdc->getDrq() ? 0 : 1) | (m_fdc->getIrq() ? 0 : 0x80);
}

bool OrionFddQueryRegister::setProperty(const string& propertyName, const EmuValuesList& values)
{
    if (AddressableDevice::setProperty(propertyName, values))
        return true;

    if (propertyName == "fdc") {
        attachFdc1793(static_cast<Fdc1793*>(g_emulation->findObject(values[0].asString())));
        return true;
    }

    return false;
}

#include "ff.h"

bool OrionFileLoader::loadFile(const std::string& fileName, bool run)
{
    FIL f;
    if (FR_OK != f_open(&f, fileName.c_str(), FA_READ))
        return false;

    if (f_size(&f) < 16) {
        f_close(&f);
        return false;
    }
    char p16[16];
    UINT br;
    f_read(&f, p16, 16, &br);
    int bruOffset = 0;
    int len = (p16[0x0b] << 8) | p16[0x0a];;

    if (!memcmp(p16, "Orion-128 file\r\n", 16)) {
        // ORI file
        bruOffset = 16;
        f_lseek(&f, 0);
    } else if (len == 0)
        // RKO file
        bruOffset = 0x4d;

    int fileSize = f_size(&f);
    if (bruOffset != 0) {
        if (fileSize < bruOffset + 16) {
            f_close(&f);
            return false;
        }
        fileSize -= bruOffset;
        f_lseek(&f, bruOffset);
        f_read(&f, p16, 16, &br);
        len = (p16[0x0b] << 8) | p16[0x0a];
        f_lseek(&f, bruOffset);
    }

    if (fileSize < len) {
        f_close(&f);
        return false;
    }

    if (run) {
        uint16_t begAddr = (p16[0x09] << 8) | p16[0x08];
        uint16_t nBytes = (p16[0x0b] << 8) | p16[0x0a];
        bruOffset += 16;
        f_lseek(&f, bruOffset);
        if (len < nBytes) {
            f_close(&f);
            return false;
        }


        m_platform->reset();
        Cpu* bc = m_platform->getCpu();
        if (bc)
        if (Cpu8080Compatible* cpu = bc->asCpu8080Compatible()) {
            g_emulation->exec((uint64_t)cpu->getKDiv() * 3000000, true);
            cpu->setPC(begAddr);
        }

        for (int i = 0; i < nBytes; i++) {
            uint8_t v;
            f_read(&f, &v, 1, &br);
            m_as->writeByte(begAddr++, v);
        }
    } else {
        for (uint16_t addr = 0; addr < len; addr++) {
            uint8_t v;
            f_read(&f, &v, 1, &br);
            m_ramDisk->writeByte(addr, v);
        }
        m_ramDisk->writeByte(len, 0xff);
    }

    f_close(&f);
    return true;
}


bool OrionFileLoader::setProperty(const std::string& propertyName, const EmuValuesList& values)
{
    if (FileLoader::setProperty(propertyName, values))
        return true;

    if (propertyName == "ramDiskAddrSpace") {
        m_ramDisk = static_cast<AddressableDevice*>(g_emulation->findObject(values[0].asString()));
        return true;
    }

    return false;
}
