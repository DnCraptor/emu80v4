/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2018-2022
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
#include "Pal.h"
#include "PalFile.h"
#include "Memory.h"
#include "EmuWindow.h"
#include "Emulation.h"
#include "Vector.h"
#include "RamDisk.h"

using namespace std;


RamDisk::RamDisk(unsigned nPages, unsigned defPageSize)
{
    m_nPages = nPages;
    m_defPageSize = defPageSize;
    m_pages = new AddressableDevice* [nPages];
    for (unsigned i = 0; i < nPages; i++)
        m_pages[i] = nullptr;
}


RamDisk::~RamDisk()
{
    delete[] m_pages;
}


void RamDisk::attachPage(unsigned pageNo, AddressableDevice* as)
{
    m_pages[pageNo] = as;
}




void RamDisk::saveFileAs()
{
    string oldFileName = m_fileName;
    m_fileName = "";
    saveToFile();
    if (m_fileName.empty())
        m_fileName = oldFileName;
}


void RamDisk::saveToFile()
{
    if (m_fileName.empty()) {
        m_fileName = palOpenFileDialog("Save RAM disk file", m_filter, true, m_machine->getWindow());
        g_emulation->restoreFocus();
        if (m_fileName == "")
            return;
    }

    PalFile file;
    file.open(m_fileName, "w");
    if (!file.isOpen())
        return;

    for (unsigned i = 0; i < m_nPages; i++) {
        unsigned pageSize = m_defPageSize;

        if (m_pages[i]) {
            SRam* sram = m_pages[i]->asSRam();
            if (sram)
               pageSize = sram->getSize();
            else {
                Ram* ram = m_pages[i]->asRam();
                if (ram)
                    pageSize = ram->getSize();
            }
        }

        for (unsigned pos = 0; pos < pageSize; pos++)
            file.write8(m_pages[i]->readByte(pos));
        if (pageSize < m_defPageSize)
            for (unsigned pos = 0; pos < m_defPageSize - pageSize; pos++)
                file.write8(0);
    }

    file.close();
}


void RamDisk::openFile()
{
    string oldFileName = m_fileName;
    m_fileName = "";
    loadFromFile();
    if (m_fileName.empty())
        m_fileName = oldFileName;
}


void RamDisk::loadFromFile()
{
    if (m_fileName.empty()) {
        m_fileName = palOpenFileDialog("Load RAM disk file", m_filter, false, m_machine->getWindow());
        g_emulation->restoreFocus();
        if (m_fileName == "")
            return;
    }

    PalFile file;
    file.open(m_fileName, "r");
    if (!file.isOpen())
        return;

    unsigned expectedSize = 0;
    for (unsigned i = 0; i < m_nPages; i++) {
        unsigned pageSize = m_defPageSize;

        SRam* sram = m_pages[i]->asSRam();
        if (sram)
            pageSize = sram->getSize();
        else {
            Ram* ram = m_pages[i]->asRam();
            if (ram)
                pageSize = ram->getSize();
        }
        if (pageSize < m_defPageSize)
            pageSize = m_defPageSize;

        expectedSize += m_defPageSize;
    }

    if (file.getSize() == expectedSize) {

        for (unsigned i = 0; i < m_nPages; i++) {
            unsigned pageSize = m_defPageSize;

            if (m_pages[i]) {
                SRam* sram = m_pages[i]->asSRam();
                if (sram)
                   pageSize = sram->getSize();
                else {
                    Ram* ram = m_pages[i]->asRam();
                    if (ram)
                        pageSize = ram->getSize();
                }
            }

            for (unsigned pos = 0; pos < pageSize; pos++)
                m_pages[i]->writeByte(pos, file.read8());

            if (pageSize < m_defPageSize)
                for (unsigned pos = 0; pos < m_defPageSize - pageSize; pos++)
                    file.read8();
        }
    } else {
        emuLog << "Invalid file size: " << m_fileName << "\n";
    }

    file.close();
}


void RamDisk::init()
{
    if (m_autoLoad && !m_fileName.empty())
        loadFromFile();
}


void RamDisk::shutdown()
{
    if (m_autoSave && !m_fileName.empty())
        saveToFile();
}
