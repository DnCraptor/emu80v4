/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2016-2023
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

#include "Globals.h"

#include "EmuObjects.h"
#include "Emulation.h"

using namespace std;

void EmuObject::setFrequency(int64_t freq)
{
    m_kDiv = (g_emulation->getFrequency() + freq / 2) / freq;
}




IActive* IActive::s_firstActive = nullptr;
IActive* IActive::s_lastActive = nullptr;


IActive::IActive()
{
    if (s_lastActive)
        s_lastActive->m_nextActive = this;
    else
        s_firstActive = this;
    s_lastActive = this;

    // Если стартовая регистрация уже прошла, устройство подхватывается на ходу.
    // Проверка указателя безопасна и при статической инициализации.
    if (g_emulation && g_emulation->activeDevicesRegistered()) {
        m_curClock = g_emulation->getCurClock();
        g_emulation->registerActiveDevice(this);
    }
}


IActive::~IActive()
{
    IActive** link = &s_firstActive;
    IActive* prev = nullptr;
    while (*link && *link != this) {
        prev = *link;
        link = &(*link)->m_nextActive;
    }
    if (*link) {
        *link = m_nextActive;
        if (s_lastActive == this)
            s_lastActive = prev;
    }

    if (g_emulation)
        g_emulation->unregisterActiveDevice(this);
}


void IActive::syncronize()
{
    m_curClock = g_emulation->getCurClock();
}
