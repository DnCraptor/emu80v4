/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2016-2024
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

#include "Globals.h"
#include "Platform.h"
#include "Emulation.h"
#include "EmuObjects.h"
#include "EmuWindow.h"
#include "Memory.h"
#include "Cpu.h"
#include "Cpu8080.h"
#include "AddrSpace.h"
#include "Vector.h"
#include "PlatformCore.h"
#include "KbdLayout.h"
#include "CrtRenderer.h"
#include "DiskImage.h"
#include "FileLoader.h"
#include "Keyboard.h"
#include "RamDisk.h"
#include "KbdTapper.h"
#include "Ppi8255.h"
#include "SoundMixer.h"
#include "Covox.h"
#include "Pit8253.h"
#include "Pit8253Sound.h"
#include "Psg3910.h"
#include "Fdc1793.h"
#include "AtaDrive.h"
#include "TapeRedirector.h"
#include "RkTapeHooks.h"
#include "CloseFileHook.h"
#include "CpuHook.h"


using namespace std;

Platform::Platform()
{

    EmuWindow* window = new EmuWindow;
    window->setPlatform(this);
    window->setCaption("Вектор-06Ц");
    addChild(window);
    m_window = window;

    Ram* ram = new Ram(0x10000);
    addChild(ram);

    Rom* rom = new Rom(0x8000, "vector/loader.rom");
    addChild(rom);

    Cpu8080* cpu = new Cpu8080;
    cpu->setFrequency(3000000);
    cpu->setStartAddr(0x0000);
    addChild(cpu);
    m_cpu = cpu;

    VectorAddrSpace* addrSpace = new VectorAddrSpace;
    addrSpace->attachRam(ram);
    addrSpace->attachRom(rom);
    addrSpace->attachCpu(cpu);
    addChild(addrSpace);

    AddrSpace* ioAddrSpace = new AddrSpace;
    addChild(ioAddrSpace);

    cpu->attachAddrSpace(addrSpace);
    cpu->attachIoAddrSpace(ioAddrSpace);

    VectorRenderer* crtRenderer = new VectorRenderer;
    crtRenderer->attachMemory(ram);
    crtRenderer->setVisibleArea(true);
    addChild(crtRenderer);
    m_renderer = crtRenderer;

    addrSpace->attachCrtRenderer(crtRenderer);

    VectorCore* core = new VectorCore;
    core->attachWindow(window);
    core->attachCrtRenderer(crtRenderer);
    addChild(core);
    m_core = core;

    cpu->attachCore(core);

    VectorKeyboard* keyboard = new VectorKeyboard;
    addChild(keyboard);
    m_keyboard = keyboard;

    VectorKbdLayout* kbdLayout = new VectorKbdLayout;
    kbdLayout->setQwertyMode();
    addChild(kbdLayout);
    m_kbdLayout = kbdLayout;

    KbdTapper* kbdTapper = new KbdTapper;
    kbdTapper->setPressTime(20);
    kbdTapper->setReleaseTime(20);
    kbdTapper->setCrDelay(100);
    addChild(kbdTapper);
    m_kbdTapper = kbdTapper;

    VectorPpi8255Circuit* ppiCircuit = new VectorPpi8255Circuit;
    ppiCircuit->attachRenderer(crtRenderer);
    ppiCircuit->attachKeyboard(keyboard);
    addChild(ppiCircuit);

    GeneralSoundSource* tapeSoundSource = new GeneralSoundSource;
    addChild(tapeSoundSource);
    ppiCircuit->attachTapeSoundSource(tapeSoundSource);

    Ppi8255* ppi = new Ppi8255;
    ppi->setNoReset(true);
    ppi->attachPpi8255Circuit(ppiCircuit);
    addChild(ppi);

    AddrSpaceInverter* invertedPpi = new AddrSpaceInverter(ppi);
    addChild(invertedPpi);
    ioAddrSpace->addRange(0x00, 0x03, invertedPpi);

    VectorColorRegister* colorReg = new VectorColorRegister;
    colorReg->attachRenderer(crtRenderer);
    addChild(colorReg);
    ioAddrSpace->addRange(0x0C, 0x0F, colorReg);

    Covox* covox = new Covox(7);
    covox->setNegative(true);
    addChild(covox);

    VectorPpi8255Circuit2* covoxCircuit = new VectorPpi8255Circuit2;
    covoxCircuit->attachCovox(covox);
    addChild(covoxCircuit);

    Ppi8255* ppi2 = new Ppi8255;
    ppi2->attachPpi8255Circuit(covoxCircuit);
    addChild(ppi2);

    AddrSpaceInverter* invertedPpi2 = new AddrSpaceInverter(ppi2);
    addChild(invertedPpi2);
    ioAddrSpace->addRange(0x04, 0x07, invertedPpi2);

    Pit8253* pit = new Pit8253;
    pit->setFrequency(1500000);
    addChild(pit);

    Pit8253SoundSource* sndSource = new Pit8253SoundSource;
    sndSource->attachPit(pit);
    sndSource->setNegative(true);
    addChild(sndSource);

    AddrSpaceInverter* invertedPit = new AddrSpaceInverter(pit);
    addChild(invertedPit);
    ioAddrSpace->addRange(0x08, 0x0B, invertedPit);

    Psg3910* ay = new Psg3910;
    ay->setFrequency(1750000);
    addChild(ay);
    ioAddrSpace->addRange(0x14, 0x15, ay);

    Psg3910SoundSource* psgSoundSource = new Psg3910SoundSource;
    psgSoundSource->attachPsg(ay);
    addChild(psgSoundSource);

    Fdc1793* fdc = new Fdc1793;
    addChild(fdc);

    AddrSpaceInverter* invertedFdc = new AddrSpaceInverter(fdc);
    addChild(invertedFdc);
    ioAddrSpace->addRange(0x18, 0x1B, invertedFdc);

    VectorFddControlRegister* fddReg = new VectorFddControlRegister;
    fddReg->attachFdc1793(fdc);
    addChild(fddReg);
    ioAddrSpace->addRange(0x1C, 0x1C, fddReg);

    AtaDrive* ataDrive = new AtaDrive;
    ataDrive->setVectorGeometry();
    addChild(ataDrive);

    VectorHddRegisters* hddRegisters = new VectorHddRegisters;
    hddRegisters->attachAtaDrive(ataDrive);
    addChild(hddRegisters);
    ioAddrSpace->addRange(0x50, 0x5F, hddRegisters);

    FdImage* diskA = new FdImage(80, 2, 5, 1024);
    diskA->setLabel("A");
    diskA->setFilter("Образы дисков Вектора (*.fdd)|*.fdd;*.FDD|Все файлы (*.*)|*");
    addChild(diskA);
    m_diskA = diskA;
    fdc->attachFdImage(0, diskA);

    FdImage* diskB = new FdImage(80, 2, 5, 1024);
    diskB->setLabel("B");
    diskB->setFilter("Образы дисков Вектора (*.fdd)|*.fdd;*.FDD|Все файлы (*.*)|*");
    addChild(diskB);
    m_diskB = diskB;
    fdc->attachFdImage(1, diskB);

    DiskImage* hdd = new DiskImage;
    hdd->setLabel("HDD");
    hdd->setFilter("Образы HDD Вектора (*.hdd;*.img)|*.hdd;*.HDD;*.img;*.IMG|Все файлы (*.*)|*");
    addChild(hdd);
    m_hdd = hdd;
    ataDrive->assignDiskImage(hdd);

    VectorFileLoader* loader = new VectorFileLoader;
    loader->attachAddrSpace(ram);
    loader->setFilter("Файлы Вектора (*.rom;*.r0m;*.vec;*.cas;*.bas;*fdd)|*.rom;*.ROM;*.rom;*.R0M;*.vec;*.VEC;*.cas;*.CAS;*.bas;*.BAS;*.fdd;*.FDD|Все файлы (*.*)|*");
    addChild(loader);
    m_loader = loader;

    TapeRedirector* tapeInFile = new TapeRedirector;
    tapeInFile->setMode("r");
    tapeInFile->setFilter("Файлы RK-совместимых ПК (*.rk?)|*.rk;*.rk?;*.RK;*.RK?|Файлы Бейсика (*.cas)|*.cas;*.CAS|Все файлы (*.*)|*");
    addChild(tapeInFile);

    TapeRedirector* tapeOutFile = new TapeRedirector;
    tapeOutFile->setMode("w");
    tapeOutFile->setFilter(".rk|.cas");
    addChild(tapeOutFile);

    RkTapeInHook* tapeInHookBas = new RkTapeInHook(0x2B05);
    tapeInHookBas->setSignature("C5D50E0057DB");
    tapeInHookBas->setTapeRedirector(tapeInFile);
    addChild(tapeInHookBas);
    cpu->addHook(tapeInHookBas);

    RkTapeOutHook* tapeOutHookBas = new RkTapeOutHook(0x2B60);
    tapeOutHookBas->setOutputRegisterA(true);
    tapeOutHookBas->setSignature("C5D5F5570E08");
    tapeOutHookBas->setTapeRedirector(tapeOutFile);
    addChild(tapeOutHookBas);
    cpu->addHook(tapeOutHookBas);

    CloseFileHook* closeFileHookBas = new CloseFileHook(0x2B8E);
    closeFileHookBas->setSignature("C506003A203C");
    closeFileHookBas->addTapeRedirector(tapeInFile);
    closeFileHookBas->addTapeRedirector(tapeOutFile);
    addChild(closeFileHookBas);
    cpu->addHook(closeFileHookBas);

    RkTapeInHook* tapeInHookMon = new RkTapeInHook(0xF840);
    tapeInHookMon->setSignature("C5D50E0057DB");
    tapeInHookMon->setTapeRedirector(tapeInFile);
    addChild(tapeInHookMon);
    cpu->addHook(tapeInHookMon);

    RkTapeOutHook* tapeOutHookMon = new RkTapeOutHook(0xF89B);
    tapeOutHookMon->setOutputRegisterA(true);
    tapeOutHookMon->setSignature("C5D5F5573E02");
    tapeOutHookMon->setTapeRedirector(tapeOutFile);
    addChild(tapeOutHookMon);
    cpu->addHook(tapeOutHookMon);

    Ret8080Hook* skipHookMon = new Ret8080Hook(0xEDDC);
    skipHookMon->setSignature("CD1097FB76F3");
    addChild(skipHookMon);
    cpu->addHook(skipHookMon);

    CloseFileHook* closeFileHookMon = new CloseFileHook(0xFEFF);
    closeFileHookMon->setSignature("3AFDFFE604CD");
    closeFileHookMon->addTapeRedirector(tapeInFile);
    closeFileHookMon->addTapeRedirector(tapeOutFile);
    addChild(closeFileHookMon);
    cpu->addHook(closeFileHookMon);

    RkTapeInHook* tapeInHookEmuRk = new RkTapeInHook(0xFC31);
    tapeInHookEmuRk->setSignature("F3C5D50E0057");
    tapeInHookEmuRk->setTapeRedirector(tapeInFile);
    addChild(tapeInHookEmuRk);
    cpu->addHook(tapeInHookEmuRk);

    RkTapeOutHook* tapeOutHookEmuRk = new RkTapeOutHook(0xFC7D);
    tapeOutHookEmuRk->setOutputRegisterA(true);
    tapeOutHookEmuRk->setSignature("F3C5D5F51608");
    tapeOutHookEmuRk->setTapeRedirector(tapeOutFile);
    addChild(tapeOutHookEmuRk);
    cpu->addHook(tapeOutHookEmuRk);

    CloseFileHook* closeFileHookEmuRk = new CloseFileHook(0xFF18);
    closeFileHookEmuRk->setSignature("FB3A61F6E604");
    closeFileHookEmuRk->addTapeRedirector(tapeInFile);
    closeFileHookEmuRk->addTapeRedirector(tapeOutFile);
    addChild(closeFileHookEmuRk);
    cpu->addHook(closeFileHookEmuRk);

    SRam* ramDiskMem = new SRam(0x40000);
    addChild(ramDiskMem);
    addrSpace->attachRamDisk(0, ramDiskMem);

    RamDisk* ramDisk = new RamDisk(1, 0x40000);
    ramDisk->setFilter("Файлы RAM-диска Вектора (*.edd)|*.edd;*.EDD|Все файлы (*.*)|*");
    ramDisk->attachPage(0, ramDiskMem);
    addChild(ramDisk);
    m_ramDisk = ramDisk;

    VectorRamDiskSelector* ramDiskSelector = new VectorRamDiskSelector;
    ramDiskSelector->attachVectorAddrSpace(addrSpace);
    ramDiskSelector->setDiskNum(0);
    addChild(ramDiskSelector);
    ioAddrSpace->addRange(0x10, 0x10, ramDiskSelector);

    SRam* ramDiskMem2 = new SRam(0x40000);
    addChild(ramDiskMem2);
    addrSpace->attachRamDisk(1, ramDiskMem2);

    RamDisk* ramDisk2 = new RamDisk(1, 0x40000);
    ramDisk2->setLabel("EDD2");
    ramDisk2->setFilter("Файлы RAM-диска Вектора (*.edd)|*.edd;*.EDD|Все файлы (*.*)|*");
    ramDisk2->attachPage(0, ramDiskMem2);
    addChild(ramDisk2);
    m_ramDisk2 = ramDisk2;

    VectorRamDiskSelector* ramDiskSelector2 = new VectorRamDiskSelector;
    ramDiskSelector2->attachVectorAddrSpace(addrSpace);
    ramDiskSelector2->setDiskNum(1);
    addChild(ramDiskSelector2);
    ioAddrSpace->addRange(0x11, 0x11, ramDiskSelector2);

    VectorCpuWaits* cpuWaits = new VectorCpuWaits;
    addChild(cpuWaits);
    cpu->attachCpuWaits(cpuWaits);

    m_tapeHooks = {
        tapeOutHookBas, tapeInHookBas, closeFileHookBas,
        tapeOutHookMon, tapeInHookMon, closeFileHookMon,
        tapeOutHookEmuRk, tapeInHookEmuRk, closeFileHookEmuRk,
        skipHookMon
    };

    Platform::init();

    Platform::reset();

    if (m_window)
        m_window->show();
}


