﻿/*
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


class Platform;
class EmuConfigControl;
class EmuWindow;
class Cpu;
class PlatformCore;
class KbdLayout;
class CrtRenderer;
class DiskImage;
class FileLoader;
class RamDisk;
class EmuConfigTab;
class Keyboard;
class EmuObjectGroup;
class KbdTapper;

class EmuObject
{
    public:
        virtual Platform* asPlatform() { return nullptr; };
        virtual EmuConfigControl* asEmuConfigControl() { return nullptr; }
        virtual EmuWindow* asEmuWindow() { return nullptr; }
        virtual Cpu* asCpu() { return nullptr; }
        virtual PlatformCore* asPlatformCore() { return nullptr; }
        virtual KbdLayout* asKbdLayout() { return nullptr; }
        virtual CrtRenderer* asCrtRenderer() { return nullptr; }
        virtual DiskImage* asDiskImage() { return nullptr; }
        virtual FileLoader* asFileLoader() { return nullptr; }
        virtual RamDisk* asRamDisk() { return nullptr; }
        virtual EmuConfigTab* asEmuConfigTab() { return nullptr; }
        virtual Keyboard* asKeyboard() { return nullptr; }
        virtual EmuObjectGroup* asEmuObjectGroup() { return nullptr; }
        virtual KbdTapper* asKbdTapper() { return nullptr; }
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

class Ram;

class AddressableDevice : public EmuObject
{
    public:
        virtual Ram* asRam() { return nullptr; }
        //AddressableDevice();
        virtual ~AddressableDevice() {} // !!!

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

};


class ParentObject : public EmuObject
{
    public:
        virtual void addChild(EmuObject* child) = 0;
};


class EmuObjectGroup : public EmuObject
{
    public:
        virtual EmuObjectGroup* asEmuObjectGroup() { return this; }
        bool setProperty(const std::string& propertyName, const EmuValuesList& values) override;
        std::string getPropertyStringValue(const std::string& propertyName) override;
        void addItem(EmuObject* item);

        static EmuObject* create(const EmuValuesList&) {return new EmuObjectGroup();}

    private:
        std::list<EmuObject*> m_objectList;
};

#endif // EMUOBJECTS_H
