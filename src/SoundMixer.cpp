/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2016-2022
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

// SoundMixer.cpp
// Реализация класса звукового микшера и базовых классов источника звука

#include "Globals.h"
#include "Emulation.h"

#include "Pal.h"
#include "SoundMixer.h"

using namespace std;

// Вызывается 48000 (SAMPLE_RATE) раз в секунду для получения текущего сэмпла и его проигрывания
void __not_in_flash_func(SoundMixer::operate)()
{
    int leftSample = 0;
    int rightSample = 0;
    for (int i = 0; i < m_soundSourceCount; i++) {
        int left, right;
        m_soundSources[i]->getSample(left, right);
        leftSample += m_volume < 7 ? left : abs(left);
        rightSample += m_volume < 7 ? right : abs(right);
    }

    leftSample = m_muted ? m_silenceLevel : (leftSample >> m_sampleShift) + m_silenceLevel;
    rightSample = m_muted ? m_silenceLevel : (rightSample >> m_sampleShift) + m_silenceLevel;

    palPlaySample(leftSample, rightSample);

    m_curClock += m_ticksPerSample;

    m_error += m_ticksPerSampleRemainder;
    int delta = m_error / m_sampleRate;
    m_error -= delta * m_sampleRate;
    m_curClock += delta;
}


void SoundMixer::addSoundSource(SoundSource* snd)
{
    if (m_soundSourceCount >= MAX_SOUND_SOURCES) {
        palMsgBox("Error: Too many sound sources.", true);
        palRequestForQuit();
        return;
    }

    m_soundSources[m_soundSourceCount++] = snd;
}


void SoundMixer::removeSoundSource(SoundSource* snd)
{
    int index = 0;
    while (index < m_soundSourceCount && m_soundSources[index] != snd)
        index++;

    if (index == m_soundSourceCount)
        return;

    for (int i = index + 1; i < m_soundSourceCount; i++)
        m_soundSources[i - 1] = m_soundSources[i];

    m_soundSources[--m_soundSourceCount] = nullptr;
}


void SoundMixer::setFrequency(int64_t freq)
{
    m_sampleRate = g_emulation->getSampleRate();
    m_ticksPerSample = freq / m_sampleRate;
    m_ticksPerSampleRemainder = freq % m_sampleRate;
}


void SoundMixer::toggleMute()
{
    m_muted = !m_muted;
}


void SoundMixer::setVolume(int volume)
{
    if (volume < 1 || volume > 7)
        return;

    m_volume = volume;
    m_sampleShift = 7 - volume;

    if (volume <= 6)
        m_silenceLevel = 0;
    else
        m_silenceLevel = -32768;
}


int SoundMixer::getVolume()
{
    return m_volume;
}


SoundSource* SoundSource::s_firstSource = nullptr;
SoundSource* SoundSource::s_lastSource = nullptr;


SoundSource::SoundSource()
{
    if (s_lastSource)
        s_lastSource->m_nextSource = this;
    else
        s_firstSource = this;
    s_lastSource = this;

    if (g_emulation && g_emulation->getSoundMixer()
                    && g_emulation->getSoundMixer()->sourcesCollected())
        g_emulation->getSoundMixer()->addSoundSource(this);
}


SoundSource::~SoundSource()
{
    SoundSource** link = &s_firstSource;
    SoundSource* prev = nullptr;
    while (*link && *link != this) {
        prev = *link;
        link = &(*link)->m_nextSource;
    }
    if (*link) {
        *link = m_nextSource;
        if (s_lastSource == this)
            s_lastSource = prev;
    }

    if (g_emulation && g_emulation->getSoundMixer())
        g_emulation->getSoundMixer()->removeSoundSource(this);
}


void SoundMixer::collectSoundSources()
{
    for (SoundSource* s = SoundSource::firstSource(); s; s = s->nextSource())
        addSoundSource(s);
    m_sourcesCollected = true;
}


void SoundSource::setNegative(bool negative)
{
    m_negative = negative;
    updateAmpFactor();
}


void SoundSource::setMuted(bool muted)
{
    m_muted = muted;
    updateAmpFactor();
}


void SoundSource::updateAmpFactor()
{
    m_ampFactor = m_muted ? 0 : m_negative ? -1 : 1;
}


void __not_in_flash_func(SoundSource::getSample)(int& left, int& right)
{
    int val = calcValue();
    left = right = val;
}




void GeneralSoundSource::setValue(int value)
{
    updateStats();
    m_curValue = value;
}


// Обновляет внутренние счетчики, вызывается перед установкой нового значения либо перед получением текущего
void GeneralSoundSource::updateStats()
{
    uint64_t curClock = g_emulation->getCurClock();
    if (m_curValue) {
        int clocks = curClock - prevClock;
        sumVal += clocks;
    }

    prevClock = curClock;
}

// Получение текущего значения
int __not_in_flash_func(GeneralSoundSource::calcValue)()
{
    updateStats();

    int res = 0;

    const uint64_t curClock = g_emulation->getCurClock();
    const uint64_t ticks = curClock - initClock;
    if (ticks) {
        // ticks — длительность одного сэмпла (~35000 тактов при 48 кГц), то есть
        // делитель по смыслу 32-битный. Он неявно расширялся до uint64_t, и
        // каждый сэмпл стоил вызова __aeabi_uldivmod вместо одной инструкции
        // деления. Умножение sumVal * MAX_SND_AMP как было 32-битным, так и
        // остаётся, поэтому результат совпадает с прежним побитово.
        if (ticks <= 0xFFFFFFFFull && sumVal >= 0 && sumVal <= 0x7FFFFFFF / MAX_SND_AMP)
            res = int(uint32_t(sumVal) * uint32_t(MAX_SND_AMP) / uint32_t(ticks));
        else
            res = int(int64_t(sumVal) * MAX_SND_AMP / int64_t(ticks));
    }
    sumVal = 0;
    initClock = curClock;

    return res * m_ampFactor;
}
