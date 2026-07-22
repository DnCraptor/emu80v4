/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2019-2024
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

#include <algorithm>
#include <cstring>
#include <new>

#include "Globals.h"
#include "EmuCalls.h"
#include "Vector.h"
#include "Cpu.h"
#include "CpuZ80.h"
#include "Cpu8080.h"
#include "AddrSpace.h"
#include "Emulation.h"
#include "Memory.h"
#include "CrtRenderer.h"
#include "DiskImage.h"
#include "Keyboard.h"
#include "KbdLayout.h"
#include "Fdc1793.h"
#include "SoundMixer.h"
#include "WavReader.h"
#include "Covox.h"
#include "PrnWriter.h"
#include "AtaDrive.h"
#include "KbdTapper.h"
#include "Ppi8255.h"
#include "Pit8253.h"
#include "Pit8253Sound.h"
#include "Psg3910.h"
#include "TapeRedirector.h"
#include "WavWriter.h"
#include "RkTapeHooks.h"
#include "CloseFileHook.h"
#include "CpuHook.h"
#include "RamDisk.h"

using namespace std;


// ---------------------------------------------------------------------------
// Крупные буферы существуют ровно в одном экземпляре на всю прошивку, поэтому
// размещены статически: так они попадают в .bss и видны в отчёте линковщика
// (-Wl,--print-memory-usage), а не растворяются в куче, размер которой
// определяется тем, что осталось. После этого печатаемое линковщиком значение
// RAM и есть фактический статический бюджет, а разница до объёма SRAM —
// запас под стеки и оставшиеся динамические объекты.
//
//   кадровый буфер   626 * 288 = 180 288 байт
//   основное ОЗУ     64 * 1024 =  65 536 байт
//   итого                        245 824 байта
// ---------------------------------------------------------------------------

static const int c_frameBufSize = 626 * 288;   // 626 = 704 / 13.5 * pixelFreq
static const int c_mainRamSize = 0x10000;

static uint8_t s_frameBuffer[c_frameBufSize];
static uint8_t s_mainRam[c_mainRamSize];


// ---------------------------------------------------------------------------
// Явное размещение устройств машины.
//
// Состав машины фиксирован, прошивка не завершается, деструкторы у этих
// объектов никогда не отработают. Поэтому они размещены статически: попадают
// в .bss и видны в отчёте линковщика, а не растворяются в куче.
//
// ПОРЯДОК ОБЪЯВЛЕНИЯ ЗНАЧИМ. Он совпадает с прежним порядком создания через
// new, а от него зависит порядок регистрации активных устройств и источников
// звука. Планировщик разрешает совпадение тактов по позиции в массиве
// (совпадения CPU и рендерера случаются на каждой границе кадра), поэтому
// переставлять поля нельзя.
//
// Конструкторы всех этих классов инертны: они не обращаются к g_emulation и
// не трогают файловую систему, поэтому безопасно выполняются до main().
// Вся настройка по-прежнему делается в конструкторе VectorCore, который
// вызывается из Emulation::init(), когда глобальные объекты уже готовы.
// ---------------------------------------------------------------------------

// Ядро CPU заменяется на ходу (8080 <-> Z80), поэтому лежит в отдельном буфере
// и создаётся placement new. Буфер рассчитан на большее из двух ядер.
static constexpr size_t c_cpuSlotSize =
    sizeof(Cpu8080) > sizeof(CpuZ80) ? sizeof(Cpu8080) : sizeof(CpuZ80);

namespace {

struct CpuSlot {
    alignas(8) uint8_t storage[c_cpuSlotSize];
    Cpu8080Compatible* cpu = nullptr;
    VectorCpuType type = VECTOR_CPU_8080;

    // Ядро создаётся здесь, а не в VectorCore, чтобы сохранить свою позицию
    // в порядке регистрации активных устройств
    CpuSlot() {cpu = new (storage) Cpu8080();}

    // Замена ядра на ходу. Резидентно всегда одно: старое разрушается,
    // новое создаётся тем же placement new в том же буфере.
    void replace(VectorCpuType newType)
    {
        if (newType == type)
            return;
        cpu->~Cpu8080Compatible();
        cpu = (newType == VECTOR_CPU_Z80)
            ? static_cast<Cpu8080Compatible*>(new (storage) CpuZ80())
            : static_cast<Cpu8080Compatible*>(new (storage) Cpu8080());
        type = newType;
    }
};

struct Devices {
    Ram                      ram{s_mainRam, c_mainRamSize};
    Rom                      rom{0x8000, "vector/loader.rom"};
    CpuSlot                  cpuSlot;
    VectorAddrSpace          addrSpace;
    AddrSpace                ioAddrSpace;
    VectorRenderer           renderer;
    VectorKeyboard           keyboard;
    VectorKbdLayout          kbdLayout;
    KbdTapper                kbdTapper;
    VectorPpi8255Circuit     ppiCircuit;
    GeneralSoundSource       tapeSoundSource;
    Ppi8255                  ppi;
    VectorColorRegister      colorReg;
    Covox                    covox{7};
    VectorPpi8255Circuit2    covoxCircuit;
    Ppi8255                  ppi2;
    Pit8253                  pit;
    Pit8253SoundSource       sndSource;
    Psg3910                  ay;
    Psg3910SoundSource       psgSoundSource;
    Fdc1793                  fdc;
    VectorFddControlRegister fddReg;
    AtaDrive                 ataDrive;
    VectorHddRegisters       hddRegisters;
    FdImage                  diskA{80, 2, 5, 1024};
    FdImage                  diskB{80, 2, 5, 1024};
    DiskImage                hdd;
    VectorFileLoader         loader;
    WavWriter                wavWriter;
    TapeRedirector           tapeInFile;
    TapeRedirector           tapeOutFile;
    RkTapeInHook             tapeInHookBas{0x2B05};
    RkTapeOutHook            tapeOutHookBas{0x2B60};
    CloseFileHook            closeFileHookBas{0x2B8E, &tapeInFile, &tapeOutFile};
    RkTapeInHook             tapeInHookMon{0xF840};
    RkTapeOutHook            tapeOutHookMon{0xF89B};
    Ret8080Hook              skipHookMon{0xEDDC};
    CloseFileHook            closeFileHookMon{0xFEFF, &tapeInFile, &tapeOutFile};
    RkTapeInHook             tapeInHookEmuRk{0xFC31};
    RkTapeOutHook            tapeOutHookEmuRk{0xFC7D};
    CloseFileHook            closeFileHookEmuRk{0xFF18, &tapeInFile, &tapeOutFile};
    SRam                     ramDiskMem{0x40000};
    RamDisk                  ramDisk{0x40000};
    VectorRamDiskSelector    ramDiskSelector;
    SRam                     ramDiskMem2{0x40000};
    RamDisk                  ramDisk2{0x40000};
    VectorRamDiskSelector    ramDiskSelector2;
};

} // namespace

static Devices s_devices;



void VectorAddrSpace::reset() {
    m_romEnabled = true;
    m_inRamPagesMask = 0;
    m_stackDiskEnabled = false;
    m_inRamDiskPage = 0;
    m_stackDiskPage = 0;
    m_inRamPagesMask2 = 0;
    m_stackDiskEnabled2 = false;
    m_inRamDiskPage2 = 0;
    m_stackDiskPage2 = 0;
    m_eramSegment = 0;
    m_eramPageStartAddr = 0xA000;
    m_eramPageEndAddr = 0xDFFF;
    rebuildPageMap();
}


