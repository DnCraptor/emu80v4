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
    constexpr int ramSize = 0x4000;
    constexpr int ramTag = 1;
    constexpr int frameBufferSize = 261 * 288;

    alignas(32) static uint8_t ram0Data[ramSize] = {};
    alignas(32) static uint8_t ram1Data[ramSize] = {};
    alignas(32) static uint8_t ram2Data[ramSize] = {};
    alignas(32) static uint8_t videoRamData[ramSize] = {};
    alignas(32) static uint8_t frameBuffer[frameBufferSize] = {};

    static Platform platform;
    static EmuWindow window;

    static Ram ram0(ram0Data, sizeof(ram0Data));
    static Ram ram1(ram1Data, sizeof(ram1Data));
    static Ram ram2(ram2Data, sizeof(ram2Data));
    static Ram videoRam(videoRamData, sizeof(videoRamData));
    static Rom rom(kLvovRom, sizeof(kLvovRom));

    static AddrSpace addrSpace0;
    static AddrSpace addrSpace1;
    static AddrSpaceMapper addrSpace(2);

    static LvovRenderer renderer(frameBuffer);
    static LvovKeyboard keyboard;
    static LvovKbdLayout kbdLayout;
    static LvovCore core;

    static Ppi8255 ppi1;
    static Ppi8255 ppi2;
    static GeneralSoundSource beep;
    static GeneralSoundSource tapeSound;
    static LvovPpi8255Circuit1 ppiCircuit1;
    static LvovPpi8255Circuit2 ppiCircuit2;
    static AddrSpace ioAddrSpace;

    static LvovCpuWaits cpuWaits;
    static LvovCpuCycleWaits cpuCycleWaits;
    static Cpu8080 cpu;

    static TapeRedirector tapeOut;
    static TapeRedirector tapeIn;
    static LvovFileLoader loader;

    static MsxTapeOutHook tapeOutHook(0xe437);
    static MsxTapeOutHeaderHook tapeOutHeaderHook(0xe42b);
    static MsxTapeInHook tapeInHook(0xe4be);
    static MsxTapeInHeaderHook tapeInHeaderHook(0xe4d0);
    static CloseFileHook closeFileHook(0xe800);
    static EmuObjectGroup tapeGroup;

    static bool initialized = false;
    if (initialized)
        return &platform;
    initialized = true;

    platform.setFastReset(true, 12300000);

    addObject(&platform, &window, "window");
    window.setCaption("ПК-01 Львов");
    window.setDefaultWindowSize(800, 600);
    window.setWindowStyle(WS_AUTOSIZE);
    window.setFrameScale(FS_FIXED);
    window.setFixedYScale(2.0);
    window.setSmoothing(ST_SHARP);
    window.setWideScreen(false);
    window.setCustomScreenFormat(true);
    window.setCustomScreenFormatValue(1.111);

    addObject(&platform, &ram0, "ram0");
    addObject(&platform, &ram1, "ram1");
    addObject(&platform, &ram2, "ram2");
    addObject(&platform, &videoRam, "videoRam");
    ram0.setTag(ramTag);
    ram1.setTag(ramTag);
    ram2.setTag(ramTag);
    videoRam.setTag(ramTag);

    addObject(&platform, &rom, "rom");

    addObject(&platform, &addrSpace0, "addrSpace0");
    addrSpace0.addRange(0x0000, 0x3fff, &ram2);
    addrSpace0.addRange(0x4000, 0x7fff, &videoRam);
    addrSpace0.addRange(0x8000, 0xbfff, &ram2);
    addrSpace0.addRange(0xc000, 0xffff, &rom);

    addObject(&platform, &addrSpace1, "addrSpace1");
    addrSpace1.addRange(0x0000, 0x3fff, &ram0);
    addrSpace1.addRange(0x4000, 0x7fff, &ram1);
    addrSpace1.addRange(0x8000, 0xbfff, &ram2);
    addrSpace1.addRange(0xc000, 0xffff, &rom);

    addObject(&platform, &addrSpace, "addrSpace");
    addrSpace.attachPage(0, &addrSpace0);
    addrSpace.attachPage(1, &addrSpace1);

    addObject(&platform, &renderer, "crtRenderer");
    renderer.attachScreenMemory(&videoRam);

    addObject(&platform, &keyboard, "keyboard");
    addObject(&platform, &kbdLayout, "kbdLayout");
    kbdLayout.setQwertyMode();

    addObject(&platform, &core, "core");
    core.attachWindow(&window);
    core.attachCrtRenderer(&renderer);

    addObject(&platform, &ppi1, "ppi1");
    addObject(&platform, &ppi2, "ppi2");
    addObject(&platform, &beep, "beepSoundSource");
    addObject(&platform, &tapeSound, "tapeSoundSource");

    addObject(&platform, &ppiCircuit1, "ppiCircuit1");
    ppiCircuit1.attachRenderer(&renderer);
    ppiCircuit1.attachTapeSoundSource(&tapeSound);
    ppiCircuit1.attachBeepSoundSource(&beep);
    ppiCircuit1.attachAddrSpaceMapper(&addrSpace);
    ppi1.attachPpi8255Circuit(&ppiCircuit1);

    addObject(&platform, &ppiCircuit2, "ppiCircuit2");
    ppiCircuit2.attachKeyboard(&keyboard);
    ppi2.attachPpi8255Circuit(&ppiCircuit2);

    addObject(&platform, &ioAddrSpace, "ioAddrSpace");
    ioAddrSpace.setAddrMask(0x13);
    ioAddrSpace.addRange(0x00, 0x03, &ppi1);
    ioAddrSpace.addRange(0x10, 0x13, &ppi2);

    addObject(&platform, &cpuWaits, "cpuWaits");
    addObject(&platform, &cpuCycleWaits, "cpuCycleWaits");
    addObject(&platform, &cpu, "cpu");
    cpu.setFrequency(2222222);
    cpu.setStartAddr(0xc000);
    cpu.attachAddrSpace(&addrSpace);
    cpu.attachIoAddrSpace(&ioAddrSpace);
    cpu.attachCore(&core);
    cpu.attachCpuWaits(&cpuWaits);
    cpu.attachCpuCycleWaits(&cpuCycleWaits);

    addObject(&platform, &tapeOut, "msxTapeOutFile");
    tapeOut.setMode("w");
    tapeOut.setFilter(".lvt|.cas");
    tapeOut.setTimeout(6000);

    addObject(&platform, &tapeIn, "msxTapeInFile");
    tapeIn.setMode("r");
    tapeIn.setFilter("Файлы Львова (*.lvt)|*.lvt;*.LVT|Все файлы Львова (*.lv?)|*.lv?;*.LV?|Cas-файлы MSX (*.cas)|*.cas;*.CAS|Все файлы (*.*)|*");

    addObject(&platform, &loader, "loader");
    loader.setSkipTicks(15000000);
    loader.attachAddrSpace(&addrSpace1);
    loader.attachVideoAddrSpace(&videoRam);
    loader.attachIoAddrSpace(&ioAddrSpace);
    loader.attachTapeRedirector(&tapeIn);
    loader.setAllowMultiblock(true);
    loader.setFilter("Файлы Львова (*.lvt;*.sav)|*.lvt;*.LVT;*.sav;*.SAV|Все файлы Львова (*.lv?;*.sav)|*.lv?;*.LV?;*.sav;*.SAV|Cas-файлы MSX (*.cas)|*.cas;*.CAS|Все файлы (*.*)|*");

    addObject(&platform, &tapeOutHook, "tapeOutHook");
    tapeOutHook.setTapeRedirector(&tapeOut);
    cpu.addHook(&tapeOutHook);

    addObject(&platform, &tapeOutHeaderHook, "tapeOutHeaderHook");
    tapeOutHeaderHook.setTapeRedirector(&tapeOut);
    cpu.addHook(&tapeOutHeaderHook);

    addObject(&platform, &tapeInHook, "tapeInHook");
    tapeInHook.setTapeRedirector(&tapeIn);
    tapeInHook.setLvovFix(true);
    cpu.addHook(&tapeInHook);

    addObject(&platform, &tapeInHeaderHook, "tapeInHeaderHook");
    tapeInHeaderHook.setTapeRedirector(&tapeIn);
    cpu.addHook(&tapeInHeaderHook);

    addObject(&platform, &closeFileHook, "closeFileHook");
    closeFileHook.addTapeRedirector(&tapeIn);
    closeFileHook.addTapeRedirector(&tapeOut);
    cpu.addHook(&closeFileHook);

    addObject(&platform, &tapeGroup, "tapeGrp");
    tapeGroup.addItem(&tapeOutHook);
    tapeGroup.addItem(&tapeInHook);
    tapeGroup.addItem(&tapeOutHeaderHook);
    tapeGroup.addItem(&tapeInHeaderHook);
    tapeGroup.addItem(&closeFileHook);

    platform.attachWindow(&window);
    platform.attachCpu(&cpu);
    platform.attachCore(&core);
    platform.attachKbdLayout(&kbdLayout);
    platform.attachRenderer(&renderer);
    platform.attachLoader(&loader);
    platform.attachKeyboard(&keyboard);
    platform.start();
    return &platform;
}
