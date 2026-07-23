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

// Pit8253.cpp

// Реализация программируемого интервального таймера КР580ВИ53

#include "Globals.h"
#include "Emulation.h"
#include "Pit8253.h"
#include "Vector.h"

using namespace std;


Pit8253Counter::Pit8253Counter(Pit8253* pit)
{
    m_pit = pit;
    m_prevClock = g_emulation->getCurClock();
    m_clockPhase = uint32_t(m_prevClock % uint64_t(m_kDiv));
    m_isCounting = false;
    m_gate = true;
    m_out = false;
    m_counterInitValue = 0xffff;
    m_counter = 0xffff;
    m_mode = 0;
    m_countDelay = 0;
}


void __not_in_flash_func(Pit8253Counter::planIrq)()
{
    if (!m_helper)
        return;

    int ticksToUpdate = 0;

    if (m_mode == 3)
        ticksToUpdate = (m_counter + 1) / 2 + m_countDelay;
    else if (m_mode == 2)
        ticksToUpdate = !m_out ? 1 : m_counter - 1  + m_countDelay;
    else if (m_mode == 0 && !m_out)
        ticksToUpdate = m_counter + m_countDelay;

    if (ticksToUpdate)
        m_helper->updateAndScheduleNext((g_emulation->getCurClock() / m_kDiv + ticksToUpdate) * m_kDiv);
    else
        m_helper->updateAndScheduleNext(0); // don't schedule, just update and generate int if necessary
}


void __not_in_flash_func(Pit8253Counter::operateForTicks)(int ticks)
{
    if (m_countDelay) {
        int ticksToSkip = min(ticks, m_countDelay);
        ticks -= ticksToSkip;
        m_countDelay -= ticksToSkip;
        if (m_out)
            m_tempSumOut += ticksToSkip;
        if (!ticks)
            return;
    }

    if (!m_gate) {
        if (m_out)
            m_tempSumOut += ticks;
        return;
    }

    switch (m_mode) {
        case 0:
            if (m_isCounting) {
                if (!m_out) {
                    if (ticks >= m_counter) {
                        m_tempSumOut += (ticks - m_counter);
                        m_out = true;
                        ++m_sumOutTicks;
                    }
                } else
                    m_tempSumOut += ticks;
                m_counter = (m_counter - ticks) & 0xffff;
            }
            break;
        case 2:
            {
                if (!m_isCounting) {
                    m_tempSumOut += ticks;
                    break;
                }

                int fullCycles = ticks / m_counterInitValue;

                m_tempSumOut += fullCycles * (m_counterInitValue - 1);
                m_sumOutTicks += fullCycles;
                ticks -= fullCycles * m_counterInitValue;

                m_counter -= ticks;
                m_tempSumOut += ticks;
                if (m_counter <= 0) {
                    m_tempSumOut--;
                    m_sumOutTicks++;
                    m_counter += m_counterInitValue;
                }
                m_out = m_counter != 1;
            }
        break;
        case 3:
            {
                if (!m_isCounting) {
                    m_tempSumOut += ticks;
                    break;
                }

                int hiPeriod = (m_counterInitValue + 1) / 2;
                int loPeriod = m_counterInitValue / 2;
                if (m_counterInitValue == 3)
                    loPeriod = 32769;
                else if (m_counterInitValue == 1) {
                    hiPeriod = 32769;
                    loPeriod = 32768;
                }

                int fullPeriod = hiPeriod + loPeriod;

                int fullCycles = ticks / fullPeriod;
                m_tempSumOut += fullCycles * hiPeriod;
                m_sumOutTicks += fullCycles;
                ticks -= fullCycles * fullPeriod;

                int counter = m_out ? (m_counter + 1) / 2 : m_counter / 2;

                int last = counter;
                int curPeriod = m_out ? hiPeriod : loPeriod;
                int nextPeriod = m_out ? loPeriod : hiPeriod;

                if (ticks < last) {
                    counter -= ticks;
                    if (m_out)
                        m_tempSumOut += ticks;
                } else if (ticks < last + nextPeriod) {
                    counter = nextPeriod - (ticks - last);
                    if (m_out)
                        m_tempSumOut += last;
                    else
                        m_tempSumOut += (ticks - last);
                    m_out = !m_out;
                    if (m_out)
                        ++m_sumOutTicks;
                } else { // if (ticks >= last + nextPeriod)
                    counter = curPeriod - (ticks - last - nextPeriod);
                    if (m_out)
                        m_tempSumOut += (ticks - nextPeriod);
                    else
                        m_tempSumOut += nextPeriod;
                }
                m_counter = counter * 2;
                if (m_counter > fullPeriod)
                    --m_counter;
            }
            break;
        case 1:
        case 4:
        case 5:
        default:
            if (m_out)
                m_tempSumOut += ticks;
            break;
    }

}