void __not_in_flash_func(VectorAddrSpace::writeByte)(int addr, uint8_t value)
{
    if (m_eram) {
        // ERAM
        if (m_stackDiskEnabled && m_cpu->checkForStackOperation())
            m_ramDisk->writeByte(m_eramSegment * 0x40000 + m_stackDiskPage * 0x10000 + addr, value);
        else if (m_inRamPagesMask & 2 && addr >= m_eramPageStartAddr && addr <= m_eramPageEndAddr)
            m_ramDisk->writeByte(m_eramSegment * 0x40000 + m_inRamDiskPage * 0x10000 + addr, value);
        else {
            if (addr >= 0x8000 && m_crtRenderer)
                m_crtRenderer->vidMemWriteNotify();
            m_mainMemory->writeByte(addr, value);
        }
        return;
    }

    // Barkar
    if (m_stackDiskEnabled && m_cpu->checkForStackOperation())
        m_ramDisk->writeByte(m_stackDiskPage * 0x10000 + addr, value);
    else if (m_stackDiskEnabled2 && m_cpu->checkForStackOperation())
        m_ramDisk2->writeByte(m_stackDiskPage2 * 0x10000 + addr, value);
    else if (m_inRamPagesMask && (addr >= 0x8000) && m_inRamPagesMask & (1 << ((addr & 0x6000) >> 13)))
        m_ramDisk->writeByte(m_inRamDiskPage * 0x10000 + addr, value);
    else if (m_inRamPagesMask2 && (addr >= 0x8000) && m_inRamPagesMask2 & (1 << ((addr & 0x6000) >> 13)))
        m_ramDisk2->writeByte(m_inRamDiskPage2 * 0x10000 + addr, value);
    else {
        if (addr >= 0x8000 && m_crtRenderer)
            m_crtRenderer->vidMemWriteNotify();
        m_mainMemory->writeByte(addr, value);
    }
}


uint8_t __not_in_flash_func(VectorAddrSpace::readByte)(int addr)
{
    if (m_eram) {
        // ERAM
        if (m_stackDiskEnabled && m_cpu->checkForStackOperation())
            return m_ramDisk->readByte(m_eramSegment * 0x40000 + m_stackDiskPage * 0x10000 + addr);
        if (m_inRamPagesMask & 2 && addr >= m_eramPageStartAddr && addr <= m_eramPageEndAddr)
            return m_ramDisk->readByte(m_eramSegment * 0x40000 + m_inRamDiskPage * 0x10000 + addr);
        if (m_romEnabled && addr < m_rom->getSize())
            return m_rom->readByte(addr); // add rom check
        else
            return m_mainMemory->readByte(addr);
    }

    // Barkar
    if (m_stackDiskEnabled && m_cpu->checkForStackOperation())
        return m_ramDisk->readByte(m_stackDiskPage * 0x10000 + addr);
    if (m_stackDiskEnabled2 && m_cpu->checkForStackOperation())
        return m_ramDisk2->readByte(m_stackDiskPage2 * 0x10000 + addr);
    if (m_inRamPagesMask && (addr >= 0x8000) && m_inRamPagesMask & (1 << ((addr & 0x6000) >> 13)))
        return m_ramDisk->readByte(m_inRamDiskPage * 0x10000 + addr);
    if (m_inRamPagesMask2 && (addr >= 0x8000) && m_inRamPagesMask2 & (1 << ((addr & 0x6000) >> 13)))
        return m_ramDisk2->readByte(m_inRamDiskPage2 * 0x10000 + addr);
    if (m_romEnabled && addr < m_rom->getSize())
        return m_rom->readByte(addr); // add rom check
    else
        return m_mainMemory->readByte(addr);
}


void VectorAddrSpace::attachRamDisk(int diskNum, SRam* ramDisk)
{
    if (diskNum == 0)
        m_ramDisk = ramDisk;
    else // if (diskNum == 1)
        m_ramDisk2 = ramDisk;
    rebuildPageMap();
}


void VectorAddrSpace::enableRom()
{
    m_romEnabled = true;
    rebuildPageMap();
}


void VectorAddrSpace::disableRom()
{
    m_romEnabled = false;
    rebuildPageMap();
}


// Пересекается ли страница [first..last] с областью, отданной RAM-диску.
// Условия дословно повторяют readByte()/writeByte(), включая отсутствие
// проверки указателя диска: если маска выставлена, страница уходит на прежний
// путь и ведёт себя в точности как раньше.
bool VectorAddrSpace::pageHitsRamDisk(int first, int last) const
{
    if (m_eram) {
        // ERAM: окно задаётся произвольной парой адресов, поэтому проверяем
        // пересечение интервалов, а не отдельные точки
        return (m_inRamPagesMask & 2) &&
               first <= int(m_eramPageEndAddr) && last >= int(m_eramPageStartAddr);
    }

    // Barkar: страницы по 8 КиБ в верхней половине адресного пространства.
    // Номер банка внутри 256-байтной страницы постоянен.
    if (first < 0x8000)
        return false;
    const int bank = 1 << ((first & 0x6000) >> 13);
    return (m_inRamPagesMask & bank) || (m_inRamPagesMask2 & bank);
}


void VectorAddrSpace::rebuildPageMap()
{
    if (!m_cpu)
        return;

    m_cpu->attachCrtRenderer(m_crtRenderer);
    m_cpu->clearPageMap();

    // Переключение стековой страницы зависит от признака текущей команды
    // (checkForStackOperation), поэтому в статическую карту не ложится:
    // пока оно включено, карта остаётся пустой и работает прежний путь.
    if (m_stackDiskEnabled || m_stackDiskEnabled2)
        return;

    if (!m_mainMemory || m_mainMemory->getSize() < 0x10000)
        return;

    uint8_t* ramBase = m_mainMemory->getDataPtr();
    if (!ramBase)
        return;

    const uint8_t* romBase = m_rom ? m_rom->getDataPtr() : nullptr;
    const int romSize = m_rom ? m_rom->getSize() : 0;
    const bool romActive = m_romEnabled && romBase && romSize > 0;

    for (int pg = 0; pg < 256; pg++) {
        const int first = pg << 8;
        const int last = first + 0xFF;

        if (pageHitsRamDisk(first, last))
            continue;   // обе карты остаются пустыми, страница идёт прежним путём

        // Запись: ПЗУ для записи прозрачно, writeByte() всегда адресует
        // основное ОЗУ. Уведомление рендерера для addr >= 0x8000 выполняет
        // сам CPU в as_output().
        m_cpu->setWritePage(pg, ramBase);

        // Чтение: ПЗУ перекрывает начало адресного пространства
        if (!romActive || first >= romSize)
            m_cpu->setReadPage(pg, ramBase);
        else if (last < romSize)
            m_cpu->setReadPage(pg, romBase);
        // иначе страница пересекает границу ПЗУ — оставляем прежний путь
    }
}

void VectorAddrSpace::ramDiskControl(int diskNum, int inRamPagesMask, bool stackEnabled, int inRamPage, int stackPage)
{
    if (diskNum == 0) {
        m_inRamPagesMask = inRamPagesMask;
        m_stackDiskEnabled = stackEnabled;
        m_inRamDiskPage = inRamPage;
        m_stackDiskPage = stackPage;
    } else { // if (diskNum == 1)
        m_inRamPagesMask2 = inRamPagesMask;
        m_stackDiskEnabled2 = stackEnabled;
        m_inRamDiskPage2 = inRamPage;
        m_stackDiskPage2 = stackPage;
    }
    rebuildPageMap();
}


void VectorAddrSpace::eramControl(int eramSegment, int eramPageStartAddr, int eramPageEndAddr)
{
    m_eramSegment = eramSegment;
    m_eramPageStartAddr = eramPageStartAddr;
    m_eramPageEndAddr = eramPageEndAddr;
    rebuildPageMap();
}

void VectorCore::inte(bool isActive)
{
    m_intsEnabled = isActive;
    if (!isActive)
        m_intReq = false;
    else if (m_intReq) {
        Cpu8080Compatible* cpu = getCpu();
        if (cpu->getInte()) {
            cpu->intRst(7);
            cpu->hrq(cpu->getKDiv() * 5); // add waits to RST
        }
    }
}


void VectorCore::vrtc(bool isActive)
{
    if (isActive && m_intsEnabled) {
        m_intReq = true;
        Cpu8080Compatible* cpu = getCpu();
        if (cpu->getInte()) {
            cpu->intRst(7);
            cpu->hrq(cpu->getKDiv() * 5); // add waits to RST
        }
    }
}



VectorRenderer::VectorRenderer()
{
    m_sizeX = 512;
    m_sizeY = 256;
    m_pixelData = s_frameBuffer;
    m_frameBuf = m_pixelData;

    memset(m_colorPalette, 0, 16);
    memset(m_bwPalette, 0, 16);
    m_palette = m_colorPalette;
}


