/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2018-2024
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

// Psg3919.cpp

// Implementation of programmable sound generator AY-3-3910
// (Inspired by Emuscriptoria project)


#include <string>
#include <cstring>

#include "Globals.h"
#include "Emulation.h"
#include "Psg3910.h"
#include "Vector.h"
#include "hway.h"
#include "ff.h"
#include <pico/time.h>

// --- Лог записей в регистры AY ---
// Пишется сразу на карту, строкой фиксированной длины на каждую команду.
// snprintf не используется: форматирование ручное. Логируются только
// команды AY (регистр и значение), значения звукового потока — нет,
// поэтому объём одинаков в HWAY и в PWM.
//
// Формат строки: TTTTTTTT R VV F
//   T — время в микросекундах, шестнадцатерично
//   R — номер регистра, V — значение
//   F — H, если запись ушла в реальный чип, иначе "-"
//
// Файл открывается сам при первой записи и синхронизируется каждые 64
// строки, поэтому останавливать лог вручную не требуется: достаточно
// выключить питание после нужного фрагмента.
namespace {

#if AY_DEBUG
FIL      s_ayLog;
int      s_ayLogState = 0;      // 0 — не открыт, 1 — пишем, -1 — ошибка
unsigned s_ayLogCount = 0;

inline char ayHex(unsigned v) { return "0123456789ABCDEF"[v & 0xF]; }

void ayLogWrite(uint8_t reg, uint8_t val, bool toHw)
{
    if (s_ayLogState < 0)
        return;
    if (s_ayLogState == 0) {
        if (f_open(&s_ayLog, "/v06c_ay.log", FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
            s_ayLogState = -1;
            return;
        }
        s_ayLogState = 1;
    }

    char b[16];
    uint32_t t = time_us_32();
    for (int i = 7; i >= 0; --i) { b[i] = ayHex(t); t >>= 4; }
    b[8]  = ' ';
    b[9]  = ayHex(reg);
    b[10] = ' ';
    b[11] = ayHex(val >> 4);
    b[12] = ayHex(val);
    b[13] = ' ';
    b[14] = toHw ? 'H' : '-';
    b[15] = '\n';

    UINT bw;
    f_write(&s_ayLog, b, sizeof(b), &bw);
    if (++s_ayLogCount >= 64) {
        s_ayLogCount = 0;
        f_sync(&s_ayLog);
    }
}
#else
#define ayLogWrite(...)
#endif
} // namespace
#include "pico/picoPal.h"

using namespace std;


// Формат представления уровня выхода каналов: Q16, 65536 == 1.0
static constexpr int PSG_AMP_BITS = 16;

namespace {

#pragma pack(push, 1)
struct Psg3910CounterSnapshotStateV1 {
    uint32_t freq;
    uint32_t amp;
    uint32_t counter;
    int32_t outValue;
    uint8_t var;
    uint8_t toneGate;
    uint8_t noiseGate;
    uint8_t toneValue;
};

struct Psg3910SnapshotStateV1 {
    uint64_t prevClock;
    uint64_t discreteClock;
    int64_t accum[3];
    Psg3910CounterSnapshotStateV1 counters[3];
    uint32_t noiseFreq;
    uint32_t envFreq;
    uint32_t envCounter;
    uint32_t envCounter2;
    int32_t noise;
    uint32_t noiseCounter;
    uint32_t envValue;
    uint32_t curReg;
    uint8_t regs[16];
    uint8_t att;
    uint8_t alt;
    uint8_t hold;
    uint8_t noiseValue;
    uint8_t enabled;
};

struct Psg3910SoundSourceSnapshotStateV1 {
    uint8_t stereo;
    uint8_t acbOrder;
};
#pragma pack(pop)

}


Psg3910::Psg3910()
{
    m_prevClock = g_emulation->getCurClock();
    m_discreteClock = m_prevClock - m_prevClock % (m_kDiv * 8);
    Psg3910::reset();
}


void Psg3910::reset()
{
    // The emulation clock is not reset together with the machine devices.
    // Drop all timing history here; otherwise getOutputs() keeps integrating
    // from the pre-reset interval and may return silence after a disk/ROM reset.
    m_prevClock = g_emulation->getCurClock();
    const uint64_t stepTicks = uint64_t(m_kDiv) * 8;
    m_discreteClock = m_prevClock - m_prevClock % stepTicks;

    m_curReg = 0;
    for (int i = 0; i < 16; i++)
        m_regs[i] = 0;
    if (palAudioIsHwAy())
        hway_reset();

    m_noiseFreq = 0;
    m_envFreq = 0;
    m_envCounter = 0;
    m_envCounter2 = 0;
    m_envValue = 0;

    m_noiseCounter = 0;
    m_noise = 1;

    m_noiseValue = false;
    m_att = false;
    m_alt = false;
    m_hold = false;

    for (int i = 0; i < 3; i++) {
        m_counters[i].freq = 0;
        m_counters[i].amp = 0;
        m_counters[i].var = false;
        m_counters[i].toneGate = true;
        m_counters[i].noiseGate = true;
        m_counters[i].counter = 0;
        m_counters[i].toneValue = false;
        m_counters[i].outValue = 0;
        m_accum[i] = 0;
    }

}

void Psg3910::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;

