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
#include <cwctype>
#include <cstring>
#include <new>

#include "Globals.h"
#include "EmuCalls.h"
#include "Korvet.h"
#include "KorvetVideo.h"
#include "pico/korvet_font.bin.h"
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
#include "hway.h"
#include "pico/picoPal.h"
#include "PrnWriter.h"
#include "KbdTapper.h"
#include "Ppi8255.h"
#include "Pit8253.h"
#include "Pic8259.h"
#include "Pit8253Sound.h"
#include "TapeRedirector.h"
#include "WavWriter.h"
#include "RkTapeHooks.h"
#include "CloseFileHook.h"
#include "CpuHook.h"
#include "RamDisk.h"
#include "Version.h"
#include "pico/korvet.mapper.mem.h"
#include "ff.h"
#include "pico/picoMenu.h"
#include "graphics.h"

extern "C" FIL g_file;

using namespace std;


// ---------------------------------------------------------------------------
// Крупные буферы существуют ровно в одном экземпляре на всю прошивку, поэтому
// размещены статически: так они попадают в .bss и видны в отчёте линковщика
// (-Wl,--print-memory-usage), а не растворяются в куче, размер которой
// определяется тем, что осталось. После этого печатаемое линковщиком значение
// RAM и есть фактический статический бюджет, а разница до объёма SRAM —
// запас под стеки и оставшиеся динамические объекты.
//
//   кадровый буфер   521 * 288 = 150 048 байт
//   основное ОЗУ     64 * 1024 =  65 536 байт
//   итого                        215 584 байта
// ---------------------------------------------------------------------------

static const int c_frameBufWidth = 521;
static const int c_frameBufHeight = 288;
static const int c_frameBufSize = c_frameBufWidth * c_frameBufHeight;
#ifndef PICO_RP2040
alignas(4) static uint8_t s_frameBuffer[c_frameBufSize];
alignas(4) static uint8_t s_korvetLineBuffer[c_frameBufWidth];
#endif

static const int c_mainRamSize = 0x10000;
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
// Вся настройка по-прежнему делается в конструкторе KorvetCore, который
// вызывается из Emulation::init(), когда глобальные объекты уже готовы.
// ---------------------------------------------------------------------------

namespace {

class KorvetUnmappedPage : public AddressableDevice
{
    public:
        void writeByte(int, uint8_t) override {}
        uint8_t readByte(int) override {return 0xFF;}
};

class KorvetDevicesPage : public AddressableDevice
{
    public:
        uint8_t readByte(int addr) override {return m_devices.readByte(addr & 0x3F);}
        void writeByte(int addr, uint8_t value) override {m_devices.writeByte(addr & 0x3F, value);}
        void addRange(int first, int last, AddressableDevice* device, int devFirst = 0, bool invert = false)
        {
            m_devices.addRange(first, last, device, devFirst, invert);
        }
    private:
        AddrSpace m_devices;
};

struct Devices {
    Ram                      ram{s_mainRam, c_mainRamSize};
    Rom                      rom1{0x2000, "korvet/rom1.bin"};
    Rom                      rom2{0x2000, "korvet/rom2.bin"};
    Rom                      rom3{0x2000, "korvet/rom3.bin"};
    Cpu8080                  cpu;
    KorvetAddrSpace          addrSpace;
    KorvetAddrSpaceSelector  addrSpaceSelector;
    KorvetUnmappedPage       unmappedPage;
    KorvetTextAdapter        textAdapter;
    KorvetGraphicsAdapter    graphicsAdapter;
    KorvetColorRegister      korvetColorRegister;
    KorvetLutRegister        korvetLutRegister;
    KorvetFddMotor           fddMotor;
    KorvetVideoPpiCircuit    korvetVideoPpiCircuit;
    Ppi8255                  korvetVideoPpi;
    KorvetDevicesPage        korvetDevicesPage;
    AddrSpace                korvetRegistersPage;
    AddrSpace                ioAddrSpace;
    KorvetRenderer           renderer;
    KorvetKeyboard           keyboard;
    KorvetKeyboardRegisters  keyboardRegisters;
    KorvetKbdLayout          kbdLayout;
    KbdTapper                kbdTapper;
    KorvetPpi8255Circuit     ppiCircuit;
    GeneralSoundSource       tapeSoundSource;
    Ppi8255                  ppi;
    KorvetColorRegister      colorReg;
    Covox                    covox{7};
    KorvetPpi8255Circuit2    covoxCircuit;
    Ppi8255                  ppi2;
    Pit8253                  pit;
    Pic8259                  pic;
    KorvetPit8253SoundSource sndSource;
    Ppi8255                  ppi3;
    Fdc1793                  fdc;
    KorvetFddControlRegister fddReg;
    FdImage                  diskA{80, 2, 5, 1024};
    FdImage                  diskB{80, 2, 5, 1024};
    FdImage                  diskC{80, 2, 5, 1024};
    FdImage                  diskD{80, 2, 5, 1024};
    KorvetFileLoader         loader;
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
};

} // namespace

static Devices s_devices;



void KorvetAddrSpace::reset()
{
    // The Korvet mapper is dynamic, so CPU direct page maps must stay disabled.
    rebuildPageMap();
}


void __not_in_flash_func(KorvetAddrSpace::writeByte)(int addr, uint8_t value)
{
    const uint8_t page = korvet_mapper_mem[(m_addrSpaceSelector->getMemoryConfig() << 6) | (addr >> 8)];
    if (page == 5 && m_cpu)
        m_cpu->hrq(2);
    const int deviceAddr = page >= 1 && page <= 3 ? addr & 0x1FFF
                         : page == 6 ? addr & 0x00FF
                         : addr;
    m_pages[page]->writeByte(deviceAddr, value);
}


uint8_t __not_in_flash_func(KorvetAddrSpace::readByte)(int addr)
{
    const uint8_t page = korvet_mapper_mem[(m_addrSpaceSelector->getMemoryConfig() << 6) | (addr >> 8)];
    if (page == 5 && m_cpu)
        m_cpu->hrq(2);
    const int deviceAddr = page >= 1 && page <= 3 ? addr & 0x1FFF
                         : page == 6 ? addr & 0x00FF
                         : addr;
    return m_pages[page]->readByte(deviceAddr);
}


void KorvetAddrSpace::rebuildPageMap()
{
    if (!m_cpu)
        return;

    m_cpu->attachCrtRenderer(m_crtRenderer);
    m_cpu->clearPageMap();
}

namespace {

constexpr uint32_t c_addrSpaceSnapshotSection =
    makeSnapshotSectionId('A', 'D', 'D', 'R');

#pragma pack(push, 1)
struct KorvetAddrSpaceSnapshotStateV1 {
    uint32_t mainRamSize;
    uint8_t romEnabled;
};
#pragma pack(pop)

} // namespace

uint32_t KorvetAddrSpace::snapshotSectionId() const
{
    return c_addrSpaceSnapshotSection;
}

uint16_t KorvetAddrSpace::snapshotSectionVersion() const
{
    return 1;
}

bool KorvetAddrSpace::saveState(SnapshotWriter& writer) const
{
    if (!m_mainMemory)
        return false;

    KorvetAddrSpaceSnapshotStateV1 state{};
    state.mainRamSize = static_cast<uint32_t>(m_mainMemory->getSize());
    state.romEnabled = m_romEnabled ? 1 : 0;

    if (!writer.writeValue(state) ||
        !writer.write(m_mainMemory->getDataPtr(), state.mainRamSize))
        return false;

    return true;
}

bool KorvetAddrSpace::loadState(SnapshotReader& reader, uint16_t version)
{
    if (version != snapshotSectionVersion() || !m_mainMemory ||
        reader.remaining() < sizeof(KorvetAddrSpaceSnapshotStateV1))
        return false;

    KorvetAddrSpaceSnapshotStateV1 state{};
    if (!reader.readValue(state))
        return false;

    const uint32_t mainRamSize = static_cast<uint32_t>(m_mainMemory->getSize());
    const uint64_t expectedSize = static_cast<uint64_t>(mainRamSize);
    if (state.mainRamSize != mainRamSize ||
        reader.remaining() != expectedSize ||
        state.romEnabled > 1)
        return false;

    if (!reader.read(m_mainMemory->getDataPtr(), mainRamSize))
        return false;


    m_romEnabled = state.romEnabled != 0;
    return true;
}

void KorvetAddrSpace::postLoad()
{
    rebuildPageMap();
}

void KorvetCore::inte(bool isActive)
{
    m_intsEnabled = isActive;
    s_devices.pic.inte(isActive);
}


void KorvetCore::vrtc(bool isActive)
{
    if (m_curVrtc == isActive)
        return;

    m_curVrtc = isActive;
    if (m_videoPpiCircuit)
        m_videoPpiCircuit->setVbl(!isActive);

    // IRQ4 remains disconnected until its timing and polarity are verified.
}


void KorvetCore::int4(bool isActive)
{
    s_devices.pic.irq(4, isActive);
}


void KorvetCore::hrtc(bool isActive)
{
    if (m_curHrtc == isActive)
        return;

    m_curHrtc = isActive;
    if (isActive && m_pit)
        m_pit->getCounter(2)->operateForTicks(1);
}



KorvetRenderer::KorvetRenderer()
{
    m_sizeX = 512;
    m_sizeY = 256;
#ifndef PICO_RP2040
    m_pixelData = s_frameBuffer;
    m_frameBuf = m_pixelData;
#endif
    static const uint8_t levels[2][8] = {
        {0x00, 0x00, 0x00, 0x00, 0x80, 0x80, 0x80, 0x80},
        {0x40, 0x40, 0x40, 0x40, 0xFF, 0xFF, 0xFF, 0xFF}
    };
    for (int i = 0; i < 16; ++i) {
        const uint8_t bright = uint8_t(i >> 3);
        const uint8_t r = levels[bright][(i & 4) ? 4 : 0];
        const uint8_t g = levels[bright][(i & 2) ? 4 : 0];
        const uint8_t bl = levels[bright][(i & 1) ? 4 : 0];
        m_colorPalette[i] = RGB888(r, g, bl);
    }
    m_palette = m_colorPalette;
}


