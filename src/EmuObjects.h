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

#ifndef EMUOBJECTS_H
#define EMUOBJECTS_H

#include <cstdint>

#include <vector>
#include <list>
#include <string>

#include "Parameters.h"

typedef enum EmuObjectType {
    EmuObjectV,
    AddressableDeviceV,
    ActiveDeviceV,
    ParentObjectV,
    EmuObjectGroupV,
    PlatformV,
    EmuConfigV,
    EmuConfigControlV,
    EmuConfigRadioSelectorV,
    EmuConfigTabV,
    CpuV, // 10
    Cpu8080CompatibleV,
    EmuWindowV,
    DebugWindowV,
    PalWindowV,
    CpuZ80V,
    PlatformCoreV,
    KbdLayoutV,
    CrtRendererV,
    DiskImageV,
    FileLoaderV, // 20
    RamDiskV,
    KeyboardV,
    KbdTapperV,
    RamV,
} EmuObjectType;

typedef uint32_t emu_obj_t;

class Platform;

class EmuObject
{
    public:
        static const emu_obj_t obj_type = (1 << EmuObjectV);
        static EmuObject* validateAs(EmuObjectType ot, EmuObject* eo) {
            if (eo == NULL || !eo->isInstanceOf(ot)) return NULL;
            return eo;
        }
        virtual bool isInstanceOf(EmuObjectType ot) { return !!((1 << ot) & obj_type); }
        EmuObject();
        virtual ~EmuObject();

        void setName(std::string name);
        std::string getName();

        int getKDiv() {return m_kDiv;}

        virtual void setFrequency(int64_t freq); // лучше бы в одном из производных классов, но пусть пока будет здесь
        virtual void init() {}
        virtual void shutdown() {}
        virtual void reset() {}

        virtual void setPlatform(Platform* platform) {m_platform = platform;}
        Platform* getPlatform() {return m_platform;}

        virtual bool setProperty(const std::string& propertyName, const EmuValuesList& values);
        virtual std::string getPropertyStringValue(const std::string& propertyName);

        virtual std::string getDebugInfo() {return "";}
        virtual void notify(EmuObject* /*sender*/, int /*data*/) {}

    protected:
        int m_kDiv = 1;
        Platform* m_platform = nullptr;
        static EmuObject* findObj(const std::string& objName);

    private:
        std::string m_name;
};


class AddressableDevice : public EmuObject
{
    public:
        static const emu_obj_t obj_type = (1 << AddressableDeviceV) | EmuObject::obj_type;
        //AddressableDevice();
        virtual ~AddressableDevice() {} // !!!
        virtual bool isInstanceOf(EmuObjectType ot) { return !!((1 << ot) & obj_type); }

        bool setProperty(const std::string& propertyName, const EmuValuesList& values) override;

        virtual void writeByte(int addr, uint8_t value) = 0;
        virtual uint8_t readByte(int) {return 0xFF;}

        uint8_t readByteEx(int addr, int& tag);
        void writeByteEx(int addr, uint8_t value, int& tag);

        void setAddrMask(int mask) {m_addrMask = mask;}

    protected:
        int m_addrMask = 0;

        bool m_supportsTags = false;
        int m_tag = 0;
        static int m_lastTag;

    private:
};


class IActive
{
    public:
        IActive();
        virtual ~IActive();
        uint64_t getClock() {return m_curClock;}
        //void setClock(uint64_t clock) {m_curClock = clock;}
        void pause() {m_isPaused = true; m_curClock = -1;}
        void resume() {m_isPaused = false;}
        void syncronize(uint64_t curClock) {m_curClock = curClock;}
        void syncronize();
        inline bool isPaused() {return m_isPaused;}
        virtual void operate() = 0;

    protected:
        //int m_kDiv = 1;
        uint64_t m_curClock = 0;
        bool m_isPaused = false;
};


class ActiveDevice : public EmuObject, public IActive
{
    public:
        static const emu_obj_t obj_type = (1 << ActiveDeviceV) | EmuObject::obj_type;
        virtual bool isInstanceOf(EmuObjectType ot) { return !!((1 << ot) & obj_type); }
};


class ParentObject : public EmuObject
{
    public:
        virtual void addChild(EmuObject* child) = 0;
        static const emu_obj_t obj_type = (1 << ParentObjectV) | EmuObject::obj_type;
        virtual bool isInstanceOf(EmuObjectType ot) { return !!((1 << ot) & obj_type); }
};


class EmuObjectGroup : public EmuObject
{
    public:
        bool setProperty(const std::string& propertyName, const EmuValuesList& values) override;
        std::string getPropertyStringValue(const std::string& propertyName) override;
        void addItem(EmuObject* item);

        static EmuObject* create(const EmuValuesList&) {return new EmuObjectGroup();}
        
        static const emu_obj_t obj_type = (1 << EmuObjectGroupV) | EmuObject::obj_type;
        virtual bool isInstanceOf(EmuObjectType ot) { return !!((1 << ot) & obj_type); }

    private:
        std::list<EmuObject*> m_objectList;
};

#endif // EMUOBJECTS_H
