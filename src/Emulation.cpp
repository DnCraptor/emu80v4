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
#include "Platform.h"
#include "EmuWindow.h"
#include "SoundMixer.h"
#include "WavReader.h"
#include "PrnWriter.h"
#include "EmuCalls.h"
#include "Shortcuts.h"

using namespace std;

Emulation::Emulation()
{
    m_fpsLimit = 0;
    m_vsync = true;
    m_sampleRate = 48000;

    g_emulation = this;
    setName("emulation");

    addObject(this);

    m_mixer = new SoundMixer;
    m_mixer->setName("soundMixer");

    m_wavReader = new WavReader;
    m_wavReader->setName("wavReader");

    m_prnWriter = new PrnWriter;
    m_prnWriter->setName("prnWriter");

    setFrequency(1680000000);
    setSampleRate(96000);
    m_mixer->setVolume(6);
    setFrameRate(100);
    setVsync(true);

    m_debuggerOptions.swapF5F9 = true;
    m_debuggerOptions.mnemo8080UpperCase = true;
    m_debuggerOptions.mnemoZ80UpperCase = false;
    m_debuggerOptions.forceZ80Mnemonics = false;
    m_debuggerOptions.resetKeys = true;

    m_activePlatform = new Platform("vector");
    if (!m_activePlatform->getWindow()) {
        delete m_activePlatform;
        m_activePlatform = nullptr;
        palMsgBox("Error: Can't create Vector-06C platform.", true);
        palRequestForQuit();
    }
}

Emulation::~Emulation()
{
    delete m_activePlatform;

    delete m_wavReader; // перед m_mixer!
    delete m_mixer;

    // Удяляем оставшиеся объекты
    list<EmuObject*> tempList = m_objectList; // второй список, так как в деструкторе удаление из основного списка
    for (auto it = tempList.begin(); it != tempList.end(); it++)
        if ((*it) != this)
            delete (*it);
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


/// TODO: .h
extern void processKeys();

void Emulation::exec(uint64_t ticks, bool forced)
{
    processKeys();
    if (m_isPaused)
        return;

    uint64_t toTime = m_curClock + ticks - m_clockOffset;

    while ((m_curClock < toTime) && (!m_debugReqCpu || forced)) {
        uint64_t time = -1;
        IActive* curDev = nullptr;

        inCycle = true;
        IActive* device;
        uint64_t tm;
        for (int i = 0; i < nDevices && inCycle; i++) {
            device = m_activeDevices[i];
            if (!device->isPaused()) {
                tm = device->getClock();
                if (tm < time) {
                    time = tm;
                    curDev = device;
                }
            }
        }

        m_curClock = time;
        curDev->operate();
    }

    m_clockOffset = m_curClock - toTime;

    if (!forced && m_debugReqCpu)
        m_clockOffset = 0;

}


void Emulation::screenUpdateReq()
{
    m_scrUpdateReq = true;
}


void Emulation::draw()
{
    m_scrUpdateReq = false;
    m_timeAfterLastDraw = 0;
}


void Emulation::processKey(EmuWindow* wnd, PalKeyCode keyCode, bool isPressed, unsigned unicodeKey)
{
    // нужно отправлять клавишу только активной платформе
    if (m_activePlatform && wnd == m_activePlatform->getWindow())
        m_activePlatform->processKey(keyCode, isPressed, unicodeKey);
    else if (keyCode != PK_NONE)
        wnd->processKey(keyCode, isPressed);
}

static bool isAltPressed = false;
static bool isShiftPressed = false;
static bool isCtrlPressed = false;
void Emulation::activePlatformKey(PalKeyCode keyCode, bool isPressed, unsigned unicodeKey) {
#if LOG
    emuLog << to_string(keyCode) << " / " << isPressed << "\n";
#endif
    if (m_activePlatform) {
        if (keyCode == PK_LSHIFT || keyCode == PK_RSHIFT) isShiftPressed = isPressed;
        else if (keyCode == PK_LALT || keyCode == PK_RALT) isAltPressed = isPressed;
        else if (keyCode == PK_LCTRL || keyCode == PK_RCTRL) isCtrlPressed = isPressed;
        if (isAltPressed && isCtrlPressed && keyCode == PK_DEL) {
            watchdog_enable(100, true);
            while(true) sleep_ms(20);
        }
        SysReq sr = TranslateKeyToSysReq(keyCode, isPressed, isAltPressed, isShiftPressed);
        if (sr) m_activePlatform->sysReq(sr);
        else m_activePlatform->processKey(keyCode, isPressed, unicodeKey);
    }
}

void Emulation::resetKeys(EmuWindow* wnd)
{
    isAltPressed = false;
    isShiftPressed = false;
    isCtrlPressed = false;
    if (m_activePlatform && wnd == m_activePlatform->getWindow())
        m_activePlatform->resetKeys();
}


void Emulation::sysReq(EmuWindow* wnd, SysReq sr)
{
    Platform* platform = m_activePlatform && wnd == m_activePlatform->getWindow()
        ? m_activePlatform : nullptr;

    // enable debug if in paused state
    if (sr == SR_DEBUG)
        m_isPaused = false;

    switch(sr) {
        case SR_EXIT:
            palRequestForQuit();
            break;
        case SR_CLOSE:
            if (platform) {
                delete m_activePlatform;
                m_activePlatform = nullptr;
                palRequestForQuit();
            } else
                wnd->closeRequest();
            break;
        case SR_CONFIG:
        case SR_HELP:
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
}

void Emulation::restoreFocus()
{
    if (m_activePlatform)
        m_activePlatform->getWindow()->bringToFront();
}


void Emulation::dropFile(EmuWindow* wnd, const string& fileName)
{
    if (m_activePlatform && wnd == m_activePlatform->getWindow())
        m_activePlatform->loadFile(fileName);
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