void __not_in_flash_func(Pit8253Counter::updateState)()
{
    const uint64_t curClock = g_emulation->getCurClock();
    const uint32_t kDiv = uint32_t(m_kDiv);

    // Прежний код брал два частных и два остатка от полного 64-битного такта
    // эмуляции, то есть до четырёх вызовов __aeabi_uldivmod (аппаратного
    // 64-битного деления нет ни у RP2040, ни у RP2350). Метод вызывается для
    // каждого из трёх счётчиков на каждый звуковой сэмпл, то есть ~576000 раз
    // в секунду при 48 кГц.
    //
    // Поскольку m_clockPhase инвариантно равно m_prevClock % m_kDiv, справедливо
    //     (m_prevClock % kDiv) + (cur - m_prevClock) == cur - kDiv * (m_prevClock / kDiv),
    // поэтому частное от этой суммы даёт ровно cur/kDiv - m_prevClock/kDiv,
    // а остаток — ровно cur % kDiv. Результат совпадает с прежним побитово.
    const uint32_t prevPhase = m_clockPhase;
    const uint64_t delta = curClock - m_prevClock;

    int ticks;
    uint32_t curPhase;

    if (delta < 0x80000000ull) {
        const uint32_t acc = prevPhase + uint32_t(delta);
        ticks = int(acc / kDiv);
        curPhase = acc - uint32_t(ticks) * kDiv;
    } else {
        // Редкий путь: пауза, отладчик, ход часов назад после рассинхронизации.
        ticks = int(curClock / m_kDiv - m_prevClock / m_kDiv);
        curPhase = uint32_t(curClock % uint64_t(kDiv));
    }

    // Порядок сохранён: operateForTicks() может изменить m_out даже при ticks == 0
    // (режимы 2 и 3 пересчитывают m_out безусловно), поэтому две проверки m_out
    // не сворачиваются в одну и ранний выход при нулевом приращении недопустим.
    if (m_out)
        m_tempAddOutClocks -= int(prevPhase);

    operateForTicks(ticks);

    if (m_out)
        m_tempAddOutClocks += int(curPhase);

    m_clockPhase = curPhase;
    m_prevClock = curClock;
}


int __not_in_flash_func(Pit8253Counter::getAvgOut)()
{
    const uint64_t curClock = g_emulation->getCurClock();
    m_avgOut = 0;

    const uint64_t dt = curClock - m_sampleClock;
    if (dt) {
        // Числитель — время, проведённое выходом во взведённом состоянии,
        // в тактах эмуляции.
        int num = m_tempSumOut * m_kDiv + m_tempAddOutClocks;

        // Он может оказаться отрицательным: если выход был высоким в начале
        // интервала и упал внутри operateForTicks(), вычитание фазы уже
        // выполнено, а парного прибавления не будет. Прежний код приводил
        // такое значение к uint64_t и получал ~1.8e19, то есть заведомо
        // испорченный сэмпл. Отрицательного времени быть не может.
        if (num < 0)
            num = 0;

        // dt — длительность одного сэмпла (~35000 тактов при 48 кГц), поэтому
        // num * MAX_SND_AMP помещается в 32 бита и деление из 64-битного
        // становится 32-битным. Метод вызывается трижды на каждый сэмпл.
        if (dt <= 0xFFFFFFFFull && num <= 0x7FFFFFFF / MAX_SND_AMP)
            m_avgOut = int(uint32_t(num) * uint32_t(MAX_SND_AMP) / uint32_t(dt));
        else
            m_avgOut = int(uint64_t(num) * MAX_SND_AMP / dt);
    }
    return m_avgOut;
}



void Pit8253Counter::resetStats()
{
    m_avgOut = 0;
    m_sumOutTicks = 0;
    m_tempSumOut = 0;
    m_tempAddOutClocks = 0;
    const uint64_t clock = g_emulation->getCurClock();
    // resetStats() вызывается сразу после updateState() на том же такте, поэтому
    // фаза почти всегда уже актуальна. Пересчитываем её только если такт всё-таки
    // другой, чтобы не вернуть 64-битное деление на путь каждого сэмпла.
    if (clock != m_prevClock)
        m_clockPhase = uint32_t(clock % uint64_t(m_kDiv));
    m_prevClock = clock;
    m_sampleClock = m_prevClock;
}


