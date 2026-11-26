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
#include <algorithm>

#include <hardware/watchdog.h>
#include <pico/stdlib.h>

#include "Pal.h"

#include "Globals.h"
#include "EmuObjects.h"
#include "Emulation.h"
#include "CmdLine.h"
#include "Platform.h"
#include "EmuWindow.h"
#include "EmuConfig.h"
#include "SoundMixer.h"
#include "WavReader.h"
#include "PrnWriter.h"
#include "FileLoader.h"
#include "EmuCalls.h"
#include "Shortcuts.h"
#include "OrionMenu.h"
#include "Cpu.h"
#include "CrtRenderer.h"

using namespace std;

Emulation::Emulation(CmdLine& cmdLine) : m_cmdLine(cmdLine)
{
    m_fpsLimit = 0;
    m_vsync = true;
    m_sampleRate = 50000;

    g_emulation = this;
    setName("emulation");

    addObject(this);

    m_config = new EmuConfig();
    m_config->setName("config");

    m_mixer = new SoundMixer;
    m_mixer->setName("soundMixer");

    m_wavReader = new WavReader;
    m_wavReader->setName("wavReader");

    m_prnWriter = new PrnWriter;
    m_prnWriter->setName("prnWriter");

    setFrequency(1680000000);
    m_mixer->setVolume(6);
    m_activePlatform = new Platform("orion/orion-2.conf", "orion");
    addChild(m_activePlatform);
    getConfig()->updateConfig();
}

Emulation::~Emulation()
{
    // Удяляем платформы с дочерними объектами
    for (auto it = m_platformList.begin(); it != m_platformList.end(); it++)
        delete (*it);

    delete m_config;
    delete m_wavReader; // перед m_mixer!
    delete m_mixer;

    // Удяляем оставшиеся объекты
    list<EmuObject*> tempList = m_objectList; // второй список, так как в деструкторе удаление из основного списка
    for (auto it = tempList.begin(); it != tempList.end(); it++)
        if ((*it) != this)
            delete (*it);
}


void Emulation::checkPlatforms()
{
    // Delete platforms without windows (missing conf files)
    // and request for quit if no platform left
    for (auto it = m_platformList.begin(); it != m_platformList.end();)
        if (!(*it)->getWindow())
            m_platformList.erase(it++);
        else
            it++;

    if (m_platformList.empty()) {
        palMsgBox("Error: Can't create platform, exiting.\nRun again to select another one.", true);
        palRequestForQuit();
    }
}


void Emulation::processCmdLine()
{
    // The firmware has no command-line platform or configuration loader.
}

bool Emulation::runPlatform(const string& platformName)
{
    return platformName == "orion" && m_activePlatform != nullptr;
}

void Emulation::newPlatform(const string& platformName)
{
    // Only one machine graph is created per boot. Model changes use the OSD.
    if (platformName == "orion")
        orionMenu();
}

void Emulation::registerActiveDevice(IActive* device)
{
    m_activeDevVector.push_back(device);
    nDevices++;
    m_activeDevices = m_activeDevVector.data();
    inCycle = false;
}


void Emulation::unregisterActiveDevice(IActive* device)
{
    m_activeDevVector.erase(remove(m_activeDevVector.begin(), m_activeDevVector.end(), device));
    nDevices--;
    m_activeDevices = m_activeDevVector.data();
    inCycle = false;
}


void Emulation::addObject(EmuObject* obj)
{
    m_objectList.push_back(obj);
}


void Emulation::removeObject(EmuObject* obj)
{
    m_objectList.remove(obj);
}


EmuObject* Emulation::findObject(string name)
{
    for (auto it = m_objectList.begin(); it != m_objectList.end(); it++)
        if ((*it)->getName() == name)
            return *it;
    return nullptr;
}


void Emulation::addChild(EmuObject* child)
{
    if (child)
        if (Platform* pl = child->asPlatform())
            m_platformList.push_back(pl);
};

/// TODO: .h
extern void processKeys();

