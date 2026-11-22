#include "Specialist.h"

#include <cstring>

#include "Emulation.h"
#include "Globals.h"
#include "WavReader.h"
#include "pico/picoPal.h"

namespace {
constexpr uint8_t casSync[] = {0x1f, 0xa6, 0xde, 0xba, 0xcc, 0x13, 0x7d, 0x74};
}

bool SpecRamDisk::open()
{
    if (m_file.isOpen())
        return true;
    if (!palEnsureSdMounted())
        return false;

    constexpr auto path = "/specialist/mx.ramdisk";
    if (m_file.open(path, "r+")) {
        if (m_file.getSize() == diskSize)
            return true;
        m_file.close();
        return false;
    }

    if (!m_file.open(path, "w"))
        return false;
    for (unsigned offset = 0; offset < diskSize; offset += blockSize) {
        if (m_file.write(m_data, blockSize) != blockSize) {
            m_file.close();
            f_unlink(path);
            return false;
        }
    }
    const bool complete = m_file.sync();
    m_file.close();
    if (!complete) {
        f_unlink(path);
        return false;
    }
    return m_file.open(path, "r+");
}

bool SpecRamDisk::flush()
{
    if (!m_dirty)
        return true;
    m_file.seek(m_block);
    if (m_file.getPos() != m_block || m_file.write(m_data, blockSize) != blockSize || !m_file.sync())
        return false;
    m_dirty = false;
    return true;
}

bool SpecRamDisk::cache(unsigned address)
{
    const unsigned block = address & ~(blockSize - 1);
    if (block == m_block)
        return true;
    if (!m_file.isOpen() || !flush())
        return false;
    m_block = diskSize;
    m_file.seek(block);
    if (m_file.getPos() != block || m_file.read(m_data, blockSize) != blockSize)
        return false;
    m_block = block;
    return true;
}

uint8_t SpecRamDisk::read(unsigned address)
{
    return cache(address) ? m_data[address & (blockSize - 1)] : 0;
}

bool SpecRamDisk::write(unsigned address, uint8_t value)
{
    if (!cache(address))
        return false;
    auto& byte = m_data[address & (blockSize - 1)];
    m_dirty |= byte != value;
    byte = value;
    return true;
}

bool SpecTapeHook::hookProc()
{
    if (!m_isEnabled || (m_hasSignature && !checkSignature()) ||
        g_emulation->getWavReader()->isPlaying() || m_file->isCancelled())
        return false;

    const bool mx = g_emulation->getSpecialist()->getModel() == SpecModel::Mx;
    const bool header = m_operation == Operation::InputHeader || m_operation == Operation::OutputHeader;
    if (header && !m_file->isOpen())
        m_file->openFile();
    if (m_file->isCancelled())
        return false;

    uint16_t af = m_cpu->getAF() & 0xfffe;
    switch (m_operation) {
        case Operation::InputHeader:
            if (m_file->isTsx())
                m_file->advanceToNextBlock();
            else if (m_file->isCas())
                m_file->waitForSequence(casSync, sizeof(casSync));
            if (!mx && (!m_file->isOpen() || m_file->isEof()))
                af |= 1;
            m_cpu->setAF(af);
            break;
        case Operation::Input: {
            const bool eof = !m_file->isOpen() || m_file->isEof();
            const uint8_t value = eof ? 0 : m_file->readByte();
            m_cpu->setAF((af & 0xff) | (uint16_t(value) << 8) | (eof && !mx));
            break;
        }
        case Operation::OutputHeader:
            if (!m_file->isOpen())
                return false;
            if (m_file->isCas()) {
                for (unsigned padding = (0u - m_file->getPos()) & 7; padding; --padding)
                    m_file->writeByte(0);
                for (uint8_t byte : casSync)
                    m_file->writeByte(byte);
            }
            break;
        case Operation::Output:
            if (!m_file->isOpen())
                return false;
            m_file->writeByte(m_cpu->getBC() & 0xff);
            break;
    }
    m_cpu->ret();
    return true;
}

void SpecPitSound::setMode(SpecModel model)
{
    m_model = model;
    m_total = m_high = 0;
    m_gate = false;
    m_pit->getCounter(2)->setExtClockMode(model == SpecModel::Mx);
}

void SpecPitSound::updateMx()
{
    auto* first = m_pit->getCounter(0);
    auto* second = m_pit->getCounter(1);
    auto* third = m_pit->getCounter(2);
    first->updateState();
    second->updateState();
    uint64_t total, high;
    first->getStats(total, high);
    third->operateForTicks(second->getSumOutTicks());
    m_total += total;
    if (!m_gate)
        m_high += third->getOut() ? total : high;
    first->resetStats();
    second->resetStats();
    third->resetStats();
}

int SpecPitSound::calcValue()
{
    if (m_model == SpecModel::Classic)
        return 0;
    if (m_model == SpecModel::Sp580)
        return Pit8253SoundSource::calcValue();
    updateMx();
    const int level = m_total ? m_high * MAX_SND_AMP / m_total : 0;
    m_total = m_high = 0;
    return level * m_ampFactor;
}

void SpecPitSound::setGate(bool gate)
{
    updateMx();
    m_gate = gate;
}
