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

#include "Specialist.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

#include "Emulation.h"
#include "Globals.h"
#include "Pal.h"
#include "WavReader.h"
#include "graphics.h"
#include "pico/spec_m2.rom.h"
#include "pico/picoPal.h"

namespace {

constexpr const char* rksFilter = ".rks";
constexpr uint8_t colors4[] = {63, 48, 12, 3};
constexpr uint8_t colors8[] = {63, 60, 51, 48, 15, 12, 3, 0};

#ifdef SPECIALIST_SCANLINES
alignas(4) uint32_t scanlineBuffer[SpecMemory::width / 4];
#else
alignas(4) uint32_t frame[SpecMemory::height][SpecMemory::width / 4];
#endif

// Expand four MSB-first pixels at a time. Background is always black.
constexpr uint32_t pixelMask(unsigned bits)
{
    return ((bits & 8) ? 0x000000ffu : 0) |
           ((bits & 4) ? 0x0000ff00u : 0) |
           ((bits & 2) ? 0x00ff0000u : 0) |
           ((bits & 1) ? 0xff000000u : 0);
}

constexpr uint32_t masks[] = {
    pixelMask(0), pixelMask(1), pixelMask(2), pixelMask(3),
    pixelMask(4), pixelMask(5), pixelMask(6), pixelMask(7),
    pixelMask(8), pixelMask(9), pixelMask(10), pixelMask(11),
    pixelMask(12), pixelMask(13), pixelMask(14), pixelMask(15),
};

} // namespace

void SpecMemory::mapCpuPages(Cpu& cpu)
{
    for (unsigned addr = 0; addr < romStart; addr += 256) {
        cpu.setReadPage(addr >> 8, m_ram + addr);
        if (addr < videoStart)
            cpu.setWritePage(addr >> 8, m_ram + addr);
    }
    for (unsigned offset = 0; offset < sizeof(specMonitor); offset += 256)
        cpu.setReadPage((romStart + offset) >> 8, specMonitor + offset);
}

uint8_t __not_in_flash_func(SpecMemory::readByte)(int addr)
{
    if (addr < int(romStart))
        return m_ram[addr];
    if (addr < int(romStart + sizeof(specMonitor)))
        return specMonitor[addr - romStart];
    if (addr >= 0xf800)
        return m_keyboard.readByte(addr & 3);
    if (addr >= 0xf000)
        return m_expansion.readByte(addr & 3);
    return 0xff;
}

void __not_in_flash_func(SpecMemory::writeByte)(int addr, uint8_t value)
{
    if (addr < int(videoStart)) {
        m_ram[addr] = value;
    } else if (addr < int(romStart)) {
        const unsigned offset = addr - videoStart;
#ifdef SPECIALIST_SCANLINES
        __atomic_store_n(&m_ram[addr], value, __ATOMIC_RELAXED);
        __atomic_store_n(&m_colors[offset], m_color, __ATOMIC_RELAXED);
#else
        if (m_ram[addr] != value || m_colors[offset] != m_color) {
            m_ram[addr] = value;
            m_colors[offset] = m_color;
            const unsigned row = offset & 255;
            m_dirtyRows[row / 32] |= uint32_t(1) << (row % 32);
        }
#endif
    } else if (addr >= 0xf800) {
        m_keyboard.writeByte(addr & 3, value);
    } else if (addr >= 0xf000) {
        m_expansion.writeByte(addr & 3, value);
    }
}

void SpecMemory::setColorLatch(uint8_t value)
{
    // PC7 and PC6 supply the top two color bits, PC4 the low bit.
    m_color = ((value & 0xc0) >> 5) | ((value & 0x10) >> 4);
}

#ifndef SPECIALIST_SCANLINES
void SpecMemory::invalidateVideo()
{
    std::fill_n(m_dirtyRows, height / 32, 0xffffffffu);
}

uint32_t SpecMemory::takeDirtyRows(unsigned block)
{
    const uint32_t dirty = m_dirtyRows[block];
    m_dirtyRows[block] = 0;
    return dirty;
}
#endif

void SpecMemory::reset()
{
    // RESET leaves RAM and previously written video attributes intact.
    m_color = 0;
}

void SpecRenderer::init()
{
#ifdef SPECIALIST_SCANLINES
    graphics_set_buffer(nullptr, SpecMemory::width, SpecMemory::height);
    graphics_set_scanline_source(scanline, this);
#else
    graphics_set_buffer(reinterpret_cast<uint8_t*>(frame), SpecMemory::width, SpecMemory::height);
    m_memory.invalidateVideo();
#endif
    graphics_set_duplicateLines(true);
}