    if (!enabled)
        updateState();

    m_enabled = enabled;
    // Выключенный PSG больше не пишет в чип, поэтому гасим его амплитуды
    // явно: иначе реальный AY продолжит тянуть последнюю ноту.
    if (!enabled && palAudioIsHwAy())
        for (int i = 8; i <= 10; i++) {
            hway_ay_address(uint8_t(i));
            hway_ay_data(0);
        }

    if (enabled) {
        m_prevClock = g_emulation->getCurClock();
        const uint64_t stepTicks = uint64_t(m_kDiv) * 8;
        m_discreteClock = m_prevClock - m_prevClock % stepTicks;
        for (int i = 0; i < 3; i++)
            m_accum[i] = 0;
    }
}


void Psg3910::postLoad()
{
    if (!palAudioIsHwAy())
        return;
    // Snapshot восстановил регистры только в эмуляторе. Реальный чип нужно
    // перезалить целиком, иначе он продолжит играть доснимочное состояние.
    hway_reset();
    for (int i = 0; i < 16; i++) {
        hway_ay_address(uint8_t(i));
        hway_ay_data(m_regs[i]);
    }
    hway_ay_address(uint8_t(m_curReg));
}

void Psg3910::writeByte(int addr, uint8_t value)
{
    if (!m_enabled)
        return;

    updateState();

    if (addr & 1) {
        // reg number
        m_curReg = value & 0xF;
        // Адрес отдельно в железо не идёт: он уйдёт вместе с данными,
        // когда очередь дойдёт до своего момента реального времени.
    } else {
        // register
        // Пропуск повторных значений убран: он расходился с состоянием
        // чипа. После сброса m_regs обнулены, а регистры микросхемы хранят
        // прежнее содержимое, и совпадение с m_regs означало пропуск нужной
        // записи, после чего расхождение уже не исправлялось.
        const bool toHw = palAudioIsHwAy();
        if (toHw) {
            // Метка — эмулированное время в микросекундах: по нему очередь
            // восстановит исходный темп при выдаче в микросхему.
            const uint32_t emuUs = uint32_t(
                (g_emulation->getCurClock() * 1000000ull) / g_emulation->getFrequency());
            hway_ay_queue(uint8_t(m_curReg), value, emuUs);
        }
        ayLogWrite(uint8_t(m_curReg), value, toHw);
        m_regs[m_curReg] = value;
        switch (m_curReg) {
        case 0:
            m_counters[0].freq = (m_counters[0].freq & 0xF00) | value;
            break;
        case 1:
            m_counters[0].freq = (m_counters[0].freq & 0xFF) | ((value & 0x0F) << 8);
            break;
        case 2:
            m_counters[1].freq = (m_counters[1].freq & 0xF00) | value;
            break;
        case 3:
            m_counters[1].freq = (m_counters[1].freq & 0xFF) | ((value & 0x0F) << 8);
            break;
        case 4:
            m_counters[2].freq = (m_counters[2].freq & 0xF00) | value;
            break;
        case 5:
            m_counters[2].freq = (m_counters[2].freq & 0xFF) | ((value & 0x0F) << 8);
            break;
        case 6:
            m_noiseFreq = (value & 0x1F) << 1; // double freq due to step() called with f/8, not f/16
            break;
        case 7:
            m_counters[0].toneGate = value & 0x01;
            m_counters[1].toneGate = value & 0x02;
            m_counters[2].toneGate = value & 0x04;
            m_counters[0].noiseGate = value & 0x08;
            m_counters[1].noiseGate = value & 0x10;
            m_counters[2].noiseGate = value & 0x20;
            break;
        case 8:
            m_counters[0].amp = value & 0x0F;
            m_counters[0].var = value & 0x10;
            break;
        case 9:
            m_counters[1].amp = value & 0x0F;
            m_counters[1].var = value & 0x10;
            break;
        case 0xA:
            m_counters[2].amp = value & 0x0F;
            m_counters[2].var = value & 0x10;
            break;
        case 0xB:
            m_envFreq = (m_envFreq & 0x1FE00) | (value << 1);  // double freq due to step() called with f/8, not f/16
            break;
        case 0xC:
            m_envFreq = (m_envFreq & 0x1FE) | (value << 9);    // double freq due to step() called with f/8, not f/16
            break;
        case 0xD:
            m_hold = value & 1;
            m_alt = value & 2;
            m_att = value & 4;
            if (!(value & 8)) {
                m_hold = true;
                m_alt = m_att;
            }
            m_envCounter2 = 0;
            break;
        default:
            break;
        }
    }
}