void Emulation::exec(uint64_t ticks, bool forced)
{
    processKeys();
    if (m_isPaused)
        return;

    if (ticks <= m_clockOffset) {
        m_clockOffset -= ticks;
        return;
    }
    uint64_t toTime = m_curClock + ticks - m_clockOffset;
    IActive* cpuDevice = m_activePlatform ? m_activePlatform->getCpu() : nullptr;

    while ((m_curClock < toTime) && (!m_debugReqCpu || forced)) {
        uint64_t time = -1;
        uint64_t nextOther = -1;
        IActive* curDev = nullptr;

        inCycle = true;
        IActive* device;
        uint64_t tm;
        for (int i = 0; i < nDevices && inCycle; i++) {
            device = m_activeDevices[i];
            if (!device->isPaused()) {
                tm = device->getClock();
                if (device != cpuDevice && tm < nextOther) nextOther = tm;
                if (tm < time) {
                    time = tm;
                    curDev = device;
                }
            }
        }

        if (!inCycle) continue;
        if (!curDev) { m_curClock = toTime; break; }
        m_curClock = time;
        curDev->operate();
        if (curDev == cpuDevice) {
            // Keep the original event order: yield on ties, on registration
            // changes, and at the same emulation time-slice boundary.
            while (inCycle && !cpuDevice->isPaused() && (!m_debugReqCpu || forced) &&
                   m_curClock < toTime && cpuDevice->getClock() < nextOther) {
                m_curClock = cpuDevice->getClock();
                cpuDevice->operate();
            }
        }
    }

    m_clockOffset = m_curClock > toTime ? m_curClock - toTime : 0;

    if (!forced && m_debugReqCpu) {
        m_clockOffset = 0;
        // show debugger
        /**
        for (auto it = m_platformList.begin(); it != m_platformList.end(); it++)
        if ((*it)->getCpu() == m_debugReqCpu) {
            (*it)->showDebugger();
            break;
        }
        */
    }

}


void Emulation::screenUpdateReq()
{
    m_scrUpdateReq = true;
}


void Emulation::draw()
{
///    for (auto it = m_platformList.begin(); it != m_platformList.end(); it++) {
///        (*it)->draw();
///    }
    m_scrUpdateReq = false;
    m_timeAfterLastDraw = 0;
}


void Emulation::processKey(EmuWindow* wnd, PalKeyCode keyCode, bool isPressed, unsigned unicodeKey)
{
    // нужно отправлять клавишу только активной платформе
    Platform* platform = platformByWindow(wnd);
    if (platform)
        platform->processKey(keyCode, isPressed, unicodeKey);
    else if (keyCode != PK_NONE)
        wnd->processKey(keyCode, isPressed);
}

static bool isAltPressed = false;
static unsigned altKeys = 0;
static bool altStandalone = false;
static bool heldKeys[PK_JOY_RIGHT + 1]{};
static unsigned heldKeyCount = 0;
static bool isShiftPressed = false;
static bool isCtrlPressed = false;
void Emulation::activePlatformKey(PalKeyCode keyCode, bool isPressed, unsigned unicodeKey) {
#if LOG
    emuLog << to_string(keyCode) << " / " << isPressed << "\n";
#endif
    if (m_activePlatform) {
        // Orion has no Alt in the RK keyboard matrix. Open the OSD only on
        // a standalone Alt release, not when Alt was used in a shortcut.
        if (keyCode == PK_LALT || keyCode == PK_RALT) {
            const unsigned bit = keyCode == PK_LALT ? 1u : 2u;
            if (isPressed) {
                if (!(altKeys & bit)) {
                    altStandalone = !altKeys && heldKeyCount == 0;
                    altKeys |= bit;
                }
            } else if (altKeys & bit) {
                altKeys &= ~bit;
                const bool openMenu = !altKeys && altStandalone;
                isAltPressed = altKeys != 0;
                if (!altKeys) altStandalone = false;
                if (openMenu) orionMenu();
            }
            isAltPressed = altKeys != 0;
            return;
        }
        if (isPressed && altKeys && keyCode != PK_NONE) altStandalone = false;
        if (keyCode > PK_NONE && keyCode <= PK_JOY_RIGHT && heldKeys[keyCode] != isPressed) {
            heldKeys[keyCode] = isPressed;
            if (isPressed) ++heldKeyCount;
            else --heldKeyCount;
        }
        isShiftPressed = heldKeys[PK_LSHIFT] || heldKeys[PK_RSHIFT];
        isCtrlPressed = heldKeys[PK_LCTRL] || heldKeys[PK_RCTRL];
        if (isAltPressed && isCtrlPressed && keyCode == PK_DEL) {
            watchdog_enable(100, true);
            while(true) sleep_ms(20);
        }
        SysReq sr = TranslateKeyToSysReq(keyCode, isPressed, isAltPressed, isShiftPressed);
        if (sr) sysReq(m_activePlatform->getWindow(), sr);
        else m_activePlatform->processKey(keyCode, isPressed, unicodeKey);
    }
}

