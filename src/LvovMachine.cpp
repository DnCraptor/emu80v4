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

#include <new>

namespace {

constexpr int kRamSize = 0x4000;
constexpr int kFrameBufferSize = 261 * 288;
constexpr int kCpuHookCount = 5;
constexpr int kRamTag = 1;

template<class T>
struct StaticSlot {
    alignas(T) unsigned char data[sizeof(T)];

    template<class... Args>
    T* construct(Args&&... args)
    {
        return new (data) T(static_cast<Args&&>(args)...);
    }
};

struct LvovMachineStorage {
    alignas(32) uint8_t ram0Data[kRamSize] = {};
    alignas(32) uint8_t ram1Data[kRamSize] = {};
    alignas(32) uint8_t ram2Data[kRamSize] = {};
    alignas(32) uint8_t videoRamData[kRamSize] = {};
    alignas(32) uint8_t frameBuffer[kFrameBufferSize] = {};
    alignas(32) CpuHook* cpuHookStorage[kCpuHookCount] = {};

    StaticSlot<Platform> platform;
    StaticSlot<EmuWindow> window;

    StaticSlot<Ram> ram0;
    StaticSlot<Ram> ram1;
    StaticSlot<Ram> ram2;
    StaticSlot<Ram> videoRam;
    StaticSlot<Rom> rom;

    StaticSlot<AddrSpace> addrSpace0;
    StaticSlot<AddrSpace> addrSpace1;
    StaticSlot<AddrSpaceMapper> addrSpace;

    StaticSlot<LvovRenderer> renderer;
    StaticSlot<LvovKeyboard> keyboard;
    StaticSlot<LvovKbdLayout> kbdLayout;
    StaticSlot<LvovCore> core;

    StaticSlot<Ppi8255> ppi1;
    StaticSlot<Ppi8255> ppi2;
    StaticSlot<GeneralSoundSource> beep;
    StaticSlot<GeneralSoundSource> tapeSound;
    StaticSlot<LvovPpi8255Circuit1> ppiCircuit1;
    StaticSlot<LvovPpi8255Circuit2> ppiCircuit2;
    StaticSlot<AddrSpace> ioAddrSpace;

    StaticSlot<LvovCpuWaits> cpuWaits;
    StaticSlot<LvovCpuCycleWaits> cpuCycleWaits;
    StaticSlot<Cpu8080> cpu;

    StaticSlot<TapeRedirector> tapeOut;
    StaticSlot<TapeRedirector> tapeIn;
    StaticSlot<LvovFileLoader> loader;

    StaticSlot<MsxTapeOutHook> tapeOutHook;
    StaticSlot<MsxTapeOutHeaderHook> tapeOutHeaderHook;
    StaticSlot<MsxTapeInHook> tapeInHook;
    StaticSlot<MsxTapeInHeaderHook> tapeInHeaderHook;
    StaticSlot<CloseFileHook> closeFileHook;

    bool initialized = false;
};

static LvovMachineStorage g_lvovStorage;

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
    auto& s = g_lvovStorage;
    if (s.initialized)
        return reinterpret_cast<Platform*>(s.platform.data);
    s.initialized = true;

    auto* platform = s.platform.construct();
    auto* window = s.window.construct();

    auto* ram0 = s.ram0.construct(s.ram0Data, sizeof(s.ram0Data));
    auto* ram1 = s.ram1.construct(s.ram1Data, sizeof(s.ram1Data));
    auto* ram2 = s.ram2.construct(s.ram2Data, sizeof(s.ram2Data));
    auto* videoRam = s.videoRam.construct(s.videoRamData, sizeof(s.videoRamData));
    auto* rom = s.rom.construct(kLvovRom, sizeof(kLvovRom));

    auto* addrSpace0 = s.addrSpace0.construct();
    auto* addrSpace1 = s.addrSpace1.construct();
    auto* addrSpace = s.addrSpace.construct(2);

    auto* renderer = s.renderer.construct(s.frameBuffer);
    auto* keyboard = s.keyboard.construct();
    auto* kbdLayout = s.kbdLayout.construct();
    auto* core = s.core.construct();

    auto* ppi1 = s.ppi1.construct();
    auto* ppi2 = s.ppi2.construct();
    auto* beep = s.beep.construct();
    auto* tapeSound = s.tapeSound.construct();
    auto* ppiCircuit1 = s.ppiCircuit1.construct();
    auto* ppiCircuit2 = s.ppiCircuit2.construct();
    auto* ioAddrSpace = s.ioAddrSpace.construct();

    auto* cpuWaits = s.cpuWaits.construct();
    auto* cpuCycleWaits = s.cpuCycleWaits.construct();
    auto* cpu = s.cpu.construct();

    auto* tapeOut = s.tapeOut.construct();
    auto* tapeIn = s.tapeIn.construct();
    auto* loader = s.loader.construct();

    auto* tapeOutHook = s.tapeOutHook.construct(0xe437);
    auto* tapeOutHeaderHook = s.tapeOutHeaderHook.construct(0xe42b);
    auto* tapeInHook = s.tapeInHook.construct(0xe4be);
    auto* tapeInHeaderHook = s.tapeInHeaderHook.construct(0xe4d0);
    auto* closeFileHook = s.closeFileHook.construct(0xe800);

    platform->setFastReset(true, 12300000);

    addObject(platform, window, "window");
    window->setCaption("ПК-01 Львов");
    window->setDefaultWindowSize(800, 600);
    window->setWindowStyle(WS_AUTOSIZE);
    window->setFrameScale(FS_FIXED);
    window->setFixedYScale(2.0);
    window->setSmoothing(ST_SHARP);
    window->setWideScreen(false);
    window->setCustomScreenFormat(true);
    window->setCustomScreenFormatValue(1.111);