uint8_t Psg3910::readByte(int addr)
{
    if (!m_enabled)
        return 0xFF;

    if (addr & 1)
        // reg number
        return m_curReg;
    else
        // register
        return m_regs[m_curReg];
}


void __not_in_flash_func(Psg3910::step)()
{
    // Logarithmic DAC table from Emuscriptoria, в формате Q16: round(x * 65536).
    // Исходные значения:
    // {0.0, 0.0137, 0.0205, 0.0291, 0.0424, 0.0618, 0.0847, 0.1369,
    //  0.1691, 0.2647, 0.3527, 0.4499, 0.5704, 0.6837, 0.8482, 1.0}
    static const int32_t logAmps[16] =
    {0, 898, 1343, 1907, 2779, 4050, 5551, 8972, 11082, 17347, 23115, 29485, 37382, 44807, 55588, 65536};

    if (++m_noiseCounter >= m_noiseFreq) {
        m_noiseCounter = 0;
        m_noise = (m_noise >> 1) | (((m_noise & 1) ^ ((m_noise & 4) >> 2)) << 16);
        m_noiseValue = m_noise & 2;
    }

    if (++m_envCounter >= m_envFreq) {
        m_envCounter = 0;
        envStep();
    }

    for (unsigned i = 0; i < 3; i++) {
        if (++m_counters[i].counter >= m_counters[i].freq) {
            m_counters[i].counter = 0;
            m_counters[i].toneValue = !m_counters[i].toneValue;
        }

        bool tone = m_counters[i].toneValue;
        bool val = (m_counters[i].toneGate || tone) && (m_counters[i].noiseGate || m_noiseValue);

        m_counters[i].outValue = val ? logAmps[m_counters[i].var ? m_envValue : m_counters[i].amp] : 0;
    }
}


void Psg3910::envStep()
{
    if (m_envCounter2 >= 16) {
        if (m_hold) {
            m_envValue = (m_alt ? m_att : !m_att) ? 0 : 15;
            return;
        }
        m_envCounter2 = 0;
        if (m_alt)
            m_att = !m_att;
    }
    m_envValue = m_att ? m_envCounter2++ : 15 - m_envCounter2++;
}


void __not_in_flash_func(Psg3910::updateState)()
{
    const uint64_t curClock = g_emulation->getCurClock();
    const int stepTicks = m_kDiv * 8;

    while (m_discreteClock < curClock) {
        step();
        m_discreteClock += stepTicks;
        for (int i = 0; i < 3; i++)
            m_accum[i] += int64_t(m_counters[i].outValue) * stepTicks;
    }
}


