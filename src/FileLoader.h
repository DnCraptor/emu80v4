﻿/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2017-2024
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

#ifndef FILELOADER_H
#define FILELOADER_H

#include "EmuObjects.h"

class TapeRedirector;


class FileLoader : public EmuObject
{
    public:
        virtual FileLoader* asFileLoader() override { return this; }
        //FileLoader();
        //~FileLoader();

        bool setProperty(const std::string& propertyName, const EmuValuesList& values) override;
        std::string getPropertyStringValue(const std::string& propertyName) override;

        virtual bool loadFile(const std::string& fileName, bool run = false) = 0;

        bool chooseAndLoadFile(bool run = false);
        void setFilter(const std::string& filter);
        void attachAddrSpace(AddressableDevice* as);
        void attachTapeRedirector(TapeRedirector* tapeRedirector);

    protected:
        AddressableDevice* m_as = nullptr;
        TapeRedirector* m_tapeRedirector = nullptr;
        std::string m_filter;
        int m_skipTicks = 2000000;
        bool m_multiblockAvailable = false;
        bool m_allowMultiblock = false;

    private:
        std::string m_lastFile;
};


class RkFileLoader : public FileLoader
{
    public:
        RkFileLoader() {m_multiblockAvailable = true;}
        bool loadFile(const std::string& fileName, bool run = false) override;

        static EmuObject* create(const EmuValuesList&) {return new RkFileLoader();}

    private:
        virtual void afterReset() {}

};


class MsxFileParser
{
public:
    enum class Format {
        MF_UNKNOWN,
        MF_CAS,
        MF_TSX
    };

    MsxFileParser(uint8_t* data, int len);

    Format getFormat() {return m_format;}
    int getCurrentPos() {return m_size;}
    bool getNextBlock(int& pos, int& len);

private:
    uint8_t* m_data;
    int m_size = 0;
    int m_curPos = 0;
    Format m_format = Format::MF_UNKNOWN;
    int m_nextTsxBlockPos = 0;
};


#endif // FILELOADER_H
