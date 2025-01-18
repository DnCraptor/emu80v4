#ifndef PICOFILE_H
#define PICOFILE_H

#include <string>

#include "ff.h"

class PalFile
{
    public:
        bool open(std::string fileName, std::string mode = "r") {
            m_file = new FIL();
            BYTE modeb = FA_READ;
            if (mode == "r+") modeb = FA_READ | FA_WRITE;
            else if (mode == "a") modeb = FA_WRITE | FA_OPEN_APPEND | FA_CREATE_NEW;
            else if (mode == "a+") modeb = FA_WRITE | FA_OPEN_APPEND | FA_CREATE_ALWAYS;
            else if (mode == "w") modeb = FA_WRITE | FA_CREATE_NEW;
            else if (mode == "w+") modeb = FA_WRITE | FA_CREATE_ALWAYS;
            if (FR_OK != f_open(m_file, fileName.c_str(), modeb)) {
                close();
                return false;
            }
            return true;
        }
        void close() {
            if (m_file) {
                f_close(m_file);
                delete m_file;
                m_file = 0;
            }
        }
        bool isOpen() { return m_file != 0; }
        bool eof() { return !m_file || f_eof(m_file); }
        uint8_t read8() {
            if (!m_file) return 0;
            uint8_t res;
            UINT br;
            f_read(m_file, &res, 1, &br);
            return res;
        }
        uint16_t read16() {
            if (!m_file) return 0;
            uint16_t res;
            UINT br;
            f_read(m_file, &res, 2, &br);
            return res;
        }
        uint32_t read32() {
            if (!m_file) return 0;
            uint32_t res;
            UINT br;
            f_read(m_file, &res, 4, &br);
            return res;
        }
        void write8(uint8_t value) {
            if (!m_file) return;
            UINT br;
            f_write(m_file, &value, 1, &br);
        }
        void write16(uint16_t value) {
            if (!m_file) return;
            UINT br;
            f_write(m_file, &value, 2, &br);
        }
        void write32(uint32_t value) {
            if (!m_file) return;
            UINT br;
            f_write(m_file, &value, 4, &br);
        }
        int64_t getSize() {
            if (!m_file) return 0;
            return f_size(m_file);
        }
        int64_t getPos() {
            if (!m_file) return 0;
            return f_tell(m_file);
        }
        void seek(int position) {
            if (m_file) f_lseek(m_file, position);
        }
        void skip(int len) {
            if (!m_file) return;
            f_lseek(m_file, f_tell(m_file) + len);
        }

        static bool create(std::string fileName);
        static bool del(std::string fileName);
        static bool mkDir(std::string dirName);
        static bool moveRename(std::string src, std::string dst);

    private:
        FIL* m_file = nullptr;
};

#endif // PICOFILE_H
