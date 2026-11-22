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
#include <cctype>
#include <cstdlib>
#include <strings.h>
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
bool hasExtension(const std::string& path, const char* ext)
{
    const auto dot = path.find_last_of('.');
    return dot != std::string::npos && strcasecmp(path.c_str() + dot, ext) == 0;
}

unsigned word(const uint8_t* bytes)
{
    return bytes[0] | (unsigned(bytes[1]) << 8);
}

bool readHex(char*& cursor, unsigned& value)
{
    char* end;
    const auto n = std::strtoul(cursor, &end, 16);
    if (end == cursor || n > 0xffff || (*end && !std::isspace(static_cast<unsigned char>(*end))))
        return false;
    cursor = end;
    value = n;
    return true;
}

bool nextCasBlock(PalFile& file)
{
    constexpr uint8_t sync[] = {0x1f,0xa6,0xde,0xba,0xcc,0x13,0x7d,0x74};
    file.seek((file.getPos() + 7) & ~7);
    uint8_t chunk[sizeof(sync)];
    while (file.read(chunk, sizeof(chunk)) == sizeof(chunk)) {
        if (!std::memcmp(chunk, sync, sizeof(sync)))
            return true;
    }
    return false;
}

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

extern "C" {
extern const uint8_t specSp580Boot[0x800];
extern const uint8_t specSp580High[0x800];
extern const uint8_t specMxRamfos[0xb800];
}

void SpecMemory::setBuiltInRom()
{
    m_highRom = specSp580High;
    switch (m_model) {
        case SpecModel::Classic: m_rom = specMonitor; m_romSize = sizeof(specMonitor); break;
        case SpecModel::Sp580: m_rom = specSp580Boot; m_romSize = sizeof(specSp580Boot); break;
        case SpecModel::Mx: m_rom = specMxRamfos; m_romSize = sizeof(specMxRamfos); break;
    }
}

void SpecMemory::restoreRom()
{
    m_romOverride.reset();
    m_highRomOverride.reset();
    setBuiltInRom();
    mapPages();
}

void SpecMemory::setModel(SpecModel model)
{
    m_model = model;
    m_color = 0;
    std::fill_n(m_ram, videoStart, 0);
    for (unsigned addr = videoStart; addr < romStart; ++addr)
        writeVideo(addr, 0);
    std::fill(m_ram + romStart, m_ram + sizeof(m_ram), 0);
    m_mxPage = 0;
    restoreRom();
}

bool SpecMemory::loadRom(const std::string& path, bool high)
{
    if (high && m_model != SpecModel::Sp580)
        return false;
    const unsigned capacity = m_model == SpecModel::Sp580 ? 0x800
        : m_model == SpecModel::Mx ? 0xc000 : 0x3000;
    PalFile file;
    if (!file.open(path))
        return false;
    const auto count = file.getSize();
    // MX firmware images from Emu80 may include bytes past the ROM window.
    const bool valid = count > 0 && (count <= capacity ||
        (m_model == SpecModel::Mx && count == 0x10000));
    const unsigned length = valid ? std::min<unsigned>(count, capacity) : 0;
    std::unique_ptr<uint8_t[]> image(length ? new (std::nothrow) uint8_t[length] : nullptr);
    const bool loaded = image && file.read(image.get(), length) == length;
    file.close();
    if (!loaded || (high && length != capacity))
        return false;

    if (high) {
        m_highRomOverride = std::move(image);
        m_highRom = m_highRomOverride.get();
    } else {
        m_romOverride = std::move(image);
        m_rom = m_romOverride.get();
        m_romSize = length;
    }
    mapPages();
    return true;
}

void SpecMemory::mapCpuPages(Cpu& cpu)
{
    m_cpu = &cpu;
    if (!m_rom)
        setBuiltInRom();
    mapPages();
}

void SpecMemory::mapPages()
{
    if (!m_cpu)
        return;
    for (unsigned page = 0; page < 256; ++page) {
        const unsigned addr = page * 256;
        const uint8_t* read = nullptr;
        uint8_t* write = nullptr;
        if (m_model == SpecModel::Mx) {
            if (m_mxPage <= 1) {
                if (addr < romStart && m_mxPage == 0) {
                    if (addr + 256 <= m_romSize)
                        read = m_rom + addr;
                } else if (addr < 0xff00) {
                    read = m_ram + addr;
                    if (addr < videoStart || addr >= romStart)
                        write = m_ram + addr;
                }
            }
        } else if (addr < romStart) {
            read = m_ram + addr;
            if (addr < videoStart)
                write = m_ram + addr;
        } else if (addr + 256 <= romStart + m_romSize) {
            read = m_rom + addr - romStart;
        } else if (m_model == SpecModel::Sp580 && addr >= 0xf800) {
            read = m_highRom + addr - 0xf800;
        }
        m_cpu->setReadPage(page, read);
        m_cpu->setWritePage(page, write);
    }
}

