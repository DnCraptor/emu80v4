#include "Lvov.h"

#include "AddrSpace.h"
#include "CloseFileHook.h"
#include "Cpu8080.h"
#include "EmuObjects.h"
#include "EmuWindow.h"
#include "LvovRom.h"
#include "Memory.h"
#include "MsxTapeHooks.h"
#include "Platform.h"
#include "Ppi8255.h"
#include "SoundMixer.h"
#include "TapeRedirector.h"

namespace {

template<class T>
T* addObject(Platform* platform, T* object, const char* name)
{
    object->setName(std::string("lvov.") + name);
    platform->addChild(object);
    return object;
}

}

Platform* createLvovPlatform()
{
    auto* platform = new Platform();
    platform->setFastReset(true, 12300000);

    auto* window = addObject(platform, new EmuWindow(), "window");
    window->setCaption("ПК-01 Львов");
    window->setDefaultWindowSize(800, 600);
    window->setWindowStyle(WS_AUTOSIZE);
    window->setFrameScale(FS_FIXED);
    window->setFixedYScale(2.0);
    window->setSmoothing(ST_SHARP);
    window->setWideScreen(false);
    window->setCustomScreenFormat(true);
    window->setCustomScreenFormatValue(1.111);

    constexpr int ramSize = 0x4000;
    constexpr int ramTag = 1;

    auto* ram0 = addObject(platform, new Ram(ramSize), "ram0");
    auto* ram1 = addObject(platform, new Ram(ramSize), "ram1");
    auto* ram2 = addObject(platform, new Ram(ramSize), "ram2");
    auto* videoRam = addObject(platform, new Ram(ramSize), "videoRam");
    ram0->setTag(ramTag);
    ram1->setTag(ramTag);
    ram2->setTag(ramTag);
    videoRam->setTag(ramTag);

    auto* rom = addObject(platform, new Rom(kLvovRom, sizeof(kLvovRom)), "rom");

    auto* addrSpace0 = addObject(platform, new AddrSpace(), "addrSpace0");
    addrSpace0->addRange(0x0000, 0x3fff, ram2);
    addrSpace0->addRange(0x4000, 0x7fff, videoRam);
    addrSpace0->addRange(0x8000, 0xbfff, ram2);
    addrSpace0->addRange(0xc000, 0xffff, rom);

    auto* addrSpace1 = addObject(platform, new AddrSpace(), "addrSpace1");
    addrSpace1->addRange(0x0000, 0x3fff, ram0);
    addrSpace1->addRange(0x4000, 0x7fff, ram1);
    addrSpace1->addRange(0x8000, 0xbfff, ram2);
    addrSpace1->addRange(0xc000, 0xffff, rom);

    auto* addrSpace = addObject(platform, new AddrSpaceMapper(2), "addrSpace");
    addrSpace->attachPage(0, addrSpace0);
    addrSpace->attachPage(1, addrSpace1);

    auto* renderer = addObject(platform, new LvovRenderer(), "crtRenderer");
    renderer->attachScreenMemory(videoRam);

    auto* keyboard = addObject(platform, new LvovKeyboard(), "keyboard");
    auto* kbdLayout = addObject(platform, new LvovKbdLayout(), "kbdLayout");
    kbdLayout->setQwertyMode();

    auto* core = addObject(platform, new LvovCore(), "core");
    core->attachWindow(window);
    core->attachCrtRenderer(renderer);

    auto* ppi1 = addObject(platform, new Ppi8255(), "ppi1");
    auto* ppi2 = addObject(platform, new Ppi8255(), "ppi2");
    auto* beep = addObject(platform, new GeneralSoundSource(), "beepSoundSource");
    auto* tapeSound = addObject(platform, new GeneralSoundSource(), "tapeSoundSource");

    auto* ppiCircuit1 = addObject(platform, new LvovPpi8255Circuit1(), "ppiCircuit1");
    ppiCircuit1->attachRenderer(renderer);
    ppiCircuit1->attachTapeSoundSource(tapeSound);
    ppiCircuit1->attachBeepSoundSource(beep);
    ppiCircuit1->attachAddrSpaceMapper(addrSpace);
    ppi1->attachPpi8255Circuit(ppiCircuit1);

    auto* ppiCircuit2 = addObject(platform, new LvovPpi8255Circuit2(), "ppiCircuit2");
    ppiCircuit2->attachKeyboard(keyboard);
    ppi2->attachPpi8255Circuit(ppiCircuit2);

    auto* ioAddrSpace = addObject(platform, new AddrSpace(), "ioAddrSpace");
    ioAddrSpace->setAddrMask(0x13);
    ioAddrSpace->addRange(0x00, 0x03, ppi1);
    ioAddrSpace->addRange(0x10, 0x13, ppi2);

    auto* cpuWaits = addObject(platform, new LvovCpuWaits(), "cpuWaits");
    auto* cpuCycleWaits = addObject(platform, new LvovCpuCycleWaits(), "cpuCycleWaits");
    auto* cpu = addObject(platform, new Cpu8080(), "cpu");
    cpu->setFrequency(2222222);
    cpu->setStartAddr(0xc000);
    cpu->attachAddrSpace(addrSpace);
    cpu->attachIoAddrSpace(ioAddrSpace);
    cpu->attachCore(core);
    cpu->attachCpuWaits(cpuWaits);
    cpu->attachCpuCycleWaits(cpuCycleWaits);

    auto* tapeOut = addObject(platform, new TapeRedirector(), "msxTapeOutFile");
    tapeOut->setMode("w");
    tapeOut->setFilter(".lvt|.cas");
    tapeOut->setTimeout(6000);

    auto* tapeIn = addObject(platform, new TapeRedirector(), "msxTapeInFile");
    tapeIn->setMode("r");
    tapeIn->setFilter("Файлы Львова (*.lvt)|*.lvt;*.LVT|Все файлы Львова (*.lv?)|*.lv?;*.LV?|Cas-файлы MSX (*.cas)|*.cas;*.CAS|Все файлы (*.*)|*");

    auto* loader = addObject(platform, new LvovFileLoader(), "loader");
    loader->setSkipTicks(15000000);
    loader->attachAddrSpace(addrSpace1);
    loader->attachVideoAddrSpace(videoRam);
    loader->attachIoAddrSpace(ioAddrSpace);
    loader->attachTapeRedirector(tapeIn);
    loader->setAllowMultiblock(true);
    loader->setFilter("Файлы Львова (*.lvt;*.sav)|*.lvt;*.LVT;*.sav;*.SAV|Все файлы Львова (*.lv?;*.sav)|*.lv?;*.LV?;*.sav;*.SAV|Cas-файлы MSX (*.cas)|*.cas;*.CAS|Все файлы (*.*)|*");

    auto* tapeOutHook = addObject(platform, new MsxTapeOutHook(0xe437), "tapeOutHook");
    tapeOutHook->setTapeRedirector(tapeOut);
    cpu->addHook(tapeOutHook);

    auto* tapeOutHeaderHook = addObject(platform, new MsxTapeOutHeaderHook(0xe42b), "tapeOutHeaderHook");
    tapeOutHeaderHook->setTapeRedirector(tapeOut);
    cpu->addHook(tapeOutHeaderHook);

    auto* tapeInHook = addObject(platform, new MsxTapeInHook(0xe4be), "tapeInHook");
    tapeInHook->setTapeRedirector(tapeIn);
    tapeInHook->setLvovFix(true);
    cpu->addHook(tapeInHook);

    auto* tapeInHeaderHook = addObject(platform, new MsxTapeInHeaderHook(0xe4d0), "tapeInHeaderHook");
    tapeInHeaderHook->setTapeRedirector(tapeIn);
    cpu->addHook(tapeInHeaderHook);

    auto* closeFileHook = addObject(platform, new CloseFileHook(0xe800), "closeFileHook");
    closeFileHook->addTapeRedirector(tapeIn);
    closeFileHook->addTapeRedirector(tapeOut);
    cpu->addHook(closeFileHook);

    auto* tapeGroup = addObject(platform, new EmuObjectGroup(), "tapeGrp");
    tapeGroup->addItem(tapeOutHook);
    tapeGroup->addItem(tapeInHook);
    tapeGroup->addItem(tapeOutHeaderHook);
    tapeGroup->addItem(tapeInHeaderHook);
    tapeGroup->addItem(closeFileHook);

    platform->attachWindow(window);
    platform->attachCpu(cpu);
    platform->attachCore(core);
    platform->attachKbdLayout(kbdLayout);
    platform->attachRenderer(renderer);
    platform->attachLoader(loader);
    platform->attachKeyboard(keyboard);
    platform->start();
    return platform;
}