void __not_in_flash_func(Psg3910::getOutputs)(uint16_t* outputs)
{
    updateState();

    const uint64_t curClock = g_emulation->getCurClock();
    const int64_t dt = int64_t(curClock - m_prevClock);

    // После updateState() всегда m_discreteClock >= curClock, и разность лежит
    // в пределах одного шага. Она одинакова для всех трёх каналов.
    const int64_t tail = int64_t(m_discreteClock - curClock);

    for (int i = 0; i < 3; i++) {
        const int64_t delta = int64_t(m_counters[i].outValue) * tail;
        const int64_t num = m_accum[i] - delta;

        // num измеряется в единицах «Q16 x такт»: делённое на dt оно даёт долю
        // 0..65536. Умножение на MAX_SND_AMP и снятие Q16 выполняются ДО
        // деления — тогда числитель помещается в 32 бита, и вместо деления
        // double остаётся обычное 32-битное целочисленное.
        int32_t out = 0;
        if (dt > 0) {
            const int64_t scaled = (num * MAX_SND_AMP) >> PSG_AMP_BITS;
            if (dt <= 0x7FFFFFFF && scaled >= -0x7FFFFFFF - 1 && scaled <= 0x7FFFFFFF)
                out = int32_t(scaled) / int32_t(dt);
            else
                out = int32_t(scaled / dt);
        }

        if (out < 0)
            out = 0;
        else if (out > MAX_SND_AMP)
            out = MAX_SND_AMP;
        outputs[i] = uint16_t(out);

        m_accum[i] = delta;
    }

    m_prevClock = curClock;
}


int Psg3910SoundSource::calcValue()
{
    // not used since getSample is implemented
    return 0; //m_psg ? m_psg->getOutput() * m_ampFactor : 0;
}


void __not_in_flash_func(Psg3910SoundSource::getSample)(int& left, int& right)
{
    if (!m_psg) {
        left = right = 0;
        return;
    }

    uint16_t outputs[3];

    // getOutputs() внутри вызывает updateState(), поэтому его нельзя
    // пропускать даже когда звук не нужен: иначе m_prevClock застынет, и
    // первый же updateState() (например, из setEnabled) будет догонять
    // накопившийся интервал секундами.
    m_psg->getOutputs(outputs);

    // При HWAY музыку играет реальная микросхема: программный источник
    // отдаёт тишину, иначе он продублируется в ЦАП port B второго чипа.
    if (palAudioIsHwAy()) {
        left = right = 0;
        return;
    }

    // max amp = 3
    if (m_stereo) {
        // Stereo ABC/ACB: A is left, the selected middle channel is centered,
        // and the remaining channel is right.
        const int middle = m_acbOrder ? outputs[2] : outputs[1];
        const int rightChannel = m_acbOrder ? outputs[1] : outputs[2];
        left =  m_ampFactor * (outputs[0] * 3 / 2 + middle + rightChannel / 2);
        right = m_ampFactor * (outputs[0] / 2 + middle + rightChannel * 3 / 2);
    } else {
        // Mono
        // L = 1/3*A + 1/3*B + 1/3*C
        // R = 1/3*A + 1/3*B + 1/3*C
        left = right = m_ampFactor * (outputs[0] + outputs[1] + outputs[2]);
    }
}


uint32_t Psg3910::snapshotSectionId() const
{
    return makeSnapshotSectionId('A', 'Y', ' ', ' ');
}

uint16_t Psg3910::snapshotSectionVersion() const
{
    return 1;
}