void SpecMemory::selectMxPage(unsigned page)
{
    if (page == m_mxPage)
        return;
    const bool remap = m_mxPage <= 1 || page <= 1;
    m_mxPage = page;
    if (remap)
        mapPages();
}

uint8_t __not_in_flash_func(SpecMemory::readByte)(int addr)
{
    if (m_model == SpecModel::Mx) {
        if (addr < 0xffc0) {
            if (m_mxPage >= 2)
                return m_ramDisk.read((m_mxPage - 2) * 65536u + addr);
            if (m_mxPage == 0 && addr < int(romStart))
                return unsigned(addr) < m_romSize ? m_rom[addr] : 0xff;
            return m_ram[addr];
        }
        if (addr < 0xffe0) return m_ram[addr];
        if (addr < 0xffe4) return m_keyboard.readByte(addr & 3);
        if (addr < 0xffe8) return m_expansion.readByte(addr & 3);
        if (addr < 0xffec) return m_fdc.readByte(addr & 3);
        if (addr < 0xfff0) return m_pit.readByte(addr & 3);
        return 0xff;
    }
    if (addr < int(romStart)) return m_ram[addr];
    if (unsigned(addr) < romStart + m_romSize) return m_rom[addr - romStart];
    if (m_model == SpecModel::Sp580) {
        if (addr >= 0xf800) return m_highRom[addr - 0xf800];
        if (addr >= 0xf000) return m_keyboard.readByte(addr & 3);
        if (addr >= 0xe800) return m_expansion.readByte(addr & 3);
        if (addr >= 0xe000) return m_pit.readByte(addr & 3);
    } else {
        if (addr >= 0xf800) return m_keyboard.readByte(addr & 3);
        if (addr >= 0xf000) return m_expansion.readByte(addr & 3);
    }
    return 0xff;
}

void __not_in_flash_func(SpecMemory::writeVideo)(unsigned addr, uint8_t value)
{
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
}

