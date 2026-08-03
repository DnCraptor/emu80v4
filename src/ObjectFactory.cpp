/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2019-2024
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

#include "ObjectFactory.h"

#include "EmuObjects.h"
#include "EmuWindow.h"
#include "AddrSpace.h"
#include "Memory.h"
#include "Cpu8080.h"
#include "Ppi8255.h"
#include "SoundMixer.h"
#include "KbdLayout.h"
#include "FileLoader.h"
#include "TapeRedirector.h"
#include "MsxTapeHooks.h"
#include "CloseFileHook.h"
#include "Lvov.h"
#include "EmuConfig.h"
using namespace std;

#define REG_EMU_CLASS(name) reg(#name, &name::create)

ObjectFactory::ObjectFactory()
{
    REG_EMU_CLASS(EmuWindow);
    REG_EMU_CLASS(EmuObjectGroup);
    REG_EMU_CLASS(AddrSpace);
    REG_EMU_CLASS(AddrSpaceMapper);
    REG_EMU_CLASS(Ram);
    REG_EMU_CLASS(Rom);
    REG_EMU_CLASS(Cpu8080);
    REG_EMU_CLASS(Ppi8255);
    REG_EMU_CLASS(GeneralSoundSource);
    REG_EMU_CLASS(TapeRedirector);
    REG_EMU_CLASS(MsxTapeOutHook);
    REG_EMU_CLASS(MsxTapeOutHeaderHook);
    REG_EMU_CLASS(MsxTapeInHook);
    REG_EMU_CLASS(MsxTapeInHeaderHook);
    REG_EMU_CLASS(CloseFileHook);
    REG_EMU_CLASS(LvovCore);
    REG_EMU_CLASS(LvovRenderer);
    REG_EMU_CLASS(LvovPpi8255Circuit1);
    REG_EMU_CLASS(LvovPpi8255Circuit2);
    REG_EMU_CLASS(LvovKeyboard);
    REG_EMU_CLASS(LvovKbdLayout);
    REG_EMU_CLASS(LvovCpuWaits);
    REG_EMU_CLASS(LvovCpuCycleWaits);
    REG_EMU_CLASS(LvovFileLoader);

    reg("ConfigTab", &EmuConfigTab::create);
    reg("ConfigRadioSelector", &EmuConfigRadioSelector::create);

}


void ObjectFactory::reg(const string& objectClassName, CreateObjectFunc pfnCreate)
{
    m_objectMap[objectClassName] = pfnCreate;
}


EmuObject* ObjectFactory::createObject(const string& objectClassName, const EmuValuesList& parameters)
{
    auto it = m_objectMap.find(objectClassName);
    if (it != m_objectMap.end())
        return it->second(parameters);
    return nullptr;
}
