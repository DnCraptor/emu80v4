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
constexpr int kFrameBufferSize = 256 * 256;
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

    T* get()
    {
        return reinterpret_cast<T*>(data);
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
T* bindObject(Platform* platform, T* object)
{
    object->setPlatform(platform);
    return object;
}

template<class F>
void forEachLvovObject(F&& f)
{
    auto& s = g_lvovStorage;

    f(s.window.get());

    f(s.ram0.get());
    f(s.ram1.get());
    f(s.ram2.get());
    f(s.videoRam.get());
    f(s.rom.get());

    f(s.addrSpace0.get());
    f(s.addrSpace1.get());
    f(s.addrSpace.get());

    f(s.renderer.get());
    f(s.keyboard.get());
    f(s.kbdLayout.get());
    f(s.core.get());

    f(s.ppi1.get());
    f(s.ppi2.get());
    f(s.beep.get());
    f(s.tapeSound.get());
    f(s.ppiCircuit1.get());
    f(s.ppiCircuit2.get());
    f(s.ioAddrSpace.get());

    f(s.cpuWaits.get());
    f(s.cpuCycleWaits.get());
    f(s.cpu.get());

    f(s.tapeOut.get());
    f(s.tapeIn.get());
    f(s.loader.get());

    f(s.tapeOutHook.get());
    f(s.tapeOutHeaderHook.get());
    f(s.tapeInHook.get());
    f(s.tapeInHeaderHook.get());
    f(s.closeFileHook.get());
}

void initLvovObjects()
{
    forEachLvovObject([](EmuObject* object) { object->init(); });
}

void resetLvovObjects()
{
    forEachLvovObject([](EmuObject* object) { object->reset(); });
}

void shutdownLvovObjects()
{
    forEachLvovObject([](EmuObject* object) { object->shutdown(); });
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
    platform->setLifecycleCallbacks(
        initLvovObjects,
        resetLvovObjects,
        shutdownLvovObjects
    );

    bindObject(platform, window);
    window->setCaption("ПК-01 Львов");
    window->setDefaultWindowSize(800, 600);
    window->setWindowStyle(WS_AUTOSIZE);
    window->setFrameScale(FS_FIXED);
    window->setFixedYScale(2.0);
    window->setSmoothing(ST_SHARP);
    window->setWideScreen(false);
    window->setCustomScreenFormat(true);
    window->setCustomScreenFormatValue(1.111);

    bindObject(platform, ram0);
    bindObject(platform, ram1);
    bindObject(platform, ram2);
    bindObject(platform, videoRam);
    ram0->setTag(kRamTag);
    ram1->setTag(kRamTag);
    ram2->setTag(kRamTag);
    videoRam->setTag(kRamTag);

    bindObject(platform, rom);

    bindObject(platform, addrSpace0);
    addrSpace0->addRange(0x0000, 0x3fff, ram2);
    addrSpace0->addRange(0x4000, 0x7fff, videoRam);
    addrSpace0->addRange(0x8000, 0xbfff, ram2);
    addrSpace0->addRange(0xc000, 0xffff, rom);

    bindObject(platform, addrSpace1);
    addrSpace1->addRange(0x0000, 0x3fff, ram0);
    addrSpace1->addRange(0x4000, 0x7fff, ram1);
    addrSpace1->addRange(0x8000, 0xbfff, ram2);
    addrSpace1->addRange(0xc000, 0xffff, rom);

    bindObject(platform, addrSpace);
    addrSpace->attachPage(0, addrSpace0);
    addrSpace->attachPage(1, addrSpace1);

    bindObject(platform, renderer);
    renderer->attachScreenMemory(videoRam);

    bindObject(platform, keyboard);
    bindObject(platform, kbdLayout);
    kbdLayout->setQwertyMode();

    bindObject(platform, core);
    core->attachWindow(window);
    core->attachCrtRenderer(renderer);

    bindObject(platform, ppi1);
    bindObject(platform, ppi2);
    bindObject(platform, beep);
    bindObject(platform, tapeSound);

    bindObject(platform, ppiCircuit1);
    ppiCircuit1->attachRenderer(renderer);
    ppiCircuit1->attachTapeSoundSource(tapeSound);
    ppiCircuit1->attachBeepSoundSource(beep);
    ppiCircuit1->attachAddrSpaceMapper(addrSpace);
    ppi1->attachPpi8255Circuit(ppiCircuit1);

    bindObject(platform, ppiCircuit2);
    ppiCircuit2->attachKeyboard(keyboard);
    ppi2->attachPpi8255Circuit(ppiCircuit2);

    bindObject(platform, ioAddrSpace);
    ioAddrSpace->setAddrMask(0x13);
    ioAddrSpace->addRange(0x00, 0x03, ppi1);
    ioAddrSpace->addRange(0x10, 0x13, ppi2);

    bindObject(platform, cpuWaits);
    bindObject(platform, cpuCycleWaits);
    bindObject(platform, cpu);
    cpu->attachHookStorage(s.cpuHookStorage, 5);
    cpu->setFrequency(2222222);
    cpu->setStartAddr(0xc000);
    cpu->attachAddrSpace(addrSpace);
    cpu->attachIoAddrSpace(ioAddrSpace);
    cpu->attachCore(core);
    cpu->attachCpuWaits(cpuWaits);
    cpu->attachCpuCycleWaits(cpuCycleWaits);

    bindObject(platform, tapeOut);
    tapeOut->setMode("w");
    tapeOut->setFilter(".lvt|.cas");
    tapeOut->setTimeout(6000);

    bindObject(platform, tapeIn);
    tapeIn->setMode("r");
    tapeIn->setFilter("Файлы Львова (*.lvt)|*.lvt;*.LVT|Все файлы Львова (*.lv?)|*.lv?;*.LV?|Cas-файлы MSX (*.cas)|*.cas;*.CAS|Все файлы (*.*)|*");

    bindObject(platform, loader);
    loader->setSkipTicks(15000000);
    loader->attachAddrSpace(addrSpace1);
    loader->attachVideoAddrSpace(videoRam);
    loader->attachIoAddrSpace(ioAddrSpace);
    loader->attachTapeRedirector(tapeIn);
    loader->setAllowMultiblock(true);
    loader->setFilter("Файлы Львова (*.lvt;*.sav)|*.lvt;*.LVT;*.sav;*.SAV|Все файлы Львова (*.lv?;*.sav)|*.lv?;*.LV?;*.sav;*.SAV|Cas-файлы MSX (*.cas)|*.cas;*.CAS|Все файлы (*.*)|*");

    bindObject(platform, tapeOutHook);
    tapeOutHook->setTapeRedirector(tapeOut);
    cpu->addHook(tapeOutHook);

    bindObject(platform, tapeOutHeaderHook);
    tapeOutHeaderHook->setTapeRedirector(tapeOut);
    cpu->addHook(tapeOutHeaderHook);

    bindObject(platform, tapeInHook);
    tapeInHook->setTapeRedirector(tapeIn);
    tapeInHook->setLvovFix(true);
    cpu->addHook(tapeInHook);

    bindObject(platform, tapeInHeaderHook);
    tapeInHeaderHook->setTapeRedirector(tapeIn);
    cpu->addHook(tapeInHeaderHook);

    bindObject(platform, closeFileHook);
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