void Pit8253Counter::setMode(int mode)
{
    if (!m_extClockMode)
        updateState();

    m_mode = mode;

    switch (m_mode) {
        case 0:
            m_out = false;
            m_isCounting = false;
            break;
        case 2:
        case 3:
            m_isCounting = false;
            m_out = true;
            break;
        case 1:
        case 4:
        case 5:
            // not implemented yet
            m_isCounting = false;
            m_out = true;
            break;
        default:
            break;
    }
    planIrq();
}


void Pit8253Counter::setHalfOfCounter()
{

    switch (m_mode) {
        case 0:
            m_isCounting = false;
        case 2:
        case 3:
            break;
        case 1:
        case 4:
        case 5:
            // not implemented yet
            break;
    }
    planIrq();
}


void Pit8253Counter::setCounter(uint16_t counter)
{
    if (!m_extClockMode)
        updateState();

    m_counterInitValue = counter;

    if (m_counterInitValue == 0)
        m_counterInitValue = 0x10000;

    switch (m_mode) {
        case 0:
            m_counter = m_counterInitValue;
            m_isCounting = true;
            m_out = false;
            m_countDelay = 2;
            break;
        case 2:
        case 3:
            if (!m_isCounting)
                m_counter = m_counterInitValue;
            m_isCounting = true;
            m_countDelay = 2;
            break;
        case 1:
        case 4:
        case 5:
        default:
            // not implemented yet
            break;
    }

    planIrq();
}


void Pit8253Counter::setGate(bool gate)
{
    if (gate == m_gate)
        return;

    if (!m_extClockMode)
        updateState();

    m_gate = gate;
    switch (m_mode) {
        case 0:
            m_isCounting = gate;
            break;
        case 2:
        case 3:
            m_isCounting = gate;
            m_out = !gate;
            if (gate)
                m_counter = m_counterInitValue;
            break;
        default:
            break;
    }

    planIrq();
}


bool Pit8253Counter::getOut()
{
    switch (m_mode) {
        case 0:
            return m_out;
        case 2:
        case 3:
            return (m_isCounting && m_out) || !m_gate;
        default:
            return true;
    }
}


Pit8253::Pit8253() :
    m_counter0(this),
    m_counter1(this),
    m_counter2(this),
    m_counters{&m_counter0, &m_counter1, &m_counter2}
{
    for (int i = 0; i < 3; i++) {
        m_counters[i]->m_kDiv = m_kDiv;
        m_counters[i]->syncClockPhase();
    }

    for (int i = 0; i < 3; i++) {
        m_latches[i] = 0;
    }
}


Pit8253::~Pit8253() = default;


void Pit8253::setFrequency(int64_t freq)
{
    EmuObject::setFrequency(freq);
    for (int i = 0; i < 3; i++) {
        m_counters[i]->m_kDiv = m_kDiv;
        m_counters[i]->syncClockPhase();
    }
}


void Pit8253::mute()
{
    for (int i = 0; i < 3; i++) {
        m_counters[i]->setMode(0);
    }
}


void __not_in_flash_func(Pit8253::updateState)()
{
    for (int i = 0; i < 3; i++)
        m_counters[i]->updateState();
}


bool Pit8253::getOut(int counter)
{
    return m_counters[counter]->getOut();
}