    addObject(platform, ram0, "ram0");
    addObject(platform, ram1, "ram1");
    addObject(platform, ram2, "ram2");
    addObject(platform, videoRam, "videoRam");
    ram0->setTag(kRamTag);
    ram1->setTag(kRamTag);
    ram2->setTag(kRamTag);
    videoRam->setTag(kRamTag);

    addObject(platform, rom, "rom");

    addObject(platform, addrSpace0, "addrSpace0");
    addrSpace0->addRange(0x0000, 0x3fff, ram2);
    addrSpace0->addRange(0x4000, 0x7fff, videoRam);
    addrSpace0->addRange(0x8000, 0xbfff, ram2);
    addrSpace0->addRange(0xc000, 0xffff, rom);

    addObject(platform, addrSpace1, "addrSpace1");
    addrSpace1->addRange(0x0000, 0x3fff, ram0);
    addrSpace1->addRange(0x4000, 0x7fff, ram1);
    addrSpace1->addRange(0x8000, 0xbfff, ram2);
    addrSpace1->addRange(0xc000, 0xffff, rom);

    addObject(platform, addrSpace, "addrSpace");
    addrSpace->attachPage(0, addrSpace0);
    addrSpace->attachPage(1, addrSpace1);

    addObject(platform, renderer, "crtRenderer");
    renderer->attachScreenMemory(videoRam);

    addObject(platform, keyboard, "keyboard");
    addObject(platform, kbdLayout, "kbdLayout");
    kbdLayout->setQwertyMode();

    addObject(platform, core, "core");
    core->attachWindow(window);
    core->attachCrtRenderer(renderer);

    addObject(platform, ppi1, "ppi1");
    addObject(platform, ppi2, "ppi2");
    addObject(platform, beep, "beepSoundSource");
    addObject(platform, tapeSound, "tapeSoundSource");

    addObject(platform, ppiCircuit1, "ppiCircuit1");
    ppiCircuit1->attachRenderer(renderer);
    ppiCircuit1->attachTapeSoundSource(tapeSound);
    ppiCircuit1->attachBeepSoundSource(beep);
    ppiCircuit1->attachAddrSpaceMapper(addrSpace);
    ppi1->attachPpi8255Circuit(ppiCircuit1);

    addObject(platform, ppiCircuit2, "ppiCircuit2");
    ppiCircuit2->attachKeyboard(keyboard);
    ppi2->attachPpi8255Circuit(ppiCircuit2);

    addObject(platform, ioAddrSpace, "ioAddrSpace");
    ioAddrSpace->setAddrMask(0x13);
    ioAddrSpace->addRange(0x00, 0x03, ppi1);
    ioAddrSpace->addRange(0x10, 0x13, ppi2);

    addObject(platform, cpuWaits, "cpuWaits");
    addObject(platform, cpuCycleWaits, "cpuCycleWaits");
    addObject(platform, cpu, "cpu");
    cpu->attachHookStorage(s.cpuHookStorage, 5);
    cpu->setFrequency(2222222);
    cpu->setStartAddr(0xc000);
    cpu->attachAddrSpace(addrSpace);
    cpu->attachIoAddrSpace(ioAddrSpace);
    cpu->attachCore(core);
    cpu->attachCpuWaits(cpuWaits);
    cpu->attachCpuCycleWaits(cpuCycleWaits);

    addObject(platform, tapeOut, "msxTapeOutFile");
    tapeOut->setMode("w");
    tapeOut->setFilter(".lvt|.cas");
    tapeOut->setTimeout(6000);

    addObject(platform, tapeIn, "msxTapeInFile");
    tapeIn->setMode("r");
    tapeIn->setFilter("Файлы Львова (*.lvt)|*.lvt;*.LVT|Все файлы Львова (*.lv?)|*.lv?;*.LV?|Cas-файлы MSX (*.cas)|*.cas;*.CAS|Все файлы (*.*)|*");

    addObject(platform, loader, "loader");
    loader->setSkipTicks(15000000);
    loader->attachAddrSpace(addrSpace1);
    loader->attachVideoAddrSpace(videoRam);
    loader->attachIoAddrSpace(ioAddrSpace);
    loader->attachTapeRedirector(tapeIn);
    loader->setAllowMultiblock(true);
    loader->setFilter("Файлы Львова (*.lvt;*.sav)|*.lvt;*.LVT;*.sav;*.SAV|Все файлы Львова (*.lv?;*.sav)|*.lv?;*.LV?;*.sav;*.SAV|Cas-файлы MSX (*.cas)|*.cas;*.CAS|Все файлы (*.*)|*");

    addObject(platform, tapeOutHook, "tapeOutHook");
    tapeOutHook->setTapeRedirector(tapeOut);
    cpu->addHook(tapeOutHook);

    addObject(platform, tapeOutHeaderHook, "tapeOutHeaderHook");
    tapeOutHeaderHook->setTapeRedirector(tapeOut);
    cpu->addHook(tapeOutHeaderHook);

    addObject(platform, tapeInHook, "tapeInHook");
    tapeInHook->setTapeRedirector(tapeIn);
    tapeInHook->setLvovFix(true);
    cpu->addHook(tapeInHook);

    addObject(platform, tapeInHeaderHook, "tapeInHeaderHook");
    tapeInHeaderHook->setTapeRedirector(tapeIn);
    cpu->addHook(tapeInHeaderHook);

    addObject(platform, closeFileHook, "closeFileHook");
    closeFileHook->addTapeRedirector(tapeIn);
    closeFileHook->addTapeRedirector(tapeOut);
    cpu->addHook(closeFileHook);

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
