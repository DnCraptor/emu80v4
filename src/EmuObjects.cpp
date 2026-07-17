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




IActive::IActive()
{
    m_curClock = g_emulation->getCurClock();
    g_emulation->registerActiveDevice(this);
}


IActive::~IActive()
{
    g_emulation->unregisterActiveDevice(this);
}


void IActive::syncronize()
{
    m_curClock = g_emulation->getCurClock();
}


//bool IActive::isPaused()
//{
//    return (m_curClock == -1);
//}
