#include <cstring>
#include "KorvetVideo.h"
#include "Korvet.h"
#include "Pic8259.h"
#include "Emulation.h"
#include "Globals.h"
#include "WavReader.h"

namespace {
// Размер обязан совпадать с c_pageCount * c_pageSize; выводим из того же
// макроса KORVET_VIDEO_PAGE_COUNT (см. KorvetVideo.h), чтобы не разъехалось.
uint8_t s_graphicsMemory[3][KORVET_VIDEO_PAGE_COUNT * 0x4000];
}

KorvetGraphicsAdapter::KorvetGraphicsAdapter()
{
    for (int plane = 0; plane < 3; ++plane)
        m_planes[plane] = s_graphicsMemory[plane];
    reset();
}

void KorvetGraphicsAdapter::reset()
{
    m_colorRegisterValue = 0;
    m_rwPage = 0;
    std::memset(s_graphicsMemory, 0, sizeof(s_graphicsMemory));
}

void KorvetGraphicsAdapter::setRwPage(uint8_t page)
{
    m_rwPage = page & (c_pageCount - 1);
}

void KorvetGraphicsAdapter::writeByte(int addr, uint8_t value)
{
    addr = (addr & (c_pageSize - 1)) + m_rwPage * c_pageSize;
    if (m_colorRegisterValue & 0x80) {
        for (int plane = 0; plane < 3; ++plane) {
            const uint8_t mask = uint8_t(0x02u << plane);
            if (m_colorRegisterValue & mask)
                m_planes[plane][addr] |= value;
            else
                m_planes[plane][addr] &= uint8_t(~value);
        }
        return;
    }
    const bool setBits = (m_colorRegisterValue & 1) != 0;
    for (int plane = 0; plane < 3; ++plane) {
        const uint8_t mask = uint8_t(0x02u << plane);
        if (m_colorRegisterValue & mask)
            continue;
        if (setBits)
            m_planes[plane][addr] |= value;
        else
            m_planes[plane][addr] &= uint8_t(~value);
    }
}

uint8_t KorvetGraphicsAdapter::readByte(int addr)
{
    addr = (addr & (c_pageSize - 1)) + m_rwPage * c_pageSize;
    uint8_t value = 0;
    if (m_colorRegisterValue & 0x80) {
        for (int plane = 0; plane < 3; ++plane) {
            const uint8_t mask = uint8_t(0x10u << plane);
            value |= (m_colorRegisterValue & mask)
                ? uint8_t(~m_planes[plane][addr])
                : m_planes[plane][addr];
        }
        return value;
    }
    for (int plane = 0; plane < 3; ++plane) {
        const uint8_t mask = uint8_t(0x10u << plane);
        if (m_colorRegisterValue & mask)
            value |= m_planes[plane][addr];
    }
    return value;
}

KorvetTextAdapter::KorvetTextAdapter() { reset(); }

void KorvetTextAdapter::reset()
{
    m_attrMask = 0;
    m_curAttr = 0;
    std::memset(m_symbols, 0, sizeof(m_symbols));
    std::memset(m_attrs, 0, sizeof(m_attrs));
}

void KorvetTextAdapter::writeByte(int addr, uint8_t value)
{
    addr &= 0x03FF;
    m_symbols[addr] = value;
    switch (m_attrMask) {
    case 1: m_attrs[addr] = 0xFF; break;
    case 2: m_attrs[addr] = 0x00; break;
    case 3: m_attrs[addr] = m_curAttr; break;
    default: break;
    }
}

uint8_t KorvetTextAdapter::readByte(int addr)
{
    addr &= 0x03FF;
    if (m_attrMask == 3)
        m_curAttr = m_attrs[addr];
    return m_symbols[addr];
}

void KorvetLutRegister::reset()
{
    std::memset(m_lut, 0, sizeof(m_lut));
}

void KorvetLutRegister::writeByte(int, uint8_t value)
{
    const uint8_t index = value & 0x0F;
    m_lut[index] = value >> 4;
    if (m_renderer)
        m_renderer->setLutValue(index, m_lut[index]);
}

KorvetFddMotor::KorvetFddMotor()
{
    pause();
    setFrequency(1);
}

void KorvetFddMotor::on()
{
    resume();
    syncronize();
    m_curClock += m_kDiv * 3;
}

void KorvetFddMotor::operate()
{
    pause();
    if (m_pic) {
        m_pic->irq(7, true);
        m_pic->irq(7, false);
    }
}

uint8_t KorvetVideoPpiCircuit::getPortA()
{
    const uint8_t attr = m_textAdapter && m_textAdapter->getAttr() ? 0x08 : 0x00;
    const uint8_t tapeIn = g_emulation->getWavReader()->getCurValue() ? 0x01 : 0x00;
    const bool displayActive = m_renderer ? m_renderer->isDisplayActive() : m_vbl;
    return 0xF0 | tapeIn | 0x04 | (displayActive ? 0x02 : 0x00) | attr;
}