void VectorRenderer::init()
{
    // Требует готового g_emulation, поэтому не может выполняться в конструкторе
    m_ticksPerPixel = g_emulation->getFrequency() / 12000000;

    m_curFramePixel = 0;
    m_curFrameClock = m_curClock;

    prepareFrame(); // prepare 1st frame dimensions
}


VectorRenderer::~VectorRenderer()
{
}


void __not_in_flash_func(VectorRenderer::operate)()
{
    advanceTo(m_curClock);
    m_curFrameClock = m_curClock;
    m_curFramePixel = 0;
    m_curClock += m_ticksPerPixel * 768 * 312;
    m_lineOffsetIsLatched = false;
    renderFrame();
    m_machine->vrtc(true);
    m_lastColor = 0;
}


// Деление на 768 = 3 * 256. Сдвиг убирает множитель 256, а деление на 3
// заменяется умножением: (n * 0xAAAB) >> 17 == n / 3 для всех n < 65536.
// Аргумент здесь всегда меньше 312 * 768, поэтому n < 936 — с запасом.
static inline int divBy768(int px)
{
    return int((unsigned(px) >> 8) * 0xAAABu >> 17);
}


void __not_in_flash_func(VectorRenderer::advanceTo)(uint64_t clock)
{
    const int bias = 189;

    if (clock <= m_curFrameClock)
        return;

    // Кадр длится m_ticksPerPixel * 768 * 312 = 33546240 тактов и заведомо
    // помещается в 32 бита, а планировщик не даёт CPU уйти дальше ближайшего
    // кадрового события. Прежнее 64-битное деление на каждую запись в
    // видеопамять было вызовом __aeabi_uldivmod; теперь это обычное 32-битное.
    const uint64_t delta = clock - m_curFrameClock;
    int toPixel = (delta < 0x100000000ull ? int(uint32_t(delta) / m_ticksPerPixel)
                                          : int(delta / m_ticksPerPixel)) + bias;

    if (toPixel <= m_curFramePixel) {
        if (toPixel < 40 * 768 || toPixel >= 296 * 768)
            m_lastColor = m_borderColor;
        return;
    }

    if (toPixel >= 312 * 768)
        toPixel = 312 * 768 - 1;

    if (!m_lineOffsetIsLatched && toPixel > 768 * 40 + 180) {
        m_lineOffsetIsLatched = true;
        m_latchedLineOffset = m_lineOffset;
    }

    const int firstLine = divBy768(m_curFramePixel);
    const int firstPixel = m_curFramePixel - firstLine * 768;
    const int lastLine = divBy768(toPixel);
    const int lastPixel = toPixel - lastLine * 768;
    m_curFramePixel = toPixel;
    renderLine(firstLine, firstPixel, firstLine == lastLine ? lastPixel : 768);
    for (int line = firstLine + 1; line < lastLine; line++)
        renderLine(line, 0, 768);
    if (firstLine != lastLine)
        renderLine(lastLine, 0, lastPixel);
}


void VectorRenderer::setBorderColor(uint8_t color)
{
    advanceTo(g_emulation->getCurClock() + m_ticksPerPixel * 48);
    m_borderColor = color;
}


void VectorRenderer::set512pxMode(bool mode512)
{
    advanceTo(g_emulation->getCurClock() + m_ticksPerPixel * 34);
    m_mode512px = mode512;
}


void VectorRenderer::setLineOffset(uint8_t lineOffset)
{
    advanceTo(g_emulation->getCurClock() + m_ticksPerPixel * 48);
    m_lineOffset = lineOffset;
}


void VectorRenderer::setPaletteColor(uint8_t color)
{
    advanceTo(g_emulation->getCurClock() + m_ticksPerPixel * 27);
    register uint32_t c = ((color & 0x7) << 21) | ((color & 0x7) << 18) | ((color & 0x6) << 15) |
                          ((color & 0x38) << 10) | ((color & 0x38) << 7) | ((color & 0x30) << 4) |
                          (color & 0xC0) | ((color & 0xC0) >> 2) | ((color & 0xC0) >> 4) | ((color & 0xC0) >> 6);
    m_colorPalette[m_lastColor] = RGB888(((c >> 16) & 0xFF), ((c >> 8) & 0xFF), (c & 0xFF));
    register uint8_t bw = c_bwMap[color];
    m_bwPalette[m_lastColor] = RGB888((bw << 16), (bw << 8), bw);
}


void __not_in_flash_func(VectorRenderer::vidMemWriteNotify)()
{
    advanceTo(g_emulation->getCurClock() + m_ticksPerPixel * 40);
}


void __not_in_flash_func(VectorRenderer::renderLine)(int nLine, int firstPx, int lastPx)
{
    // Render scan line #nLine
    // Vertical: 0-22 - invisible, 23-39 - border, 40-295 - visible, 296-311 - border) from firstPx to lastPx
    // Horizonlal: 0-123 - invisible, 124-180 - border, 181-692 - active area, 693-749 - border, 750-767 - invisible
    // (lastPx is non-inclusive)

    if (nLine < 24) {
        m_lastColor = m_borderColor;
        return;
    }

    uint8_t* linePtr = m_frameBuf + (nLine - 24) * 626;
    uint8_t* ptr;

    if (nLine < 40 || nLine >= 296) {
        // upper and lower borders
        if (firstPx < 124) firstPx = 124;
        ptr = linePtr + firstPx - 124;
        m_lastColor = m_borderColor;
        for (int px = firstPx; px < lastPx && px < 750; px++) {
            *ptr++ = m_palette[m_mode512px ? (px & 1 ? m_borderColor & 0x0c : m_borderColor & 0x03) : m_borderColor];
        }
    } else {
        // left border
        if (firstPx < 124) firstPx = 124;
        ptr = linePtr + firstPx - 124;
        for (int px = firstPx; px < lastPx && px < 181; px++)
            *ptr++ = m_palette[m_mode512px ? (px & 1 ? m_borderColor & 0x0c : m_borderColor & 0x03) : m_borderColor];

        // active area
        if (firstPx < 181) firstPx = 181;
        ptr = linePtr + firstPx - 124;
        const uint8_t rollOff = uint8_t(m_latchedLineOffset - nLine + 40);

        // Адрес байта в видеопамяти определяется битами 4..8 номера пикселя,
        // то есть одна четвёрка байт обслуживает 16 подряд идущих пикселей
        // (8 бит по два выходных пикселя на бит). Прежний цикл перечитывал эти
        // четыре байта на каждом пикселе — 2048 обращений к памяти на строку
        // вместо 128. Внутренняя часть цикла оставлена без изменений.
        {
            int px = firstPx - 181;
            int endPx = lastPx - 181;
            if (endPx > 693 - 181)
                endPx = 693 - 181;

            while (px < endPx) {
                const int offset = ((px & 0x1F0) << 4) | rollOff;
                const uint8_t bY = m_screenMemory[0x8000 + offset];
                const uint8_t bR = m_screenMemory[0xA000 + offset];
                const uint8_t bG = m_screenMemory[0xC000 + offset];
                const uint8_t bB = m_screenMemory[0xE000 + offset];

                int groupEnd = (px & ~0x0F) + 16;
                if (groupEnd > endPx)
                    groupEnd = endPx;

                for (; px < groupEnd; px++) {
                    const int dot = (px & 0x0E) >> 1;
                    uint8_t btY = bY << dot;
                    uint8_t btR = bR << dot;
                    uint8_t btG = bG << dot;
                    uint8_t btB = bB << dot;
                    int logBGcolor = ((btG & 0x80) >> 6) | ((btB & 0x80) >> 7);
                    int logYRcolor = ((btY & 0x80) >> 4) | ((btR & 0x80) >> 5);
                    m_lastColor = logYRcolor | logBGcolor;
                    if (m_mode512px) {
                        *ptr++ = m_palette[px & 1 ? m_lastColor & 0x0c : m_lastColor & 0x03];
                    } else {
                        *ptr++ = m_palette[m_lastColor];
                    }
                }
            }
        }

        // right border
        if (firstPx < 693) firstPx = 693;
        ptr = linePtr + firstPx - 124;
        for (int px = firstPx; px < lastPx && px < 750; px++)
            *ptr++ = m_palette[m_mode512px ? (px & 1 ? m_borderColor & 0x0c : m_borderColor & 0x03) : m_borderColor];

        if (lastPx < 182 || lastPx >= 694)
            m_lastColor = m_borderColor;
    }
}


