#include "Pic8259.h"
#include "Cpu.h"

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