void KorvetVideoPpiCircuit::setPortB(uint8_t value)
{
    if (!m_fdc)
        return;

    if (value & 0x01)
        m_fdc->setDrive(0);
    else if (value & 0x02)
        m_fdc->setDrive(1);
    else if (value & 0x04)
        m_fdc->setDrive(2);
    else if (value & 0x08)
        m_fdc->setDrive(3);

    m_fdc->setHead((value >> 4) & 1);

    const bool motorBit = (value & 0x20) != 0;
    if (motorBit && !m_motorBit && m_motor)
        m_motor->on();
    m_motorBit = motorBit;
}

void KorvetVideoPpiCircuit::setPortC(uint8_t value)
{
    if (m_graphicsAdapter)
        m_graphicsAdapter->setRwPage(value >> 6);
    if (m_textAdapter)
        m_textAdapter->setAttrMask((value >> 4) & 3);
    m_displayPage = value & 3;
    m_fontNumber = (value >> 2) & 1;
    m_wideCharMode = (value & 0x08) != 0;
    if (m_renderer) {
        m_renderer->setDisplayPage(m_displayPage);
        m_renderer->setFontNumber(m_fontNumber);
        m_renderer->setWideCharMode(m_wideCharMode);
    }
}


namespace {

#pragma pack(push, 1)
struct KorvetGraphicsSnapshotStateV1 {
    uint8_t colorRegisterValue;
    uint8_t rwPage;
};

struct KorvetTextSnapshotStateV1 {
    uint8_t attrMask;
    uint8_t curAttr;
};
#pragma pack(pop)

}

uint32_t KorvetGraphicsAdapter::snapshotSectionId() const
{
    return makeSnapshotSectionId('G', 'R', 'A', 'F');
}

uint16_t KorvetGraphicsAdapter::snapshotSectionVersion() const
{
    return 1;
}

bool KorvetGraphicsAdapter::saveState(SnapshotWriter& writer) const
{
    const KorvetGraphicsSnapshotStateV1 state{m_colorRegisterValue, m_rwPage};
    return writer.writeValue(state) &&
           writer.write(s_graphicsMemory, sizeof(s_graphicsMemory));
}

bool KorvetGraphicsAdapter::loadState(SnapshotReader& reader, uint16_t version)
{
    if (version != snapshotSectionVersion() ||
        reader.remaining() != sizeof(KorvetGraphicsSnapshotStateV1) + sizeof(s_graphicsMemory))
        return false;

    KorvetGraphicsSnapshotStateV1 state{};
    if (!reader.readValue(state) || state.rwPage >= c_pageCount ||
        !reader.read(s_graphicsMemory, sizeof(s_graphicsMemory)))
        return false;

    m_colorRegisterValue = state.colorRegisterValue;
    m_rwPage = state.rwPage;
    return true;
}

uint32_t KorvetTextAdapter::snapshotSectionId() const
{
    return makeSnapshotSectionId('T', 'E', 'X', 'T');
}

uint16_t KorvetTextAdapter::snapshotSectionVersion() const
{
    return 1;
}

bool KorvetTextAdapter::saveState(SnapshotWriter& writer) const
{
    const KorvetTextSnapshotStateV1 state{m_attrMask, m_curAttr};
    return writer.writeValue(state) &&
           writer.write(m_symbols, sizeof(m_symbols)) &&
           writer.write(m_attrs, sizeof(m_attrs));
}

bool KorvetTextAdapter::loadState(SnapshotReader& reader, uint16_t version)
{
    if (version != snapshotSectionVersion() ||
        reader.remaining() != sizeof(KorvetTextSnapshotStateV1) + sizeof(m_symbols) + sizeof(m_attrs))
        return false;

    KorvetTextSnapshotStateV1 state{};
    if (!reader.readValue(state) || state.attrMask > 3 ||
        !reader.read(m_symbols, sizeof(m_symbols)) ||
        !reader.read(m_attrs, sizeof(m_attrs)))
        return false;

    m_attrMask = state.attrMask;
    m_curAttr = state.curAttr;
    return true;
}

uint32_t KorvetLutRegister::snapshotSectionId() const
{
    return makeSnapshotSectionId('L', 'U', 'T', ' ');
}

uint16_t KorvetLutRegister::snapshotSectionVersion() const
{
    return 1;
}

bool KorvetLutRegister::saveState(SnapshotWriter& writer) const
{
    return writer.write(m_lut, sizeof(m_lut));
}

bool KorvetLutRegister::loadState(SnapshotReader& reader, uint16_t version)
{
    return version == snapshotSectionVersion() &&
           reader.remaining() == sizeof(m_lut) &&
           reader.read(m_lut, sizeof(m_lut));
}

void KorvetLutRegister::postLoad()
{
    if (!m_renderer)
        return;
    for (int i = 0; i < 16; ++i)
        m_renderer->setLutValue(i, m_lut[i]);
}