void __not_in_flash_func(SpecRenderer::renderRow)(unsigned row, uint32_t* dst) const
{
    const auto mode = getColorMode();
    uint32_t colors[8];
    for (unsigned i = 0; i < 8; ++i) {
        const uint8_t color = mode == SpecColorMode::Mono ? RGB888(192, 192, 192)
            : mode == SpecColorMode::Color4 ? colors4[i >> 1] : colors8[i];
        colors[i] = uint32_t(color) * 0x01010101u;
    }

    const uint8_t* bitmap = m_memory.video() + row;
    const uint8_t* attributes = m_memory.colors() + row;
    for (unsigned x = 0; x < SpecMemory::videoSize; x += SpecMemory::height) {
#ifdef SPECIALIST_SCANLINES
        const uint8_t bits = __atomic_load_n(&bitmap[x], __ATOMIC_RELAXED);
        const uint32_t fg = colors[__atomic_load_n(&attributes[x], __ATOMIC_RELAXED)];
#else
        const uint8_t bits = bitmap[x];
        const uint32_t fg = colors[attributes[x]];
#endif
        *dst++ = masks[bits >> 4] & fg;
        *dst++ = masks[bits & 15] & fg;
    }
}

#ifdef SPECIALIST_SCANLINES
const uint8_t* __not_in_flash_func(SpecRenderer::scanline)(void* context, unsigned row)
{
    static_cast<SpecRenderer*>(context)->renderRow(row, scanlineBuffer);
    return reinterpret_cast<const uint8_t*>(scanlineBuffer);
}
#else
void SpecRenderer::renderFrame()
{
    for (unsigned block = 0; block < SpecMemory::height / 32; ++block) {
        uint32_t dirty = m_memory.takeDirtyRows(block);
        while (dirty) {
            const unsigned row = block * 32 + unsigned(__builtin_ctz(dirty));
            dirty &= dirty - 1;
            renderRow(row, frame[row]);
        }
    }
}
#endif

void SpecRenderer::redraw()
{
#ifndef SPECIALIST_SCANLINES
    m_memory.invalidateVideo();
    renderFrame();
#endif
}

void SpecRenderer::operate()
{
    if (!g_emulation->getPausedState()) {
#ifndef SPECIALIST_SCANLINES
        renderFrame();
#endif
        g_emulation->notifyFrameRendered();
    }
    // Original raster: 512 dots by 312 lines at an 8 MHz pixel clock.
    m_curClock += g_emulation->getFrequency() * 512 * 312 / 8000000;
}

void SpecRenderer::setColorMode(SpecColorMode mode)
{
    if (mode == getColorMode())
        return;
    __atomic_store_n(&m_mode, static_cast<uint8_t>(mode), __ATOMIC_RELAXED);
#ifndef SPECIALIST_SCANLINES
    m_memory.invalidateVideo();
#endif
}

void SpecKeyboard::resetKeys()
{
    std::fill_n(m_vKeys, 12, 0);
    std::fill_n(m_hKeys, 6, 0);
    m_shift = false;
}

void SpecKeyboard::processKey(EmuKey key, bool isPressed)
{
    if (key == EK_NONE)
        return;
    if (key == EK_SHIFT) {
        m_shift = isPressed;
        return;
    }

    for (unsigned col = 0; col < 12; ++col) {
        for (unsigned row = 0; row < 6; ++row) {
            if (m_keyMatrix[col][row] != key)
                continue;
            if (isPressed) {
                m_vKeys[col] |= 1u << row;
                m_hKeys[row] |= 1u << col;
            } else {
                m_vKeys[col] &= ~(1u << row);
                m_hKeys[row] &= ~(1u << col);
            }
            return;
        }
    }
}

uint8_t SpecKeyboard::getVMatrixData()
{
    uint8_t keys = 0;
    for (unsigned mask = m_vMask & 0xfff; mask; mask &= mask - 1)
        keys |= m_vKeys[__builtin_ctz(mask)];
    return ~keys;
}

uint16_t SpecKeyboard::getHMatrixData()
{
    uint16_t keys = 0;
    for (unsigned mask = m_hMask & 0x3f; mask; mask &= mask - 1)
        keys |= m_hKeys[__builtin_ctz(mask)];
    return ~keys;
}

