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
#include "Vector.h"
#include "EmuWindow.h"
#include "SoundMixer.h"
#include "WavReader.h"
#include "PrnWriter.h"
#include "EmuCalls.h"
#include "Shortcuts.h"

using namespace std;

Emulation::Emulation()
{
    m_vsync = true;
    m_sampleRate = 48000;

    g_emulation = this;

    m_mixer = std::make_unique<SoundMixer>();

    m_wavReader = std::make_unique<WavReader>();

    m_prnWriter = std::make_unique<PrnWriter>();

    setFrequency(1680000000);
    setSampleRate(96000);
    m_mixer->setVolume(6);
    setVsync(true);

    m_debuggerOptions.swapF5F9 = true;
    m_debuggerOptions.mnemo8080UpperCase = true;
    m_debuggerOptions.mnemoZ80UpperCase = false;
    m_debuggerOptions.forceZ80Mnemonics = false;
    m_debuggerOptions.resetKeys = true;

    m_vector = std::make_unique<VectorCore>();
    if (!m_vector->getWindow()) {
        m_vector.reset();
        palMsgBox("Error: Can't create Vector-06C machine.", true);
        palRequestForQuit();
    }
}

Emulation::~Emulation() = default;


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


void Emulation::processKey(EmuWindow* wnd, PalKeyCode keyCode, bool isPressed, unsigned unicodeKey)
{
    // Эмуляционная клавиша относится к единственной машине.
    if (m_vector && wnd == m_vector->getWindow())
        m_vector->processKey(keyCode, isPressed, unicodeKey);
    else if (keyCode != PK_NONE)
        wnd->processKey(keyCode, isPressed);
}

static bool isAltPressed = false;
static bool isShiftPressed = false;
static bool isCtrlPressed = false;
void Emulation::machineKey(PalKeyCode keyCode, bool isPressed, unsigned unicodeKey) {
#if LOG
    emuLog << to_string(keyCode) << " / " << isPressed << "\n";
#endif
    if (m_vector) {
        if (keyCode == PK_LSHIFT || keyCode == PK_RSHIFT) isShiftPressed = isPressed;
        else if (keyCode == PK_LALT || keyCode == PK_RALT) isAltPressed = isPressed;
        else if (keyCode == PK_LCTRL || keyCode == PK_RCTRL) isCtrlPressed = isPressed;
        if (isAltPressed && isCtrlPressed && keyCode == PK_DEL) {
            watchdog_enable(100, true);
            while(true) sleep_ms(20);
        }
        SysReq sr = TranslateKeyToSysReq(keyCode, isPressed, isAltPressed, isShiftPressed);
        if (sr) m_vector->sysReq(sr);
        else m_vector->processKey(keyCode, isPressed, unicodeKey);
    }
}

void Emulation::resetKeys(EmuWindow* wnd)
{
    isAltPressed = false;
    isShiftPressed = false;
    isCtrlPressed = false;
    if (m_vector && wnd == m_vector->getWindow())
        m_vector->resetKeys();
}


void Emulation::sysReq(EmuWindow* wnd, SysReq sr)
{
    VectorCore* machine = m_vector && wnd == m_vector->getWindow()
        ? m_vector.get() : nullptr;

    // enable debug if in paused state
    if (sr == SR_DEBUG)
        m_isPaused = false;

    switch(sr) {
        case SR_EXIT:
            palRequestForQuit();
            break;
        case SR_CLOSE:
            if (machine) {
                m_vector.reset();
                palRequestForQuit();
            } else
                wnd->closeRequest();
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
            if (machine)
                machine->sysReq(sr);
    }
}

void Emulation::mainLoopCycle()
{
    if (m_prevSysClock == 0) // first run
        m_prevSysClock = palGetCounter() - palGetCounterFreq() / 500;
    m_sysClock = palGetCounter();
    unsigned dt = m_sysClock - m_prevSysClock;

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
    if (m_vector)
        m_vector->getWindow()->bringToFront();
}


void Emulation::dropFile(EmuWindow* wnd, const string& fileName)
{
    if (m_vector && wnd == m_vector->getWindow())
        m_vector->loadFile(fileName);
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
