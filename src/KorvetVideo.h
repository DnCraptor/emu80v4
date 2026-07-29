#ifndef KORVET_VIDEO_H
#define KORVET_VIDEO_H

#include <cstdint>
#include "EmuObjects.h"
#include "Ppi8255Circuit.h"
#include "Fdc1793.h"

class KorvetRenderer;
class Pic8259;

class KorvetGraphicsAdapter : public AddressableDevice
{
public:
    KorvetGraphicsAdapter();
    void reset() override;
    void writeByte(int addr, uint8_t value) override;
    uint8_t readByte(int addr) override;
    void setColorRegisterValue(uint8_t value) {m_colorRegisterValue = value;}
    void setRwPage(uint8_t page);
    const uint8_t* getPlane(int plane) const {return m_planes[plane];}
    int getPageCount() const {return c_pageCount;}
private:
#if 0
    static constexpr int c_pageCount = 4;
#else
    static constexpr int c_pageCount = 1;
#endif
    static constexpr int c_pageSize = 0x4000;
    uint8_t m_colorRegisterValue = 0;
    uint8_t m_rwPage = 0;
    uint8_t* m_planes[3] = {};
};

class KorvetTextAdapter : public AddressableDevice
{
public:
    KorvetTextAdapter();
    void reset() override;
    void writeByte(int addr, uint8_t value) override;
    uint8_t readByte(int addr) override;
    void setAttrMask(uint8_t attrMask) {m_attrMask = attrMask & 3;}
    bool getAttr() const {return m_curAttr != 0;}
    const uint8_t* getSymbols() const {return m_symbols;}
    const uint8_t* getAttrs() const {return m_attrs;}
private:
    uint8_t m_attrMask = 0;
    uint8_t m_curAttr = 0;
    uint8_t m_symbols[1024] = {};
    uint8_t m_attrs[1024] = {};
};

class KorvetLutRegister : public AddressableDevice
{
public:
    void attachRenderer(KorvetRenderer* renderer) {m_renderer = renderer;}
    void reset() override;
    void writeByte(int addr, uint8_t value) override;
    uint8_t readByte(int) override {return 0xFF;}
    uint8_t getValue(int index) const {return m_lut[index & 0x0F];}
private:
    KorvetRenderer* m_renderer = nullptr;
    uint8_t m_lut[16] = {};
};

class KorvetFddMotor : public ActiveDevice
{
public:
    KorvetFddMotor();
    void attachPic(Pic8259* pic) {m_pic = pic;}
    void on();
    void operate() override;
private:
    Pic8259* m_pic = nullptr;
};

class KorvetVideoPpiCircuit : public Ppi8255Circuit
{
public:
    void attachGraphicsAdapter(KorvetGraphicsAdapter* adapter) {m_graphicsAdapter = adapter;}
    void attachTextAdapter(KorvetTextAdapter* adapter) {m_textAdapter = adapter;}
    void attachRenderer(KorvetRenderer* renderer) {m_renderer = renderer;}
    void attachFdc1793(Fdc1793* fdc) {m_fdc = fdc;}
    void attachFddMotor(KorvetFddMotor* motor) {m_motor = motor;}
    uint8_t getPortA() override;
    void setPortB(uint8_t value) override;
    void setPortC(uint8_t value) override;
    uint8_t getDisplayPage() const {return m_displayPage;}
    uint8_t getFontNumber() const {return m_fontNumber;}
    bool getWideCharMode() const {return m_wideCharMode;}
    void setVbl(bool vbl) {m_vbl = vbl;}
private:
    KorvetGraphicsAdapter* m_graphicsAdapter = nullptr;
    KorvetTextAdapter* m_textAdapter = nullptr;
    KorvetRenderer* m_renderer = nullptr;
    Fdc1793* m_fdc = nullptr;
    KorvetFddMotor* m_motor = nullptr;
    uint8_t m_displayPage = 0;
    uint8_t m_fontNumber = 0;
    bool m_wideCharMode = false;
    bool m_vbl = true;
    bool m_motorBit = false;
};

#endif
