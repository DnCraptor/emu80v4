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

#include <sstream>

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

Platform::Platform(string name)
{
    m_baseName = name;

    // Если платформа с таким именем уже есть, добавляем в конец "$" и номер, начиная от 1
    if (g_emulation->findObject(name)) {
        ostringstream oss;
        int i = 1;
        do
            oss << i++;
        while (g_emulation->findObject(name + "$" + oss.str()));
        name = name + "$" + oss.str();
    }

    setName(name);

    EmuWindow* window = new EmuWindow;
    window->setName(getName() + ".window");
    window->setPlatform(this);
    window->setCaption("Вектор-06Ц");
    window->setDefaultWindowSize(800, 600);
    window->setWindowStyle(WS_AUTOSIZE);
    addChild(window);

    Ram* ram = new Ram(0x10000);
    ram->setName(getName() + ".ram");
    addChild(ram);

    Rom* rom = new Rom(0x8000, "vector/loader.rom");
    rom->setName(getName() + ".rom");
    addChild(rom);

    Cpu8080* cpu = new Cpu8080;
    cpu->setName(getName() + ".cpu");
    cpu->setFrequency(3000000);
    cpu->setStartAddr(0x0000);
    addChild(cpu);

    VectorAddrSpace* addrSpace = new VectorAddrSpace;
    addrSpace->setName(getName() + ".addrSpace");
    addrSpace->attachRam(ram);
    addrSpace->attachRom(rom);
    addrSpace->attachCpu(cpu);
    addChild(addrSpace);

    AddrSpace* ioAddrSpace = new AddrSpace;
    ioAddrSpace->setName(getName() + ".ioAddrSpace");
    addChild(ioAddrSpace);

    cpu->attachAddrSpace(addrSpace);
    cpu->attachIoAddrSpace(ioAddrSpace);

    VectorRenderer* crtRenderer = new VectorRenderer;
    crtRenderer->setName(getName() + ".crtRenderer");
    crtRenderer->attachMemory(ram);
    crtRenderer->setVisibleArea(true);
    addChild(crtRenderer);

    addrSpace->attachCrtRenderer(crtRenderer);

    VectorCore* core = new VectorCore;
    core->setName(getName() + ".core");
    core->attachWindow(window);
    core->attachCrtRenderer(crtRenderer);
    addChild(core);

    cpu->attachCore(core);

    VectorKeyboard* keyboard = new VectorKeyboard;
    keyboard->setName(getName() + ".keyboard");
    addChild(keyboard);

    VectorKbdLayout* kbdLayout = new VectorKbdLayout;
    kbdLayout->setName(getName() + ".kbdLayout");
    kbdLayout->setQwertyMode();
    addChild(kbdLayout);

    KbdTapper* kbdTapper = new KbdTapper;
    kbdTapper->setName(getName() + ".kbdTapper");
    kbdTapper->setPressTime(20);
    kbdTapper->setReleaseTime(20);
    kbdTapper->setCrDelay(100);
    addChild(kbdTapper);

    VectorPpi8255Circuit* ppiCircuit = new VectorPpi8255Circuit;
    ppiCircuit->setName(getName() + ".ppiCircuit");
    ppiCircuit->attachRenderer(crtRenderer);
    ppiCircuit->attachKeyboard(keyboard);
    addChild(ppiCircuit);

    GeneralSoundSource* tapeSoundSource = new GeneralSoundSource;
    tapeSoundSource->setName(getName() + ".tapeSoundSource");
    addChild(tapeSoundSource);
    ppiCircuit->attachTapeSoundSource(tapeSoundSource);

    Ppi8255* ppi = new Ppi8255;
    ppi->setName(getName() + ".ppi");
    ppi->setNoReset(true);
    ppi->attachPpi8255Circuit(ppiCircuit);
    addChild(ppi);

    AddrSpaceInverter* invertedPpi = new AddrSpaceInverter(ppi);
    invertedPpi->setName(getName() + ".invertedPpi");
    addChild(invertedPpi);
    ioAddrSpace->addRange(0x00, 0x03, invertedPpi);

    VectorColorRegister* colorReg = new VectorColorRegister;
    colorReg->setName(getName() + ".colorReg");
    colorReg->attachRenderer(crtRenderer);
    addChild(colorReg);
    ioAddrSpace->addRange(0x0C, 0x0F, colorReg);

    Covox* covox = new Covox(7);
    covox->setName(getName() + ".covox");
    covox->setNegative(true);
    addChild(covox);

    VectorPpi8255Circuit2* covoxCircuit = new VectorPpi8255Circuit2;
    covoxCircuit->setName(getName() + ".covoxCircuit");
    covoxCircuit->attachCovox(covox);
    addChild(covoxCircuit);

    Ppi8255* ppi2 = new Ppi8255;
    ppi2->setName(getName() + ".ppi2");
    ppi2->attachPpi8255Circuit(covoxCircuit);
    addChild(ppi2);

    AddrSpaceInverter* invertedPpi2 = new AddrSpaceInverter(ppi2);
    invertedPpi2->setName(getName() + ".invertedPpi2");
    addChild(invertedPpi2);
    ioAddrSpace->addRange(0x04, 0x07, invertedPpi2);

    Pit8253* pit = new Pit8253;
    pit->setName(getName() + ".pit");
    pit->setFrequency(1500000);
    addChild(pit);

    Pit8253SoundSource* sndSource = new Pit8253SoundSource;
    sndSource->setName(getName() + ".sndSource");
    sndSource->attachPit(pit);
    sndSource->setNegative(true);
    addChild(sndSource);

    AddrSpaceInverter* invertedPit = new AddrSpaceInverter(pit);
    invertedPit->setName(getName() + ".invertedPit");
    addChild(invertedPit);
    ioAddrSpace->addRange(0x08, 0x0B, invertedPit);

    Psg3910* ay = new Psg3910;
    ay->setName(getName() + ".ay");
    ay->setFrequency(1750000);
    addChild(ay);
    ioAddrSpace->addRange(0x14, 0x15, ay);

    Psg3910SoundSource* psgSoundSource = new Psg3910SoundSource;
    psgSoundSource->setName(getName() + ".psgSoundSource");
    psgSoundSource->attachPsg(ay);
    addChild(psgSoundSource);

    Fdc1793* fdc = new Fdc1793;
    fdc->setName(getName() + ".fdc");
    addChild(fdc);

    AddrSpaceInverter* invertedFdc = new AddrSpaceInverter(fdc);
    invertedFdc->setName(getName() + ".invertedFdc");
    addChild(invertedFdc);
    ioAddrSpace->addRange(0x18, 0x1B, invertedFdc);

    VectorFddControlRegister* fddReg = new VectorFddControlRegister;
    fddReg->setName(getName() + ".fddReg");
    fddReg->attachFdc1793(fdc);
    addChild(fddReg);
    ioAddrSpace->addRange(0x1C, 0x1C, fddReg);

    AtaDrive* ataDrive = new AtaDrive;
    ataDrive->setName(getName() + ".ataDrive");
    ataDrive->setVectorGeometry();
    addChild(ataDrive);

    VectorHddRegisters* hddRegisters = new VectorHddRegisters;
    hddRegisters->setName(getName() + ".hddRegisters");
    hddRegisters->attachAtaDrive(ataDrive);
    addChild(hddRegisters);
    ioAddrSpace->addRange(0x50, 0x5F, hddRegisters);

    FdImage* diskA = new FdImage(80, 2, 5, 1024);
    diskA->setName(getName() + ".diskA");
    diskA->setLabel("A");
    diskA->setFilter("Образы дисков Вектора (*.fdd)|*.fdd;*.FDD|Все файлы (*.*)|*");
    addChild(diskA);
    fdc->attachFdImage(0, diskA);

    FdImage* diskB = new FdImage(80, 2, 5, 1024);
    diskB->setName(getName() + ".diskB");
    diskB->setLabel("B");
    diskB->setFilter("Образы дисков Вектора (*.fdd)|*.fdd;*.FDD|Все файлы (*.*)|*");
    addChild(diskB);
    fdc->attachFdImage(1, diskB);

    DiskImage* hdd = new DiskImage;
    hdd->setName(getName() + ".hdd");
    hdd->setLabel("HDD");
    hdd->setFilter("Образы HDD Вектора (*.hdd;*.img)|*.hdd;*.HDD;*.img;*.IMG|Все файлы (*.*)|*");
    addChild(hdd);
    ataDrive->assignDiskImage(hdd);

    VectorFileLoader* loader = new VectorFileLoader;
    loader->setName(getName() + ".loader");
    loader->attachAddrSpace(ram);
    loader->setFilter("Файлы Вектора (*.rom;*.r0m;*.vec;*.cas;*.bas;*fdd)|*.rom;*.ROM;*.rom;*.R0M;*.vec;*.VEC;*.cas;*.CAS;*.bas;*.BAS;*.fdd;*.FDD|Все файлы (*.*)|*");
    addChild(loader);

    TapeRedirector* tapeInFile = new TapeRedirector;
    tapeInFile->setName(getName() + ".tapeInFile");
    tapeInFile->setMode("r");
    tapeInFile->setFilter("Файлы RK-совместимых ПК (*.rk?)|*.rk;*.rk?;*.RK;*.RK?|Файлы Бейсика (*.cas)|*.cas;*.CAS|Все файлы (*.*)|*");
    addChild(tapeInFile);

    TapeRedirector* tapeOutFile = new TapeRedirector;
    tapeOutFile->setName(getName() + ".tapeOutFile");
    tapeOutFile->setMode("w");
    tapeOutFile->setFilter(".rk|.cas");
    addChild(tapeOutFile);

    RkTapeInHook* tapeInHookBas = new RkTapeInHook(0x2B05);
    tapeInHookBas->setName(getName() + ".tapeInHookBas");
    tapeInHookBas->setSignature("C5D50E0057DB");
    tapeInHookBas->setTapeRedirector(tapeInFile);
    addChild(tapeInHookBas);
    cpu->addHook(tapeInHookBas);

    RkTapeOutHook* tapeOutHookBas = new RkTapeOutHook(0x2B60);
    tapeOutHookBas->setName(getName() + ".tapeOutHookBas");
    tapeOutHookBas->setOutputRegisterA(true);
    tapeOutHookBas->setSignature("C5D5F5570E08");
    tapeOutHookBas->setTapeRedirector(tapeOutFile);
    addChild(tapeOutHookBas);
    cpu->addHook(tapeOutHookBas);

    CloseFileHook* closeFileHookBas = new CloseFileHook(0x2B8E);
    closeFileHookBas->setName(getName() + ".closeFileHookBas");
    closeFileHookBas->setSignature("C506003A203C");
    closeFileHookBas->addTapeRedirector(tapeInFile);
    closeFileHookBas->addTapeRedirector(tapeOutFile);
    addChild(closeFileHookBas);
    cpu->addHook(closeFileHookBas);

    RkTapeInHook* tapeInHookMon = new RkTapeInHook(0xF840);
    tapeInHookMon->setName(getName() + ".tapeInHookMon");
    tapeInHookMon->setSignature("C5D50E0057DB");
    tapeInHookMon->setTapeRedirector(tapeInFile);
    addChild(tapeInHookMon);
    cpu->addHook(tapeInHookMon);

    RkTapeOutHook* tapeOutHookMon = new RkTapeOutHook(0xF89B);
    tapeOutHookMon->setName(getName() + ".tapeOutHookMon");
    tapeOutHookMon->setOutputRegisterA(true);
    tapeOutHookMon->setSignature("C5D5F5573E02");
    tapeOutHookMon->setTapeRedirector(tapeOutFile);
    addChild(tapeOutHookMon);
    cpu->addHook(tapeOutHookMon);

    Ret8080Hook* skipHookMon = new Ret8080Hook(0xEDDC);
    skipHookMon->setName(getName() + ".skipHookMon");
    skipHookMon->setSignature("CD1097FB76F3");
    addChild(skipHookMon);
    cpu->addHook(skipHookMon);

    CloseFileHook* closeFileHookMon = new CloseFileHook(0xFEFF);
    closeFileHookMon->setName(getName() + ".closeFileHookMon");
    closeFileHookMon->setSignature("3AFDFFE604CD");
    closeFileHookMon->addTapeRedirector(tapeInFile);
    closeFileHookMon->addTapeRedirector(tapeOutFile);
    addChild(closeFileHookMon);
    cpu->addHook(closeFileHookMon);

    RkTapeInHook* tapeInHookEmuRk = new RkTapeInHook(0xFC31);
    tapeInHookEmuRk->setName(getName() + ".tapeInHookEmuRk");
    tapeInHookEmuRk->setSignature("F3C5D50E0057");
    tapeInHookEmuRk->setTapeRedirector(tapeInFile);
    addChild(tapeInHookEmuRk);
    cpu->addHook(tapeInHookEmuRk);

    RkTapeOutHook* tapeOutHookEmuRk = new RkTapeOutHook(0xFC7D);
    tapeOutHookEmuRk->setName(getName() + ".tapeOutHookEmuRk");
    tapeOutHookEmuRk->setOutputRegisterA(true);
    tapeOutHookEmuRk->setSignature("F3C5D5F51608");
    tapeOutHookEmuRk->setTapeRedirector(tapeOutFile);
    addChild(tapeOutHookEmuRk);
    cpu->addHook(tapeOutHookEmuRk);

    CloseFileHook* closeFileHookEmuRk = new CloseFileHook(0xFF18);
    closeFileHookEmuRk->setName(getName() + ".closeFileHookEmuRk");
    closeFileHookEmuRk->setSignature("FB3A61F6E604");
    closeFileHookEmuRk->addTapeRedirector(tapeInFile);
    closeFileHookEmuRk->addTapeRedirector(tapeOutFile);
    addChild(closeFileHookEmuRk);
    cpu->addHook(closeFileHookEmuRk);

    SRam* ramDiskMem = new SRam(0x40000);
    ramDiskMem->setName(getName() + ".ramDiskMem");
    addChild(ramDiskMem);
    addrSpace->attachRamDisk(0, ramDiskMem);

    VectorRamDiskSelector* ramDiskSelector = new VectorRamDiskSelector;
    ramDiskSelector->setName(getName() + ".ramDiskSelector");
    ramDiskSelector->attachVectorAddrSpace(addrSpace);
    ramDiskSelector->setDiskNum(0);
    addChild(ramDiskSelector);
    ioAddrSpace->addRange(0x10, 0x10, ramDiskSelector);

    VectorCpuWaits* cpuWaits = new VectorCpuWaits;
    cpuWaits->setName(getName() + ".cpuWaits");
    addChild(cpuWaits);
    cpu->attachCpuWaits(cpuWaits);

    EmuObjectGroup* tapeGrp = new EmuObjectGroup;
    tapeGrp->setName(getName() + ".tapeGrp");
    tapeGrp->addItem(tapeOutHookBas);
    tapeGrp->addItem(tapeInHookBas);
    tapeGrp->addItem(closeFileHookBas);
    tapeGrp->addItem(tapeOutHookMon);
    tapeGrp->addItem(tapeInHookMon);
    tapeGrp->addItem(closeFileHookMon);
    tapeGrp->addItem(tapeOutHookEmuRk);
    tapeGrp->addItem(tapeInHookEmuRk);
    tapeGrp->addItem(closeFileHookEmuRk);
    tapeGrp->addItem(skipHookMon);
    addChild(tapeGrp);

    // ищем объект-окно, должен быть единственным
    for (auto it = m_objList.begin(); it != m_objList.end(); it++)
        if (*it)
            if (m_window = (*it)->asEmuWindow())
                break;

    // ищем объект-прцессор, должен быть единственным
    for (auto it = m_objList.begin(); it != m_objList.end(); it++)
        if (*it)
            if (m_cpu = (*it)->asCpu())
                break;

    // ищем объект-ядро, должен быть единственным
    for (auto it = m_objList.begin(); it != m_objList.end(); it++)
        if (*it)
            if (m_core = (*it)->asPlatformCore())
                break;

    // ищем объект - раскладку клавиатуры, должен быть единственным
    for (auto it = m_objList.begin(); it != m_objList.end(); it++)
        if (*it)
            if ((m_kbdLayout = (*it)->asKbdLayout()))
                break;

    // ищем объекты - рендереры, может быть два
    for (auto it = m_objList.begin(); it != m_objList.end(); it++) {
        if (*it) {
            auto renderer = (*it)->asCrtRenderer();
            if (renderer) {
                if (!m_renderer)
                    m_renderer = renderer;
                else {
                    m_renderer2 = renderer;
                    break;
                }
            }
        }
    }

    // ищем объекты - образы дисков A и B
    for (auto it = m_objList.begin(); it != m_objList.end(); it++) {
        DiskImage* img = (*it) ? (*it)->asDiskImage() : nullptr;
        if (img) {
            if (img->getLabel() == "A")
                m_diskA = img;
            else if (img->getLabel() == "B")
                m_diskB = img;
            else if (img->getLabel() == "C")
                m_diskC = img;
            else if (img->getLabel() == "D")
                m_diskD = img;
            else if (img->getLabel() == "HDD")
                m_hdd = img;
        }

    // ищем объект - загрузчик, должен быть единственным
    for (auto it = m_objList.begin(); it != m_objList.end(); it++)
        if(*it)
            if ((m_loader = (*it)->asFileLoader()))
                break;
    }

    // ищем объекты - RAM-диски
    for (auto it = m_objList.begin(); it != m_objList.end(); it++) {
        RamDisk* ramDisk;
        if ((ramDisk = (*it) ? (*it)->asRamDisk() : nullptr)) {
            if (ramDisk->getLabel() != "EDD2")
                m_ramDisk = ramDisk;
            else
                m_ramDisk2 = ramDisk;
        }
    }

    // ищем объект - клавиатуру, должен быть единственным
    for (auto it = m_objList.begin(); it != m_objList.end(); it++)
        if ((m_keyboard = (*it) ? (*it)->asKeyboard() : nullptr))
            break;

    // ищем объект - группу tapeGrp
    for (auto it = m_objList.begin(); it != m_objList.end(); it++) {
        EmuObjectGroup* grp = (*it) ? (*it)->asEmuObjectGroup() : nullptr;
        if (grp) {
            name = grp->getName();
            if (name.substr(name.find_last_of(".")) == ".tapeGrp") {
                m_tapeGrp = grp;
                break;
            }
        }
    }

    // ищем объект - KbdTapper, должен быть единственным
    for (auto it = m_objList.begin(); it != m_objList.end(); it++)
        if ((m_kbdTapper = (*it) ? (*it)->asKbdTapper() : nullptr))
            break;

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
            if (m_fastReset && m_fastResetCpuTicks) {
                Cpu* cpu = getCpu();
                cpu->disableHooks();
                g_emulation->exec((int64_t)cpu->getKDiv() * m_fastResetCpuTicks); // no 2d parameter: no fast reset when debugger is active
                cpu->enableHooks();
            }
            updateDebugger();
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
        case SR_DISKC:
            // open floppy disk C image
            if (m_diskC)
                m_diskC->chooseFile();
            break;
        case SR_DISKD:
            // open floppy disk D image
            if (m_diskD)
                m_diskD->chooseFile();
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
            //showDebugger();
            break;
        case SR_LOADRAMDISK:
            if (m_ramDisk)
                m_ramDisk->loadFromFile();
            break;
        case SR_SAVERAMDISK:
            if (m_ramDisk)
                m_ramDisk->saveToFile();
            break;
        case SR_OPENRAMDISK:
            if (m_ramDisk)
                m_ramDisk->openFile();
            break;
        case SR_SAVERAMDISKAS:
            if (m_ramDisk)
                m_ramDisk->saveFileAs();
            break;
        case SR_LOADRAMDISK2:
            if (m_ramDisk2)
                m_ramDisk2->loadFromFile();
            break;
        case SR_SAVERAMDISK2:
            if (m_ramDisk2)
                m_ramDisk2->saveToFile();
            break;
        case SR_OPENRAMDISK2:
            if (m_ramDisk2)
                m_ramDisk2->openFile();
            break;
        case SR_SAVERAMDISK2AS:
            if (m_ramDisk2)
                m_ramDisk2->saveFileAs();
            break;
        case SR_FASTRESET:
            if (m_fastResetCpuTicks) {
                m_fastReset = !m_fastReset;
            }
            break;
        case SR_TAPEHOOK:
            if (m_tapeGrp) {
                //EmuValuesList param;
                string val = m_tapeGrp->getPropertyStringValue("enabled");
                if (val == "yes")
                    val = "no";
                else if (val == "no")
                    val = "yes";
                else
                    break;

                m_tapeGrp->setProperty("enabled", val);
            }
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
        updateDebugger();
        return true;
    }
    return false;
}