void Emulation::prepareOrionRestart()
{
    if (m_activePlatform) m_activePlatform->shutdown();
}

void Emulation::orionAction(SysReq sr)
{
    if (m_activePlatform) sysReq(m_activePlatform->getWindow(), sr);
}

void Emulation::restoreOrionScreen()
{
    if (m_activePlatform && m_activePlatform->getRenderer())
        m_activePlatform->getRenderer()->renderFrame();
}

void Emulation::resetOrionKeys()
{
    if (m_activePlatform) resetKeys(m_activePlatform->getWindow());
}

void Emulation::resetKeys(EmuWindow* wnd)
{
    isAltPressed = false;
    altKeys = 0;
    altStandalone = false;
    std::fill(heldKeys, heldKeys + PK_JOY_RIGHT + 1, false);
    heldKeyCount = 0;
    isShiftPressed = false;
    isCtrlPressed = false;
    Platform* platform = platformByWindow(wnd);
    if (platform)
        platform->resetKeys();
}


void Emulation::sysReq(EmuWindow* wnd, SysReq sr)
{
    Platform* platform = platformByWindow(wnd);

    // The Pico firmware has no interactive debugger.
    if (sr == SR_DEBUG) return;

    switch(sr) {
        case SR_EXIT:
        case SR_CLOSE:
            orionMenu();
            break;
        case SR_CONFIG:
            orionMenu();
            break;
        case SR_HELP:
            orionMenu();
            break;
        case SR_CHPLATFORM:
        case SR_CHCONFIG:
            orionMenu();
            break;
        case SR_PAUSEON:
            m_isPaused = true;
            break;
        case SR_PAUSEOFF:
            m_isPaused = false;
            break;
        case SR_PAUSE:
            m_isPaused = !m_isPaused;
            break;
        case SR_SPEEDUP:
            setTemporarySpeedUpFactorDbl(m_speedUpFactor <= 4 ? m_speedUpFactor * 4 : 16);
            break;
        case SR_FULLTHROTTLE:
            m_fullThrottle = true;
            setTemporarySpeedUpFactor(1);
            break;
        case SR_SPEEDNORMAL:
            m_fullThrottle = false;
            setTemporarySpeedUpFactor(0);
            break;
        case SR_SPEEDSTEPUP:
            if (m_speedGrade < 44)
                setSpeedByGrade(m_speedGrade += 4);
            else
                setSpeedByGrade(m_speedGrade = 48);
            break;
        case SR_SPEEDSTEPDOWN:
            if (m_speedGrade > -44)
                setSpeedByGrade(m_speedGrade -= 4);
            else
                setSpeedByGrade(m_speedGrade = -48);
            break;
            break;
        case SR_SPEEDSTEPUPFINE:
            if (m_speedGrade < 48)
                setSpeedByGrade(++m_speedGrade);
            break;
        case SR_SPEEDSTEPDOWNFINE:
            if (m_speedGrade > -48)
                setSpeedByGrade(--m_speedGrade);
            break;
        case SR_SPEEDSTEPNORMAL:
            m_speedGrade = 0;
            setSpeedByGrade(0);
            break;
        case SR_LOADWAV:
            if (m_wavReader) {
                m_wavReader->chooseAndLoadFile();
            }
            break;
        case SR_PRNCAPTURE_ON:
            m_prnWriter->startPrinting();
            break;
        case SR_PRNCAPTURE_OFF:
            m_prnWriter->stopPrinting();
            break;
        case SR_PRNCAPTURE:
            if (!m_prnWriter->isPrinting())
                m_prnWriter->startPrinting();
            else
                m_prnWriter->stopPrinting();
            break;
        case SR_MUTE:
            m_mixer->toggleMute();
            break;
        default:
            if (wnd)
                wnd->sysReq(sr);
            if (platform)
                platform->sysReq(sr);
    }
}

