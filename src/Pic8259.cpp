#include "Pic8259.h"
#include "Cpu.h"
#include "Korvet.h"

void Pic8259::reset()
{
    m_irs = 0;
    m_imr = 0;
    m_irr = 0;
    m_isr = 0;
    m_curIcwIndex = 0;
    m_totalIcws = 2;
    m_addrInterval = 8;
    m_isrPageAddr = 0;
    m_levelMode = false;
    m_curInServiceLevel = 8;
    m_curRequestLevel = 8;
    m_highestPrio = 0;
    m_readIsrFlag = false;
    m_pollMode = false;
    m_specialMask = false;
    m_autoEoi = false;
    m_rotateOnAeoi = false;
    m_inte = false;
}

void Pic8259::writeByte(int addr, uint8_t value)
{
    addr &= 1;

    if (!m_curIcwIndex) {
        if (addr) {
            m_imr = value;
        } else if (value & 0x10) {
            m_curInServiceLevel = 8;
            m_curRequestLevel = 8;
            m_imr = 0;
            m_irr = 0;
            m_levelMode = value & 8;
            m_addrInterval = value & 4 ? 4 : 8;
            m_highestPrio = 0;
            m_curIcwIndex = 1;
            m_autoEoi = false;
            m_rotateOnAeoi = false;
            m_totalIcws = 2;
            if (!(value & 2))
                m_totalIcws = value & 1 ? 4 : 3;
            m_isrPageAddr = (m_isrPageAddr & 0xFF00) | (value & 0xE0);
            if (m_addrInterval == 8)
                m_isrPageAddr &= ~0x20;
        } else if (value & 0x08) {
            if (value & 2)
                m_readIsrFlag = value & 1;
            m_pollMode = value & 4;
            if (value & 0x40)
                m_specialMask = value & 0x20;
        } else {
            int level = (value & 0x40) ? (value & 7) : m_curInServiceLevel;
            switch (value >> 5) {
            case 1:
            case 3:
                eoi(level);
                break;
            case 5:
            case 7:
                eoi(level);
                m_highestPrio = (level + 1) & 7;
                break;
            case 6:
                m_highestPrio = level;
                break;
            case 0:
            case 4:
                m_rotateOnAeoi = value & 4;
                break;
            default:
                break;
            }
        }
    } else {
        if (m_curIcwIndex == 1)
            m_isrPageAddr = (m_isrPageAddr & 0x00FF) | (value << 8);
        else if (m_curIcwIndex == 3)
            m_autoEoi = value & 2;

        if (++m_curIcwIndex == m_totalIcws)
            m_curIcwIndex = 0;
    }
}

uint8_t Pic8259::readByte(int addr)
{
    addr &= 1;

    if (addr == 1)
        return m_imr;

    if (!m_pollMode)
        return m_readIsrFlag ? m_isr : m_irr;

    m_pollMode = false;
    return m_curInServiceLevel == 8 ? 0 : m_curInServiceLevel | 0x80;
}

void Pic8259::irq(int vect, bool state)
{
    uint8_t mask = 1 << vect;

    if (state) {
        if (m_levelMode || !(m_irs & mask)) {
            m_irr |= mask;
            if (!(m_imr & mask))
                serviceInt();
        }
        m_irs |= mask;
    } else {
        m_irs &= ~mask;
        m_irr &= ~mask;
    }
}

void Pic8259::inte(bool active)
{
    m_inte = active;
    if (active && (m_irr & ~m_imr))
        serviceInt();
}

void Pic8259::serviceInt()
{
    updateCurLevels();

    if (m_curInServiceLevel == 8 ||
        (((m_curRequestLevel + m_highestPrio) & 7) < ((m_curInServiceLevel + m_highestPrio) & 7)) ||
        m_specialMask) {
        if (m_inte && m_cpu && m_curRequestLevel != 8) {
            m_isr |= 1 << m_curRequestLevel;
            m_irr &= ~(1 << m_curRequestLevel);
            updateCurLevels();
            m_cpu->intCall(m_isrPageAddr + m_curInServiceLevel * m_addrInterval);
            if (m_autoEoi)
                eoi(m_curInServiceLevel);
        }
    }
}

void Pic8259::updateCurLevels()
{
    m_curInServiceLevel = 8;
    m_curRequestLevel = 8;

    uint8_t maskedIrr = m_irr & ~m_imr;
    int level = m_highestPrio;
    for (int i = 0; i < 8; i++) {
        uint8_t mask = 1 << level;
        if (!m_curIcwIndex && (maskedIrr & mask)) {
            m_curRequestLevel = level;
            break;
        }
        level = (level + 1) & 7;
    }

    level = m_highestPrio;
    for (int i = 0; i < 8; i++) {
        uint8_t mask = 1 << level;
        if (!m_curIcwIndex && (m_isr & mask)) {
            m_curInServiceLevel = level;
            break;
        }
        level = (level + 1) & 7;
    }
}