bool Psg3910::saveState(SnapshotWriter& writer) const
{
    Psg3910SnapshotStateV1 state{};
    state.prevClock = m_prevClock;
    state.discreteClock = m_discreteClock;
    for (int i = 0; i < 3; i++) {
        state.accum[i] = m_accum[i];
        const Psg3910Counter& src = m_counters[i];
        Psg3910CounterSnapshotStateV1& dst = state.counters[i];
        dst.freq = src.freq;
        dst.amp = src.amp;
        dst.counter = src.counter;
        dst.outValue = src.outValue;
        dst.var = src.var ? 1 : 0;
        dst.toneGate = src.toneGate ? 1 : 0;
        dst.noiseGate = src.noiseGate ? 1 : 0;
        dst.toneValue = src.toneValue ? 1 : 0;
    }
    state.noiseFreq = m_noiseFreq;
    state.envFreq = m_envFreq;
    state.envCounter = m_envCounter;
    state.envCounter2 = m_envCounter2;
    state.noise = m_noise;
    state.noiseCounter = m_noiseCounter;
    state.envValue = m_envValue;
    state.curReg = m_curReg;
    memcpy(state.regs, m_regs, sizeof(state.regs));
    state.att = m_att ? 1 : 0;
    state.alt = m_alt ? 1 : 0;
    state.hold = m_hold ? 1 : 0;
    state.noiseValue = m_noiseValue ? 1 : 0;
    state.enabled = m_enabled ? 1 : 0;
    return writer.writeValue(state);
}

bool Psg3910::loadState(SnapshotReader& reader, uint16_t version)
{
    if (version != snapshotSectionVersion() ||
        reader.remaining() != sizeof(Psg3910SnapshotStateV1))
        return false;

    Psg3910SnapshotStateV1 state{};
    if (!reader.readValue(state) || state.curReg > 15 ||
        state.envValue > 15 || state.att > 1 || state.alt > 1 ||
        state.hold > 1 || state.noiseValue > 1 || state.enabled > 1)
        return false;

    for (int i = 0; i < 3; i++) {
        const Psg3910CounterSnapshotStateV1& src = state.counters[i];
        if (src.freq > 0x0FFF || src.amp > 15 || src.var > 1 ||
            src.toneGate > 1 || src.noiseGate > 1 || src.toneValue > 1)
            return false;
    }

    m_prevClock = state.prevClock;
    m_discreteClock = state.discreteClock;
    for (int i = 0; i < 3; i++) {
        m_accum[i] = state.accum[i];
        Psg3910Counter& dst = m_counters[i];
        const Psg3910CounterSnapshotStateV1& src = state.counters[i];
        dst.freq = src.freq;
        dst.amp = src.amp;
        dst.counter = src.counter;
        dst.outValue = src.outValue;
        dst.var = src.var != 0;
        dst.toneGate = src.toneGate != 0;
        dst.noiseGate = src.noiseGate != 0;
        dst.toneValue = src.toneValue != 0;
    }
    m_noiseFreq = state.noiseFreq;
    m_envFreq = state.envFreq;
    m_envCounter = state.envCounter;
    m_envCounter2 = state.envCounter2;
    m_noise = state.noise;
    m_noiseCounter = state.noiseCounter;
    m_envValue = state.envValue;
    m_curReg = state.curReg;
    memcpy(m_regs, state.regs, sizeof(m_regs));
    m_att = state.att != 0;
    m_alt = state.alt != 0;
    m_hold = state.hold != 0;
    m_noiseValue = state.noiseValue != 0;
    m_enabled = state.enabled != 0;
    return true;
}

uint32_t Psg3910SoundSource::snapshotSectionId() const
{
    return makeSnapshotSectionId('A', 'Y', 'M', 'X');
}

uint16_t Psg3910SoundSource::snapshotSectionVersion() const
{
    return 1;
}

bool Psg3910SoundSource::saveState(SnapshotWriter& writer) const
{
    const Psg3910SoundSourceSnapshotStateV1 state = {
        static_cast<uint8_t>(m_stereo ? 1 : 0),
        static_cast<uint8_t>(m_acbOrder ? 1 : 0)
    };
    return writer.writeValue(state);
}

bool Psg3910SoundSource::loadState(SnapshotReader& reader, uint16_t version)
{
    if (version != snapshotSectionVersion() ||
        reader.remaining() != sizeof(Psg3910SoundSourceSnapshotStateV1))
        return false;
    Psg3910SoundSourceSnapshotStateV1 state{};
    if (!reader.readValue(state) || state.stereo > 1 || state.acbOrder > 1)
        return false;
    m_stereo = state.stereo != 0;
    m_acbOrder = state.acbOrder != 0;
    return true;
}