void VectorRenderer::renderFrame()
{
    g_emulation->notifyFrameRendered();
    /**
    if (m_showBorder)
        memcpy(m_pixelData, m_frameBuf, m_sizeX * m_sizeY);
    else {
        uint8_t* ptr = m_frameBuf + 626 * 16 + 57;
        for (int i = 0; i < 256 * 512; i += 512) {
            memcpy(m_pixelData + i, ptr, 512);
            ptr += 626;
        }
    }
*/
    swapBuffers();
    prepareFrame();
    graphics_set_buffer(m_frameBuf, m_sizeX, m_sizeY);
}


void VectorRenderer::prepareFrame()
{
    if (!m_showBorder) {
        m_sizeX = 512;
        m_sizeY = 256;
    } else {
        m_sizeX = 626;
        m_sizeY = 288;
    }
}


void VectorRenderer::setColorMode(bool colorMode)
{
    m_colorMode = colorMode;
    m_palette = m_colorMode ? m_colorPalette : m_bwPalette;
}


void VectorRenderer::toggleColorMode()
{
    setColorMode(!m_colorMode);
}


void VectorRenderer::toggleCropping()
{
    m_showBorder = !m_showBorder;
}


void VectorRenderer::attachMemory(Ram* memory)
{
    m_screenMemory = memory->getDataPtr();
}

bool VectorFileLoader::chooseAndLoadFile(bool run)
{
    bool readOnly = false;
    string fileName = palOpenFileDialog("Open file", m_filter, false, &readOnly);
    if (fileName.empty())
        return true;
    if (!loadFile(fileName, run, readOnly)) {
        emuLog << "Error loading file: " << fileName << "\n";
        return false;
    }
    return true;
}


bool VectorFileLoader::loadFile(const std::string& fileName, bool run, bool readOnly)
{
    auto periodPos = fileName.find_last_of(".");
    string ext = periodPos != string::npos ? fileName.substr(periodPos) : fileName;
    transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".fdd") {
        if (!m_machine->assignDiskAFileName(fileName, readOnly))
            return false;

        Keyboard* keyboard = m_machine->getKeyboard();
        keyboard->disableKeysReset();
        m_machine->reset();
        keyboard->enableKeysReset();

        Cpu8080Compatible* cpu = m_machine->getCpu();
        VectorAddrSpace* addrSpace = m_machine->getAddrSpace();
        addrSpace->enableRom();
        g_emulation->exec((int64_t)cpu->getKDiv() * 25000000, true);

        if (run) {
            m_machine->reset();
            addrSpace->disableRom();
        }
        return true;
    }

    FIL f;
    if (f_open(&f, fileName.c_str(), FA_READ) != FR_OK)
        return false;
    bool basFile = false;
    uint16_t begAddr = 0x100;

    // check for "r0m"
    if (fileName.size() >= 4) {
        string ext = fileName.substr(fileName.size() - 4, 4);
        if (ext == ".r0m" || ext == ".R0M")
            begAddr = 0;
        else if (ext == ".bas" || ext == ".BAS" || ext == ".cas" || ext == ".CAS")
            basFile = true;
    }

    Cpu8080Compatible* cpu = m_machine->getCpu();
    VectorAddrSpace* as = m_machine->getAddrSpace();
    m_machine->reset();
    as->enableRom();
    cpu->disableHooks();
    g_emulation->exec(int64_t(cpu->getKDiv()) * m_skipTicks, true);
    cpu->enableHooks();

    for (unsigned i = 0; i < 0x100; i++)
        m_addrSpace->writeByte(i, 0x00);

    UINT br;
    if (!basFile)
        for (int i = 0; i < f_size(&f); ++i) {
            uint16_t addr = begAddr + i;
            uint8_t v;
            f_read(&f, &v, 1, &br);
            m_addrSpace->writeByte(addr, v);
            if (!run && (addr & 0xFF) == 0) {
                // paint block
                int block = addr >> 8;
                uint16_t blockAddr = 0xC018 + (block % 32) * 0x100 + (block / 32) * 0x18;
                for (int i = 0; i < 8; i++)
                    m_addrSpace->writeByte(blockAddr + i, 0x7E);
            }
        }
    else {
        int fileSize = f_size(&f);
        uint32_t v;
        f_read(&f, &v, 4, &br);
        // check for CAS
        if (fileSize >= 14 && v == 0xD3D3D3D3) {
            // Cas file
            while (fileSize) {
                uint8_t v;
                f_read(&f, &v, 1, &br);
                if (v == 0xE6) break;
                fileSize--;
            }

            if (fileSize < 7) {
                f_close(&f);
                return false;
            }
            f_lseek(&f, f_tell(&f) + 5);
            fileSize -= 5;
        }

        for (int i = 0; i < 0x39c6; i++)
            m_addrSpace->writeByte(0x0100 + i, as->readByte(0x08C5 + i));
        as->disableRom();
        cpu->setPC(begAddr);
        cpu->setIFF(false);
        cpu->disableHooks();
        g_emulation->exec(int64_t(cpu->getKDiv()) * 4000000, true);
        cpu->enableHooks();
        m_addrSpace->writeByte(0x4300, 0);

        uint16_t addr, nextAddr;
        addr = nextAddr = 0x4301;
        for(;;) {
            if (addr == nextAddr + 1) {
                f_lseek(&f, f_tell(&f) - 1);
                f_read(&f, &nextAddr, 2, &br); // TODO: ensure order of bytes
                f_lseek(&f, f_tell(&f) - 1);
            }
            uint8_t v;
            f_read(&f, &v, 1, &br);
            m_addrSpace->writeByte(addr++, v);
            fileSize--;
            if (nextAddr == 0 || fileSize == 0 || addr >= 0x7EFF)
                break;
        }
        m_addrSpace->writeByte(0x4045, addr & 0xFF);
        m_addrSpace->writeByte(0x4046, addr >> 8);
        m_addrSpace->writeByte(0x4047, addr & 0xFF);
        m_addrSpace->writeByte(0x4048, addr >> 8);
        m_addrSpace->writeByte(0x4049, addr & 0xFF);
        m_addrSpace->writeByte(0x404A, addr >> 8);
        f_close(&f);

        if (run) {
            m_addrSpace->writeByte(0x3DBF, 'R');
            m_addrSpace->writeByte(0x3DC0, 'U');
            m_addrSpace->writeByte(0x3DC1, 'N');
            m_addrSpace->writeByte(0x3DC2, '\r');
            m_addrSpace->writeByte(0x3DB8, 4);
            m_addrSpace->writeByte(0x3DB9, 4);
            m_addrSpace->writeByte(0x3DBA, 0);
        }

        return true;
    }


    f_close(&f);

    if (run) {
        as->disableRom();
        cpu->setPC(begAddr);
        cpu->setIFF(false);
    } else {
        as->enableRom();
        cpu->setPC(0xDF);
    }

    return true;
}


// Port 01
void VectorPpi8255Circuit::setPortC(uint8_t value)
{
    m_tapeSoundSource->setValue(value & 1);
    m_machine->tapeOut(value & 1);
}


// Port 02
void VectorPpi8255Circuit::setPortB(uint8_t value)
{
    // order is important!
    m_renderer->set512pxMode(value & 0x10);
    m_renderer->setBorderColor(value & 0x0f);
}


// Port 03
void VectorPpi8255Circuit::setPortA(uint8_t value)
{
    m_renderer->setLineOffset(value);
    m_kbd->setMatrixMask(value);
}


uint8_t VectorPpi8255Circuit::getPortB()
{
    return m_kbd->getMatrixData();
}


uint8_t VectorPpi8255Circuit::getPortC()
{
    return (m_kbd->getCtrlKeys() & 0xEF) | (g_emulation->getWavReader()->getCurValue() ? 0x10 : 0x00);
}



void VectorColorRegister::writeByte(int, uint8_t value)
{
    m_renderer->setPaletteColor(value);
}



VectorKeyboard::VectorKeyboard()
{
    VectorKeyboard::resetKeys();
}


void VectorKeyboard::resetKeys()
{
    for (int i = 0; i < 8; i++)
        m_keys[i] = 0;
    m_mask = 0;
    m_ctrlKeys = 0;
}


