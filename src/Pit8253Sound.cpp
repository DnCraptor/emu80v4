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

#include "Pit8253.h"
#include "Pit8253Sound.h"

#include "Emulation.h"
#include "Globals.h"

using namespace std;

void Pit8253SoundSource::attachPit(Pit8253* pit)
{
    m_pit = pit;
    tuneupPit();
}


int __not_in_flash_func(Pit8253SoundSource::calcValue)()
{
    int res = 0;

    if (m_pit) {
        m_pit->updateState();
        for (int i = 0; i < 3; i++) {
            res += MAX_SND_AMP - (m_pit->getCounter(i)->getAvgOut());
            m_pit->getCounter(i)->resetStats();
        }
    }

    return res * m_ampFactor;
}
