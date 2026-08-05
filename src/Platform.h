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


#include "PalKeys.h"
#include "EmuTypes.h"
#include "EmuObjects.h"

class EmuWindow;
class Cpu;
class FileLoader;
class PlatformCore;
class KbdLayout;
class CrtRenderer;
class Keyboard;


class Platform : public EmuObject
{
    public:
        virtual Platform* asPlatform() override { return this; }
        Platform();
        virtual ~Platform();
        void init() override;
        void shutdown() override;
        void reset() override;

        void sysReq(SysReq sr);
        virtual void draw();
        void processKey(PalKeyCode keyCode, bool isPressed, unsigned unicodeKey = 0);
        void resetKeys();
        bool loadFile(std::string fileName, bool run = true);
        void mouseDrag(int x, int y);
        void updateScreenOnce();

        const std::string& getBaseDir()
        {
            static const std::string empty;
            return empty;
        }

        EmuWindow* getWindow() {return m_window;}
        Cpu* getCpu() {return m_cpu;}
        FileLoader* getLoader() {return m_loader;}
        PlatformCore* getCore() {return m_core;}
        KbdLayout* getKbdLayout() {return m_kbdLayout;}
        CrtRenderer* getRenderer() {return m_renderer;}
        Keyboard* getKeyboard() {return m_keyboard;}

        void attachWindow(EmuWindow* window) {m_window = window;}
        void attachCpu(Cpu* cpu) {m_cpu = cpu;}
        void attachCore(PlatformCore* core) {m_core = core;}
        void attachKbdLayout(KbdLayout* layout) {m_kbdLayout = layout;}
        void attachRenderer(CrtRenderer* renderer) {m_renderer = renderer;}
        void attachLoader(FileLoader* loader) {m_loader = loader;}
        void attachKeyboard(Keyboard* keyboard) {m_keyboard = keyboard;}
        using LifecycleCallback = void (*)();

        void setFastReset(bool enabled, int cpuTicks) {m_fastReset = enabled; m_fastResetCpuTicks = cpuTicks;}
        void setLifecycleCallbacks(
            LifecycleCallback initCallback,
            LifecycleCallback resetCallback,
            LifecycleCallback shutdownCallback
        ) {
            m_initCallback = initCallback;
            m_resetCallback = resetCallback;
            m_shutdownCallback = shutdownCallback;
        }
        void start();

        void showDebugger();
        void updateDebugger();
        void reqScreenUpdateForDebug();
        CodePage getCodePage() {return m_codePage;}
        bool getMuteTapeFlag() {return m_muteTape;}

        const std::string& getBaseName()
        {
            static const std::string baseName = "lvov";
            return baseName;
        }

    private:
        LifecycleCallback m_initCallback = nullptr;
        LifecycleCallback m_resetCallback = nullptr;
        LifecycleCallback m_shutdownCallback = nullptr;

        PlatformCore* m_core = nullptr;
        Cpu* m_cpu = nullptr;
        EmuWindow* m_window = nullptr;
        KbdLayout* m_kbdLayout = nullptr;
        CrtRenderer* m_renderer = nullptr;
        FileLoader* m_loader = nullptr;
        Keyboard* m_keyboard = nullptr;

        CodePage m_codePage = CP_RK;
        bool m_muteTape = false;
        bool m_fastReset = false;
        int m_fastResetCpuTicks = 0;
};


#endif  // PLATFORM_H