EmuKey SpecKbdLayout::translateKey(PalKeyCode keyCode)
{
    switch (keyCode) {
    case PK_LCTRL:
    case PK_RCTRL:
        return EK_RPT;
    default:
        break;
    }

    EmuKey key = RkKbdLayout::translateKey(keyCode);
    if (key != EK_NONE)
        return key;

    switch (keyCode) {
    case PK_F6:
        return EK_F6;
    case PK_F7:
        return EK_F7;
    case PK_F8:
        return EK_F8;
    case PK_F9:
        return EK_F9;
    case PK_F10:
        return EK_F10;
    case PK_F11:
        return EK_F11;
    case PK_F12:
        return EK_CLEAR;

    default:
        return EK_NONE;
    }
}

uint8_t SpecPpiCircuit::getPortA()
{
    return m_keyboard.getHMatrixData() & 0xff;
}

uint8_t SpecPpiCircuit::getPortB()
{
    return (m_keyboard.getVMatrixData() << 2) | (m_keyboard.getShift() ? 0 : 2)
        | (g_emulation->getWavReader()->getCurValue() ? 0 : 1);
}

uint8_t SpecPpiCircuit::getPortC()
{
    return (m_keyboard.getHMatrixData() >> 8) & 15;
}

void SpecPpiCircuit::setPortA(uint8_t value)
{
    m_kbdMask = (m_kbdMask & 0xf00) | (m_portAInput ? 0xff : value);
    m_keyboard.setVMatrixMask(m_kbdMask);
}

void SpecPpiCircuit::setPortB(uint8_t value)
{
    m_keyboard.setHMatrixMask(m_portBInput ? 0x3f : (value >> 2));
}

void SpecPpiCircuit::setPortC(uint8_t value)
{
    m_kbdMask = (m_kbdMask & 0xff) | ((m_portCLoInput ? 15 : value & 15) << 8);
    m_keyboard.setVMatrixMask(m_kbdMask);
    m_speaker.setValue((value >> 5) & 1);
    m_tapeOut = (value & 0x80) != 0;
    m_tape.setValue(m_tapeOut);
    m_memory.setColorLatch(value);
}

void SpecPpiCircuit::setPortAMode(bool input)
{
    m_portAInput = input;
    if (input)
        setPortA(0xff);
}

void SpecPpiCircuit::setPortBMode(bool input)
{
    m_portBInput = input;
    if (input)
        setPortB(0xff);
}

void SpecPpiCircuit::setPortCLoMode(bool input)
{
    m_portCLoInput = input;
    if (input) {
        m_kbdMask |= 0xf00;
        m_keyboard.setVMatrixMask(m_kbdMask);
    }
}

void SpecPpiCircuit::reset()
{
    m_speaker.setValue(0);
    m_tapeOut = false;
    m_tape.setValue(0);
}

SpecialistCore::SpecialistCore()
    : m_cpu(new (m_cpuStorage) Cpu8080())
{
    m_keyboardPpi.attachPpi8255Circuit(&m_ppiCircuit);
    m_layout.setMachine(this);
    m_layout.setQwertyMode();
    m_tapeIn.setMachine(this);
    m_tapeIn.setMode("r");
    m_tapeIn.setFilter(rksFilter);
    m_tapeOut.setMachine(this);
    m_tapeOut.setMode("w");
    m_tapeOut.setFilter(".rks");
    m_tapeInHook.setTapeRedirector(&m_tapeIn);
    m_tapeInHook.setSignature("C5D50E00573A");
    m_tapeOutHook.setSignature("C5D5F5570E08");
    m_closeHook.setSignature("C5D5E5CDC8C2");
    m_tapeOutHook.setTapeRedirector(&m_tapeOut);
    m_tapeOutHook.setOutputRegisterA(true);
    attachCpu();
}

SpecialistCore::~SpecialistCore()
{
    ejectTapeFiles();
    m_cpu->~Cpu8080Compatible();
}

void SpecialistCore::init()
{
    m_wavWriter.init();
    m_renderer.init();
    reset();
}

void SpecialistCore::attachCpu()
{
    m_cpu->setFrequency(m_cpuFrequency);
    m_cpu->setStartAddr(SpecMemory::romStart);
    m_cpu->attachAddrSpace(&m_memory);
    m_memory.mapCpuPages(*m_cpu);
    m_cpu->addHook(&m_tapeInHook);
    m_cpu->addHook(&m_tapeOutHook);
    m_cpu->addHook(&m_closeHook);
}

