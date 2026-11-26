// Dedicated machine wiring, based on the Orion platform descriptions in dist/orion.
#include "Platform.h"
#include "Orion.h"
#include "OrionMenu.h"
#include "Cpu8080.h"
#include "CpuZ80.h"
#include "Memory.h"
#include "Ppi8255.h"
#include "RkKeyboard.h"
#include "KbdLayout.h"
#include "KbdTapper.h"
#include "RkPpi8255Circuit.h"
#include "RkRomDisk.h"
#include "Fdc1793.h"
#include "DiskImage.h"
#include "AtaDrive.h"
#include "PpiAtaAdapter.h"
#include "RamDisk.h"
#include "RkTapeHooks.h"
#include "CloseFileHook.h"
#include "TapeRedirector.h"
#include "SoundMixer.h"
#include "EmuWindow.h"
#include <pico.h>

namespace {
// Orion has a 60K banked window, 1K common RAM and memory-mapped I/O.
// Decode the high byte directly rather than searching generic address ranges.
class OrionBus final : public AddressableDevice {
public:
    AddressableDevice* banks[8]{};
    uint8_t* resident[8]{};
    uint8_t* common = nullptr;
    const uint8_t* rom = nullptr;
    Ppi8255* ppi[3]{};
    Fdc1793* fdc = nullptr;
    OrionRenderer* renderer = nullptr;
    ExtRkRomDisk* extendedRom = nullptr;
    unsigned bankMask = 3;
    unsigned bank = 0;

    void reset() override { bank = 0; }

    uint8_t __not_in_flash_func(readByte)(int address) override {
        unsigned addr = static_cast<uint16_t>(address);
        if (addr < 0xF000) {
            if (resident[bank]) return resident[bank][addr];
            return banks[bank]->readByte(addr);
        }
        if (addr < 0xF400) return common[addr & 0x3ff];
        if (addr >= 0xF800) return rom[addr & 0x7ff];
        unsigned port = (addr >> 8) & 7;
        unsigned reg = addr & 0xff;
        if (port < 7) return ppi[port-4] ? ppi[port-4]->readByte(reg) : 0xff;
        if (!fdc) return 0xff;
        if (reg < 4 || (reg >= 0x10 && reg <= 0x13)) return fdc->readByte(reg & 3);
        if (reg == 4) return (fdc->getDrq() ? 0 : 1) | (fdc->getIrq() ? 0 : 0x80);
        return 0xff;
    }

    void __not_in_flash_func(writeByte)(int address, uint8_t value) override {
        unsigned addr = static_cast<uint16_t>(address);
        if (addr < 0xF000) {
            if (resident[bank]) resident[bank][addr] = value;
            else banks[bank]->writeByte(addr, value);
            return;
        }
        if (addr < 0xF400) { common[addr & 0x3ff] = value; return; }
        unsigned reg = addr & 0xff;
        switch (addr >> 8) {
        case 0xF4: case 0xF5: case 0xF6:
            if (ppi[(addr >> 8)-0xF4]) ppi[(addr >> 8)-0xF4]->writeByte(reg, value);
            break;
        case 0xF7:
            if (!fdc) break;
            if (reg < 4 || (reg >= 0x10 && reg <= 0x13)) fdc->writeByte(reg & 3, value);
            else if (reg == 8) { fdc->setDrive((value >> 1) & 1); fdc->setHead(value & 1); }
            else if (reg == 0x14 || reg == 0x20) { fdc->setDrive(value & 1); fdc->setHead(((value >> 4) & 1) ^ 1); }
            break;
        case 0xF8: renderer->setColorModeByte(value & 7); break;
        case 0xF9: bank = value & bankMask; break;
        case 0xFA: renderer->setScreenBase((~value & 3) << 14); break;
        case 0xFE: if (extendedRom) extendedRom->setPage(value & 15); break;
        default: break;
        }
    }
};

void property(EmuObject* object, const char* name, const std::string& value)
{
    EmuValuesList values(value);
    object->setProperty(name, values);
    values.clearList();
}
}