void Emulation::mainLoopCycle()
{
    if (m_prevSysClock == 0) // first run
        m_prevSysClock = palGetCounter() - palGetCounterFreq() / 500;
/**
    if (m_scrUpdateReq) {
        if (m_fpsLimit == 0 || m_timeAfterLastDraw > palGetCounterFreq() / m_fpsLimit) {
            draw();
        }
    } else {
        // min 30 fps when there are no requests for screen update from emulation core
        if (m_timeAfterLastDraw > palGetCounterFreq() / 30) {
            draw();
        }
    }
*/
    m_sysClock = palGetCounter();
    unsigned dt = m_sysClock - m_prevSysClock;
    m_timeAfterLastDraw += dt;

    // provide at least 20 fps when CPU power is not enough to emulate at 100% speed
    if (dt > palGetCounterFreq() / 20) // 1/20 s
        dt = palGetCounterFreq() / 20;

    uint64_t ticks = m_curFrequency * dt / palGetCounterFreq();
    m_prevSysClock = m_sysClock;

    int64_t cntBeforeExec = palGetCounter();
    exec(ticks);
    int64_t cntAfterExec = palGetCounter();

    int nn = 1;
    static int avgNn = 1;
    if (m_fullThrottle) {
        while (cntAfterExec - cntBeforeExec < (dt * 7 / 8)) {
            nn++;
            exec(ticks);
            cntAfterExec = palGetCounter();
        }
        avgNn = (2 * avgNn + nn) / 3;
        m_mixer->setFrequency(m_frequency * avgNn);
    }
}


void Emulation::setFrequency(int64_t freq)
{
    m_frequency = freq;
    updateFrequency();
}


// установка частоты кадров, 0 - max
void Emulation::setFrameRate(int frameRate)
{
    m_fpsLimit = frameRate;
    // для изменения "на лету" добавить перебор всех окон и пересоздание рендерера при необходимости
    //palSetFrameRate(frameRate);
}


// установка vsync
void Emulation::setVsync(bool vsync)
{
    m_vsync = vsync;
    palSetVsync(vsync);
}


void Emulation::setSampleRate(int sampleRate)
{
    if (palSetSampleRate(sampleRate)) {
        m_sampleRate = sampleRate;
        m_mixer->setFrequency(m_curFrequency);
    }
}


// Установка фокуса на окно
void Emulation::setWndFocus(EmuWindow* wnd)
{
    wnd->focusChanged(true);
    if (Platform* platform = platformByWindow(wnd))
        m_lastActivePlatform = platform;
}

void Emulation::restoreFocus()
{
    if (m_lastActivePlatform)
        m_lastActivePlatform->getWindow()->bringToFront();
}


void Emulation::dropFile(EmuWindow* wnd, const string& fileName)
{
    Platform* platform = platformByWindow(wnd);
    if (platform)
        platform->loadFile(fileName);
}


void Emulation::updateFrequency()
{
    m_curFrequency = m_frequency * m_currentSpeedUpFactor;
    m_mixer->setFrequency(m_curFrequency);
}


void Emulation::setTemporarySpeedUpFactor(unsigned speed)
{
    if (speed)
        m_currentSpeedUpFactor = speed;
    else  // speed = 0, cancel temporary speed up
        m_currentSpeedUpFactor = m_speedUpFactor;

    updateFrequency();
}


void Emulation::setTemporarySpeedUpFactorDbl(double speed)
{
    m_currentSpeedUpFactor = speed;
    updateFrequency();
}


void Emulation::setSpeedByGrade(int speedGrade)
{
    const double powers[12] = {1.0, 1.0595, 1.1225, 1.1892, 1.2599, 1.3348, 1.4242, 1.4983, 1.5874, 1.6818, 1.7818, 1.8877};

        if (speedGrade == 0) {
        m_speedUpFactor = m_currentSpeedUpFactor = 1.0;
        updateFrequency();
        return;
    }

    bool slowDown = speedGrade < 0;
    speedGrade = abs(speedGrade);
    double k = (1 << (speedGrade / 12)) * powers[speedGrade % 12];

    m_speedUpFactor = m_currentSpeedUpFactor = slowDown ? (1 / k) : k;
    updateFrequency();
}