void SpecialistCore::setCpuType(SpecCpuType type)
{
    if (type == m_cpuType)
        return;

    const int index = g_emulation->getActiveDeviceIndex(m_cpu);
    m_cpu->~Cpu8080Compatible();
    if (type == SpecCpuType::Z80)
        m_cpu = new (m_cpuStorage) CpuZ80();
    else
        m_cpu = new (m_cpuStorage) Cpu8080();
    if (index >= 0)
        g_emulation->moveActiveDevice(m_cpu, index);

    m_cpuType = type;
    attachCpu();
    reset();
}

void SpecialistCore::setCpuFrequency(unsigned frequency)
{
    m_cpuFrequency = frequency;
    m_cpu->setFrequency(frequency);
}

void SpecialistCore::reset()
{
    m_expansionPpi.reset();
    m_keyboardPpi.reset();
    m_ppiCircuit.reset();
    m_memory.reset();
    resetKeys();
    m_tapeIn.reset();
    m_tapeOut.reset();
    m_tapeInHook.reset();
    m_tapeOutHook.reset();
    m_cpu->reset();
    m_cpu->syncronize();
}

void SpecialistCore::processKey(PalKeyCode key, bool pressed, unsigned unicode)
{
    m_layout.processKey(key, pressed, unicode);
}

void SpecialistCore::resetKeys()
{
    m_layout.resetKeys();
}

int SpecialistCore::getKbdLayoutModeIndex() const
{
    return m_layout.getMode();
}

void SpecialistCore::setTapeHooksEnabled(bool enabled)
{
    m_tapeHooksEnabled = enabled;
    m_tapeInHook.setEnabled(enabled);
    m_tapeOutHook.setEnabled(enabled);
    m_closeHook.setEnabled(enabled);
}

void SpecialistCore::ejectTapeFiles()
{
    m_tapeIn.ejectFile();
    m_tapeOut.ejectFile();
}

bool SpecialistCore::loadFile(const std::string& path, bool run)
{
    PalFile file;
    if (!file.open(path))
        return false;

    uint8_t header[4] = {};
    bool valid = file.read(header, 1) == 1;
    if (valid && header[0] == 0xe6)
        valid = file.read(header, 1) == 1;
    valid = valid && file.read(header + 1, 3) == 3;
    if (valid && header[0] == 0xd9 && header[1] == 0xd9 && header[2] == 0xd9) {
        uint8_t ch = header[3];
        while (valid && ch != 0xe6)
            valid = file.read(&ch, 1) == 1;
        valid = valid && file.read(header, sizeof(header)) == sizeof(header);
    }

    const unsigned first = header[0] | (unsigned(header[1]) << 8);
    const unsigned last = header[2] | (unsigned(header[3]) << 8);
    const unsigned size = last >= first ? last - first + 1 : 0;
    if (!valid || !size || last >= SpecMemory::romStart || file.getSize() - file.getPos() < size) {
        file.close();
        return false;
    }

    std::unique_ptr<uint8_t[]> data(new (std::nothrow) uint8_t[size]);
    const bool complete = data && file.read(data.get(), size) == size;
    file.close();
    if (!complete)
        return false;

    if (run) {
        reset();
        g_emulation->exec(uint64_t(m_cpu->getKDiv()) * 2000000, true);
    }
    for (unsigned i = 0; i < size; ++i)
        m_memory.writeByte(first + i, data[i]);
    if (run)
        m_cpu->setPC(first);
    return true;
}

void SpecialistCore::sysReq(SysReq request)
{
    switch (request) {
        case SR_RESET:
            reset();
            break;
        case SR_QUERTY:
            m_layout.setQwertyMode();
            break;
        case SR_JCUKEN:
            m_layout.setJcukenMode();
            break;
        case SR_SMART:
            m_layout.setSmartMode();
            break;
        case SR_COLOR:
            setColorMode(static_cast<SpecColorMode>((static_cast<int>(getColorMode()) + 1) % 3));
            break;
        case SR_LOADWAV:
            g_emulation->getWavReader()->chooseAndLoadFile();
            break;
        case SR_TAPEHOOK:
            setTapeHooksEnabled(!tapeHooksEnabled());
            break;
        case SR_LOAD:
        case SR_LOADRUN: {
            const auto path = palOpenFileDialog("Load Specialist program", rksFilter, false);
            if (!path.empty() && !loadFile(path, request == SR_LOADRUN))
                palModalMessage("Load failed", "Invalid or unreadable RKS file");
            break;
        }
        default:
            break;
    }
    g_emulation->resetKeys();
}