void Pic8259::eoi(int level)
{
    if (level == 8)
        return;

    m_isr &= ~(1 << level);
    if (m_rotateOnAeoi)
        m_highestPrio = (level + 1) & 7;

    if (m_irr & ~m_imr)
        serviceInt();
    else
        updateCurLevels();
}


namespace {

#pragma pack(push, 1)
struct Pic8259SnapshotStateV1 {
    uint8_t irs;
    uint8_t imr;
    uint8_t irr;
    uint8_t isr;
    int32_t curIcwIndex;
    int32_t totalIcws;
    int32_t addrInterval;
    uint16_t isrPageAddr;
    int32_t curInServiceLevel;
    int32_t curRequestLevel;
    int32_t highestPrio;
    uint8_t levelMode;
    uint8_t readIsrFlag;
    uint8_t pollMode;
    uint8_t specialMask;
    uint8_t autoEoi;
    uint8_t rotateOnAeoi;
    uint8_t inte;
};
#pragma pack(pop)

}

uint32_t Pic8259::snapshotSectionId() const
{
    return makeSnapshotSectionId('P', 'I', 'C', ' ');
}

uint16_t Pic8259::snapshotSectionVersion() const
{
    return 1;
}

bool Pic8259::saveState(SnapshotWriter& writer) const
{
    Pic8259SnapshotStateV1 state{};
    state.irs = m_irs;
    state.imr = m_imr;
    state.irr = m_irr;
    state.isr = m_isr;
    state.curIcwIndex = m_curIcwIndex;
    state.totalIcws = m_totalIcws;
    state.addrInterval = m_addrInterval;
    state.isrPageAddr = m_isrPageAddr;
    state.curInServiceLevel = m_curInServiceLevel;
    state.curRequestLevel = m_curRequestLevel;
    state.highestPrio = m_highestPrio;
    state.levelMode = m_levelMode ? 1 : 0;
    state.readIsrFlag = m_readIsrFlag ? 1 : 0;
    state.pollMode = m_pollMode ? 1 : 0;
    state.specialMask = m_specialMask ? 1 : 0;
    state.autoEoi = m_autoEoi ? 1 : 0;
    state.rotateOnAeoi = m_rotateOnAeoi ? 1 : 0;
    state.inte = m_inte ? 1 : 0;
    return writer.writeValue(state);
}

bool Pic8259::loadState(SnapshotReader& reader, uint16_t version)
{
    if (version != snapshotSectionVersion() ||
        reader.remaining() != sizeof(Pic8259SnapshotStateV1))
        return false;

    Pic8259SnapshotStateV1 state{};
    if (!reader.readValue(state) ||
        state.curIcwIndex < 0 || state.curIcwIndex > 3 ||
        state.totalIcws < 2 || state.totalIcws > 4 ||
        (state.addrInterval != 4 && state.addrInterval != 8) ||
        state.curInServiceLevel < 0 || state.curInServiceLevel > 8 ||
        state.curRequestLevel < 0 || state.curRequestLevel > 8 ||
        state.highestPrio < 0 || state.highestPrio > 7 ||
        state.levelMode > 1 || state.readIsrFlag > 1 ||
        state.pollMode > 1 || state.specialMask > 1 ||
        state.autoEoi > 1 || state.rotateOnAeoi > 1 || state.inte > 1)
        return false;

    m_irs = state.irs;
    m_imr = state.imr;
    m_irr = state.irr;
    m_isr = state.isr;
    m_curIcwIndex = state.curIcwIndex;
    m_totalIcws = state.totalIcws;
    m_addrInterval = state.addrInterval;
    m_isrPageAddr = state.isrPageAddr;
    m_curInServiceLevel = state.curInServiceLevel;
    m_curRequestLevel = state.curRequestLevel;
    m_highestPrio = state.highestPrio;
    m_levelMode = state.levelMode != 0;
    m_readIsrFlag = state.readIsrFlag != 0;
    m_pollMode = state.pollMode != 0;
    m_specialMask = state.specialMask != 0;
    m_autoEoi = state.autoEoi != 0;
    m_rotateOnAeoi = state.rotateOnAeoi != 0;
    m_inte = state.inte != 0;
    return true;
}

void Pic8259::postLoad()
{
    updateCurLevels();
}