void Pit8253::writeByte(int addr, uint8_t value)
{
    addr &= 0x3;

    if (addr == 0x03) {
        // CSW
        int counterNum = (value & 0xC0) >> 6;
        if (counterNum == 3)
            return; // некорректный номер счетчика
        uint8_t loadMode = (value & 0x30) >> 4;
        uint8_t counterMode = (value & 0x0E) >> 1;
        if (counterMode == 6 || counterMode == 7)
            counterMode &= 3;
        if (loadMode == PRLM_LATCH) {
            // команда защелкивания
            if (!m_latched[counterNum]) {
                if (!m_counters[counterNum]->m_extClockMode)
                    m_counters[counterNum]->updateState();
                m_latched[counterNum] = true;
                m_latches[counterNum] = m_counters[counterNum]->m_counter;
                m_waitingHi[counterNum] = false;
            }
        } else {
            // установка режима счетчика
            m_latches[counterNum] = 0;
            m_rlModes[counterNum] = (PitReadLoadMode)loadMode;
            m_latched[counterNum] = false;
            m_waitingHi[counterNum] = (loadMode == PRLM_HIGHBYTE);
            m_counters[counterNum]->setMode(counterMode);
        }

    } else {
        // регистр счетчика
        if (!m_waitingHi[addr]) {
            m_latches[addr] = (m_latches[addr] & 0xff00) | value;
            if (m_rlModes[addr] == PRLM_WORD) {
                m_waitingHi[addr] = true;
                m_counters[addr]->setHalfOfCounter();
            }
            else
                m_counters[addr]->setCounter(m_latches[addr]);
        }
        else { // if m_rlStates[addr] = PRLS_WAITHIGH
            m_latches[addr] = (m_latches[addr] & 0xff) | (value << 8);
            if (m_rlModes[addr] == PRLM_WORD)
                m_waitingHi[addr] = false;
            m_counters[addr]->setCounter(m_latches[addr]);
        }
    }
}


uint8_t Pit8253::readByte(int addr)
{
    addr &= 0x3;

    if (addr == 0x03) {
        return 0xFF; //!!!
    } else {
        // регистр счетчика
        if (!m_counters[addr]->getExtClockMode())
            m_counters[addr]->updateState();
        uint16_t cntVal = m_latched[addr] ? m_latches[addr] : m_counters[addr]->m_counter;
        uint8_t res = m_waitingHi[addr] ? (cntVal & 0xff00) >> 8 : cntVal & 0xff;
        if (/* m_latched[addr] && */ m_waitingHi[addr])
            m_latched[addr] = false;

        if (m_rlModes[addr] == PRLM_WORD)
            m_waitingHi[addr] = !m_waitingHi[addr];

        return res;
    }
}

Pit8253Helper::Pit8253Helper()
{
    pause();
}


void __not_in_flash_func(Pit8253Helper::updateAndScheduleNext)(uint64_t time)
{
    if (time) {
        m_curClock = time;
        resume();
    } else
        pause();
}


void __not_in_flash_func(Pit8253Helper::operate)()
{
    m_counter->updateState();
    m_counter->planIrq();
}

namespace {

#pragma pack(push, 1)
struct Pit8253CounterSnapshotStateV1 {
    uint64_t prevClock;
    uint64_t sampleClock;
    int32_t avgOut;
    int32_t sumOutTicks;
    int32_t tempSumOut;
    int32_t tempAddOutClocks;
    uint32_t clockPhase;
    int32_t mode;
    int32_t counter;
    int32_t counterInitValue;
    int32_t countDelay;
    uint8_t extClockMode;
    uint8_t gate;
    uint8_t out;
    uint8_t isCounting;
};

struct Pit8253SnapshotStateV1 {
    Pit8253CounterSnapshotStateV1 counters[3];
    uint16_t latches[3];
    uint8_t latched[3];
    uint8_t rlModes[3];
    uint8_t waitingHi[3];
};

struct Pit8253SnapshotStateV2 {
    Pit8253SnapshotStateV1 pit;
    uint64_t helperClock[3];
    uint8_t helperPaused[3];
};
#pragma pack(pop)

}

uint32_t Pit8253::snapshotSectionId() const
{
    return makeSnapshotSectionId('P', 'I', 'T', ' ');
}

uint16_t Pit8253::snapshotSectionVersion() const
{
    return 2;
}

