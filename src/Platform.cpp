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

#include "Globals.h"
#include "Platform.h"
#include "Emulation.h"
#include "EmuObjects.h"
#include "EmuWindow.h"
#include "Cpu.h"
#include "PlatformCore.h"
#include "KbdLayout.h"
#include "CrtRenderer.h"
#include "DiskImage.h"
#include "FileLoader.h"
#include "Keyboard.h"
#include "RamDisk.h"
#include "KbdTapper.h"

using namespace std;

Platform::Platform()
{
}


void Platform::init()
{
    if (m_initCallback)
        m_initCallback();
}


void Platform::shutdown()
{
    if (m_shutdownCallback)
        m_shutdownCallback();
}


void Platform::reset()
{
    if (m_resetCallback)
        m_resetCallback();
}


Platform::~Platform()
{
    Platform::shutdown();
}


void Platform::start()
{
    init();
    reset();
    if (m_window)
        m_window->show();
}


void Platform::sysReq(SysReq sr)
{
    switch (sr) {
        case SR_RESET:
            reset();
            if (m_fastReset && m_fastResetCpuTicks) {
                Cpu* cpu = getCpu();
                cpu->disableHooks();
                g_emulation->exec((int64_t)cpu->getKDiv() * m_fastResetCpuTicks); // no 2d parameter: no fast reset when debugger is active
                cpu->enableHooks();
            }
            updateDebugger();
            break;
        case SR_QUERTY:
            if (m_kbdLayout) {
                m_kbdLayout->setQwertyMode();
            }
            break;
        case SR_JCUKEN:
            if (m_kbdLayout) {
                m_kbdLayout->setJcukenMode();
            }
            break;
        case SR_SMART:
            if (m_kbdLayout) {
                m_kbdLayout->setSmartMode();
            }
            break;
        case SR_FONT:
            if (m_renderer) {
                m_renderer->toggleRenderingMethod();
            }
            break;
        case SR_CROPTOVISIBLE:
            if (m_renderer) {
                m_renderer->toggleCropping();
            }
            break;
        case SR_COLOR:
            if (m_renderer) {
                m_renderer->toggleColorMode();
            }
            break;
        case SR_COPYTXT:
            if (m_renderer) {
                const char* text = m_renderer->getTextScreen();
                if (text)
                    palCopyTextToClipboard(text);
            }
            break;
        case SR_PASTE:
            if (m_kbdTapper && m_kbdLayout->getMode() == KbdLayout::KLM_SMART) {
                m_kbdTapper->typeText(palGetTextFromClipboard());
            }
            break;
        case SR_DISKA:
            // open disk A image
            if (m_diskA)
                m_diskA->chooseFile();
            break;
        case SR_DISKB:
            // open disk B image
            if (m_diskB)
                m_diskB->chooseFile();
            break;
        case SR_DISKC:
            // open floppy disk C image
            if (m_diskC)
                m_diskC->chooseFile();
            break;
        case SR_DISKD:
            // open floppy disk D image
            if (m_diskD)
                m_diskD->chooseFile();
            break;
        case SR_HDD:
            // open HDD/CF image
            if (m_hdd)
                m_hdd->chooseFile();
        break;
        case SR_LOAD:
            if (m_loader) {
                m_loader->chooseAndLoadFile();
            }
            break;
        case SR_LOADRUN:
            if (m_loader) {
                m_loader->chooseAndLoadFile(true);
            }
            break;
        case SR_DEBUG:
            // show debugger
            g_emulation->debugRequest(m_cpu);
            //showDebugger();
            break;
        case SR_LOADRAMDISK:
            if (m_ramDisk)
                m_ramDisk->loadFromFile();
            break;
        case SR_SAVERAMDISK:
            if (m_ramDisk)
                m_ramDisk->saveToFile();
            break;
        case SR_OPENRAMDISK:
            if (m_ramDisk)
                m_ramDisk->openFile();
            break;
        case SR_SAVERAMDISKAS:
            if (m_ramDisk)
                m_ramDisk->saveFileAs();
            break;
        case SR_LOADRAMDISK2:
            if (m_ramDisk2)
                m_ramDisk2->loadFromFile();
            break;
        case SR_SAVERAMDISK2:
            if (m_ramDisk2)
                m_ramDisk2->saveToFile();
            break;
        case SR_OPENRAMDISK2:
            if (m_ramDisk2)
                m_ramDisk2->openFile();
            break;
        case SR_SAVERAMDISK2AS:
            if (m_ramDisk2)
                m_ramDisk2->saveFileAs();
            break;
        case SR_FASTRESET:
            if (m_fastResetCpuTicks) {
                m_fastReset = !m_fastReset;
            }
            break;
        default:
            break;
    }
    g_emulation->resetKeys(nullptr);
}


void Platform::processKey(PalKeyCode keyCode, bool isPressed, unsigned unicodeKey)
{
    emuLog << "Platform::processKey " << to_string(keyCode) << " / " << isPressed << "\n";
    if (m_kbdLayout)
        m_kbdLayout->processKey(keyCode, isPressed, unicodeKey);
}


void Platform::resetKeys()
{
    if (m_kbdLayout)
        m_kbdLayout->resetKeys();
}


void Platform::mouseDrag(int x, int y)
{
    if (m_renderer)
        m_renderer->mouseDrag(x, y);
}


bool Platform::loadFile(string fileName, bool run)
{
    if (m_loader) {
        m_loader->loadFile(fileName, run);
        updateDebugger();
        return true;
    }
    return false;
}


void Platform::draw()
{
    if (m_core)
        m_core->draw();
}


void Platform::showDebugger()
{
}


void Platform::updateDebugger()
{
}


void Platform::reqScreenUpdateForDebug()
{
    if (m_renderer)
        m_renderer->prepareDebugScreen();
    if (m_renderer2)
        m_renderer2->prepareDebugScreen();
}


void Platform::updateScreenOnce()
{
    if (m_renderer)
        m_renderer->updateScreenOnce();
//    if (m_renderer2)
//        m_renderer2->updateScreenOnce();
    updateDebugger();
}
