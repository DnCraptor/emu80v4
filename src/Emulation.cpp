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

#include <hardware/watchdog.h>
#include <pico/stdlib.h>

#include "Pal.h"

#include "Globals.h"
#include "EmuObjects.h"
#include "Emulation.h"
#include "Vector.h"
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

    m_mixer = new SoundMixer();

    m_wavReader = new WavReader();

    m_prnWriter = new PrnWriter();

    setFrequency(1680000000);
    setSampleRate(96000);
    m_mixer->setVolume(6);
    setVsync(true);

    m_vector = new VectorCore();
}

Emulation::~Emulation()
{
    delete m_vector;
    delete m_prnWriter;
    delete m_wavReader;
    delete m_mixer;
}


void Emulation::registerActiveDevice(IActive* device)
{
    if (nDevices >= MAX_ACTIVE_DEVICES) {
        palMsgBox("Error: Too many active devices.", true);
        palRequestForQuit();
        return;
    }

    m_activeDevices[nDevices++] = device;
    inCycle = false;
}


void Emulation::unregisterActiveDevice(IActive* device)
{
    int index = 0;
    while (index < nDevices && m_activeDevices[index] != device)
        index++;

    if (index == nDevices)
        return;

    for (int i = index + 1; i < nDevices; i++)
        m_activeDevices[i - 1] = m_activeDevices[i];

    m_activeDevices[--nDevices] = nullptr;
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


void Emulation::processKey(PalKeyCode keyCode, bool isPressed, unsigned unicodeKey)
{
    if (m_vector)
        m_vector->processKey(keyCode, isPressed, unicodeKey);
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

void Emulation::resetKeys()
{
    isAltPressed = false;
    isShiftPressed = false;
    isCtrlPressed = false;
    if (m_vector)
        m_vector->resetKeys();
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
