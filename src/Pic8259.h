#ifndef PIC8259_H
#define PIC8259_H

#include "EmuObjects.h"

class Cpu8080Compatible;

class Pic8259 : public AddressableDevice, public SnapshotSerializable
{
public:
    void reset() override;
    void writeByte(int addr, uint8_t value) override;
    uint8_t readByte(int addr) override;

    void attachCpu(Cpu8080Compatible* cpu) {m_cpu = cpu;}
    void irq(int level, bool state);
    void inte(bool active);

    uint32_t snapshotSectionId() const override;
    uint16_t snapshotSectionVersion() const override;
    bool saveState(SnapshotWriter& writer) const override;
    bool loadState(SnapshotReader& reader, uint16_t version) override;
    void postLoad() override;

private:
    Cpu8080Compatible* m_cpu = nullptr;

    uint8_t m_irs = 0;
    uint8_t m_imr = 0;
    uint8_t m_irr = 0;
    uint8_t m_isr = 0;

    int m_curIcwIndex = 0;
    int m_totalIcws = 2;
    int m_addrInterval = 8;
    uint16_t m_isrPageAddr = 0;
    bool m_levelMode = false;
    int m_curInServiceLevel = 8;
    int m_curRequestLevel = 8;
    int m_highestPrio = 0;
    bool m_readIsrFlag = false;
    bool m_pollMode = false;
    bool m_specialMask = false;
    bool m_autoEoi = false;
    bool m_rotateOnAeoi = false;
    bool m_inte = false;

    void updateCurLevels();
    void serviceInt();
    void eoi(int level);
};

#endif
