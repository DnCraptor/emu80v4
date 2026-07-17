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

// platform.h

#ifndef PLATFORM_H
#define PLATFORM_H

#include <list>
#include <string>
#include <vector>

#include "PalKeys.h"
#include "EmuTypes.h"
#include "EmuObjects.h"

class EmuWindow;
class Cpu;
class FileLoader;
class RamDisk;
class PlatformCore;
class KbdLayout;
class CrtRenderer;
class Keyboard;
class DiskImage;
class KbdTapper;
class CpuHook;


class Platform : public EmuObject
{
    public:
        explicit Platform();
        virtual ~Platform();
        void addChild(EmuObject* child);
        void init() override;
        void shutdown() override;
        void reset() override;

        void sysReq(SysReq sr);
        virtual void draw();
        void processKey(PalKeyCode keyCode, bool isPressed, unsigned unicodeKey = 0);
        void resetKeys();
        bool loadFile(std::string fileName, bool run = true);
        void mouseDrag(int x, int y);

        EmuWindow* getWindow() {return m_window;}
        Cpu* getCpu() {return m_cpu;}
        FileLoader* getLoader() {return m_loader;}
        PlatformCore* getCore() {return m_core;}
        KbdLayout* getKbdLayout() {return m_kbdLayout;}
        CrtRenderer* getRenderer() {return m_renderer;}
        Keyboard* getKeyboard() {return m_keyboard;}
        bool assignDiskAFileName(const std::string& fileName);


    private:
        std::list<EmuObject* >m_objList;

        PlatformCore* m_core = nullptr;
        Cpu* m_cpu = nullptr;
        EmuWindow* m_window = nullptr;
        KbdLayout* m_kbdLayout = nullptr;
        CrtRenderer* m_renderer = nullptr;
        DiskImage* m_diskA = nullptr;
        DiskImage* m_diskB = nullptr;
        DiskImage* m_hdd = nullptr;
        FileLoader* m_loader = nullptr;
        Keyboard* m_keyboard = nullptr;
        RamDisk* m_ramDisk = nullptr;
        RamDisk* m_ramDisk2 = nullptr;
        std::vector<CpuHook*> m_tapeHooks;
        KbdTapper* m_kbdTapper = nullptr;

};


#endif  // PLATFORM_H