void __not_in_flash_func(VectorKeyboard::processKey)(EmuKey key, bool isPressed)
{
    if (key == EK_NONE)
        return;

    int i, j;

    // Основная матрица
    for (i = 0; i < 8; i++)
        for (j = 0; j < 8; j++)
            if (key == m_keyMatrix[i][j])
                goto found;

    // Управляющие клавиши
    for (i = 0; i < 8; i++)
        if (m_ctrlKeyMatrix[i] == key) {
            if (isPressed)
                m_ctrlKeys |= (1 << i);
            else
                m_ctrlKeys &= ~(1 << i);
        }
    return;

found:
    if (isPressed)
        m_keys[i] |= (1 << j);
    else
        m_keys[i] &= ~(1 << j);
}


uint8_t VectorKeyboard::getMatrixData()
{
    uint8_t val = 0;
    uint8_t mask = m_mask;
    for (int i=0; i<8; i++) {
        if (mask & 1)
            val |= m_keys[i];
        mask >>= 1;
    }
    return ~val;
}


bool VectorKbdLayout::processSpecialKeys(PalKeyCode keyCode)
{
    VectorAddrSpace* addrSpace = m_machine->getAddrSpace();

    if (keyCode == PK_F11) {
        Keyboard* keyboard = m_machine->getKeyboard();
        keyboard->disableKeysReset();
        m_machine->reset();
        keyboard->enableKeysReset();
        return true;
    } else if (keyCode == PK_F12) {
        Keyboard* keyboard = m_machine->getKeyboard();
        keyboard->disableKeysReset();
        m_machine->reset();
        keyboard->enableKeysReset();
        addrSpace->disableRom();
        return true;
    }
    return false;
}



void VectorRamDiskSelector::setEnabled(bool enabled)
{
    m_enabled = enabled;
    if (!m_enabled && m_vectorAddrSpace)
        m_vectorAddrSpace->ramDiskControl(m_diskNum, 0, false, 0, 0);
}


void VectorRamDiskSelector::writeByte(int, uint8_t value)
{
    if (m_enabled && m_vectorAddrSpace)
        m_vectorAddrSpace->ramDiskControl(m_diskNum, ((value & 0x40) >> 6) | ((value & 0x20) >> 4) | ((value & 0x20) >> 3) | ((value & 0x80) >> 4), value & 0x10, value & 0x3, (value >> 2) & 0x3);
}



void VectorFddControlRegister::writeByte(int, uint8_t value)
{
    m_fdc->setDrive(value & 1);
    m_fdc->setHead(((value & 0x4) >> 2) ^ 1);
}




void VectorPpi8255Circuit2::setPortA(uint8_t value)
{
    if (m_covox) {
        m_covox->setValue(value >> 1);
    }

    m_printerData = value;
}


void VectorPpi8255Circuit2::setPortC(uint8_t value)
{
    bool newStrobe = value & 0x10;
    if (m_printerStrobe && !newStrobe) {
        g_emulation->getPrnWriter()->printByte(m_printerData);
    }
    m_printerStrobe = newStrobe;
}



void __not_in_flash_func(VectorHddRegisters::writeByte)(int addr, uint8_t value)
{
    if (!m_enabled || !m_ataDrive)
        return;

    if (addr == 8) {
        m_highW = value;
        return;
    }

    addr &= 7;

    if (addr == 0)
        m_ataDrive->writeReg(addr, value | m_highW << 8);
    else
        m_ataDrive->writeReg(addr, value);
}


uint8_t __not_in_flash_func(VectorHddRegisters::readByte)(int addr)
{
    if (!m_enabled || !m_ataDrive)
        return 0xFF;

    if (addr == 8)
        return m_highR;

    addr &= 7;

    uint16_t read = m_ataDrive->readReg(addr);
    if (addr == 0)
        m_highR = read >> 8;
    return read & 0x00FF;
}

