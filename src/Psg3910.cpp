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

#include "Globals.h"
#include "Emulation.h"
#include "Psg3910.h"

using namespace std;


// Формат представления уровня выхода каналов: Q16, 65536 == 1.0
static constexpr int PSG_AMP_BITS = 16;


Psg3910::Psg3910()
{
    m_prevClock = g_emulation->getCurClock();
    m_discreteClock = m_prevClock - m_prevClock % (m_kDiv * 8);
    Psg3910::reset();
}


void Psg3910::reset()
{
    m_curReg = 0;
    for (int i = 0; i < 16; i++)
        m_regs[i] = 0;

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


void Psg3910::writeByte(int addr, uint8_t value)
{
    updateState();

    if (addr & 1) {
        // reg number
        m_curReg = value & 0xF;
    } else {
        // register
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

    m_psg->getOutputs(outputs);

    // max amp = 3
    if (m_stereo) {
        // Stereo ABC
        // L = 1/2*A + 1/3*B + 1/6*C
        // R = 1/6*A + 1/3*B + 1/2*C
        left =  m_ampFactor * (outputs[0] * 3 / 2 + outputs[1] + outputs[2] / 2);
        right = m_ampFactor * (outputs[0] / 2 + outputs[1] + outputs[2] * 3 / 2);
    } else {
        // Mono
        // L = 1/3*A + 1/3*B + 1/3*C
        // R = 1/3*A + 1/3*B + 1/3*C
        left = right = m_ampFactor * (outputs[0] + outputs[1] + outputs[2]);
    }
}