bool Pit8253::saveState(SnapshotWriter& writer) const
{
    Pit8253SnapshotStateV2 state{};
    for (int i = 0; i < 3; i++) {
        const Pit8253Counter& counter = *m_counters[i];
        Pit8253CounterSnapshotStateV1& dst = state.pit.counters[i];
        dst.prevClock = counter.m_prevClock;
        dst.sampleClock = counter.m_sampleClock;
        dst.avgOut = counter.m_avgOut;
        dst.sumOutTicks = counter.m_sumOutTicks;
        dst.tempSumOut = counter.m_tempSumOut;
        dst.tempAddOutClocks = counter.m_tempAddOutClocks;
        dst.clockPhase = counter.m_clockPhase;
        dst.mode = counter.m_mode;
        dst.counter = counter.m_counter;
        dst.counterInitValue = counter.m_counterInitValue;
        dst.countDelay = counter.m_countDelay;
        dst.extClockMode = counter.m_extClockMode ? 1 : 0;
        dst.gate = counter.m_gate ? 1 : 0;
        dst.out = counter.m_out ? 1 : 0;
        dst.isCounting = counter.m_isCounting ? 1 : 0;
        state.pit.latches[i] = m_latches[i];
        state.pit.latched[i] = m_latched[i] ? 1 : 0;
        state.pit.rlModes[i] = static_cast<uint8_t>(m_rlModes[i]);
        state.pit.waitingHi[i] = m_waitingHi[i] ? 1 : 0;
        if (counter.m_helper) {
            state.helperClock[i] = counter.m_helper->getScheduledClock();
            state.helperPaused[i] = counter.m_helper->isSchedulePaused() ? 1 : 0;
        } else {
            state.helperClock[i] = uint64_t(-1);
            state.helperPaused[i] = 1;
        }
    }
    return writer.writeValue(state);
}

bool Pit8253::loadState(SnapshotReader& reader, uint16_t version)
{
    Pit8253SnapshotStateV1 state{};
    uint64_t helperClock[3] = {};
    uint8_t helperPaused[3] = {};
    bool hasHelperSchedule = false;

    if (version == 1) {
        if (reader.remaining() != sizeof(Pit8253SnapshotStateV1) ||
            !reader.readValue(state))
            return false;
    } else if (version == snapshotSectionVersion()) {
        if (reader.remaining() != sizeof(Pit8253SnapshotStateV2))
            return false;
        Pit8253SnapshotStateV2 stateV2{};
        if (!reader.readValue(stateV2))
            return false;
        state = stateV2.pit;
        for (int i = 0; i < 3; i++) {
            helperClock[i] = stateV2.helperClock[i];
            helperPaused[i] = stateV2.helperPaused[i];
            if (helperPaused[i] > 1)
                return false;
        }
        hasHelperSchedule = true;
    } else
        return false;

    for (int i = 0; i < 3; i++) {
        const Pit8253CounterSnapshotStateV1& src = state.counters[i];
        if (src.mode < 0 || src.mode > 5 || src.counter < 0 ||
            src.counter > 0x10000 || src.counterInitValue < 1 ||
            src.counterInitValue > 0x10000 || src.countDelay < 0 ||
            src.extClockMode > 1 || src.gate > 1 || src.out > 1 ||
            src.isCounting > 1 || state.latched[i] > 1 ||
            state.rlModes[i] > PRLM_WORD || state.waitingHi[i] > 1)
            return false;
    }

    for (int i = 0; i < 3; i++) {
        Pit8253Counter& counter = *m_counters[i];
        const Pit8253CounterSnapshotStateV1& src = state.counters[i];
        counter.m_prevClock = src.prevClock;
        counter.m_sampleClock = src.sampleClock;
        counter.m_avgOut = src.avgOut;
        counter.m_sumOutTicks = src.sumOutTicks;
        counter.m_tempSumOut = src.tempSumOut;
        counter.m_tempAddOutClocks = src.tempAddOutClocks;
        counter.m_clockPhase = src.clockPhase;
        counter.m_mode = src.mode;
        counter.m_counter = src.counter;
        counter.m_counterInitValue = src.counterInitValue;
        counter.m_countDelay = src.countDelay;
        counter.m_extClockMode = src.extClockMode != 0;
        counter.m_gate = src.gate != 0;
        counter.m_out = src.out != 0;
        counter.m_isCounting = src.isCounting != 0;
        m_latches[i] = state.latches[i];
        m_latched[i] = state.latched[i] != 0;
        m_rlModes[i] = static_cast<PitReadLoadMode>(state.rlModes[i]);
        m_waitingHi[i] = state.waitingHi[i] != 0;
        if (hasHelperSchedule && counter.m_helper)
            counter.m_helper->restoreSchedule(helperClock[i], helperPaused[i] != 0);
    }
    m_snapshotHasHelperSchedule = hasHelperSchedule;
    return true;
}

void Pit8253::postLoad()
{
    for (int i = 0; i < 3; i++) {
        Pit8253Counter& counter = *m_counters[i];
        if (counter.m_clockPhase >= static_cast<uint32_t>(counter.m_kDiv))
            counter.syncClockPhase();
        if (!m_snapshotHasHelperSchedule)
            counter.planIrq();
    }
    m_snapshotHasHelperSchedule = false;
}