Platform::Platform(std::string, std::string name)
{
    m_baseDir = "orion/";
    m_baseName = name;
    setName(name);
    const auto model = orionSettings.model;
    const bool monitor1 = model == OrionModel::Monitor1;
    const bool dsdos = model == OrionModel::Dsdos;
    const bool z80 = orionSettings.cpu == OrionCpu::Z80;
    const bool tape = monitor1 || model == OrionModel::Monitor2 || model == OrionModel::Monitor31;
    const char* const monitors[] = {"orion/rom/m1rk.bin", "orion/rom/m2rk.bin", "orion/rom/m31rk.bin", "orion/rom/m32zrk.bin", "orion/rom/m2rk.bin"};
    const char* const romDisks[] = {"", "orion/Orion128_RomDisk4.rom", "orion/romdisk2.bin", "orion/27C512-Orion-512.3-ROM-DISK.2025-01-18.rom.bin", "orion/dsdos/dsdos-128_s1024.bin"};
    unsigned modelIndex = static_cast<unsigned>(model);
    auto own = [this](auto* object, const char* suffix) {
        object->setName(getName() + "." + suffix);
        object->setPlatform(this);
        addChild(object);
        return object;
    };
    m_window = own(new EmuWindow(), "window");
    m_window->setCaption(orionModelName(model));
    auto* bus = own(new OrionBus(), "addrSpace");
    bus->bankMask = dsdos ? 7 : 3;
    for (unsigned i = 0; i <= bus->bankMask; ++i) {
        char bankName[] = "ram0"; bankName[3] += i;
#if PICO_RP2350
        const unsigned internalBanks = 4;
#else
        const unsigned internalBanks = 2;
#endif
        if (i < internalBanks) {
            auto* ram = own(new Ram(0xF000), bankName);
            bus->banks[i] = ram;
            bus->resident[i] = ram->getDataPtr();
        } else bus->banks[i] = own(new SRam(0xF000), bankName);
    }
    bus->common = own(new Ram(0x400), "sysRam")->getDataPtr();
    bus->rom = own(new Rom(0x800, monitors[modelIndex]), "rom")->getDataPtr();
    auto* renderer = own(new OrionRenderer(), "crtRenderer");
    m_renderer = bus->renderer = renderer;
    renderer->attachScreenMemory(bus->banks[0]->asRam());
    renderer->attachColorMemory(bus->banks[1]->asRam());
    if (monitor1) property(renderer, "colorMode", "mono");

    auto* core = own(new OrionCore(), "core");
    m_core = core;
    core->attachWindow(m_window);
    core->attachCrtRenderer(renderer);
    core->attachBeepSoundSource(own(new GeneralSoundSource(), "beepSoundSource"));
    m_cpu = z80 ? static_cast<Cpu*>(own(new CpuZ80(), "cpu")) : static_cast<Cpu*>(own(new Cpu8080(), "cpu"));
    m_cpu->setFrequency(z80 ? 5000000 : 2500000);
    m_cpu->setStartAddr(0xF800);
    m_cpu->attachAddrSpace(bus);
    m_cpu->attachCore(core);
    m_keyboard = own(new RkKeyboard(), "keyboard");
    m_kbdLayout = own(new RkKbdLayout(), "kbdLayout");
    auto* keyboardCircuit = own(new RkPpi8255Circuit(), "ppiCircuit");
    keyboardCircuit->attachRkKeyboard(static_cast<RkKeyboard*>(m_keyboard));
    auto* tapeSound = own(new GeneralSoundSource(), "tapeSoundSource");
    property(keyboardCircuit, "tapeSoundSource", tapeSound->getName());
    bus->ppi[0] = own(new Ppi8255(), "ppi1");
    bus->ppi[0]->attachPpi8255Circuit(keyboardCircuit);
    bus->ppi[1] = own(new Ppi8255(), "ppi2");
    if (!monitor1) {
        RkRomDisk* romDisk;
        if (dsdos) {
            bus->extendedRom = own(new ExtRkRomDisk(romDisks[modelIndex]), "romDisk");
            romDisk = bus->extendedRom;
        } else romDisk = own(new RkRomDisk(romDisks[modelIndex]), "romDisk");
        bus->ppi[1]->attachPpi8255Circuit(romDisk);
        bus->ppi[2] = own(new Ppi8255(), "ppi3");
        bus->fdc = own(new Fdc1793(), "fdc");
        auto* diskA = own(new FdImage(80, 2, 5, 1024), "diskA");
        auto* diskB = own(new FdImage(80, 2, 5, 1024), "diskB");
        m_diskA = diskA; m_diskB = diskB;
        diskA->setLabel("A"); diskB->setLabel("B");
        property(diskA, "filter", "Orion floppy (*.odi)|*.odi;*.ODI|All files|*");
        property(diskB, "filter", "Orion floppy (*.odi)|*.odi;*.ODI|All files|*");
        bus->fdc->attachFdImage(0, diskA); bus->fdc->attachFdImage(1, diskB);
        m_ramDisk = own(new RamDisk(3, 0xF000), "ramDisk");
        for (unsigned i=0; i<3; ++i) m_ramDisk->attachPage(i, bus->banks[i+1]);
        property(m_ramDisk, "filter", "Orion RAM disk|*.ramdisk|All files|*");
        auto* loader = own(new OrionFileLoader(), "loader");
        m_loader = loader;
        loader->attachAddrSpace(bus->banks[0]);
        loader->attachRamDisk(bus->banks[1]);
        loader->setFilter("Orion programs|*.rko;*.bru;*.ord;*.ori;*.RKO;*.BRU;*.ORD;*.ORI|All files|*");
        m_fastReset = true;
        m_fastResetCpuTicks = 3000000;
        if (dsdos) {
            m_hdd = own(new DiskImage(), "hdd");
            m_hdd->setLabel("HDD");
            property(m_hdd, "filter", "DSDOS HDD|*.ohi;*.hdd;*.img|All files|*");
            auto* ata = own(new AtaDrive(), "ataDrive");
            ata->assignDiskImage(m_hdd);
            auto* adapter = own(new PpiAtaAdapter(), "ataAdapter");
            adapter->attachAtaDrive(ata);
            bus->ppi[2]->attachPpi8255Circuit(adapter);
            property(m_hdd, "fileName", "orion/dsdos/hdd.img");
            bus->banks[1]->writeByte(0xBFD0, 1);
        }
    } else {
        m_loader = own(new RkFileLoader(), "loader");
        m_loader->attachAddrSpace(bus);
        m_loader->setFilter("RK programs|*.rk;*.rk?;*.gam;*.RK;*.RK?;*.GAM|All files|*");
    }
    if (tape) {
        auto* input = own(new TapeRedirector(), "tapeInFile");
        auto* output = own(new TapeRedirector(), "tapeOutFile");
        property(input, "mode", "r"); property(output, "mode", "w");
        property(input, "filter", "Tape files|*.rko;*.rk;*.rk?;*.bsm;*.edm|All files|*");
        property(output, "filter", monitor1 ? ".rk" : ".rko");
        auto* in = own(new RkTapeInHook(monitor1 ? 0xFA0D : 0xF9CD), "tapeInHook");
        auto* out = own(new RkTapeOutHook(monitor1 ? 0xFA77 : 0xFA53), "tapeOutHook");
        auto* close = own(new CloseFileHook(monitor1 ? 0xFBAF : 0xF803), "closeFileHook");
        in->setTapeRedirector(input); out->setTapeRedirector(output);
        close->addTapeRedirector(input); close->addTapeRedirector(output);
        m_cpu->addHook(in); m_cpu->addHook(out); m_cpu->addHook(close);
        m_tapeGrp = own(new EmuObjectGroup(), "tapeGrp");
        m_tapeGrp->addItem(in); m_tapeGrp->addItem(out); m_tapeGrp->addItem(close);
    }
    m_kbdTapper = own(new KbdTapper(), "kbdTapper");
    Platform::init();
    Platform::reset();
    m_window->show();
    renderer->publishScanlines();
    renderer->renderFrame();
}