void __not_in_flash_func(SpecMemory::writeByte)(int addr, uint8_t value)
{
    if (m_model == SpecModel::Mx) {
        if (addr < 0xffc0) {
            if (m_mxPage >= 2) {
                m_ramDisk.write((m_mxPage - 2) * 65536u + addr, value);
            } else if (addr >= int(romStart) || m_mxPage == 1) {
                if (addr >= int(videoStart) && addr < int(romStart))
                    writeVideo(addr, value);
                else
                    m_ram[addr] = value;
            }
        } else if (addr < 0xffe0) m_ram[addr] = value;
        else if (addr < 0xffe4) m_keyboard.writeByte(addr & 3, value);
        else if (addr < 0xffe8) m_expansion.writeByte(addr & 3, value);
        else if (addr < 0xffec) m_fdc.writeByte(addr & 3, value);
        else if (addr < 0xfff0) m_pit.writeByte(addr & 3, value);
        else if (addr == 0xfff2) m_fdc.setHead(value & 1);
        else if (addr == 0xfff3) m_fdc.setDrive(value & 1);
        else if (addr == 0xfff8) m_color = value;
        else if (addr == 0xfffc) selectMxPage(1);
        else if (addr == 0xfffd) selectMxPage(2 + (value & 7));
        else if (addr >= 0xfffe) selectMxPage(0);
        return;
    }

    if (addr < int(videoStart)) m_ram[addr] = value;
    else if (addr < int(romStart)) writeVideo(addr, value);
    else if (m_model == SpecModel::Sp580) {
        if (addr >= 0xf800) return;
        if (addr >= 0xf000) m_keyboard.writeByte(addr & 3, value);
        else if (addr >= 0xe800) m_expansion.writeByte(addr & 3, value);
        else if (addr >= 0xe000) m_pit.writeByte(addr & 3, value);
    } else {
        if (addr >= 0xf800) m_keyboard.writeByte(addr & 3, value);
        else if (addr >= 0xf000) m_expansion.writeByte(addr & 3, value);
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
    if (m_model == SpecModel::Mx)
        selectMxPage(0);
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
    static constexpr uint8_t mxColors[16] = {
        0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x14, 0x2a,
        0x15, 0x03, 0x0c, 0x0f, 0x30, 0x33, 0x3c, 0x3f
    };
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
        const uint8_t attr = __atomic_load_n(&attributes[x], __ATOMIC_RELAXED);
#else
        const uint8_t bits = bitmap[x];
        const uint8_t attr = attributes[x];
#endif
        if (mode == SpecColorMode::Mx) {
            const uint32_t fg = mxColors[attr >> 4] * 0x01010101u;
            const uint32_t bg = mxColors[attr & 15] * 0x01010101u;
            *dst++ = bg ^ (masks[bits >> 4] & (fg ^ bg));
            *dst++ = bg ^ (masks[bits & 15] & (fg ^ bg));
        } else {
            const uint32_t fg = colors[attr & 7];
            *dst++ = masks[bits >> 4] & fg;
            *dst++ = masks[bits & 15] & fg;
        }
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

    const auto& matrix = m_model == SpecModel::Sp580 ? m_keyMatrixSp580
        : m_model == SpecModel::Mx ? m_keyMatrixMx : m_keyMatrix;
    for (unsigned col = 0; col < 12; ++col) {
        for (unsigned row = 0; row < 6; ++row) {
            if (matrix[col][row] != key)
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
    const bool gate = (value & 0x20) != 0;
    if (m_memory.getModel() == SpecModel::Mx)
        m_pit.setGate(gate);
    else {
        m_speaker.setValue(gate);
        m_memory.setColorLatch(value);
    }
    m_tapeOut = (value & 0x80) != 0;
    m_tape.setValue(m_tapeOut);
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

    m_tapeOutHook.setSignature("C5D5F5570E08");
    m_tapeInHook.setSignature("C5D50E00573A");
    m_spInHead.setSignature("21E80351CD2E");
    m_spIn.setSignature("E5C5D5219EC1");
    m_spOutHead.setSignature("F5C5E5011027");
    m_spOut.setSignature("E5C5F52AF38F");
    m_mxInHead.setSignature("E5D5C52189CBE5");
    m_mxIn.setSignature("E5D5C52189CBE5");
    m_mxOutHead.setSignature("E5C52A62C63E");
    m_mxOut.setSignature("E5C52A60C6C6");
    m_mxClose.setSignature("E5D5C5CDC4CECC");
    m_tapeOutHook.setTapeRedirector(&m_tapeOut);
    m_tapeOutHook.setOutputRegisterA(true);
    m_fdc.attachFdImage(0, &m_disks[0]);
    m_fdc.attachFdImage(1, &m_disks[1]);
    m_pitSound.attachPit(&m_pit);
    configureTape();
    attachCpu();
}

SpecialistCore::~SpecialistCore()
{
    ejectTapeFiles();
    m_memory.flushMxRamDisk();
    m_cpu->~Cpu8080Compatible();
}

void SpecialistCore::init()
{
    m_wavWriter.init();
    m_pit.setFrequency(2000000);
    m_renderer.init();
    reset();
}

void SpecialistCore::attachCpu()
{
    m_cpu->setFrequency(m_cpuFrequency);
    m_cpu->setStartAddr(getModel() == SpecModel::Mx ? 0 : SpecMemory::romStart);
    m_cpu->attachAddrSpace(&m_memory);
    m_memory.mapCpuPages(*m_cpu);
    m_cpu->addHook(&m_tapeInHook);
    m_cpu->addHook(&m_tapeOutHook);
    m_cpu->addHook(&m_closeHook);
    for (CpuHook* hook : {static_cast<CpuHook*>(&m_mxClose), static_cast<CpuHook*>(&m_spInHead),
        static_cast<CpuHook*>(&m_spIn), static_cast<CpuHook*>(&m_spOutHead),
        static_cast<CpuHook*>(&m_spOut), static_cast<CpuHook*>(&m_mxInHead),
        static_cast<CpuHook*>(&m_mxIn), static_cast<CpuHook*>(&m_mxOutHead),
        static_cast<CpuHook*>(&m_mxOut)})
        m_cpu->addHook(hook);
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

bool SpecialistCore::setModel(SpecModel model)
{
    if (model == getModel())
        return true;
    if (model == SpecModel::Mx && !m_memory.openMxRamDisk())
        return false;
    if (!m_memory.flushMxRamDisk())
        return false;

    ejectTapeFiles();
    m_memory.setModel(model);
    m_keyboard.setModel(model);
    m_layout.setModel(model);
    m_cpu->setStartAddr(model == SpecModel::Mx ? 0 : SpecMemory::romStart);
    setColorMode(model == SpecModel::Sp580 ? SpecColorMode::Mono : SpecColorMode::Color8);
    configureTape();
    reset();
    return true;
}

void SpecialistCore::chooseRom(bool high)
{
    const auto path = palOpenFileDialog(high ? "Select high ROM" : "Select boot ROM",
                                        ".rom|.bin", false);
    if (path.empty())
        return;
    if (!m_memory.loadRom(path, high)) {
        palModalMessage("ROM load failed", "Invalid size or unreadable image");
        return;
    }
    reset();
}

void SpecialistCore::restoreRom()
{
    m_memory.restoreRom();
    reset();
}

void SpecialistCore::mountFloppy(unsigned drive)
{
    bool readOnly = false;
    const auto path = palOpenFileDialog(drive ? "Mount drive B" : "Mount drive A",
                                        ".odi|.ord", false, &readOnly);
    if (path.empty())
        return;
    PalFile file;
    const bool opened = file.open(path, readOnly ? "r" : "r+");
    const bool valid = opened && file.getSize() == 80 * 2 * 5 * 1024;
    file.close();
    if (!valid || !m_disks[drive].assignFileName(path, readOnly))
        palModalMessage("Mount failed", "Expected a readable 800 KiB disk image");
}

void SpecialistCore::ejectFloppy(unsigned drive)
{
    m_disks[drive].assignFileName("");
}

void SpecialistCore::setCpuFrequency(unsigned frequency)
{
    m_cpuFrequency = frequency;
    m_cpu->setFrequency(frequency);
    if (getModel() == SpecModel::Sp580)
        m_pit.setFrequency(frequency);
}

void SpecialistCore::reset()
{
    m_memory.flushMxRamDisk();
    m_pit.reset();
    m_pit.setFrequency(getModel() == SpecModel::Sp580 ? m_cpuFrequency : 2000000);
    m_pitSound.setMode(getModel());
    m_fdc.reset();
    m_fdc.setHead(0);
    m_fdc.setDrive(0);
    m_expansionPpi.reset();
    m_keyboardPpi.reset();
    m_ppiCircuit.reset();
    m_memory.reset();
    resetKeys();
    g_emulation->getWavReader()->stop();
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

void SpecialistCore::configureTape()
{
    const bool classic = getModel() == SpecModel::Classic;
    const bool sp = getModel() == SpecModel::Sp580;
    const bool mx = getModel() == SpecModel::Mx;
    m_tapeIn.setFilter(classic ? rksFilter : sp ? ".spt|.cas|.tsx" : ".rfs");
    m_tapeOut.setFilter(classic ? ".rks" : sp ? ".cas|.spt" : ".rfs");
    m_tapeInHook.setEnabled(classic && m_tapeHooksEnabled);
    m_tapeOutHook.setEnabled(classic && m_tapeHooksEnabled);
    m_closeHook.setSignature(sp ? "E5C5D5219EC1" : "C5D5E5CDC8C2");
    m_closeHook.setEnabled((classic || sp) && m_tapeHooksEnabled);
    for (auto* hook : {&m_spInHead, &m_spIn, &m_spOutHead, &m_spOut})
        hook->setEnabled(sp && m_tapeHooksEnabled);
    for (auto* hook : {&m_mxInHead, &m_mxIn, &m_mxOutHead, &m_mxOut})
        hook->setEnabled(mx && m_tapeHooksEnabled);
    m_mxClose.setEnabled(mx && m_tapeHooksEnabled);
}

void SpecialistCore::setTapeHooksEnabled(bool enabled)
{
    m_tapeHooksEnabled = enabled;
    configureTape();
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

    const bool loaded = valid && loadProgram(file, word(header), word(header + 2), word(header), run);
    file.close();
    return loaded;
}

bool SpecialistCore::loadProgram(PalFile& file, unsigned first, unsigned last, unsigned entry,
                                 bool run, const std::string* continuation, int nextBlock)
{
    if (last < first || last >= SpecMemory::romStart || entry >= SpecMemory::romStart)
        return false;
    const unsigned length = last - first + 1;
    if (file.getSize() - file.getPos() < length)
        return false;

    std::unique_ptr<uint8_t[]> data(new (std::nothrow) uint8_t[length]);
    if (!data || file.read(data.get(), length) != length)
        return false;

    const auto next = nextBlock < 0 ? file.getPos() : nextBlock;
    const bool more = continuation && next < file.getSize();
    file.close();
    if (run) {
        reset();
        g_emulation->exec(uint64_t(m_cpu->getKDiv()) * 2000000, true);
        if (getModel() == SpecModel::Sp580) {
            m_keyboard.processKey(EK_CR, true);
            g_emulation->exec(uint64_t(m_cpu->getKDiv()) * 50000, true);
            m_keyboard.processKey(EK_CR, false);
            g_emulation->exec(uint64_t(m_cpu->getKDiv()) * 1000000, true);
            m_cpu->setSP(0x8fa7);
            m_memory.writeByte(0x8fa7, 0xc0);
            m_memory.writeByte(0x8fa8, 0xfb);
        }
        if (more && !m_tapeIn.openFile(*continuation, next))
            return false;
    }

    for (unsigned i = 0; i < length; ++i)
        m_memory.writeByte(first + i, data[i]);
    if (run)
        m_cpu->setPC(entry);
    return true;
}

bool SpecialistCore::loadSp580(const std::string& path, bool run)
{
    PalFile file;
    if (!file.open(path))
        return false;

    uint8_t header[9];
    bool valid = true;
    int nextBlock = -1;
    if (hasExtension(path, ".spt")) {
        valid = file.read(header, sizeof(header)) == sizeof(header);
        // SPT stores the raw nine-byte tape header, a zero-terminated title,
        // and the data. A following record may be read by the guest loader.
        uint8_t titleByte = 0xff;
        while (valid && titleByte)
            valid = file.read(&titleByte, 1) == 1;
    } else if (hasExtension(path, ".cas")) {
        valid = nextCasBlock(file) && file.read(header, sizeof(header)) == sizeof(header);
        valid = valid && nextCasBlock(file);
    } else {
        uint8_t tsx[10];
        constexpr uint8_t signature[] = {'Z','X','T','a','p','e','!',0x1a};
        int blockEnd = 0;
        valid = file.read(tsx, sizeof(tsx)) == sizeof(tsx) &&
            !std::memcmp(tsx, signature, sizeof(signature)) &&
            TapeRedirector::nextTsxDataBlock(file, blockEnd) && blockEnd - file.getPos() >= int(sizeof(header)) &&
            file.read(header, sizeof(header)) == sizeof(header);
        if (valid) {
            file.seek(blockEnd);
            valid = TapeRedirector::nextTsxDataBlock(file, blockEnd);
            nextBlock = blockEnd;
            if (valid && blockEnd - file.getPos() < int(word(header + 3)) - int(word(header + 1)) + 1)
                valid = false;
        }
    }
    const bool loaded = valid && header[0] == 0 &&
        loadProgram(file, word(header + 1), word(header + 3), word(header + 7), run, &path, nextBlock);
    file.close();
    return loaded;
}

bool SpecialistCore::loadMx(const std::string& path, bool run)
{
    PalFile file;
    if (!file.open(path))
        return false;
    const auto length = file.getSize();
    char descriptor[512] = {};
    const bool validDescriptor = length > 0 && length < int(sizeof(descriptor)) &&
        file.read(descriptor, length) == length;
    file.close();
    if (!validDescriptor)
        return false;
    char* cursor = descriptor;
    unsigned first, entry;
    if (!readHex(cursor, first) || !readHex(cursor, entry) || first >= 0xffc0 || entry >= 0xffc0)
        return false;

    const std::string stem = path.substr(0, path.size() - 4);
    if (!file.open(stem + ".i80") && !file.open(stem + ".I80"))
        return false;
    const auto size = file.getSize();
    if (size <= 0 || size > 0xffc0 - first || (!run && size > 0xffc0 - 33)) {
        file.close();
        return false;
    }
    std::unique_ptr<uint8_t[]> data(new (std::nothrow) uint8_t[size]);
    const bool loaded = data && file.read(data.get(), size) == size;
    file.close();
    if (!loaded)
        return false;

    unsigned monAddress = 0;
    unsigned monSize = 0;
    std::unique_ptr<uint8_t[]> monitor;
    if (run) {
        while (std::isspace(static_cast<unsigned char>(*cursor)))
            ++cursor;
        const char* name = cursor;
        while (*cursor && !std::isspace(static_cast<unsigned char>(*cursor)))
            ++cursor;
        if (cursor != name) {
            std::string monPath(name, cursor - name);
            const auto slash = monPath.find_last_of("/\\");
            const auto baseName = slash == std::string::npos ? monPath : monPath.substr(slash + 1);
            if (baseName.size() < 7)
                return false;
            char address[5];
            std::memcpy(address, baseName.data() + 3, 4);
            address[4] = 0;
            char* pos = address;
            if (!readHex(pos, monAddress) || *pos || !monAddress || monAddress >= 0xffc0)
                return false;
            if (monPath[0] != '/') {
                const auto directory = path.find_last_of('/');
                if (directory != std::string::npos)
                    monPath = path.substr(0, directory + 1) + monPath;
            }
            if (!file.open(monPath))
                return false;
            const auto count = file.getSize();
            if (count > 0 && count <= 0xffc0 - monAddress) {
                monSize = count;
                monitor.reset(new (std::nothrow) uint8_t[monSize]);
            }
            const bool complete = monitor && file.read(monitor.get(), monSize) == monSize;
            file.close();
            if (!complete)
                return false;
        }
    }

    if (!run) {
        // The desktop loader places PROGRAM.EXE at offset zero. On the
        // persistent RAM disk, only do this on an unused first bank.
        for (unsigned i = 0; i < 0xffc0; ++i) {
            if (m_memory.readMxRamDisk(i) != 0) {
                palModalMessage("RAM disk is in use", "Use Load and run to keep its contents");
                return true;
            }
        }
        const unsigned last = first + size - 1;
        uint16_t checksum = 0;
        for (unsigned i = 0; i + 1 < unsigned(size); ++i)
            checksum += data[i] * 257u;
        checksum = (checksum & 0xff00) | ((checksum + data[size - 1]) & 0xff);
        const uint8_t record[] = {0xd3, 0xd3, 0xd3, 'P','R','O','G','R','A','M',' ',' ','E','X','E',
            0x8c, 1, 1, 0, 0, 0, 0, 0, 0, uint8_t(first), uint8_t(first >> 8),
            uint8_t(last), uint8_t(last >> 8), uint8_t(checksum), uint8_t(checksum >> 8)};
        unsigned addr = 0;
        for (uint8_t b : record)
            if (!m_memory.writeMxRamDisk(addr++, b)) return false;
        for (unsigned i = 0; i < unsigned(size); ++i)
            if (!m_memory.writeMxRamDisk(addr++, data[i])) return false;
        return m_memory.writeMxRamDisk(addr++, uint8_t(first)) &&
            m_memory.writeMxRamDisk(addr++, uint8_t(first >> 8)) &&
            m_memory.writeMxRamDisk(addr, 0) && m_memory.flushMxRamDisk();
    }

    reset();
    if (monitor) {
        m_memory.selectMxPage(1);
        for (unsigned i = 0; i < monSize; ++i)
            m_memory.writeByte(monAddress + i, monitor[i]);
        m_cpu->setPC(monAddress);
    }
    g_emulation->exec(uint64_t(m_cpu->getKDiv()) * 2000000, true);
    m_memory.selectMxPage(1);
    for (unsigned i = 0; i < unsigned(size); ++i)
        m_memory.writeByte(first + i, data[i]);
    m_cpu->setPC(entry);
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
            const auto model = getModel();
            const char* filter = model == SpecModel::Mx ? ".cpu"
                : model == SpecModel::Sp580 ? ".rks|.spt|.cas|.tsx" : rksFilter;
            const auto path = palOpenFileDialog("Load Specialist program", filter, false);
            if (path.empty())
                break;
            const bool run = request == SR_LOADRUN;
            const bool loaded = model == SpecModel::Mx ? loadMx(path, run)
                : model == SpecModel::Sp580 && !hasExtension(path, ".rks") ? loadSp580(path, run)
                : loadFile(path, run);
            if (!loaded)
                palModalMessage("Load failed", "Invalid or unreadable program");
            break;
        }
        default:
            break;
    }
    g_emulation->resetKeys();
}