void KorvetRenderer::init()
{
    // Требует готового g_emulation, поэтому не может выполняться в конструкторе
    m_ticksPerPixel = g_emulation->getFrequency() / 12000000;

    m_curFramePixel = 0;
    m_curFrameClock = m_curClock;

    prepareFrame(); // prepare 1st frame dimensions

#if defined(PICO_RP2040) && \
    (defined(VGA_DRV) || defined(HDMI_DVI) || defined(SOFTTV))
    /*
     * The RP2040 VGA renderer consumes Korvet RAM directly on core1.
     * Publish the RAM pointer and initial video registers immediately:
     * waiting for the first emulated frame leaves the VGA side without a
     * valid source if the renderer has not yet received its scheduler event.
     */
    applyFrameBuffer();
#endif
}


KorvetRenderer::~KorvetRenderer()
{
}


void __not_in_flash_func(KorvetRenderer::operate)()
{
    m_machine->vrtc(false);
    advanceTo(m_curClock);
    m_curFrameClock = m_curClock;
    m_curFramePixel = 0;
    m_curClock += m_ticksPerPixel * 768 * 312;
    m_lineOffsetIsLatched = false;
    renderFrame();
    m_machine->int4(true);
    m_machine->int4(false);
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


void __not_in_flash_func(KorvetRenderer::advanceTo)(uint64_t clock)
{
    if (clock <= m_curFrameClock)
        return;

    // The Korvet frame is rendered once per vertical retrace from the
    // graphics and text adapters.  Keep only the beam position here so
    // legacy register-write notifications do not draw the old Vector frame.
    const uint64_t delta = clock - m_curFrameClock;
    int toPixel = delta < 0x100000000ull
                    ? int(uint32_t(delta) / m_ticksPerPixel)
                    : int(delta / m_ticksPerPixel);
    if (toPixel < 0)
        toPixel = 0;
    if (toPixel >= 312 * 768)
        toPixel = 312 * 768 - 1;

    const int oldLine = m_curFramePixel / 768;
    const int newLine = toPixel / 768;
    for (int line = oldLine + 1; line <= newLine; ++line) {
        (void)line;
        m_machine->hrtc(true);
        m_machine->hrtc(false);
    }

    m_curFramePixel = toPixel;
}

void KorvetRenderer::setBorderColor(uint8_t color)
{
    advanceTo(g_emulation->getCurClock() + m_ticksPerPixel * 48);
    m_borderColor = color;
}


void KorvetRenderer::set512pxMode(bool mode512)
{
    advanceTo(g_emulation->getCurClock() + m_ticksPerPixel * 34);
    m_mode512px = mode512;
}


void KorvetRenderer::setLineOffset(uint8_t lineOffset)
{
    advanceTo(g_emulation->getCurClock() + m_ticksPerPixel * 48);
    m_lineOffset = lineOffset;
}


void KorvetRenderer::setPaletteColor(uint8_t color)
{
    advanceTo(g_emulation->getCurClock() + m_ticksPerPixel * 27);
    register uint32_t c = ((color & 0x7) << 21) | ((color & 0x7) << 18) | ((color & 0x6) << 15) |
                          ((color & 0x38) << 10) | ((color & 0x38) << 7) | ((color & 0x30) << 4) |
                          (color & 0xC0) | ((color & 0xC0) >> 2) | ((color & 0xC0) >> 4) | ((color & 0xC0) >> 6);
    m_colorPalette[m_lastColor] = RGB888(((c >> 16) & 0xFF), ((c >> 8) & 0xFF), (c & 0xFF));
}


void __not_in_flash_func(KorvetRenderer::vidMemWriteNotify)()
{
    advanceTo(g_emulation->getCurClock() + m_ticksPerPixel * 40);
}


bool KorvetRenderer::isDisplayActive() const
{
    // Бит VBL в portA опрашивается ЦП асинхронно, а operate() тикает раз в
    // кадр — значит хранить состояние нельзя, его надо вычислять из текущего
    // такта относительно начала кадра. Активная развёртка соответствует
    // строкам 39..295 (в оригинале vrtc(false) на строке 39 и vrtc(true) на
    // строке 296, а setVbl = !vrtc).
    if (m_ticksPerPixel == 0)
        return true;
    const uint64_t now = g_emulation->getCurClock();
    const uint64_t elapsed = now > m_curFrameClock ? now - m_curFrameClock : 0;
    int line = int((elapsed / m_ticksPerPixel) / 768);
    if (line < 0)
        line = 0;
    else if (line > 311)
        line = 311;
    return line >= 39 && line < 296;
}


#ifndef PICO_RP2040
void __not_in_flash_func(KorvetRenderer::renderLine)(int nLine, int firstPx, int lastPx, uint8_t* linePtr)
{
    // Render scan line #nLine
    // Vertical: 0-22 - invisible, 23-39 - border, 40-295 - visible, 296-311 - border) from firstPx to lastPx
    // Horizonlal: 0-123 - invisible, 124-180 - border, 181-692 - active area, 693-749 - border, 750-767 - invisible
    // (lastPx is non-inclusive)

    if (nLine < 24) {
        m_lastColor = m_borderColor;
        return;
    }

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
#endif

#ifndef PICO_RP2040
void __not_in_flash_func(KorvetRenderer::renderKorvetFrame)()
{
    if (!m_graphicsAdapter || !m_textAdapter)
        return;

    constexpr int stride = c_frameBufWidth;
    constexpr int activeX = 4;
    constexpr int activeY = 17;
    constexpr int activeHeight = 256;

    const uint8_t border = m_palette[m_korvetLut[0]];
    const uint32_t borderWord = uint32_t(border) * 0x01010101u;
    const int page = m_displayPage % m_graphicsAdapter->getPageCount();
    const int pageOffset = page * 0x4000;
    const uint8_t* plane0 = m_graphicsAdapter->getPlane(0) + pageOffset;
    const uint8_t* plane1 = m_graphicsAdapter->getPlane(1) + pageOffset;
    const uint8_t* plane2 = m_graphicsAdapter->getPlane(2) + pageOffset;
    const uint8_t* symbols = m_textAdapter->getSymbols();
    const uint8_t* attrs = m_textAdapter->getAttrs();
    const uint8_t* font = korvet_font_bin + m_fontNumber * 4096;

    for (int frameY = 0; frameY < c_frameBufHeight; ++frameY) {
        uint32_t* lineWords = reinterpret_cast<uint32_t*>(s_korvetLineBuffer);
        int lineWordCount = stride >> 2;
        while (lineWordCount--)
            *lineWords++ = borderWord;
        uint8_t* lineTail = reinterpret_cast<uint8_t*>(lineWords);
        for (int i = stride & 3; i != 0; --i)
            *lineTail++ = border;

        if (frameY >= activeY && frameY < activeY + activeHeight) {
            const int y = frameY - activeY;
            uint8_t* dst = s_korvetLineBuffer + activeX;
            const int rowBase = y * 64;
            for (int byteNo = 0; byteNo < 64; ++byteNo) {
                const int videoAddr = rowBase + byteNo;
                uint8_t bt0 = plane0[videoAddr];
                uint8_t bt1 = plane1[videoAddr];
                uint8_t bt2 = plane2[videoAddr];
                const int symbolAddr = ((videoAddr >> 4) & 0x3C0) | (videoAddr & 0x3F);
                uint8_t bt3;
                if (!m_wideCharMode) {
                    bt3 = font[(unsigned(symbols[symbolAddr]) << 4) | ((videoAddr >> 6) & 0x0F)]
                          ^ attrs[symbolAddr];
                } else {
                    const int wideAddr = symbolAddr & ~1;
                    uint8_t chr = font[(unsigned(symbols[wideAddr]) << 4) | ((videoAddr >> 6) & 0x0F)]
                                  ^ attrs[wideAddr];
                    if (symbolAddr & 1)
                        chr <<= 4;
                    bt3 = (chr & 0x80 ? 0xC0 : 0) |
                          (chr & 0x40 ? 0x30 : 0) |
                          (chr & 0x20 ? 0x0C : 0) |
                          (chr & 0x10 ? 0x03 : 0);
                }

                for (int bit = 0; bit < 8; ++bit) {
                    const uint8_t colorIndex = uint8_t((bt0 >> 7) |
                                                       ((bt1 >> 6) & 2) |
                                                       ((bt2 >> 5) & 4) |
                                                       ((bt3 >> 4) & 8));
                    *dst++ = m_palette[m_korvetLut[colorIndex]];
                    bt0 <<= 1;
                    bt1 <<= 1;
                    bt2 <<= 1;
                    bt3 <<= 1;
                }
            }
        }

        const uint8_t* src = s_korvetLineBuffer;
        uint8_t* dst = m_frameBuf + frameY * stride;
        size_t count = stride;

        while (count && (reinterpret_cast<uintptr_t>(dst) & 3u)) {
            *dst++ = *src++;
            --count;
        }

        while (count >= 4) {
            uint32_t word;
            __builtin_memcpy(&word, src, sizeof(word));
            *reinterpret_cast<uint32_t*>(dst) = word;
            src += 4;
            dst += 4;
            count -= 4;
        }

        while (count--)
            *dst++ = *src++;
    }
}
#endif

void KorvetRenderer::renderFrame()
{
#ifndef PICO_RP2040
    renderKorvetFrame();
#endif
    g_emulation->notifyFrameRendered();
    swapBuffers();
    prepareFrame();
    applyFrameBuffer();
}


void KorvetRenderer::prepareFrame()
{
    if (!m_showBorder) {
        m_sizeX = 512;
        m_sizeY = 256;
    } else {
        m_sizeX = 521;
        m_sizeY = 288;
    }
}


void KorvetRenderer::applyFrameBuffer()
{
    // Кадровый буфер физически 521 x 288 (стр. шаг 521). В режиме
    // обрезки показываем только активную область m_sizeX x m_sizeY: буфер не
    // пересоздаём и не копируем, а передаём драйверу указатель на её
    // левый-верхний угол и физический шаг строки. Драйвер читает окно нужной
    // ширины с шагом 521, поэтому строки не разъезжаются.
#if defined(PICO_RP2040) && \
    (defined(VGA_DRV) || defined(HDMI_DVI) || defined(SOFTTV))
    // RP2040 has no room for the 521x288 frame buffer. Publish the actual
    // Korvet graphics/text sources; core1 composes each scan line directly.
    if (m_graphicsAdapter && m_textAdapter) {
        const int page =
            m_displayPage % m_graphicsAdapter->getPageCount();
        const int pageOffset = page * 0x4000;

        graphics_set_korvet_source(
            m_graphicsAdapter->getPlane(0) + pageOffset,
            m_graphicsAdapter->getPlane(1) + pageOffset,
            m_graphicsAdapter->getPlane(2) + pageOffset,
            m_textAdapter->getSymbols(),
            m_textAdapter->getAttrs(),
            korvet_font_bin + m_fontNumber * 4096,
            m_colorPalette,
            m_korvetLut,
            m_wideCharMode,
            m_showBorder);
    }
#else
    if (m_showBorder) {
#ifndef PICO_RP2040
        graphics_set_buffer(m_frameBuf, m_sizeX, m_sizeY);
        graphics_set_line_stride(c_frameBufWidth);
#endif
    } else {
#ifndef PICO_RP2040
        const int stride = c_frameBufWidth;
        const int originX = 4;
        const int originY = 17;
        graphics_set_buffer(m_frameBuf + stride * originY + originX,
                            m_sizeX, m_sizeY);
        graphics_set_line_stride(stride);
#endif
    }
#endif
}


void KorvetRenderer::setColorMode(bool colorMode)
{
    m_colorMode = colorMode;
    graphics_set_color_mode(colorMode);
}


void KorvetRenderer::toggleColorMode()
{
    setColorMode(!m_colorMode);
}


void KorvetRenderer::toggleCropping()
{
    m_showBorder = !m_showBorder;
}


void KorvetRenderer::attachMemory(Ram* memory)
{
    m_screenMemory = memory->getDataPtr();
}

namespace {

#pragma pack(push, 1)
struct KorvetRendererSnapshotStateV1 {
    uint64_t curClock;
    uint64_t curFrameClock;
    int32_t curFramePixel;
    int32_t lastColor;
    uint8_t lineOffset;
    uint8_t latchedLineOffset;
    uint8_t borderColor;
    uint8_t showBorder;
    uint8_t colorMode;
    uint8_t lineOffsetIsLatched;
    uint8_t mode512px;
    uint8_t paused;
    uint8_t colorPalette[16];
};
#pragma pack(pop)

constexpr uint32_t c_rendererSnapshotSection =
    makeSnapshotSectionId('V', 'I', 'D', ' ');

} // namespace


uint32_t KorvetRenderer::snapshotSectionId() const
{
    return c_rendererSnapshotSection;
}

uint16_t KorvetRenderer::snapshotSectionVersion() const
{
    return 1;
}

bool KorvetRenderer::saveState(SnapshotWriter& writer) const
{
    KorvetRendererSnapshotStateV1 state{};
    state.curClock = m_curClock;
    state.curFrameClock = m_curFrameClock;
    state.curFramePixel = m_curFramePixel;
    state.lastColor = m_lastColor;
    state.lineOffset = m_lineOffset;
    state.latchedLineOffset = m_latchedLineOffset;
    state.borderColor = m_borderColor;
    state.showBorder = m_showBorder ? 1 : 0;
    state.colorMode = m_colorMode ? 1 : 0;
    state.lineOffsetIsLatched = m_lineOffsetIsLatched ? 1 : 0;
    state.mode512px = m_mode512px ? 1 : 0;
    state.paused = m_isPaused ? 1 : 0;
    memcpy(state.colorPalette, m_colorPalette, sizeof(state.colorPalette));
    return writer.writeValue(state) &&
           writer.skip(16) &&
#ifndef PICO_RP2040
           writer.write(m_frameBuf, c_frameBufSize);
#else
           writer.skip(c_frameBufSize);
#endif
}

bool KorvetRenderer::loadState(SnapshotReader& reader, uint16_t version)
{
    if (version != snapshotSectionVersion() ||
        reader.remaining() != sizeof(KorvetRendererSnapshotStateV1) + 16 + c_frameBufSize)
        return false;

    KorvetRendererSnapshotStateV1 state{};
    if (!reader.readValue(state) ||
        state.curFramePixel < 0 || state.curFramePixel >= 312 * 768 ||
        state.lastColor < 0 || state.lastColor > 15 ||
        state.showBorder > 1 || state.colorMode > 1 ||
        state.lineOffsetIsLatched > 1 || state.mode512px > 1 ||
        state.paused > 1 || !reader.skip(16)
#ifndef PICO_RP2040
         || !reader.read(m_frameBuf, c_frameBufSize)
#endif
    ) {
        return false;
    }
    m_curClock = state.curClock;
    m_curFrameClock = state.curFrameClock;
    m_curFramePixel = state.curFramePixel;
    m_lastColor = state.lastColor;
    m_lineOffset = state.lineOffset;
    m_latchedLineOffset = state.latchedLineOffset;
    m_borderColor = state.borderColor;
    m_showBorder = state.showBorder != 0;
    m_colorMode = state.colorMode != 0;
    m_lineOffsetIsLatched = state.lineOffsetIsLatched != 0;
    m_mode512px = state.mode512px != 0;
    m_isPaused = state.paused != 0;
    memcpy(m_colorPalette, state.colorPalette, sizeof(m_colorPalette));
    return true;
}

void KorvetRenderer::postLoad()
{
    m_ticksPerPixel = g_emulation->getFrequency() / 12000000;
    prepareFrame();
    applyFrameBuffer();
}

bool KorvetFileLoader::chooseAndLoadFile(bool run)
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

bool KorvetFileLoader::loadFile(const std::string& fileName, bool run, bool readOnly)
{
    auto periodPos = fileName.find_last_of(".");
    string ext = periodPos != string::npos ? fileName.substr(periodPos) : fileName;
    transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".kdi") {
        if (!m_machine->assignDiskAFileName(fileName, readOnly))
            return false;

        Keyboard* keyboard = m_machine->getKeyboard();
        keyboard->disableKeysReset();
        m_machine->reset();
        keyboard->enableKeysReset();

        Cpu8080Compatible* cpu = m_machine->getCpu();
        KorvetAddrSpace* addrSpace = m_machine->getAddrSpace();
        if (run) {
            m_machine->reset();
        }
        return true;
    }

    if (f_open(&g_file, fileName.c_str(), FA_READ) != FR_OK)
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
    KorvetAddrSpace* as = m_machine->getAddrSpace();
    m_machine->reset();

    for (unsigned i = 0; i < 0x100; i++)
        m_addrSpace->writeByte(i, 0x00);

    UINT br;
    if (!basFile)
        for (int i = 0; i < f_size(&g_file); ++i) {
            uint16_t addr = begAddr + i;
            uint8_t v;
            f_read(&g_file, &v, 1, &br);
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
        int fileSize = f_size(&g_file);
        uint32_t v;
        f_read(&g_file, &v, 4, &br);
        // check for CAS
        if (fileSize >= 14 && v == 0xD3D3D3D3) {
            // Cas file
            while (fileSize) {
                uint8_t v;
                f_read(&g_file, &v, 1, &br);
                if (v == 0xE6) break;
                fileSize--;
            }

            if (fileSize < 7) {
                f_close(&g_file);
                return false;
            }
            f_lseek(&g_file, f_tell(&g_file) + 5);
            fileSize -= 5;
        }

        for (int i = 0; i < 0x39c6; i++)
            m_addrSpace->writeByte(0x0100 + i, as->readByte(0x08C5 + i));
        cpu->setPC(begAddr);
        cpu->setIFF(false);
        m_addrSpace->writeByte(0x4300, 0);

        uint16_t addr, nextAddr;
        addr = nextAddr = 0x4301;
        for(;;) {
            if (addr == nextAddr + 1) {
                f_lseek(&g_file, f_tell(&g_file) - 1);
                f_read(&g_file, &nextAddr, 2, &br); // TODO: ensure order of bytes
                f_lseek(&g_file, f_tell(&g_file) - 1);
            }
            uint8_t v;
            f_read(&g_file, &v, 1, &br);
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
        f_close(&g_file);

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


    f_close(&g_file);

    if (run) {
        cpu->setPC(begAddr);
        cpu->setIFF(false);
    } else {
        cpu->setPC(0xDF);
    }

    return true;
}


// Port 01
void KorvetPpi8255Circuit::setPortC(uint8_t value)
{
    m_tapeSoundSource->setValue(value & 1);
    m_machine->tapeOut(value & 1);
}


// Port 02
void KorvetPpi8255Circuit::setPortB(uint8_t value)
{
    // order is important!
    m_renderer->set512pxMode(value & 0x10);
    m_renderer->setBorderColor(value & 0x0f);
}


// Port 03
void KorvetPpi8255Circuit::setPortA(uint8_t value)
{
    m_renderer->setLineOffset(value);
    m_kbd->setMatrixMask(value);
}


uint8_t KorvetPpi8255Circuit::getPortB()
{
    return m_kbd->getMatrixData();
}


uint8_t KorvetPpi8255Circuit::getPortC()
{
    return (m_kbd->getCtrlKeys() & 0xEF) | (g_emulation->getWavReader()->getCurValue() ? 0x10 : 0x00);
}


void KorvetColorRegister::writeByte(int, uint8_t value)
{
    if (m_renderer)
        m_renderer->setPaletteColor(value);
    if (m_graphicsAdapter)
        m_graphicsAdapter->setColorRegisterValue(value);
}



KorvetKeyboard::KorvetKeyboard()
{
    resetKeys();
}


void KorvetKeyboard::resetKeys()
{
    for (int i = 0; i < 8; ++i)
        m_keys1[i] = 0;
    for (int i = 0; i < 3; ++i)
        m_keys2[i] = 0;
    m_mask1 = 0;
    m_mask2 = 0;
}


void __not_in_flash_func(KorvetKeyboard::processKey)(EmuKey key, bool isPressed)
{
    if (key == EK_NONE)
        return;

    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j)
            if (key == m_keyMatrix1[i][j]) {
                if (isPressed)
                    m_keys1[i] |= 1u << j;
                else
                    m_keys1[i] &= ~(1u << j);
                return;
            }

    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 8; ++j)
            if (key == m_keyMatrix2[i][j]) {
                if (isPressed)
                    m_keys2[i] |= 1u << j;
                else
                    m_keys2[i] &= ~(1u << j);
                return;
            }
}


uint8_t KorvetKeyboard::getMatrix1Data()
{
    uint8_t value = 0;
    uint8_t mask = m_mask1;
    for (int i = 0; i < 8; ++i) {
        if (mask & 1)
            value |= m_keys1[i];
        mask >>= 1;
    }
    return value;
}


uint8_t KorvetKeyboard::getMatrix2Data()
{
    uint8_t value = 0;
    uint8_t mask = m_mask2;
    for (int i = 0; i < 3; ++i) {
        if (mask & 1)
            value |= m_keys2[i];
        mask >>= 1;
    }
    return value;
}


uint8_t KorvetKeyboardRegisters::readByte(int addr)
{
    if (!m_keyboard)
        return 0;

    addr &= 0x1FF;
    if (addr < 0x100) {
        m_keyboard->setMatrix1Mask(static_cast<uint8_t>(addr));
        return m_keyboard->getMatrix1Data();
    }

    m_keyboard->setMatrix2Mask(static_cast<uint8_t>(addr & 7));
    return m_keyboard->getMatrix2Data();
}


unsigned KorvetKbdLayout::translateSmartUnicodeKey(PalKeyCode keyCode) const
{
    const bool shift = isShiftPressed();

    if (keyCode >= PK_A && keyCode <= PK_Z) {
        static const wchar_t rusLetters[] = {
            L'Ф', L'И', L'С', L'В', L'У', L'А', L'П', L'Р', L'Ш', L'О', L'Л', L'Д', L'Ь',
            L'Т', L'Щ', L'З', L'Й', L'К', L'Ы', L'Е', L'Г', L'М', L'Ц', L'Ч', L'Н', L'Я'
        };
        wchar_t ch = m_smartRus ? rusLetters[keyCode - PK_A] : L'A' + (keyCode - PK_A);
        if (shift)
            ch = std::towlower(ch);
        return static_cast<unsigned>(ch);
    }

    switch (keyCode) {
    case PK_KP_PLUS: return L'+';
    case PK_1: return shift ? L'!' : L'1';
    case PK_2: return shift ? (m_smartRus ? L'"' : L'@') : L'2';
    case PK_3: return shift ? L'#' : L'3';
    case PK_4: return shift ? L'$' : L'4';
    case PK_5: return shift ? L'%' : L'5';
    case PK_6: return shift ? L'^' : L'6';
    case PK_7: return shift ? (m_smartRus ? L'?' : L'\'') : L'7';
    case PK_8: return shift ? L'*' : L'8';
    case PK_9: return shift ? L'(' : L'9';
    case PK_0: return shift ? L')' : L'0';
    default: break;
    }

    if (!m_smartRus) {
        switch (keyCode) {
        case PK_MINUS: return shift ? L'_' : L'-';
        case PK_EQU: return shift ? L'+' : L'=';
        case PK_LBRACKET: return L'[';
        case PK_RBRACKET: return L']';
        case PK_BSLASH: return shift ? L'|' : L'\\';
        case PK_SEMICOLON: return shift ? L':' : L';';
        case PK_APOSTROPHE: return shift ? L'"' : L'\'';
        case PK_COMMA: return shift ? L'<' : L',';
        case PK_PERIOD: return shift ? L'>' : L'.';
        case PK_SLASH: return shift ? L'?' : L'/';
        default: return 0;
        }
    }

    switch (keyCode) {
    case PK_MINUS: return shift ? L'_' : L'-';
    case PK_EQU: return shift ? L'+' : L'=';
    case PK_LBRACKET: return shift ? L'Х' : L'х';
    case PK_RBRACKET: return L']';
    case PK_BSLASH: return shift ? L'Э' : L'э';
    case PK_SEMICOLON: return shift ? L'+' : L'ж';
    case PK_APOSTROPHE: return shift ? L'"' : L'э';
    case PK_COMMA: return shift ? L'<' : L'б';
    case PK_PERIOD: return shift ? L'>' : L'ю';
    case PK_SLASH: return shift ? L',' : L'.';
    default: return 0;
    }
}


EmuKey KorvetKbdLayout::translateSmartKey(PalKeyCode keyCode)
{
    // In the Smart table each PC key is paired with the character printed
    // on the same key. The Korvet matrix key must therefore be selected
    // by the Latin character, not by the JCUKEN key position.
    switch (keyCode) {
    case PK_RBRACKET: return EK_RBRACKET;
    case PK_BSLASH: return EK_BKSLASH;
    case PK_SEMICOLON: return EK_SEMICOLON;
    case PK_APOSTROPHE: return EK_CARET;
    case PK_COMMA: return EK_COMMA;
    case PK_PERIOD: return EK_PERIOD;
    case PK_SLASH: return EK_SLASH;
    case PK_TILDE: return EK_NONE;
    default: break;
    }

    EmuKey key = translateCommonKeysQwerty(keyCode);
    return key != EK_NONE ? key : translateCommonKeys(keyCode);
}


EmuKey KorvetKbdLayout::translateKey(PalKeyCode keyCode)
{
    switch (keyCode) {
    case PK_INS: return EK_INS;
    case PK_HOME: return EK_SHOME;
    case PK_DEL: return EK_DEL;
    case PK_PGUP: return EK_LANG;
    case PK_KP_0: return EK_PHOME;
    case PK_DOWN: return m_downAsNumpad5 ? EK_MENU : EK_DOWN;
    default: break;
    }

    EmuKey key = m_mode == KLM_SMART ? translateSmartKey(keyCode) : translateCommonKeys(keyCode);
    if (key != EK_NONE)
        return key;

    switch (keyCode) {
    case PK_KP_1: return EK_HOME;
    case PK_LCTRL:
    case PK_RCTRL: return EK_CTRL;
    case PK_F6: return EK_UNDSCR;
    case PK_F10: return EK_GRAPH;
    case PK_F8:
    case PK_MENU: return EK_FIX;
    case PK_F12: return EK_STOP;
    case PK_F9: return EK_SEL;
    case PK_KP_MUL: return EK_INS;
    case PK_KP_DIV: return EK_DEL;
    case PK_KP_7: return EK_SHOME;
    case PK_KP_9: return EK_SEND;
    case PK_KP_3: return EK_END;
    case PK_KP_PERIOD: return EK_PEND;
    case PK_KP_5: return EK_MENU;
    case PK_KP_MINUS: return EK_CLEAR;
    default: return EK_NONE;
    }
}


EmuKey KorvetKbdLayout::translateUnicodeKey(unsigned unicodeKey, PalKeyCode keyCode, bool& shift, bool& lang)
{
    if (m_mode == KLM_SMART && unicodeKey == 0)
        unicodeKey = translateSmartUnicodeKey(keyCode);

    if (keyCode == PK_KP_MUL || keyCode == PK_KP_DIV || keyCode == PK_KP_MINUS)
        return EK_NONE;

    if (unicodeKey >= L'A' && unicodeKey <= L'Z')
        unicodeKey += 0x20;
    else if (unicodeKey >= L'a' && unicodeKey <= L'z')
        unicodeKey -= 0x20;
    else if (unicodeKey >= L'А' && unicodeKey <= L'Я')
        unicodeKey += 0x20;
    else if (unicodeKey >= L'а' && unicodeKey <= L'я')
        unicodeKey -= 0x20;

    EmuKey key = translateCommonUnicodeKeys(unicodeKey, shift, lang);
    if (unicodeKey == L'@')
        shift = false;
    else if (unicodeKey == L'\'') {
        key = EK_7;
        shift = true;
        lang = false;
    }
    else if (unicodeKey == L'`') {
        key = EK_AT;
        shift = true;
        lang = false;
    } else if (unicodeKey == L'_') {
        key = EK_UNDSCR;
        shift = true;
        lang = false;
    }
    return key;
}


bool KorvetKbdLayout::processSpecialKeys(PalKeyCode keyCode)
{
    if (m_mode == KLM_SMART && keyCode == PK_PGUP)
        m_smartRus = !m_smartRus;

    if (keyCode != PK_F11)
        return false;

    Keyboard* keyboard = m_machine->getKeyboard();
    keyboard->disableKeysReset();
    m_machine->reset();
    keyboard->enableKeysReset();
    return true;
}


void KorvetFddControlRegister::writeByte(int, uint8_t value)
{
    m_fdc->setDrive(value & 1);
    m_fdc->setHead(((value & 0x4) >> 2) ^ 1);
}




void KorvetPit8253SoundSource::tuneupPit()
{
    if (m_pit)
        m_pit->getCounter(2)->setExtClockMode(true);
}


void KorvetPit8253SoundSource::updateStats()
{
    if (!m_pit)
        return;

    m_pit->getCounter(0)->updateState();
    if (m_gate)
        m_sumValue += m_pit->getCounter(0)->getAvgOut();
}


int __not_in_flash_func(KorvetPit8253SoundSource::calcValue)()
{
    if (!m_pit)
        return 0;

    updateStats();
    const int result = m_sumValue;
    m_sumValue = 0;

    for (int i = 0; i < 3; ++i)
        m_pit->getCounter(i)->resetStats();

    return result * m_ampFactor;
}


void KorvetPit8253SoundSource::setGate(bool gate)
{
    updateStats();
    m_gate = gate;
}


void KorvetPpi8255Circuit2::setPortA(uint8_t value)
{
    m_printerData = value;
}


void KorvetPpi8255Circuit2::setPortC(uint8_t value)
{
    static const int covoxValues[4] = {-7, 0, 0, 7};
    if (m_covox)
        m_covox->setValue(covoxValues[value & 3]);
    if (m_pitSoundSource)
        m_pitSoundSource->setGate((value & 0x08) != 0);

    const bool newStrobe = (value & 0x20) != 0;
    if (!m_printerStrobe && newStrobe)
        g_emulation->getPrnWriter()->printByte(uint8_t(~m_printerData));
    m_printerStrobe = newStrobe;
}


KorvetCore::KorvetCore()
{

    // Ram с внешним буфером: владения нет, деструктор ничего не освобождает
    m_ram = &s_devices.ram;
    m_ram->setMachine(this);

    m_rom = &s_devices.rom1;
    m_rom2 = &s_devices.rom2;
    m_rom3 = &s_devices.rom3;
    m_rom->setMachine(this);

    // Корвет использует фиксированное ядро 8080.
    m_cpu = &s_devices.cpu;
    m_cpu->setMachine(this);
    m_cpu->setFrequency(m_cpuFrequency);
    m_cpu->setStartAddr(0x0000);

    m_addrSpace = &s_devices.addrSpace;
    m_addrSpace->setMachine(this);
    m_addrSpace->attachRam(m_ram);
    m_addrSpace->attachRom(m_rom);
    m_addrSpace->attachCpu(m_cpu);

    m_addrSpaceSelector = &s_devices.addrSpaceSelector;
    m_addrSpaceSelector->setMachine(this);
    m_addrSpace->attachSelector(m_addrSpaceSelector);
    m_addrSpace->setPage(0, m_ram);
    m_addrSpace->setPage(1, m_rom);
    m_addrSpace->setPage(2, m_rom2);
    m_addrSpace->setPage(3, m_rom3);
    m_addrSpace->setPage(4, &s_devices.keyboardRegisters);
    m_addrSpace->setPage(5, &s_devices.korvetDevicesPage);
    m_addrSpace->setPage(6, &s_devices.korvetRegistersPage);
    m_addrSpace->setPage(7, &s_devices.textAdapter);
    m_addrSpace->setPage(8, &s_devices.graphicsAdapter);

    s_devices.korvetRegistersPage.addRange(0x7F, 0x7F, m_addrSpaceSelector);

    m_ioAddrSpace = &s_devices.ioAddrSpace;
    m_ioAddrSpace->setMachine(this);

    s_devices.korvetColorRegister.setMachine(this);
    s_devices.korvetColorRegister.attachGraphicsAdapter(&s_devices.graphicsAdapter);
    s_devices.renderer.attachGraphicsAdapter(&s_devices.graphicsAdapter);
    s_devices.renderer.attachTextAdapter(&s_devices.textAdapter);
    s_devices.korvetLutRegister.attachRenderer(&s_devices.renderer);
    s_devices.korvetVideoPpiCircuit.attachRenderer(&s_devices.renderer);
    s_devices.korvetRegistersPage.addRange(0xBF, 0xBF, &s_devices.korvetColorRegister);

    s_devices.korvetLutRegister.setMachine(this);
    s_devices.korvetRegistersPage.addRange(0xFB, 0xFB, &s_devices.korvetLutRegister);

    m_videoPpiCircuit = &s_devices.korvetVideoPpiCircuit;
    m_videoPpiCircuit->setMachine(this);
    m_videoPpiCircuit->attachGraphicsAdapter(&s_devices.graphicsAdapter);
    m_videoPpiCircuit->attachTextAdapter(&s_devices.textAdapter);

    s_devices.korvetVideoPpi.setMachine(this);
    s_devices.korvetVideoPpi.setSnapshotIndex(3);
    s_devices.korvetVideoPpi.attachPpi8255Circuit(m_videoPpiCircuit);
    s_devices.korvetDevicesPage.addRange(0x38, 0x3B, &s_devices.korvetVideoPpi);

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
    s_devices.keyboardRegisters.setMachine(this);
    s_devices.keyboardRegisters.attachKeyboard(m_keyboard);

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
    m_ppi->setSnapshotIndex(0);
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
    m_covoxCircuit->attachPitSoundSource(&s_devices.sndSource);

    m_ppi2 = &s_devices.ppi2;
    m_ppi2->setMachine(this);
    m_ppi2->setSnapshotIndex(1);
    m_ppi2->attachPpi8255Circuit(m_covoxCircuit);

    s_devices.korvetDevicesPage.addRange(0x30, 0x33, m_ppi2);

    // ppi3 — штатная ВВ55 Корвета на портах 0x08–0x0B. POST проверяет исправность
    // шины записью/чтением её порта A, поэтому PPI должна присутствовать на шине даже
    // без AY (PSG на этой ВВ55 — необязательная периферия и здесь не подключается).
    s_devices.ppi3.setMachine(this);
    s_devices.ppi3.setSnapshotIndex(2);
    s_devices.korvetDevicesPage.addRange(0x08, 0x0B, &s_devices.ppi3);

    m_pit = &s_devices.pit;
    m_pit->setMachine(this);
    m_pit->setFrequency(2000000);
    m_pit->setOutCallback(
        [](void* context, int, bool state)
        {
            static_cast<Pic8259*>(context)->irq(5, state);
        },
        &s_devices.pic);

    m_sndSource = &s_devices.sndSource;
    m_sndSource->setMachine(this);
    m_sndSource->attachPit(m_pit);
    m_sndSource->setNegative(true);

    s_devices.korvetDevicesPage.addRange(0x00, 0x03, m_pit);

    s_devices.pic.setMachine(this);
    s_devices.pic.attachCpu(m_cpu);
    s_devices.fddMotor.setMachine(this);
    s_devices.fddMotor.attachPic(&s_devices.pic);
    s_devices.korvetDevicesPage.addRange(0x28, 0x29, &s_devices.pic);

    m_fdc = &s_devices.fdc;
    m_fdc->setMachine(this);
    s_devices.korvetDevicesPage.addRange(0x18, 0x1B, m_fdc);
    m_videoPpiCircuit->attachFdc1793(m_fdc);
    m_videoPpiCircuit->attachFddMotor(&s_devices.fddMotor);

    m_fddReg = &s_devices.fddReg;
    m_fddReg->setMachine(this);
    m_fddReg->attachFdc1793(m_fdc);

    m_diskA = &s_devices.diskA;
    m_diskA->setMachine(this);
    m_diskA->setSnapshotIndex(0);
    m_diskA->setLabel("A");
    m_diskA->setFilter("Образы дисков Корвета (*.fdd)|*.fdd;*.FDD|Все файлы (*.*)|*");
    m_fdc->attachFdImage(0, m_diskA);

    m_diskB = &s_devices.diskB;
    m_diskB->setMachine(this);
    m_diskB->setSnapshotIndex(1);
    m_diskB->setLabel("B");
    m_diskB->setFilter("Образы дисков Корвета (*.fdd)|*.fdd;*.FDD|Все файлы (*.*)|*");
    m_fdc->attachFdImage(1, m_diskB);

    m_diskC = &s_devices.diskC;
    m_diskC->setMachine(this);
    m_diskC->setSnapshotIndex(3);
    m_diskC->setLabel("C");
    m_diskC->setFilter("Образы дисков Корвета (*.fdd)|*.fdd;*.FDD|Все файлы (*.*)|*");
    m_fdc->attachFdImage(2, m_diskC);

    m_diskD = &s_devices.diskD;
    m_diskD->setMachine(this);
    m_diskD->setSnapshotIndex(4);
    m_diskD->setLabel("D");
    m_diskD->setFilter("Образы дисков Корвета (*.fdd)|*.fdd;*.FDD|Все файлы (*.*)|*");
    m_fdc->attachFdImage(3, m_diskD);

    m_loader = &s_devices.loader;
    m_loader->setMachine(this);
    m_loader->attachAddrSpace(m_ram);
    m_loader->setFilter("Файлы Корвета (*.rom;*.r0m;*.vec;*.cas;*.bas;*.kdi)|*.rom;*.ROM;*.r0m;*.R0M;*.vec;*.VEC;*.cas;*.CAS;*.bas;*.BAS;*.kdi;*.KDI|Все файлы (*.*)|*");

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


void KorvetCore::init()
{
    m_wavWriter->init();
    m_ram->init();
    m_rom->init();
    m_rom2->init();
    m_rom3->init();
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
    m_fdc->init();
    m_fddReg->init();
    m_diskA->init();
    m_diskB->init();
    m_diskC->init();
    m_diskD->init();
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
}

void KorvetCore::shutdown()
{
    m_ram->shutdown();
    m_rom->shutdown();
    m_rom2->shutdown();
    m_rom3->shutdown();
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
    m_fdc->shutdown();
    m_fddReg->shutdown();
    m_diskA->shutdown();
    m_diskB->shutdown();
    m_diskC->shutdown();
    m_diskD->shutdown();
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
}

void KorvetCore::coldReinitialize()
{
    init();
    reset();
}

KorvetCpuType KorvetCore::getCpuType() const
{
    return VECTOR_CPU_8080;
}


bool KorvetCore::getColorMode() const
{
    return m_renderer ? m_renderer->getColorMode() : true;
}


bool KorvetCore::getCroppedToVisible() const
{
    return m_renderer && m_renderer->getCroppedToVisible();
}

uint8_t KorvetCore::getVideoDisplayPage() const
{
    return m_videoPpiCircuit ? m_videoPpiCircuit->getDisplayPage() : 0;
}

uint8_t KorvetCore::getVideoFontNumber() const
{
    return m_videoPpiCircuit ? m_videoPpiCircuit->getFontNumber() : 0;
}

bool KorvetCore::getVideoWideCharMode() const
{
    return m_videoPpiCircuit && m_videoPpiCircuit->getWideCharMode();
}

uint16_t KorvetCore::getCpuPc() const
{
    return m_cpu ? m_cpu->getPC() : 0;
}

uint16_t KorvetCore::getCpuAf() const
{
    return m_cpu ? m_cpu->getAF() : 0;
}

uint16_t KorvetCore::getCpuBc() const
{
    return m_cpu ? m_cpu->getBC() : 0;
}

uint16_t KorvetCore::getCpuDe() const
{
    return m_cpu ? m_cpu->getDE() : 0;
}

uint16_t KorvetCore::getCpuHl() const
{
    return m_cpu ? m_cpu->getHL() : 0;
}

uint16_t KorvetCore::getCpuSp() const
{
    return m_cpu ? m_cpu->getSP() : 0;
}

uint8_t KorvetCore::getCpuMemoryByte(uint16_t addr) const
{
    return m_addrSpace ? m_addrSpace->readByte(addr) : 0xFF;
}

uint8_t KorvetCore::getMemoryConfig() const
{
    return m_addrSpaceSelector ? m_addrSpaceSelector->getMemoryConfig() : 0;
}


int KorvetCore::getKbdLayoutModeIndex() const
{
    if (!m_kbdLayout)
        return 0;
    switch (m_kbdLayout->getMode()) {
        case KbdLayout::KLM_JCUKEN: return 1;
        case KbdLayout::KLM_SMART:  return 2;
        default:                    return 0;
    }
}

void KorvetCore::setCpuFrequency(unsigned frequency)
{
    if (frequency == m_cpuFrequency)
        return;

    m_cpuFrequency = frequency;
    m_cpu->setFrequency(m_cpuFrequency);
}


void KorvetCore::setCpuType(KorvetCpuType type)
{
    (void)type;
    // ПК8020 использует КР580ВМ80А; смена ядра на Z80 отключена.
}


void KorvetCore::reset()
{
    m_ram->reset();
    m_rom->reset();
    m_rom2->reset();
    m_rom3->reset();
    m_cpu->reset();
    m_addrSpace->reset();
    m_addrSpaceSelector->reset();
    m_ioAddrSpace->reset();
    s_devices.graphicsAdapter.reset();
    s_devices.textAdapter.reset();
    s_devices.korvetLutRegister.reset();
    s_devices.korvetVideoPpi.reset();
    s_devices.ppi3.reset();
    s_devices.pic.reset();
    m_renderer->reset();
    m_keyboard->reset();
    m_kbdLayout->resetSmartState();
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
    m_fdc->reset();
    m_fddReg->reset();
    m_diskA->reset();
    m_diskB->reset();
    m_diskC->reset();
    m_diskD->reset();
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
    m_intReq = false;
    m_intsEnabled = false;
}

KorvetCore::~KorvetCore()
{
    // Объекты размещены статически (см. s_devices) и живут всё время работы
    // прошивки: удалять нечего. Явное уничтожение потребуется только при
    // замене ядра CPU, и оно делается через CpuSlot.
    shutdown();
}



void KorvetCore::sysReq(SysReq sr)
{
    switch (sr) {
        case SR_RESET:
            reset();
            break;
        case SR_QUERTY:
            if (m_kbdLayout) {
                m_kbdLayout->resetKeys();
                m_kbdLayout->setQwertyMode();
            }
            break;
        case SR_JCUKEN:
            if (m_kbdLayout) {
                m_kbdLayout->resetKeys();
                m_kbdLayout->setJcukenMode();
            }
            break;
        case SR_SMART:
            if (m_kbdLayout) {
                m_kbdLayout->resetKeys();
                m_kbdLayout->resetSmartState();
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
            chooseFloppyImage(KorvetFloppyDrive::A);
            break;
        case SR_DISKB:
            chooseFloppyImage(KorvetFloppyDrive::B);
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
        default:
            break;
    }
    g_emulation->resetKeys();
}


bool KorvetCore::tapeHooksEnabled() const
{
    return m_tapeHooks[0] && m_tapeHooks[0]->getEnabled();
}


void KorvetCore::setTapeHooksEnabled(bool enabled)
{
    for (CpuHook* hook : m_tapeHooks)
        if (hook)
            hook->setEnabled(enabled);
}


void KorvetCore::chooseTapeInput()
{
    if (m_tapeInFile)
        m_tapeInFile->openFile();
}


void KorvetCore::chooseTapeOutput()
{
    if (m_tapeOutFile)
        m_tapeOutFile->openFile();
}


void KorvetCore::ejectTapeFiles()
{
    if (m_tapeInFile)
        m_tapeInFile->ejectFile();
    if (m_tapeOutFile)
        m_tapeOutFile->ejectFile();
}


bool KorvetCore::tapeFilePresent() const
{
    return (m_tapeInFile && m_tapeInFile->hasFile())
        || (m_tapeOutFile && m_tapeOutFile->hasFile());
}


std::string KorvetCore::getTapeInputFileName() const
{
    return m_tapeInFile ? m_tapeInFile->getFileName() : std::string();
}


std::string KorvetCore::getTapeOutputFileName() const
{
    return m_tapeOutFile ? m_tapeOutFile->getFileName() : std::string();
}


void KorvetCore::processKey(PalKeyCode keyCode, bool isPressed, unsigned unicodeKey)
{
    emuLog << "KorvetCore::processKey " << to_string(keyCode) << " / " << isPressed << "\n";
    if (m_kbdLayout)
        m_kbdLayout->processKey(keyCode, isPressed, unicodeKey);
}


void KorvetCore::resetKeys()
{
    if (m_kbdLayout)
        m_kbdLayout->resetKeys();
}


bool KorvetCore::loadFile(const string& fileName, bool run)
{
    if (m_loader) {
        m_loader->loadFile(fileName, run);
        return true;
    }
    return false;
}


Cpu8080Compatible* KorvetCore::getCpu()
{
    return m_cpu;
}

Keyboard* __not_in_flash_func(KorvetCore::getKeyboard)()
{
    return m_keyboard;
}

namespace {
FdImage* selectFloppy(FdImage* diskA, FdImage* diskB, FdImage* diskC, FdImage* diskD, KorvetFloppyDrive drive)
{
    switch (drive) {
        case KorvetFloppyDrive::A: return diskA;
        case KorvetFloppyDrive::B: return diskB;
        case KorvetFloppyDrive::C: return diskC;
        case KorvetFloppyDrive::D: return diskD;
    }
    return nullptr;
}
}

bool KorvetCore::assignDiskAFileName(const std::string& fileName, bool readOnly)
{
    if (!m_diskA)
        return false;
    const std::string fullFileName = fileName.empty() ? std::string() : palMakeFullFileName(fileName);
    const bool duplicate = m_diskB && m_diskB->getImagePresent() && m_diskB->getFileName() == fullFileName;
    return m_diskA->assignFileName(fullFileName, readOnly || duplicate);
}

bool KorvetCore::floppyImagePresent(KorvetFloppyDrive drive) const
{
    FdImage* disk = selectFloppy(m_diskA, m_diskB, m_diskC, m_diskD, drive);
    return disk && disk->getImagePresent();
}

bool KorvetCore::floppyImageReadOnly(KorvetFloppyDrive drive) const
{
    FdImage* disk = selectFloppy(m_diskA, m_diskB, m_diskC, m_diskD, drive);
    return disk && disk->getImagePresent() && disk->getWriteProtectStatus();
}

bool KorvetCore::floppyReadOnlyMode(KorvetFloppyDrive drive) const
{
    return m_floppyReadOnlyMode[static_cast<int>(drive)];
}

bool KorvetCore::canSetFloppyReadOnly(KorvetFloppyDrive drive, bool readOnly) const
{
    FdImage* disk = selectFloppy(m_diskA, m_diskB, m_diskC, m_diskD, drive);
    if (!disk)
        return false;
    if (!disk->getImagePresent())
        return true;
    if (readOnly)
        return true;

    FdImage* drives[] = {m_diskA, m_diskB, m_diskC, m_diskD};
    for (FdImage* other : drives) {
        if (other && other != disk && other->getImagePresent()
                && other->getFileName() == disk->getFileName()
                && !other->getWriteProtectStatus())
            return false;
    }
    return true;
}

void KorvetCore::setFloppyReadOnly(KorvetFloppyDrive drive, bool readOnly)
{
    FdImage* disk = selectFloppy(m_diskA, m_diskB, m_diskC, m_diskD, drive);
    if (!disk || !canSetFloppyReadOnly(drive, readOnly))
        return;

    m_floppyReadOnlyMode[static_cast<int>(drive)] = readOnly;
    if (disk->getImagePresent())
        disk->setWriteProtection(readOnly);
}

std::string KorvetCore::getFloppyFileName(KorvetFloppyDrive drive) const
{
    FdImage* disk = selectFloppy(m_diskA, m_diskB, m_diskC, m_diskD, drive);
    return disk ? disk->getFileName() : std::string();
}

void KorvetCore::chooseFloppyImage(KorvetFloppyDrive drive)
{
    FdImage* disk = selectFloppy(m_diskA, m_diskB, m_diskC, m_diskD, drive);
    if (!disk)
        return;
    bool readOnly = m_floppyReadOnlyMode[static_cast<int>(drive)];
    static const char* const titles[] = {
        "FDD-image file as A",
        "FDD-image file as B",
        "FDD-image file as C",
        "FDD-image file as D"
    };
    const char* title = titles[static_cast<int>(drive)];
    const std::string fileName = disk->chooseFileName(title, &readOnly);
    if (fileName.empty())
        return;
    m_floppyReadOnlyMode[static_cast<int>(drive)] = readOnly;
    const std::string fullFileName = palMakeFullFileName(fileName);
    bool duplicate = false;
    FdImage* drives[] = {m_diskA, m_diskB, m_diskC, m_diskD};
    for (FdImage* other : drives) {
        if (other && other != disk && other->getImagePresent() && other->getFileName() == fullFileName) {
            duplicate = true;
            break;
        }
    }
    disk->assignFileName(fullFileName, readOnly || duplicate);
}

void KorvetCore::ejectFloppyImage(KorvetFloppyDrive drive)
{
    FdImage* disk = selectFloppy(m_diskA, m_diskB, m_diskC, m_diskD, drive);
    if (disk)
        disk->assignFileName("");
}


SnapshotWriter::SnapshotWriter(FIL& file) :
    m_file(file)
{
}

bool SnapshotWriter::good() const
{
    return m_good;
}

bool SnapshotWriter::skip(uint32_t size) {
    if (!m_good)
        return false;
    if (f_lseek(&m_file, f_tell(&m_file) + size) != FR_OK) {
        m_good = false;
    }
    return m_good;
}

bool SnapshotWriter::write(const void* data, uint32_t size)
{
    if (!m_good)
        return false;

    UINT written = 0;
    if (f_write(&m_file, data, size, &written) != FR_OK || written != size)
        m_good = false;
    return m_good;
}

bool SnapshotWriter::beginSection(uint32_t id, uint16_t version)
{
    if (!m_good || m_sectionOpen)
        return false;

    m_sectionHeader = {};
    m_sectionHeader.id = id;
    m_sectionHeader.version = version;
    m_sectionHeader.headerSize = sizeof(m_sectionHeader);
    m_sectionHeaderPos = f_tell(&m_file);
    if (!writeValue(m_sectionHeader))
        return false;

    m_sectionDataPos = f_tell(&m_file);
    m_sectionOpen = true;
    return true;
}

bool SnapshotWriter::endSection()
{
    if (!m_good || !m_sectionOpen)
        return false;

    const FSIZE_t endPos = f_tell(&m_file);
    const FSIZE_t sectionSize = endPos - m_sectionDataPos;
    if (sectionSize > UINT32_MAX) {
        m_good = false;
        return false;
    }

    m_sectionHeader.size = static_cast<uint32_t>(sectionSize);
    if (f_lseek(&m_file, m_sectionHeaderPos) != FR_OK) {
        m_good = false;
        return false;
    }

    m_sectionOpen = false;
    if (!writeValue(m_sectionHeader))
        return false;
    if (f_lseek(&m_file, endPos) != FR_OK)
        m_good = false;
    return m_good;
}

SnapshotReader::SnapshotReader(FIL& file) :
    m_file(file)
{
}

bool SnapshotReader::good() const
{
    return m_good;
}

bool SnapshotReader::read(void* data, uint32_t size)
{
    if (!m_good)
        return false;
    if (m_sectionOpen && size > remaining()) {
        m_good = false;
        return false;
    }

    UINT bytesRead = 0;
    if (f_read(&m_file, data, size, &bytesRead) != FR_OK || bytesRead != size)
        m_good = false;
    return m_good;
}

bool SnapshotReader::beginSection(SnapshotSectionHeaderV2& section)
{
    if (!m_good || m_sectionOpen)
        return false;
    if (!readValue(section))
        return false;
    if (section.headerSize < sizeof(section)) {
        m_good = false;
        return false;
    }

    if (section.headerSize > sizeof(section) &&
        !skip(section.headerSize - sizeof(section)))
        return false;

    const FSIZE_t dataPos = f_tell(&m_file);
    const FSIZE_t fileSize = f_size(&m_file);
    if (dataPos > fileSize || section.size > fileSize - dataPos) {
        m_good = false;
        return false;
    }

    m_sectionEnd = dataPos + section.size;
    m_sectionOpen = true;
    return true;
}

bool SnapshotReader::endSection()
{
    if (!m_good || !m_sectionOpen)
        return false;

    if (f_lseek(&m_file, m_sectionEnd) != FR_OK) {
        m_good = false;
        return false;
    }
    m_sectionOpen = false;
    return true;
}

bool SnapshotReader::skip(uint32_t size)
{
    if (!m_good)
        return false;
    if (m_sectionOpen && size > remaining()) {
        m_good = false;
        return false;
    }

    const FSIZE_t pos = f_tell(&m_file);
    const FSIZE_t fileSize = f_size(&m_file);
    if (pos > fileSize || size > fileSize - pos ||
        f_lseek(&m_file, pos + size) != FR_OK)
        m_good = false;
    return m_good;
}

uint32_t SnapshotReader::remaining() const
{
    if (!m_sectionOpen)
        return 0;

    const FSIZE_t pos = f_tell(&m_file);
    if (pos >= m_sectionEnd)
        return 0;
    return static_cast<uint32_t>(m_sectionEnd - pos);
}

namespace {

static constexpr uint16_t c_snapshotFormatVersion = SNAPSHOT_STATE_FORMAT_VERSION;
static constexpr uint32_t c_snapshotSectionCount = 22;

static void snapshotFileName(char* fileName, unsigned slot)
{
    char* dst = appendText(fileName, "/korvet/.snap");
    dst = appendUnsigned(dst, slot);
    *dst = '\0';
}

static bool writeSnapshotSection(SnapshotWriter& writer,
                                 const SnapshotSerializable& device)
{
    return writer.beginSection(device.snapshotSectionId(),
                               device.snapshotSectionVersion()) &&
           device.saveState(writer) &&
           writer.endSection();
}

} // namespace


namespace {

#pragma pack(push, 1)
struct KorvetCoreSnapshotStateV3 {
    uint32_t cpuFrequency;
    uint8_t intReq;
    uint8_t intsEnabled;
    uint8_t tapeOut;
    uint8_t memConfig;          // конфигурация страничного селектора памяти
};
#pragma pack(pop)

} // namespace

uint32_t KorvetCore::snapshotSectionId() const
{
    return makeSnapshotSectionId('C', 'O', 'R', 'E');
}

uint16_t KorvetCore::snapshotSectionVersion() const
{
    return 3;
}

bool KorvetCore::saveState(SnapshotWriter& writer) const
{
    KorvetCoreSnapshotStateV3 state{};
    state.cpuFrequency = m_cpuFrequency;
    state.intReq = m_intReq ? 1 : 0;
    state.intsEnabled = m_intsEnabled ? 1 : 0;
    state.tapeOut = m_tapeOut ? 1 : 0;
    state.memConfig = getMemoryConfig();
    return writer.writeValue(state);
}

bool KorvetCore::loadState(SnapshotReader& reader, uint16_t version)
{
    if (version != snapshotSectionVersion() ||
        reader.remaining() != sizeof(KorvetCoreSnapshotStateV3))
        return false;

    KorvetCoreSnapshotStateV3 state{};
    if (!reader.readValue(state) ||
        state.intReq > 1 || state.intsEnabled > 1 || state.tapeOut > 1)
        return false;

    setCpuFrequency(state.cpuFrequency);
    m_intReq = state.intReq != 0;
    m_intsEnabled = state.intsEnabled != 0;
    m_tapeOut = state.tapeOut != 0;
    // Восстанавливаем конфигурацию страничного селектора: без неё после
    // загрузки карта памяти не совпадает с сохранённой и машина падает.
    if (m_addrSpaceSelector)
        m_addrSpaceSelector->writeByte(0, state.memConfig);
    return true;
}

void KorvetCore::postLoad()
{
    inte(m_intsEnabled);
    tapeOut(m_tapeOut);
}

bool KorvetCore::saveSnapshot(unsigned slot)
{
    if (slot < 1 || slot > 12 || !g_emulation)
        return false;

    f_mkdir("/korvet");

    char fileName[32];
    snapshotFileName(fileName, slot);

    if (f_open(&g_file, fileName, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return false;

    SnapshotFileHeaderV2 header{};
    std::memcpy(header.magic, "KORVETSS", sizeof(header.magic));
    header.formatVersion = c_snapshotFormatVersion;
    header.headerSize = sizeof(header);
    header.sectionCount = c_snapshotSectionCount;
#ifdef PORT_VERSION
    {
        char* dst = appendText(header.firmwareVersion, VER_STR);
        *dst++ = '/';
        dst = appendText(dst, PORT_VERSION);
        *dst = '\0';
    }
#else
    {
        char* dst = appendText(header.firmwareVersion, VER_STR);
        *dst = '\0';
    }
#endif

    SnapshotSerializable* cpuState =
        static_cast<SnapshotSerializable*>(static_cast<Cpu8080*>(m_cpu));

    SnapshotWriter writer(g_file);
    bool ok = writer.writeValue(header);
    ok = ok && writeSnapshotSection(writer, *this); // CORE must precede CPU.
    ok = ok && writeSnapshotSection(writer, *cpuState);
    ok = ok && writeSnapshotSection(writer, *m_addrSpace);
    ok = ok && writeSnapshotSection(writer, s_devices.graphicsAdapter);
    ok = ok && writeSnapshotSection(writer, s_devices.textAdapter);
    ok = ok && writeSnapshotSection(writer, s_devices.korvetLutRegister);
    ok = ok && writeSnapshotSection(writer, s_devices.korvetVideoPpi);
    ok = ok && writeSnapshotSection(writer, *m_ppi);
    ok = ok && writeSnapshotSection(writer, *m_ppi2);
    ok = ok && writeSnapshotSection(writer, s_devices.ppi3);
    ok = ok && writeSnapshotSection(writer, s_devices.pic);
    ok = ok && writeSnapshotSection(writer, *m_pit);
    ok = ok && writeSnapshotSection(writer, *m_renderer);
    ok = ok && writeSnapshotSection(writer, *m_tapeSoundSource);
    ok = ok && writeSnapshotSection(writer, *m_covox);
    ok = ok && writeSnapshotSection(writer, *m_fdc);
    ok = ok && writeSnapshotSection(writer, *m_diskA);
    ok = ok && writeSnapshotSection(writer, *m_diskB);
    ok = ok && writeSnapshotSection(writer, *m_diskC);
    ok = ok && writeSnapshotSection(writer, *m_diskD);
    ok = ok && writeSnapshotSection(writer, *g_emulation->getSoundMixer());
    ok = ok && writeSnapshotSection(writer, *g_emulation);
    ok = ok && writer.good();

    if (f_close(&g_file) != FR_OK)
        ok = false;
    if (!ok)
        f_unlink(fileName);
    return ok;
}

bool KorvetCore::removeSnapshot(unsigned slot)
{
    if (slot < 1 || slot > 12)
        return false;

    char fileName[32];
    snapshotFileName(fileName, slot);
    const FRESULT result = f_unlink(fileName);
    return result == FR_OK || result == FR_NO_FILE || result == FR_NO_PATH;
}


uint16_t KorvetCore::snapshotFormatVersion()
{
    return c_snapshotFormatVersion;
}


bool KorvetCore::readSnapshotInfo(unsigned slot, SnapshotInfo& info) const
{
    info = SnapshotInfo{};
    if (slot < 1 || slot > 12)
        return false;

    char fileName[32];
    snapshotFileName(fileName, slot);

    const FRESULT openResult = f_open(&g_file, fileName, FA_READ);
    if (openResult == FR_NO_FILE || openResult == FR_NO_PATH)
        return true;
    if (openResult != FR_OK)
        return false;

    info.present = true;
    SnapshotReader reader(g_file);
    SnapshotFileHeaderV2 header{};
    const bool readOk = reader.readValue(header);
    const bool closeOk = f_close(&g_file) == FR_OK;
    if (!readOk || !closeOk)
        return false;

    header.firmwareVersion[sizeof(header.firmwareVersion) - 1] = 0;
    info.formatVersion = header.formatVersion;
    info.firmwareVersion = header.firmwareVersion;
    return std::memcmp(header.magic, "KORVETSS", sizeof(header.magic)) == 0 &&
           header.headerSize >= sizeof(SnapshotFileHeaderV2);
}


KorvetCore::SnapshotLoadResult KorvetCore::loadSnapshot(unsigned slot,
                                                        std::string* firmwareVersion,
                                                        uint16_t* fileFormatVersion)
{
    if (firmwareVersion)
        firmwareVersion->clear();
    if (fileFormatVersion)
        *fileFormatVersion = 0;
    if (slot < 1 || slot > 12)
        return SnapshotLoadResult::InvalidSlot;
    if (!g_emulation)
        return SnapshotLoadResult::IoError;

    char fileName[32];
    snapshotFileName(fileName, slot);

    const FRESULT openResult = f_open(&g_file, fileName, FA_READ);
    if (openResult == FR_NO_FILE || openResult == FR_NO_PATH)
        return SnapshotLoadResult::NotFound;
    if (openResult != FR_OK)
        return SnapshotLoadResult::IoError;

    SnapshotReader reader(g_file);
    SnapshotFileHeaderV2 header{};
    SnapshotLoadResult result = SnapshotLoadResult::Ok;
    if (!reader.readValue(header)) {
        result = SnapshotLoadResult::InvalidFile;
    } else {
        header.firmwareVersion[sizeof(header.firmwareVersion) - 1] = 0;
        if (firmwareVersion)
            *firmwareVersion = header.firmwareVersion;
        if (fileFormatVersion)
            *fileFormatVersion = header.formatVersion;

        if (std::memcmp(header.magic, "KORVETSS", sizeof(header.magic)) != 0 ||
            header.headerSize < sizeof(SnapshotFileHeaderV2)) {
            result = SnapshotLoadResult::InvalidFile;
        } else if (header.formatVersion != c_snapshotFormatVersion) {
            result = SnapshotLoadResult::IncompatibleFormat;
        } else if (header.headerSize > sizeof(SnapshotFileHeaderV2) &&
                   !reader.skip(header.headerSize - sizeof(SnapshotFileHeaderV2))) {
            result = SnapshotLoadResult::IoError;
        }
    }

    bool loaded[c_snapshotSectionCount] = {};
    SnapshotSerializable* devices[c_snapshotSectionCount] = {};

    if (result == SnapshotLoadResult::Ok) {
        for (uint32_t sectionIndex = 0; sectionIndex < header.sectionCount; ++sectionIndex) {
            SnapshotSectionHeaderV2 section{};
            if (!reader.beginSection(section)) {
                result = SnapshotLoadResult::InvalidFile;
                break;
            }

            SnapshotSerializable* cpuState = getCpuType() == VECTOR_CPU_Z80
                ? static_cast<SnapshotSerializable*>(static_cast<CpuZ80*>(m_cpu))
                : static_cast<SnapshotSerializable*>(static_cast<Cpu8080*>(m_cpu));
            devices[0] = this;
            devices[1] = cpuState;
            devices[2] = m_addrSpace;
            devices[3] = &s_devices.graphicsAdapter;
            devices[4] = &s_devices.textAdapter;
            devices[5] = &s_devices.korvetLutRegister;
            devices[6] = &s_devices.korvetVideoPpi;
            devices[7] = m_ppi;
            devices[8] = m_ppi2;
            devices[9] = &s_devices.ppi3;
            devices[10] = &s_devices.pic;
            devices[11] = m_pit;
            devices[12] = m_renderer;
            devices[13] = m_tapeSoundSource;
            devices[14] = m_covox;
            devices[15] = m_fdc;
            devices[16] = m_diskA;
            devices[17] = m_diskB;
            devices[18] = m_diskC;
            devices[19] = m_diskD;
            devices[20] = g_emulation->getSoundMixer();
            devices[21] = g_emulation;

            int deviceIndex = -1;
            for (uint32_t i = 0; i < c_snapshotSectionCount; ++i) {
                if (devices[i]->snapshotSectionId() == section.id) {
                    deviceIndex = static_cast<int>(i);
                    break;
                }
            }

            if (deviceIndex >= 0) {
                // CORE changes the CPU implementation, therefore it must be
                // the first known section and may occur only once.
                if ((deviceIndex != 0 && !loaded[0]) || loaded[deviceIndex] ||
                    !devices[deviceIndex]->loadState(reader, section.version))
                    result = SnapshotLoadResult::InvalidFile;
                else
                    loaded[deviceIndex] = true;
            }

            if (!reader.endSection() && result == SnapshotLoadResult::Ok)
                result = SnapshotLoadResult::IoError;
            if (result != SnapshotLoadResult::Ok)
                break;
        }

        if (result == SnapshotLoadResult::Ok) {
            for (bool sectionLoaded : loaded) {
                if (!sectionLoaded) {
                    result = SnapshotLoadResult::InvalidFile;
                    break;
                }
            }
        }
    }

    if (f_close(&g_file) != FR_OK && result == SnapshotLoadResult::Ok)
        result = SnapshotLoadResult::IoError;
    if (result != SnapshotLoadResult::Ok)
        return result;

    // Rebuild derived links and caches only after every device has accepted
    // its own state. CORE is already applied, so the current CPU pointer is final.
    SnapshotSerializable* cpuState =
        static_cast<SnapshotSerializable*>(static_cast<Cpu8080*>(m_cpu));
    devices[1] = cpuState;
    for (SnapshotSerializable* device : devices)
        device->postLoad();

    // ActiveDevice audit: host-side helpers are intentionally excluded from
    // the guest snapshot, but their scheduler state must not remain on the
    // pre-load timeline. Auto-typing is cancelled; an open tape recorder is
    // kept running from the restored clock. Pit8253Helper is restored by PIT,
    // while CPU and SoundMixer own their scheduler state directly.
    if (m_kbdTapper)
        m_kbdTapper->cancelAfterSnapshotLoad();
    if (m_wavWriter)
        m_wavWriter->resynchronizeAfterSnapshotLoad();

    return SnapshotLoadResult::Ok;
}