VectorCore::VectorCore()
{

    // Ram с внешним буфером: владения нет, деструктор ничего не освобождает
    m_ram = &s_devices.ram;
    m_ram->setMachine(this);

    m_rom = &s_devices.rom;
    m_rom->setMachine(this);

    // Ядро уже создано конструктором CpuSlot (см. s_devices) — так за ним
    // сохраняется прежняя позиция в порядке регистрации активных устройств.
    // Замена 8080 <-> Z80 на ходу выполняется через s_devices.cpuSlot.
    m_cpu = s_devices.cpuSlot.cpu;
    m_cpu->setMachine(this);
    m_cpu->setFrequency(m_cpuFrequency);
    m_cpu->setStartAddr(0x0000);

    m_addrSpace = &s_devices.addrSpace;
    m_addrSpace->setMachine(this);
    m_addrSpace->attachRam(m_ram);
    m_addrSpace->attachRom(m_rom);
    m_addrSpace->attachCpu(m_cpu);

    m_ioAddrSpace = &s_devices.ioAddrSpace;
    m_ioAddrSpace->setMachine(this);

    m_cpu->attachAddrSpace(m_addrSpace);
    m_cpu->attachIoAddrSpace(m_ioAddrSpace);

    m_renderer = &s_devices.renderer;
    m_renderer->setMachine(this);
    m_renderer->attachMemory(m_ram);
    m_renderer->setVisibleArea(true);

    m_addrSpace->attachCrtRenderer(m_renderer);

    m_cpu->attachCore(this);

    m_keyboard = &s_devices.keyboard;
    m_keyboard->setMachine(this);

    m_kbdLayout = &s_devices.kbdLayout;
    m_kbdLayout->setMachine(this);
    m_kbdLayout->setQwertyMode();

    m_kbdTapper = &s_devices.kbdTapper;
    m_kbdTapper->setMachine(this);
    m_kbdTapper->setPressTime(20);
    m_kbdTapper->setReleaseTime(20);
    m_kbdTapper->setCrDelay(100);

    m_ppiCircuit = &s_devices.ppiCircuit;
    m_ppiCircuit->setMachine(this);
    m_ppiCircuit->attachRenderer(m_renderer);
    m_ppiCircuit->attachKeyboard(m_keyboard);

    m_tapeSoundSource = &s_devices.tapeSoundSource;
    m_tapeSoundSource->setMachine(this);
    m_ppiCircuit->attachTapeSoundSource(m_tapeSoundSource);

    m_ppi = &s_devices.ppi;
    m_ppi->setMachine(this);
    m_ppi->setNoReset(true);
    m_ppi->attachPpi8255Circuit(m_ppiCircuit);

    m_ioAddrSpace->addRange(0x00, 0x03, m_ppi, 0, true);

    m_colorReg = &s_devices.colorReg;
    m_colorReg->setMachine(this);
    m_colorReg->attachRenderer(m_renderer);
    m_ioAddrSpace->addRange(0x0C, 0x0F, m_colorReg);

    m_covox = &s_devices.covox;
    m_covox->setMachine(this);
    m_covox->setNegative(true);

    m_covoxCircuit = &s_devices.covoxCircuit;
    m_covoxCircuit->setMachine(this);
    m_covoxCircuit->attachCovox(m_covox);

    m_ppi2 = &s_devices.ppi2;
    m_ppi2->setMachine(this);
    m_ppi2->attachPpi8255Circuit(m_covoxCircuit);

    m_ioAddrSpace->addRange(0x04, 0x07, m_ppi2, 0, true);

    m_pit = &s_devices.pit;
    m_pit->setMachine(this);
    m_pit->setFrequency(1500000);

    m_sndSource = &s_devices.sndSource;
    m_sndSource->setMachine(this);
    m_sndSource->attachPit(m_pit);
    m_sndSource->setNegative(true);

    m_ioAddrSpace->addRange(0x08, 0x0B, m_pit, 0, true);

    m_ay = &s_devices.ay;
    m_ay->setMachine(this);
    m_ay->setFrequency(1750000);
    m_ioAddrSpace->addRange(0x14, 0x15, m_ay);

    m_psgSoundSource = &s_devices.psgSoundSource;
    m_psgSoundSource->setMachine(this);
    m_psgSoundSource->attachPsg(m_ay);

    m_fdc = &s_devices.fdc;
    m_fdc->setMachine(this);

    m_ioAddrSpace->addRange(0x18, 0x1B, m_fdc, 0, true);

    m_fddReg = &s_devices.fddReg;
    m_fddReg->setMachine(this);
    m_fddReg->attachFdc1793(m_fdc);
    m_ioAddrSpace->addRange(0x1C, 0x1C, m_fddReg);

    m_ataDrive = &s_devices.ataDrive;
    m_ataDrive->setMachine(this);
    m_ataDrive->setVectorGeometry();

    m_hddRegisters = &s_devices.hddRegisters;
    m_hddRegisters->setMachine(this);
    m_hddRegisters->attachAtaDrive(m_ataDrive);
    m_ioAddrSpace->addRange(0x50, 0x5F, m_hddRegisters);

    m_diskA = &s_devices.diskA;
    m_diskA->setMachine(this);
    m_diskA->setLabel("A");
    m_diskA->setFilter("Образы дисков Вектора (*.fdd)|*.fdd;*.FDD|Все файлы (*.*)|*");
    m_fdc->attachFdImage(0, m_diskA);

    m_diskB = &s_devices.diskB;
    m_diskB->setMachine(this);
    m_diskB->setLabel("B");
    m_diskB->setFilter("Образы дисков Вектора (*.fdd)|*.fdd;*.FDD|Все файлы (*.*)|*");
    m_fdc->attachFdImage(1, m_diskB);

    m_hdd = &s_devices.hdd;
    m_hdd->setMachine(this);
    m_hdd->setLabel("HDD");
    m_hdd->setFilter("Образы HDD Вектора (*.hdd;*.img)|*.hdd;*.HDD;*.img;*.IMG|Все файлы (*.*)|*");
    m_ataDrive->assignDiskImage(m_hdd);

    m_loader = &s_devices.loader;
    m_loader->setMachine(this);
    m_loader->attachAddrSpace(m_ram);
    m_loader->setFilter("Файлы Вектора (*.rom;*.r0m;*.vec;*.cas;*.bas;*fdd)|*.rom;*.ROM;*.rom;*.R0M;*.vec;*.VEC;*.cas;*.CAS;*.bas;*.BAS;*.fdd;*.FDD|Все файлы (*.*)|*");

    // Единственный на прошивку. Регистрируется как активное устройство один
    // раз здесь и до открытия файла остаётся приостановленным.
    m_wavWriter = &s_devices.wavWriter;
    m_wavWriter->setMachine(this);

    m_tapeInFile = &s_devices.tapeInFile;
    m_tapeInFile->setMachine(this);
    m_tapeInFile->setMode("r");
    m_tapeInFile->setFilter("Файлы RK-совместимых ПК (*.rk?)|*.rk;*.rk?;*.RK;*.RK?|Файлы Бейсика (*.cas)|*.cas;*.CAS|Все файлы (*.*)|*");

    m_tapeOutFile = &s_devices.tapeOutFile;
    m_tapeOutFile->setMachine(this);
    m_tapeOutFile->setMode("w");
    m_tapeOutFile->setFilter(".rk|.cas");

    m_tapeInHookBas = &s_devices.tapeInHookBas;
    m_tapeInHookBas->setMachine(this);
    m_tapeInHookBas->setSignature("C5D50E0057DB");
    m_tapeInHookBas->setTapeRedirector(m_tapeInFile);
    m_cpu->addHook(m_tapeInHookBas);

    m_tapeOutHookBas = &s_devices.tapeOutHookBas;
    m_tapeOutHookBas->setMachine(this);
    m_tapeOutHookBas->setOutputRegisterA(true);
    m_tapeOutHookBas->setSignature("C5D5F5570E08");
    m_tapeOutHookBas->setTapeRedirector(m_tapeOutFile);
    m_cpu->addHook(m_tapeOutHookBas);

    m_closeFileHookBas = &s_devices.closeFileHookBas;
    m_closeFileHookBas->setMachine(this);
    m_closeFileHookBas->setSignature("C506003A203C");
    m_cpu->addHook(m_closeFileHookBas);

    m_tapeInHookMon = &s_devices.tapeInHookMon;
    m_tapeInHookMon->setMachine(this);
    m_tapeInHookMon->setSignature("C5D50E0057DB");
    m_tapeInHookMon->setTapeRedirector(m_tapeInFile);
    m_cpu->addHook(m_tapeInHookMon);

    m_tapeOutHookMon = &s_devices.tapeOutHookMon;
    m_tapeOutHookMon->setMachine(this);
    m_tapeOutHookMon->setOutputRegisterA(true);
    m_tapeOutHookMon->setSignature("C5D5F5573E02");
    m_tapeOutHookMon->setTapeRedirector(m_tapeOutFile);
    m_cpu->addHook(m_tapeOutHookMon);

    m_skipHookMon = &s_devices.skipHookMon;
    m_skipHookMon->setMachine(this);
    m_skipHookMon->setSignature("CD1097FB76F3");
    m_cpu->addHook(m_skipHookMon);

    m_closeFileHookMon = &s_devices.closeFileHookMon;
    m_closeFileHookMon->setMachine(this);
    m_closeFileHookMon->setSignature("3AFDFFE604CD");
    m_cpu->addHook(m_closeFileHookMon);

    m_tapeInHookEmuRk = &s_devices.tapeInHookEmuRk;
    m_tapeInHookEmuRk->setMachine(this);
    m_tapeInHookEmuRk->setSignature("F3C5D50E0057");
    m_tapeInHookEmuRk->setTapeRedirector(m_tapeInFile);
    m_cpu->addHook(m_tapeInHookEmuRk);

    m_tapeOutHookEmuRk = &s_devices.tapeOutHookEmuRk;
    m_tapeOutHookEmuRk->setMachine(this);
    m_tapeOutHookEmuRk->setOutputRegisterA(true);
    m_tapeOutHookEmuRk->setSignature("F3C5D5F51608");
    m_tapeOutHookEmuRk->setTapeRedirector(m_tapeOutFile);
    m_cpu->addHook(m_tapeOutHookEmuRk);

    m_closeFileHookEmuRk = &s_devices.closeFileHookEmuRk;
    m_closeFileHookEmuRk->setMachine(this);
    m_closeFileHookEmuRk->setSignature("FB3A61F6E604");
    m_cpu->addHook(m_closeFileHookEmuRk);

    m_ramDiskMem = &s_devices.ramDiskMem;
    m_ramDiskMem->setMachine(this);
    m_addrSpace->attachRamDisk(0, m_ramDiskMem);

    m_ramDisk = &s_devices.ramDisk;
    m_ramDisk->setMachine(this);
    m_ramDisk->setFilter("Файлы RAM-диска Вектора (*.edd)|*.edd;*.EDD|Все файлы (*.*)|*");
    m_ramDisk->attachPage(m_ramDiskMem);

    m_ramDiskSelector = &s_devices.ramDiskSelector;
    m_ramDiskSelector->setMachine(this);
    m_ramDiskSelector->attachVectorAddrSpace(m_addrSpace);
    m_ramDiskSelector->setDiskNum(0);
    m_ioAddrSpace->addRange(0x10, 0x10, m_ramDiskSelector);

    m_ramDiskMem2 = &s_devices.ramDiskMem2;
    m_ramDiskMem2->setMachine(this);
    m_addrSpace->attachRamDisk(1, m_ramDiskMem2);

    m_ramDisk2 = &s_devices.ramDisk2;
    m_ramDisk2->setMachine(this);
    m_ramDisk2->setLabel("EDD2");
    m_ramDisk2->setFilter("Файлы RAM-диска Вектора (*.edd)|*.edd;*.EDD|Все файлы (*.*)|*");
    m_ramDisk2->attachPage(m_ramDiskMem2);

    m_ramDiskSelector2 = &s_devices.ramDiskSelector2;
    m_ramDiskSelector2->setMachine(this);
    m_ramDiskSelector2->attachVectorAddrSpace(m_addrSpace);
    m_ramDiskSelector2->setDiskNum(1);
    m_ioAddrSpace->addRange(0x11, 0x11, m_ramDiskSelector2);

    m_tapeHooks[0] = m_tapeOutHookBas;
    m_tapeHooks[1] = m_tapeInHookBas;
    m_tapeHooks[2] = m_closeFileHookBas;
    m_tapeHooks[3] = m_tapeOutHookMon;
    m_tapeHooks[4] = m_tapeInHookMon;
    m_tapeHooks[5] = m_closeFileHookMon;
    m_tapeHooks[6] = m_tapeOutHookEmuRk;
    m_tapeHooks[7] = m_tapeInHookEmuRk;
    m_tapeHooks[8] = m_closeFileHookEmuRk;
    m_tapeHooks[9] = m_skipHookMon;

    init();

    reset();

}


