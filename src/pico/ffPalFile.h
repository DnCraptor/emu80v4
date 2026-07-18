#ifndef PICOFILE_H
#define PICOFILE_H

#include <string>

#include "ff.h"

class PalFile
{
    public:
        bool open(const std::string& fileName, const std::string& mode = "r") {
            close();
            BYTE modeb = FA_READ;
            if (mode == "r+") modeb = FA_READ | FA_WRITE;
            else if (mode == "a") modeb = FA_WRITE | FA_OPEN_APPEND | FA_CREATE_NEW;
            else if (mode == "a+") modeb = FA_WRITE | FA_OPEN_APPEND | FA_CREATE_ALWAYS;
            else if (mode == "w") modeb = FA_WRITE | FA_CREATE_NEW;
            else if (mode == "w+") modeb = FA_WRITE | FA_CREATE_ALWAYS;
            if (FR_OK != f_open(&m_file, fileName.c_str(), modeb)) {
                close();
                return false;
            }
            m_isOpen = true;
            return true;
        }
        void close() {
            if (m_isOpen) {
                f_close(&m_file);
                m_isOpen = false;
            }
        }
        bool isOpen() { return m_isOpen; }
        bool eof() { return !m_isOpen || f_eof(&m_file); }
        uint8_t read8() {
            if (!m_isOpen) return 0;
            uint8_t res;
            UINT br;
            f_read(&m_file, &res, 1, &br);
            return res;
        }
        uint16_t read16() {
            if (!m_isOpen) return 0;
            uint16_t res;
            UINT br;
            f_read(&m_file, &res, 2, &br);
            return res;
        }
        uint32_t read32() {
            if (!m_isOpen) return 0;
            uint32_t res;
            UINT br;
            f_read(&m_file, &res, 4, &br);
            return res;
        }
        void write8(uint8_t value) {
            if (!m_isOpen) return;
            UINT br;
            f_write(&m_file, &value, 1, &br);
        }
        void write16(uint16_t value) {
            if (!m_isOpen) return;
            UINT br;
            f_write(&m_file, &value, 2, &br);
        }
        void write32(uint32_t value) {
            if (!m_isOpen) return;
            UINT br;
            f_write(&m_file, &value, 4, &br);
        }
        int64_t getSize() {
            if (!m_isOpen) return 0;
            return f_size(&m_file);
        }
        int64_t getPos() {
            if (!m_isOpen) return 0;
            return f_tell(&m_file);
        }
        void seek(int position) {
            if (m_isOpen) f_lseek(&m_file, position);
        }
        void skip(int len) {
            if (!m_isOpen) return;
            f_lseek(&m_file, f_tell(&m_file) + len);
        }


    private:
        FIL m_file{};
        bool m_isOpen = false;
};

#endif // PICOFILE_H
