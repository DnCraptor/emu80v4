/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2018
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

#include <sstream>
#include <iostream>

#include <time.h>

#include "picoPal.h"
#include "ff.h"

using namespace std;

std::string palOpenFileDialog(std::string, std::string, bool, PalWindow*) {
    return "";
}

void palGetDirContent(const string& dir, list<PalFileInfo*>& fileList) {
	DIR d;
	FILINFO fileInfo;
    string utf8Dir = dir;

    if (utf8Dir[utf8Dir.size() - 1] != '/' && utf8Dir[utf8Dir.size() - 1] != '\\')
        utf8Dir += "\\";

	if (f_opendir(&d, utf8Dir.c_str()) == FR_OK) {
    	while (f_readdir(&d, &fileInfo) == FR_OK && fileInfo.fname[0] != '\0') {
            PalFileInfo* newFile = new PalFileInfo;
            newFile->fileName = fileInfo.fname;
            if (newFile->fileName == "." || newFile->fileName == "..")
                continue;
            string fullPath = utf8Dir + newFile->fileName;
            newFile->isDir = fileInfo.fattrib | AM_DIR;
            newFile->size = (uint32_t)fileInfo.fsize;
            struct tm *fileDateTime;
        //    fileDateTime = gmtime(&(fileInfo.ftime));
/// TODO:
            newFile->year = 2030; // fileDateTime->tm_year + 1900;
            newFile->month = 1; // fileDateTime->tm_mon + 1;
            newFile->day = 1; // fileDateTime->tm_mday;
            newFile->hour = 1; // fileDateTime->tm_hour;
            newFile->minute = 1; // fileDateTime->tm_min;
            newFile->second = 1; ///fileDateTime->tm_sec;
            fileList.push_back(newFile);
        }
    }
}

bool palChoosePlatform(std::vector<PlatformInfo>&, int&, bool&, bool, PalWindow*) {
    return false;
}


bool palChooseConfiguration(std::string platformName, PalWindow* wnd) {
    return false;
}


void palGetPalDefines(std::list<std::string>& defineList)
{
    defineList.push_back("SDL");
}


void palGetPlatformDefines(std::string platformName, std::map<std::string, std::string>& definesMap)
{

}


void palSetRunFileName(std::string) {
}

void palShowConfigWindow(int) {
}

void palUpdateConfig() {
}

void palAddTabToConfigWindow(int, std::string) {
}

void palRemoveTabFromConfigWindow(int) {
}

void palAddRadioSelectorToTab(int, int, std::string, std::string, std::string, SelectItem*, int) {
}

void palSetTabOptFileName(int, string) {
}

void palWxProcessMessages() {
}

void palLog(std::string s) {
    cout << s << endl;
}

EmuLog& EmuLog::operator<<(string s)
{
    palLog(s);
    return *this;
}


EmuLog& EmuLog::operator<<(const char* sz)
{
    string s = sz;
    palLog(s);
    return *this;
}


EmuLog& EmuLog::operator<<(int n)
{
    ostringstream oss;
    oss << n;
    string s = oss.str();
    palLog(s);
    return *this;
}


void palMsgBox(string msg, bool)
{
    cout << msg << endl;
}


EmuLog emuLog;
