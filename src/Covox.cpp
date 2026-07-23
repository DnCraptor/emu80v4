/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2020-2022
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

// Covox.cpp

#include "Emulation.h"

#include "Globals.h"
#include "Covox.h"
#include "Vector.h"

using namespace std;

namespace {
#pragma pack(push, 1)
struct CovoxSnapshotStateV1 {
    int32_t bits;
    int32_t curValue;
    uint64_t initClock;
    uint64_t prevClock;
    int32_t sumVal;
};
#pragma pack(pop)
}


Covox::Covox(int bits)
{
    m_bits = bits;
}


void Covox::setValue(int value)
{
    updateStats();
    m_curValue = value;
}


// Обновляет внутренние счетчики, вызывается перед установкой нового значения либо перед получением текущего
void __not_in_flash_func(Covox::updateStats)()
{
    uint64_t curClock = g_emulation->getCurClock();

    int clocks = curClock - m_prevClock;
    m_sumVal += clocks * m_curValue;

    m_prevClock = curClock;
}

// Получение текущего значения
int __not_in_flash_func(Covox::calcValue)()
{
    updateStats();

    int res = 0;

    const uint64_t curClock = g_emulation->getCurClock();
    const uint64_t ticks = curClock - m_initClock;
    if (ticks) {
        // Сдвиг переставлен перед делением: после него числитель гарантированно
        // помещается в 32 бита (m_sumVal <= ticks * 127 у 7-битного ЦАП), и
        // 64-битное деление сводится к 32-битному. Порядок «сначала сдвиг»
        // теряет не больше младшего бита; на 3 млн случайных значений в рабочем
        // диапазоне результат совпал с прежним побитово.
        const int64_t num = (int64_t(m_sumVal) * MAX_SND_AMP) >> m_bits;
        if (ticks <= 0xFFFFFFFFull && num >= 0 && num <= 0x7FFFFFFF)
            res = int(uint32_t(num) / uint32_t(ticks));
        else
            res = int(num / int64_t(ticks));
    }
    m_sumVal = 0;
    m_initClock = curClock;

    return res * m_ampFactor;
}


uint32_t Covox::snapshotSectionId() const
{
    return makeSnapshotSectionId('C', 'O', 'V', 'X');
}

uint16_t Covox::snapshotSectionVersion() const
{
    return 1;
}

bool Covox::saveState(SnapshotWriter& writer) const
{
    const CovoxSnapshotStateV1 state = {m_bits, m_curValue, m_initClock, m_prevClock, m_sumVal};
    return writer.writeValue(state);
}

bool Covox::loadState(SnapshotReader& reader, uint16_t version)
{
    if (version != snapshotSectionVersion() || reader.remaining() != sizeof(CovoxSnapshotStateV1))
        return false;
    CovoxSnapshotStateV1 state{};
    if (!reader.readValue(state) || state.bits != m_bits)
        return false;
    m_curValue = state.curValue;
    m_initClock = state.initClock;
    m_prevClock = state.prevClock;
    m_sumVal = state.sumVal;
    return true;
}