void Platform::draw()
{
    if (m_core)
        m_core->draw();
}


void Platform::showDebugger()
{
}


void Platform::updateDebugger()
{
}


void Platform::reqScreenUpdateForDebug()
{
    if (m_renderer)
        m_renderer->prepareDebugScreen();
    if (m_renderer2)
        m_renderer2->prepareDebugScreen();
}


bool Platform::setProperty(const string& propertyName, const EmuValuesList& values)
{
    if (EmuObject::setProperty(propertyName, values))
        return true;

    if (propertyName == "helpFile") {
        m_helpFile = values[0].asString();
        return true;
    } else if (propertyName == "codePage") {
        if (values[0].asString() == "rk") {
            m_codePage = CP_RK;
            return true;
        } else if (values[0].asString() == "koi8") {
            m_codePage = CP_KOI8;
            return true;
        }
    } else if (propertyName == "muteTape") {
        if (values[0].asString() == "yes" || values[0].asString() == "no") {
            m_muteTape = values[0].asString() == "yes";
            return true;
        }
    } else if (propertyName == "fastReset") {
        if (values[0].asString() == "yes" || values[0].asString() == "no") {
            m_fastReset = values[0].asString() == "yes";
            return true;
        }
    } else if (propertyName == "fastResetCpuTicks") {
            m_fastResetCpuTicks = values[0].asInt();
            return true;
    }
    return false;
}


string Platform::getPropertyStringValue(const string& propertyName)
{
    string res;

    res = EmuObject::getPropertyStringValue(propertyName);
    if (res != "")
        return res;

    if (propertyName == "helpFile")
        return m_helpFile;
    else if (propertyName == "codePage")
        return m_codePage == CP_RK ? "rk" : "koi8";
    else if (propertyName == "muteTape")
        return m_muteTape ? "yes" : "no";
    else if (propertyName == "fastReset")
        return m_fastResetCpuTicks ? m_fastReset ? "yes" : "no" : "";

    return "";
}


string Platform::getAllDebugInfo()
{
    string res = "";
    for (auto it = m_objList.begin(); it != m_objList.end(); it++) {
        string s = (*it)->getDebugInfo();
        if (s != "") {
            if (res != "")
                res += "\n\n";
            res = res + s;
        }
    }
    return res;
}


void Platform::updateScreenOnce()
{
    if (m_renderer)
        m_renderer->updateScreenOnce();
//    if (m_renderer2)
//        m_renderer2->updateScreenOnce();
    updateDebugger();
}