void Platform::init()
{
    for (auto it = m_objList.begin(); it != m_objList.end(); it++)
        (*it)->init();
}


void Platform::shutdown()
{
    for (auto it = m_objList.begin(); it != m_objList.end(); it++)
        (*it)->shutdown();
}


void Platform::reset()
{
    for (auto it = m_objList.begin(); it != m_objList.end(); it++)
        (*it)->reset();
}


Platform::~Platform()
{
    Platform::shutdown();
    for (auto it = m_objList.begin(); it != m_objList.end(); it++)
        delete *it;
}


void Platform::addChild(EmuObject* child)
{
    child->setPlatform(this);
    m_objList.push_back(child);
}


void Platform::sysReq(SysReq sr)
{
    switch (sr) {
        case SR_RESET:
            reset();
            break;
        case SR_QUERTY:
            if (m_kbdLayout) {
                m_kbdLayout->setQwertyMode();
            }
            break;
        case SR_JCUKEN:
            if (m_kbdLayout) {
                m_kbdLayout->setJcukenMode();
            }
            break;
        case SR_SMART:
            if (m_kbdLayout) {
                m_kbdLayout->setSmartMode();
            }
            break;
        case SR_FONT:
            if (m_renderer) {
                m_renderer->toggleRenderingMethod();
            }
            break;
        case SR_CROPTOVISIBLE:
            if (m_renderer) {
                m_renderer->toggleCropping();
            }
            break;
        case SR_COLOR:
            if (m_renderer) {
                m_renderer->toggleColorMode();
            }
            break;
        case SR_COPYTXT:
            if (m_renderer) {
                const char* text = m_renderer->getTextScreen();
                if (text)
                    palCopyTextToClipboard(text);
            }
            break;
        case SR_PASTE:
            if (m_kbdTapper && m_kbdLayout->getMode() == KbdLayout::KLM_SMART) {
                m_kbdTapper->typeText(palGetTextFromClipboard());
            }
            break;
        case SR_DISKA:
            // open disk A image
            if (m_diskA)
                m_diskA->chooseFile();
            break;
        case SR_DISKB:
            // open disk B image
            if (m_diskB)
                m_diskB->chooseFile();
            break;
        case SR_HDD:
            // open HDD/CF image
            if (m_hdd)
                m_hdd->chooseFile();
        break;
        case SR_LOAD:
            if (m_loader) {
                m_loader->chooseAndLoadFile();
            }
            break;
        case SR_LOADRUN:
            if (m_loader) {
                m_loader->chooseAndLoadFile(true);
            }
            break;
        case SR_DEBUG:
            // show debugger
            g_emulation->debugRequest(m_cpu);
            break;
        case SR_OPENRAMDISK:
            if (m_ramDisk)
                m_ramDisk->openFile();
            break;
        case SR_SAVERAMDISKAS:
            if (m_ramDisk)
                m_ramDisk->saveFileAs();
            break;
        case SR_OPENRAMDISK2:
            if (m_ramDisk2)
                m_ramDisk2->openFile();
            break;
        case SR_SAVERAMDISK2AS:
            if (m_ramDisk2)
                m_ramDisk2->saveFileAs();
            break;
        case SR_TAPEHOOK:
            if (!m_tapeHooks.empty()) {
                bool enabled = !m_tapeHooks.front()->getEnabled();
                for (CpuHook* hook : m_tapeHooks)
                    hook->setEnabled(enabled);
            }
            break;
        default:
            break;
    }
    g_emulation->resetKeys(nullptr);
}


void Platform::processKey(PalKeyCode keyCode, bool isPressed, unsigned unicodeKey)
{
    emuLog << "Platform::processKey " << to_string(keyCode) << " / " << isPressed << "\n";
    if (m_kbdLayout)
        m_kbdLayout->processKey(keyCode, isPressed, unicodeKey);
}


void Platform::resetKeys()
{
    if (m_kbdLayout)
        m_kbdLayout->resetKeys();
}


void Platform::mouseDrag(int x, int y)
{
    if (m_renderer)
        m_renderer->mouseDrag(x, y);
}


bool Platform::loadFile(string fileName, bool run)
{
    if (m_loader) {
        m_loader->loadFile(fileName, run);
        return true;
    }
    return false;
}


bool Platform::assignDiskAFileName(const std::string& fileName)
{
    return m_diskA && m_diskA->assignFileName(fileName);
}
