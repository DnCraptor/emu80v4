/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2016-2018
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

#ifndef MEM_H
#define MEM_H

#include <string>

#include "EmuObjects.h"

size_t psram_alloc(size_t sz, uint32_t init_val = 0);
void psram_free(size_t off);

class Ram : public AddressableDevice {
    public:
        Ram(unsigned memSize);
        static const emu_obj_t obj_type = (1 << RamV) | AddressableDevice::obj_type;
        virtual bool isInstanceOf(EmuObjectType ot) { return !!((1 << ot) & obj_type); }
        virtual ~Ram();
        void writeByte(int addr, uint8_t value) override;
        uint8_t readByte(int addr) override;
        uint8_t* getDataPtr() {
            lprintf("ERROR: TODO: RAM screen page");
            return 0;
        }
        int getSize() {return m_size;}

        static EmuObject* create(const EmuValuesList& parameters) {
            lprintf("Ram::create to allocate %d", sizeof(Ram));
            return parameters[0].isInt() ? new Ram(parameters[0].asInt()) : nullptr;
        }

    private:
        int m_size;
        int m_off = -1;
};

class Rom : public AddressableDevice {
    public:
        Rom(unsigned memSize, std::string fileName);
        virtual ~Rom();
        void writeByte(int, uint8_t)  override {}
        uint8_t readByte(int addr) override;
        int getSize() {return m_size;}

        static EmuObject* create(const EmuValuesList& parameters) {
            lprintf("Rom::create to allocate %d", sizeof(Rom));
            return parameters[1].isInt() ? new Rom(parameters[1].asInt(), parameters[0].asString()) : nullptr;
        }

    protected:
        int m_size;
        int m_off = -1;
};

class NullSpace : public AddressableDevice {
    public:
        NullSpace(uint8_t nullByte = 0xFF) {m_nullByte = nullByte;}
        void writeByte(int, uint8_t)  override {}
        uint8_t readByte(int)  override {return m_nullByte;}

        static EmuObject* create(const EmuValuesList& parameters) {return parameters[0].isInt() ? new NullSpace(parameters[0].asInt()) : nullptr;}

    private:
        uint8_t m_nullByte = 0xFF;
};


#endif // MEM_H