bool VectorCore::getPsgEnabled() const
{
    return m_ay && m_ay->getEnabled();
}


void VectorCore::setPsgEnabled(bool enabled)
{
    if (m_ay)
        m_ay->setEnabled(enabled);
    if (m_psgSoundSource)
        m_psgSoundSource->setMuted(!enabled);
}


bool VectorCore::getPsgStereo() const
{
    return m_psgSoundSource && m_psgSoundSource->getStereo();
}


void VectorCore::setPsgStereo(bool stereo)
{
    if (m_psgSoundSource)
        m_psgSoundSource->setStereo(stereo);
}


bool VectorCore::getPsgAcbOrder() const
{
    return m_psgSoundSource && m_psgSoundSource->getAcbOrder();
}


void VectorCore::setPsgAcbOrder(bool acbOrder)
{
    if (m_psgSoundSource)
        m_psgSoundSource->setAcbOrder(acbOrder);
}


bool VectorCore::getHddEnabled() const
{
    return m_hddRegisters && m_hddRegisters->getEnabled();
}


void VectorCore::setHddEnabled(bool enabled)
{
    if (m_hddRegisters)
        m_hddRegisters->setEnabled(enabled);
}


void VectorCore::init()
{
    m_wavWriter->init();
    m_ram->init();
    m_rom->init();
    m_cpu->init();
    m_addrSpace->init();
    m_ioAddrSpace->init();
    m_renderer->init();
    m_keyboard->init();
    m_kbdLayout->init();
    m_kbdTapper->init();
    m_ppiCircuit->init();
    m_tapeSoundSource->init();
    m_ppi->init();
    m_colorReg->init();
    m_covox->init();
    m_covoxCircuit->init();
    m_ppi2->init();
    m_pit->init();
    m_sndSource->init();
    m_ay->init();
    m_psgSoundSource->init();
    m_fdc->init();
    m_fddReg->init();
    m_ataDrive->init();
    m_hddRegisters->init();
    m_diskA->init();
    m_diskB->init();
    m_hdd->init();
    m_loader->init();
    m_tapeInFile->init();
    m_tapeOutFile->init();
    m_tapeInHookBas->init();
    m_tapeOutHookBas->init();
    m_closeFileHookBas->init();
    m_tapeInHookMon->init();
    m_tapeOutHookMon->init();
    m_skipHookMon->init();
    m_closeFileHookMon->init();
    m_tapeInHookEmuRk->init();
    m_tapeOutHookEmuRk->init();
    m_closeFileHookEmuRk->init();
    m_ramDiskMem->init();
    m_ramDisk->init();
    m_ramDiskSelector->init();
    m_ramDiskMem2->init();
    m_ramDisk2->init();
    m_ramDiskSelector2->init();
}

void VectorCore::shutdown()
{
    m_ram->shutdown();
    m_rom->shutdown();
    m_cpu->shutdown();
    m_addrSpace->shutdown();
    m_ioAddrSpace->shutdown();
    m_renderer->shutdown();
    m_keyboard->shutdown();
    m_kbdLayout->shutdown();
    m_kbdTapper->shutdown();
    m_ppiCircuit->shutdown();
    m_tapeSoundSource->shutdown();
    m_ppi->shutdown();
    m_colorReg->shutdown();
    m_covox->shutdown();
    m_covoxCircuit->shutdown();
    m_ppi2->shutdown();
    m_pit->shutdown();
    m_sndSource->shutdown();
    m_ay->shutdown();
    m_psgSoundSource->shutdown();
    m_fdc->shutdown();
    m_fddReg->shutdown();
    m_ataDrive->shutdown();
    m_hddRegisters->shutdown();
    m_diskA->shutdown();
    m_diskB->shutdown();
    m_hdd->shutdown();
    m_loader->shutdown();
    m_tapeInFile->shutdown();
    m_tapeOutFile->shutdown();
    m_tapeInHookBas->shutdown();
    m_tapeOutHookBas->shutdown();
    m_closeFileHookBas->shutdown();
    m_tapeInHookMon->shutdown();
    m_tapeOutHookMon->shutdown();
    m_skipHookMon->shutdown();
    m_closeFileHookMon->shutdown();
    m_tapeInHookEmuRk->shutdown();
    m_tapeOutHookEmuRk->shutdown();
    m_closeFileHookEmuRk->shutdown();
    m_ramDiskMem->shutdown();
    m_ramDisk->shutdown();
    m_ramDiskSelector->shutdown();
    m_ramDiskMem2->shutdown();
    m_ramDisk2->shutdown();
    m_ramDiskSelector2->shutdown();
}

void VectorCore::coldReinitialize()
{
    init();
    reset();
}

VectorCpuType VectorCore::getCpuType() const
{
    return s_devices.cpuSlot.type;
}


void VectorCore::setCpuFrequency(unsigned frequency)
{
    if (frequency == m_cpuFrequency)
        return;

    m_cpuFrequency = frequency;
    m_cpu->setFrequency(m_cpuFrequency);
}


void VectorCore::setCpuType(VectorCpuType type)
{
    if (type == s_devices.cpuSlot.type)
        return;

    // Позицию в массиве активных устройств надо сохранить: планировщик
    // разрешает совпадение тактов по порядку регистрации, а совпадения CPU
    // и рендерера случаются на каждой границе кадра. Деструктор старого ядра
    // выкинет его из массива, конструктор нового допишет в конец — поэтому
    // индекс запоминается заранее и восстанавливается после.
    const int index = g_emulation->getActiveDeviceIndex(m_cpu);

    shutdown();
    s_devices.cpuSlot.replace(type);
    m_cpu = s_devices.cpuSlot.cpu;

    if (index >= 0)
        g_emulation->moveActiveDevice(m_cpu, index);

    // Вся обвязка заново, в том же порядке, что и в конструкторе
    m_cpu->setMachine(this);
    m_cpu->setFrequency(m_cpuFrequency);
    m_cpu->setStartAddr(0x0000);
    m_cpu->attachAddrSpace(m_addrSpace);
    m_cpu->attachIoAddrSpace(m_ioAddrSpace);
    m_cpu->attachCore(this);

    // attachCpu перестраивает карту страниц уже в новом объекте
    m_addrSpace->attachCpu(m_cpu);

    m_cpu->addHook(m_tapeInHookBas);
    m_cpu->addHook(m_tapeOutHookBas);
    m_cpu->addHook(m_closeFileHookBas);
    m_cpu->addHook(m_tapeInHookMon);
    m_cpu->addHook(m_tapeOutHookMon);
    m_cpu->addHook(m_skipHookMon);
    m_cpu->addHook(m_closeFileHookMon);
    m_cpu->addHook(m_tapeInHookEmuRk);
    m_cpu->addHook(m_tapeOutHookEmuRk);
    m_cpu->addHook(m_closeFileHookEmuRk);

    coldReinitialize();
}


void VectorCore::reset()
{
    m_ram->reset();
    m_rom->reset();
    m_cpu->reset();
    m_addrSpace->reset();
    m_ioAddrSpace->reset();
    m_renderer->reset();
    m_keyboard->reset();
    m_kbdLayout->reset();
    m_kbdTapper->reset();
    m_ppiCircuit->reset();
    m_tapeSoundSource->reset();
    m_ppi->reset();
    m_colorReg->reset();
    m_covox->reset();
    m_covoxCircuit->reset();
    m_ppi2->reset();
    m_pit->reset();
    m_sndSource->reset();
    m_ay->reset();
    m_psgSoundSource->reset();
    m_fdc->reset();
    m_fddReg->reset();
    m_ataDrive->reset();
    m_hddRegisters->reset();
    m_diskA->reset();
    m_diskB->reset();
    m_hdd->reset();
    m_loader->reset();
    m_tapeInFile->reset();
    m_tapeOutFile->reset();
    m_tapeInHookBas->reset();
    m_tapeOutHookBas->reset();
    m_closeFileHookBas->reset();
    m_tapeInHookMon->reset();
    m_tapeOutHookMon->reset();
    m_skipHookMon->reset();
    m_closeFileHookMon->reset();
    m_tapeInHookEmuRk->reset();
    m_tapeOutHookEmuRk->reset();
    m_closeFileHookEmuRk->reset();
    m_ramDiskMem->reset();
    m_ramDisk->reset();
    m_ramDiskSelector->reset();
    m_ramDiskMem2->reset();
    m_ramDisk2->reset();
    m_ramDiskSelector2->reset();
    m_intReq = false;
    m_intsEnabled = false;
}