Platform* Emulation::platformByWindow(EmuWindow* window)
{
    if (!window)
        return nullptr;

    for (auto it = m_platformList.begin(); it != m_platformList.end(); it++)
        if (window == (*it)->getWindow())
            return (*it);

    return nullptr;
}


bool Emulation::setProperty(const string& propertyName, const EmuValuesList& values)
{
    if (EmuObject::setProperty(propertyName, values))
        return true;

    /*if (propertyName == "frequency" && values[0].isInt()) {
        setFrequency(values[0].asInt());
        return true;
    } else*/
    if (propertyName == "maxFps" && values[0].isInt()) {
        setFrameRate(values[0].asInt());
        return true;
    } else if (propertyName == "vsync") {
        if (values[0].asString() == "yes" || values[0].asString() == "no") {
            setVsync(values[0].asString() == "yes");
            return true;
        }
    } else if (propertyName == "sampleRate" && values[0].isInt()) {
        setSampleRate(values[0].asInt());
        return true;
    } else if (propertyName == "volume" && values[0].isInt()) {
        m_mixer->setVolume(values[0].asInt());
        return true;
    } else if (propertyName == "runPlatform") {
        if (!m_platformCreatedFromCmdLine) // если уже было создано окно из командной строки, больше не создаем
            runPlatform(values[0].asString());
        return true;
    } else if (propertyName == "processCmdLine") {
        processCmdLine();
        return true;
    } else if (propertyName == "debug8080MnemoUpperCase") {
        if (values[0].asString() == "yes" || values[0].asString() == "no") {
            m_debuggerOptions.mnemo8080UpperCase = values[0].asString() == "yes";
            return true;
        }
    } else if (propertyName == "debugZ80MnemoUpperCase") {
        if (values[0].asString() == "yes" || values[0].asString() == "no") {
            m_debuggerOptions.mnemoZ80UpperCase = values[0].asString() == "yes";
            return true;
        }
    } else if (propertyName == "debugForceZ80Mnemonics") {
        if (values[0].asString() == "yes" || values[0].asString() == "no") {
            m_debuggerOptions.forceZ80Mnemonics = values[0].asString() == "yes";
            return true;
        }
    } else if (propertyName == "debugSwapF5F9") {
        if (values[0].asString() == "yes" || values[0].asString() == "no") {
            m_debuggerOptions.swapF5F9 = values[0].asString() == "yes";
            return true;
        }
    } else if (propertyName == "debugResetKeys") {
        if (values[0].asString() == "yes" || values[0].asString() == "no") {
            m_debuggerOptions.resetKeys = values[0].asString() == "yes";
            return true;
        }
    }

    return false;
}


string Emulation::getPropertyStringValue(const string& propertyName)
{
    string res;

    res = EmuObject::getPropertyStringValue(propertyName);
    if (res != "")
        return res;

    if (propertyName == "volume") {
        stringstream stringStream;
        stringStream << m_mixer->getVolume();
        stringStream >> res;
    } else if (propertyName == "debug8080MnemoUpperCase")
        res = m_debuggerOptions.mnemo8080UpperCase ? "yes" : "no";
    else if (propertyName == "debugZ80MnemoUpperCase")
        res = m_debuggerOptions.mnemoZ80UpperCase ? "yes" : "no";
    else if (propertyName == "debugForceZ80Mnemonics")
        res = m_debuggerOptions.forceZ80Mnemonics ? "yes" : "no";
    else if (propertyName == "debugSwapF5F9")
        res = m_debuggerOptions.swapF5F9 ? "yes" : "no";
    else if (propertyName == "debugResetKeys")
        res = m_debuggerOptions.resetKeys ? "yes" : "no";
    /* else if (propertyName == "frameRate") {
        stringstream stringStream;
        stringStream << m_frameRate;
        stringStream >> res;
    } else if (propertyName == "sampleRate") {
        stringstream stringStream;
        stringStream << m_sampleRate;
        stringStream >> res;
    }*/
    return res;
}
