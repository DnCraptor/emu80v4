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
#include "Cpu.h"
#include "SoundMixer.h"
#include "WavReader.h"
#include "PrnWriter.h"
#include "EmuCalls.h"
#include "Shortcuts.h"
#include "pico/picoMenu.h"

using namespace std;

Emulation::Emulation()
{
    // Только присваивания полей. Указатель g_emulation выставляется здесь же,
    // но не разыменовывается, поэтому конструктор безопасен и при статическом
    // размещении объекта. Всё остальное перенесено в init().
    m_vsync = true;
    m_sampleRate = 50000;   // 20 мкс на отсчёт ровно, см. picoPal.cpp

    g_emulation = this;
}


void Emulation::init()
{
    m_mixer = new SoundMixer();

    m_wavReader = new WavReader();

    m_prnWriter = new PrnWriter();

    setFrequency(1680000000);
    setSampleRate(50000);
    m_mixer->setVolume(5);
    setVsync(true);

    m_vector = new VectorCore();

    // Все объекты созданы — собираем активные устройства и источники звука.
    // Порядок обхода списков совпадает с порядком создания, то есть с прежним
    // порядком регистрации из конструкторов.
    registerActiveDevices();
    m_mixer->collectSoundSources();

    m_vector->init();
}


void Emulation::registerActiveDevices()
{
    for (IActive* device = IActive::firstActive(); device; device = device->nextActive())
        registerActiveDevice(device);
    m_activeDevicesRegistered = true;
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
}


int Emulation::getActiveDeviceIndex(IActive* device) const
{
    for (int i = 0; i < nDevices; i++)
        if (m_activeDevices[i] == device)
            return i;
    return -1;
}


void Emulation::moveActiveDevice(IActive* device, int newIndex)
{
    const int from = getActiveDeviceIndex(device);
    if (from < 0 || newIndex < 0 || newIndex >= nDevices || from == newIndex)
        return;

    if (from < newIndex)
        for (int i = from; i < newIndex; i++)
            m_activeDevices[i] = m_activeDevices[i + 1];
    else
        for (int i = from; i > newIndex; i--)
            m_activeDevices[i] = m_activeDevices[i - 1];

    m_activeDevices[newIndex] = device;
    m_cpuDev = nullptr;   // индексы изменились
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
    m_cpuDev = nullptr;   // индексы сдвинулись, определить CPU заново
}


/// TODO: .h
extern void processKeys();