VectorCore::~VectorCore()
{
    // Объекты размещены статически (см. s_devices) и живут всё время работы
    // прошивки: удалять нечего. Явное уничтожение потребуется только при
    // замене ядра CPU, и оно делается через CpuSlot.
    shutdown();
}



void VectorCore::sysReq(SysReq sr)
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
        case SR_DISKA:
            chooseFloppyImage(VectorFloppyDrive::A);
            break;
        case SR_DISKB:
            chooseFloppyImage(VectorFloppyDrive::B);
            break;
        case SR_HDD:
            chooseHddImage();
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
            setTapeHooksEnabled(!tapeHooksEnabled());
            break;
        default:
            break;
    }
    g_emulation->resetKeys();
}


bool VectorCore::tapeHooksEnabled() const
{
    return m_tapeHooks[0] && m_tapeHooks[0]->getEnabled();
}


void VectorCore::setTapeHooksEnabled(bool enabled)
{
    for (CpuHook* hook : m_tapeHooks)
        if (hook)
            hook->setEnabled(enabled);
}


void VectorCore::chooseTapeInput()
{
    if (m_tapeInFile)
        m_tapeInFile->openFile();
}


void VectorCore::chooseTapeOutput()
{
    if (m_tapeOutFile)
        m_tapeOutFile->openFile();
}


void VectorCore::ejectTapeFiles()
{
    if (m_tapeInFile)
        m_tapeInFile->ejectFile();
    if (m_tapeOutFile)
        m_tapeOutFile->ejectFile();
}


bool VectorCore::tapeFilePresent() const
{
    return (m_tapeInFile && m_tapeInFile->hasFile())
        || (m_tapeOutFile && m_tapeOutFile->hasFile());
}


std::string VectorCore::getTapeInputFileName() const
{
    return m_tapeInFile ? m_tapeInFile->getFileName() : std::string();
}


std::string VectorCore::getTapeOutputFileName() const
{
    return m_tapeOutFile ? m_tapeOutFile->getFileName() : std::string();
}


bool VectorCore::ramDiskEnabled(int diskNum) const
{
    const VectorRamDiskSelector* selector = diskNum == 0 ? m_ramDiskSelector : m_ramDiskSelector2;
    return selector && selector->getEnabled();
}


void VectorCore::setRamDiskEnabled(int diskNum, bool enabled)
{
    VectorRamDiskSelector* selector = diskNum == 0 ? m_ramDiskSelector : m_ramDiskSelector2;
    if (selector && selector->getEnabled() != enabled)
        selector->setEnabled(enabled);
}


void VectorCore::processKey(PalKeyCode keyCode, bool isPressed, unsigned unicodeKey)
{
    emuLog << "VectorCore::processKey " << to_string(keyCode) << " / " << isPressed << "\n";
    if (m_kbdLayout)
        m_kbdLayout->processKey(keyCode, isPressed, unicodeKey);
}


void VectorCore::resetKeys()
{
    if (m_kbdLayout)
        m_kbdLayout->resetKeys();
}


bool VectorCore::loadFile(const string& fileName, bool run)
{
    if (m_loader) {
        m_loader->loadFile(fileName, run);
        return true;
    }
    return false;
}


Cpu8080Compatible* VectorCore::getCpu()
{
    return m_cpu;
}

Keyboard* __not_in_flash_func(VectorCore::getKeyboard)()
{
    return m_keyboard;
}

namespace {
FdImage* selectFloppy(FdImage* diskA, FdImage* diskB, VectorFloppyDrive drive)
{
    return drive == VectorFloppyDrive::A ? diskA : diskB;
}
}

bool VectorCore::assignDiskAFileName(const std::string& fileName, bool readOnly)
{
    if (!m_diskA)
        return false;
    const std::string fullFileName = fileName.empty() ? std::string() : palMakeFullFileName(fileName);
    const bool duplicate = m_diskB && m_diskB->getImagePresent() && m_diskB->getFileName() == fullFileName;
    return m_diskA->assignFileName(fullFileName, readOnly || duplicate);
}

bool VectorCore::floppyImagePresent(VectorFloppyDrive drive) const
{
    FdImage* disk = selectFloppy(m_diskA, m_diskB, drive);
    return disk && disk->getImagePresent();
}

bool VectorCore::floppyImageReadOnly(VectorFloppyDrive drive) const
{
    FdImage* disk = selectFloppy(m_diskA, m_diskB, drive);
    return disk && disk->getImagePresent() && disk->getWriteProtectStatus();
}

bool VectorCore::floppyReadOnlyMode(VectorFloppyDrive drive) const
{
    return m_floppyReadOnlyMode[static_cast<int>(drive)];
}

bool VectorCore::canSetFloppyReadOnly(VectorFloppyDrive drive, bool readOnly) const
{
    FdImage* disk = selectFloppy(m_diskA, m_diskB, drive);
    if (!disk)
        return false;
    if (!disk->getImagePresent())
        return true;
    if (readOnly)
        return true;

    FdImage* other = selectFloppy(m_diskA, m_diskB,
        drive == VectorFloppyDrive::A ? VectorFloppyDrive::B : VectorFloppyDrive::A);
    return !other || !other->getImagePresent()
        || other->getFileName() != disk->getFileName()
        || other->getWriteProtectStatus();
}

void VectorCore::setFloppyReadOnly(VectorFloppyDrive drive, bool readOnly)
{
    FdImage* disk = selectFloppy(m_diskA, m_diskB, drive);
    if (!disk || !canSetFloppyReadOnly(drive, readOnly))
        return;

    m_floppyReadOnlyMode[static_cast<int>(drive)] = readOnly;
    if (disk->getImagePresent())
        disk->setWriteProtection(readOnly);
}

std::string VectorCore::getFloppyFileName(VectorFloppyDrive drive) const
{
    FdImage* disk = selectFloppy(m_diskA, m_diskB, drive);
    return disk ? disk->getFileName() : std::string();
}

void VectorCore::chooseFloppyImage(VectorFloppyDrive drive)
{
    FdImage* disk = selectFloppy(m_diskA, m_diskB, drive);
    FdImage* other = selectFloppy(m_diskA, m_diskB, drive == VectorFloppyDrive::A ? VectorFloppyDrive::B : VectorFloppyDrive::A);
    if (!disk)
        return;
    bool readOnly = m_floppyReadOnlyMode[static_cast<int>(drive)];
    const char* title = drive == VectorFloppyDrive::A
        ? "FDD-image file as A"
        : "FDD-image file as B";
    const std::string fileName = disk->chooseFileName(title, &readOnly);
    if (fileName.empty())
        return;
    m_floppyReadOnlyMode[static_cast<int>(drive)] = readOnly;
    const std::string fullFileName = palMakeFullFileName(fileName);
    const bool duplicate = other && other->getImagePresent() && other->getFileName() == fullFileName;
    disk->assignFileName(fullFileName, readOnly || duplicate);
}

void VectorCore::ejectFloppyImage(VectorFloppyDrive drive)
{
    FdImage* disk = selectFloppy(m_diskA, m_diskB, drive);
    if (disk)
        disk->assignFileName("");
}

bool VectorCore::hddImagePresent() const
{
    return m_hdd && m_hdd->getImagePresent();
}

std::string VectorCore::getHddFileName() const
{
    return m_hdd ? m_hdd->getFileName() : std::string();
}

void VectorCore::chooseHddImage()
{
    if (m_hdd)
        m_hdd->chooseFile("HDD-image file");
}

void VectorCore::ejectHddImage()
{
    if (m_hdd)
        m_hdd->assignFileName("");
}
