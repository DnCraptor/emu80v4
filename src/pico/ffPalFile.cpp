#include "ffPalFile.h"

using namespace std;

bool PalFile::mkDir(const string& dirName)
{
    f_mkdir(dirName.c_str());
    return true;
}


bool PalFile::moveRename(const string& src, const string& dst)
{
    return f_rename(src.c_str(), dst.c_str()) == FR_OK;
}

bool PalFile::del(const std::string& fileName)
{
    return f_unlink(fileName.c_str()) == FR_OK;
}

bool PalFile::create(const std::string& fileName) {
    FIL f;
    bool res = FR_OK == f_open(&f, fileName.c_str(), FA_CREATE_NEW | FA_CREATE_ALWAYS | FA_WRITE);
    f_close(&f);
    return res;
}