void __not_in_flash_func(Emulation::exec)(uint64_t ticks, bool forced)
{
    processKeys();
    // forced означает «выполнить несмотря ни на что»: так загрузчик .fdd
    // прокручивает 25 млн тактов, чтобы ПЗУ успело загрузиться с дискеты.
    // Пока меню держит эмуляцию на паузе, без этой оговорки exec() выходил
    // сразу, и сценарий загрузки .fdd по Alt+F3 переставал работать.
    if (m_isPaused && !forced)
        return;

    uint64_t toTime = m_curClock + ticks - m_clockOffset;

    // Указатель на CPU нужен, чтобы не пересканировать массив на каждой его
    // команде. Состав активных устройств фиксирован с момента старта, поэтому
    // достаточно определить его один раз.
    if (!m_cpuDev && m_vector)
        m_cpuDev = m_vector->getCpu();   // Cpu -> ActiveDevice -> IActive

    while ((m_curClock < toTime) && (!m_debugReqCpu || forced)) {
        // Ближайшее событие среди всех устройств, кроме CPU. Раньше этот проход
        // выполнялся на каждую команду 8080, хотя побеждал в нём почти всегда
        // сам CPU: ближайшее чужое событие — звуковой сэмпл раз в ~7 команд.
        uint64_t next = uint64_t(-1);
        IActive* nextDev = nullptr;
        int nextIndex = nDevices;
        int cpuIndex = nDevices;

        for (int i = 0; i < nDevices; i++) {
            IActive* device = m_activeDevices[i];
            if (device == m_cpuDev) {
                cpuIndex = i;
                continue;
            }
            if (device->isPaused())
                continue;
            uint64_t tm = device->getClock();
            if (tm < next) {
                next = tm;
                nextDev = device;
                nextIndex = i;
            }
        }

        if (m_cpuDev && !m_cpuDev->isPaused()) {
            // Прежний цикл при равенстве тактов отдавал ход тому устройству,
            // которое стоит в массиве раньше. Совпадения такта CPU и рендерера
            // случаются на каждой границе кадра, поэтому порядок сохраняем:
            // граница включительная, если CPU зарегистрирован раньше соперника.
            uint64_t cpuLimit;
            if (!nextDev)
                cpuLimit = uint64_t(-1);
            else if (cpuIndex < nextIndex)
                cpuLimit = next + 1;
            else
                cpuLimit = next;

            uint64_t cpuClock;
            while (m_curClock < toTime && (cpuClock = m_cpuDev->getClock()) < cpuLimit) {
                m_curClock = cpuClock;
                m_cpuDev->operate();
            }
        }

        if (m_curClock >= toTime || !nextDev)
            break;

        m_curClock = next;
        nextDev->operate();
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
    if (!m_vector)
        return;

    const bool altKey = keyCode == PK_LALT || keyCode == PK_RALT;
    if (keyCode == PK_LSHIFT || keyCode == PK_RSHIFT)
        isShiftPressed = isPressed;
    else if (keyCode == PK_LCTRL || keyCode == PK_RCTRL)
        isCtrlPressed = isPressed;
    else if (altKey)
        isAltPressed = isPressed;

    if (altKey && isPressed) {
        if (palMainMenuIsOpen())
            palCloseMainMenu();
        else
            palOpenMainMenu();
        return;
    }

    if (palMainMenuIsOpen()) {
        // Отпускание Alt при открытом меню не делает ничего: меню закрывается
        // либо по Esc, либо каскадно после выполнения команды.
        if (altKey || !isPressed)
            return;

        // Навигация по меню имеет приоритет над горячими клавишами. Без этого
        // стрелки ушли бы в SR_SPEEDSTEPUPFINE/SR_SPEEDSTEPDOWNFINE, а Enter
        // не дошёл бы до раскрытия подменю.
        switch (keyCode) {
            case PK_ESC:
            case PK_UP:
            case PK_DOWN:
            case PK_LEFT:
            case PK_RIGHT:
            case PK_ENTER:
            case PK_KP_ENTER:
            case PK_SPACE:
                palMainMenuHandleKey(keyCode, true);
                return;
            default:
                break;
        }

        // Разрушительные команды из меню не запускаем
        if (keyCode == PK_F10 || keyCode == PK_F11 || keyCode == PK_F12)
            return;

        // Открытое меню само по себе означает «Alt нажат», поэтому сочетания
        // разбираются независимо от того, держится Alt физически или уже
        // отпущен. Прежний код требовал взведённого isAltPressed, и после
        // отпускания Alt клавиши F3/A/B переставали открывать файловый диалог.
        const SysReq sr = TranslateKeyToSysReq(keyCode, true, true, isShiftPressed);
        if (sr == SR_NONE)
            return;

        // Эмуляция остаётся на паузе, пока работает модальное окно: закрыв
        // меню раньше, мы бы отпустили рендерер, и он затёр бы диалог.
        m_vector->sysReq(sr);

        // Модальный диалог читает очередь клавиш сам, через getKey(), поэтому
        // отпускания Alt, Shift и Ctrl до machineKey не доходят и флаги
        // остались бы взведёнными: следующая же буква сработала бы как
        // сочетание с Alt, а стрелки в меню — как изменение скорости.
        resetKeys();

        // Каскадное закрытие: закрылось файловое меню — закрывается и главное
        palCloseMainMenu();
        return;
    }

    if (isAltPressed && isCtrlPressed && keyCode == PK_DEL) {
        watchdog_enable(100, true);
        while(true) sleep_ms(20);
    }

    const SysReq sr = TranslateKeyToSysReq(keyCode, isPressed, isAltPressed, isShiftPressed);
    if (sr)
        m_vector->sysReq(sr);
    else
        m_vector->processKey(keyCode, isPressed, unicodeKey);
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
