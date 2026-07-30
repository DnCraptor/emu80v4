/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2019-2022
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

#ifndef VECTOR_H
#define VECTOR_H

#include "EmuObjects.h"
#include "Ppi8255Circuit.h"
#include "CrtRenderer.h"
#include "Keyboard.h"
#include "KbdLayout.h"
#include "Pit8253Sound.h"

#include <string>
#include <cstddef>
#include <cstdint>

#include "ff.h"
#include "PalKeys.h"
#include "EmuTypes.h"

class Ram;
class SRam;
class Rom;
class Fdc1793;
class GeneralSoundSource;
class Cpu8080Compatible;
class Covox;
class AtaDrive;
class Cpu;
class RamDisk;
class DiskImage;
class KbdTapper;
class CpuHook;
class KorvetFileLoader;
class KorvetAddrSpace;
class KorvetAddrSpaceSelector;
class Cpu8080;
class AddrSpace;
class KorvetKeyboard;
class KorvetKbdLayout;
class KorvetPpi8255Circuit;
class Ppi8255;
class KorvetColorRegister;
class KorvetGraphicsAdapter;
class KorvetVideoPpiCircuit;
class KorvetPpi8255Circuit2;
class Pit8253;
class KorvetPit8253SoundSource;
class Psg3910;
class Psg3910SoundSource;
class KorvetFddControlRegister;
class KorvetHddRegisters;
class FdImage;
class TapeRedirector;
class RkTapeInHook;
class RkTapeOutHook;
class CloseFileHook;
class Ret8080Hook;
class KorvetRamDiskSelector;
class WavWriter;


class KorvetGraphicsAdapter;
class KorvetTextAdapter;

class KorvetRenderer : public CrtRenderer, public IActive, public SnapshotSerializable
{
    public:
        KorvetRenderer();
        ~KorvetRenderer();

        void renderFrame() override;

        // derived from EmuObject

        // derived from CrtRenderer
        void toggleCropping() override;
        void toggleColorMode() override;

        // derived from ActiveDevice
        void operate() override;
        void init() override;

        void attachMemory(Ram* memory);
        void attachGraphicsAdapter(KorvetGraphicsAdapter* adapter) {m_graphicsAdapter = adapter;}
        void attachTextAdapter(KorvetTextAdapter* adapter) {m_textAdapter = adapter;}
        void setDisplayPage(uint8_t page) {m_displayPage = page & 3;}
        void setFontNumber(uint8_t fontNumber) {m_fontNumber = fontNumber & 1;}
        void setWideCharMode(bool wideCharMode) {m_wideCharMode = wideCharMode;}
        void setLutValue(uint8_t index, uint8_t value) {m_korvetLut[index & 0x0F] = value & 0x0F;}
        void setVisibleArea(bool visible) {m_showBorder = visible;}

        // Текущее состояние для отметок в меню.
        bool getColorMode() const {return m_colorMode;}
        bool getCroppedToVisible() const {return !m_showBorder;}

        // Бит VBL (portA бит 1): находится ли луч в видимом поле в текущий такт.
        // Вычисляется по позиции луча, т.к. operate() тикает раз в кадр.
        bool isDisplayActive() const;

        void setBorderColor(uint8_t color);
        void set512pxMode(bool mode512);
        void setLineOffset(uint8_t lineOffset);
        void setPaletteColor(uint8_t color);
        void vidMemWriteNotify();

        uint32_t snapshotSectionId() const override;
        uint16_t snapshotSectionVersion() const override;
        bool saveState(SnapshotWriter& writer) const override;
        bool loadState(SnapshotReader& reader, uint16_t version) override;
        void postLoad() override;


    private:
        const uint8_t c_bwMap[256] = {
            0, 13, 20, 34, 42, 56, 63, 76, 20, 34, 41, 54, 63, 76, 83, 97,
            42, 56, 63, 76, 85, 98, 105, 118, 63, 76, 83, 97, 105, 118, 126, 139,
            82, 96, 103, 116, 125, 138, 145, 158, 103, 116, 123, 137, 145, 158, 166, 179,
            125, 138, 145, 158, 167, 180, 187, 201, 145, 158, 166, 179, 187, 201, 208, 221,
            13, 27, 34, 47, 56, 69, 76, 89, 34, 47, 54, 68, 76, 89, 97, 110,
            56, 69, 76, 89, 98, 111, 118, 132, 76, 89, 97, 110, 118, 132, 139, 152,
            96, 109, 116, 129, 138, 151, 158, 172, 116, 129, 137, 150, 158, 172, 179, 192,
            138, 151, 158, 172, 180, 194, 201, 214, 158, 172, 179, 192, 201, 214, 221, 235,
            20, 34, 41, 54, 63, 76, 83, 97, 41, 54, 61, 75, 83, 97, 104, 117,
            63, 76, 83, 97, 105, 118, 126, 139, 83, 97, 104, 117, 126, 139, 146, 159,
            103, 116, 123, 137, 145, 158, 166, 179, 123, 137, 144, 157, 166, 179, 186, 199,
            145, 158, 166, 179, 187, 201, 208, 221, 166, 179, 186, 199, 208, 221, 228, 242,
            34, 47, 54, 68, 76, 89, 97, 110, 54, 68, 75, 88, 97, 110, 117, 130,
            76, 89, 97, 110, 118, 132, 139, 152, 97, 110, 117, 130, 139, 152, 159, 173,
            116, 129, 137, 150, 158, 172, 179, 192, 137, 150, 157, 170, 179, 192, 199, 213,
            158, 172, 179, 192, 201, 214, 221, 235, 179, 192, 199, 213, 221, 235, 242, 255
            // .02-version
            /*0, 17, 22, 40, 37, 54, 59, 76, 21, 38, 43, 60, 57, 75, 80, 97,
            40, 58, 63, 80, 77, 94, 100, 117, 61, 78, 84, 101, 98, 115, 120, 138,
            81, 98, 103, 121, 118, 135, 140, 157, 102, 119, 124, 141, 138, 156, 161, 178,
            121, 138, 144, 161, 158, 175, 180, 198, 142, 159, 164, 182, 179, 196, 201, 218,
            16, 33, 38, 56, 53, 70, 75, 92, 37, 54, 59, 76, 73, 91, 96, 113,
            56, 73, 79, 96, 93, 110, 115, 133, 77, 94, 99, 117, 114, 131, 136, 153,
            97, 114, 119, 136, 133, 151, 156, 173, 117, 135, 140, 157, 154, 171, 177, 194,
            137, 154, 160, 177, 174, 191, 196, 214, 158, 175, 180, 198, 195, 212, 217, 234,
            21, 38, 43, 60, 57, 75, 80, 97, 41, 59, 64, 81, 78, 95, 101, 118,
            61, 78, 84, 101, 98, 115, 120, 138, 82, 99, 104, 122, 119, 136, 141, 158,
            102, 119, 124, 141, 138, 156, 161, 178, 122, 140, 145, 162, 159, 176, 182, 199,
            142, 159, 164, 182, 179, 196, 201, 218, 163, 180, 185, 202, 199, 217, 222, 239,
            37, 54, 59, 76, 73, 91, 96, 113, 57, 75, 80, 97, 94, 111, 117, 134,
            77, 94, 99, 117, 114, 131, 136, 153, 98, 115, 120, 137, 134, 152, 157, 174,
            117, 135, 140, 157, 154, 171, 177, 194, 138, 155, 161, 178, 175, 192, 197, 215,
            158, 175, 180, 198, 195, 212, 217, 234, 179, 196, 201, 218, 215, 233, 238, 255*/
        };

#ifndef PICO_RP2040
        uint8_t* m_frameBuf = nullptr;
#endif
        const uint8_t* m_screenMemory;
        KorvetGraphicsAdapter* m_graphicsAdapter = nullptr;
        KorvetTextAdapter* m_textAdapter = nullptr;
        uint8_t m_displayPage = 0;
        uint8_t m_fontNumber = 0;
        bool m_wideCharMode = false;
        uint8_t m_korvetLut[16] = {};

        bool m_showBorder = false;
        bool m_colorMode = true;

        uint8_t m_lineOffset = 0xFF;
        uint8_t m_latchedLineOffset = 0xFF;
        bool m_lineOffsetIsLatched = false;
        uint8_t m_borderColor = 0;
        bool m_mode512px = false;
        uint8_t m_colorPalette[16];
        uint8_t m_bwPalette[16];
        const uint8_t* m_palette = m_colorPalette;
        int m_lastColor = 0;

        unsigned m_ticksPerPixel;

        uint64_t m_curFrameClock;
        int m_curFramePixel;

        void setColorMode(bool colorMode);
        void prepareFrame();
        void applyFrameBuffer();
#ifndef PICO_RP2040
        void renderKorvetFrame();
        void renderLine(int nLine, int firstPx, int LastPx, uint8_t* linePtr);
#endif
        void advanceTo(uint64_t clocks);
};




constexpr uint16_t SNAPSHOT_STATE_FORMAT_VERSION = 2;

constexpr uint32_t makeSnapshotSectionId(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

#pragma pack(push, 1)
struct SnapshotFileHeaderV2 {
    char magic[8];
    uint16_t formatVersion;
    uint16_t headerSize;
    uint32_t sectionCount;
    char firmwareVersion[32];
    uint8_t reserved[16];
};

struct SnapshotSectionHeaderV2 {
    uint32_t id;
    uint16_t version;
    uint16_t headerSize;
    uint32_t size;
};
#pragma pack(pop)

class SnapshotWriter {
public:
    explicit SnapshotWriter(FIL& file);

    bool good() const;
    bool write(const void* data, uint32_t size);
    bool skip(uint32_t size);

    template<typename T>
    bool writeValue(const T& value)
    {
        return write(&value, sizeof(value));
    }

    bool beginSection(uint32_t id, uint16_t version);
    bool endSection();

private:
    FIL& m_file;
    bool m_good = true;
    bool m_sectionOpen = false;
    FSIZE_t m_sectionHeaderPos = 0;
    FSIZE_t m_sectionDataPos = 0;
    SnapshotSectionHeaderV2 m_sectionHeader{};
};

class SnapshotReader {
public:
    explicit SnapshotReader(FIL& file);

    bool good() const;
    bool read(void* data, uint32_t size);

    template<typename T>
    bool readValue(T& value)
    {
        return read(&value, sizeof(value));
    }

    bool beginSection(SnapshotSectionHeaderV2& section);
    bool endSection();
    bool skip(uint32_t size);
    uint32_t remaining() const;

private:
    FIL& m_file;
    bool m_good = true;
    bool m_sectionOpen = false;
    FSIZE_t m_sectionEnd = 0;
};

enum KorvetCpuType {
    VECTOR_CPU_8080 = 0,
    VECTOR_CPU_Z80  = 1
};

enum class KorvetFloppyDrive : uint8_t {
    A = 0,
    B = 1,
    C = 2,
    D = 3
};


class KorvetCore : public SnapshotSerializable
{
    public:
        KorvetCore();
        ~KorvetCore();

        void init();
        void shutdown();
        void reset();
        void coldReinitialize();

        void sysReq(SysReq sr);
        void processKey(PalKeyCode keyCode, bool isPressed, unsigned unicodeKey = 0);
        void resetKeys();
        bool loadFile(const std::string& fileName, bool run = true);
        bool saveSnapshot(unsigned slot);
        bool removeSnapshot(unsigned slot);

        enum class SnapshotLoadResult {
            Ok,
            InvalidSlot,
            NotFound,
            IoError,
            InvalidFile,
            IncompatibleFormat
        };
        struct SnapshotInfo {
            bool present = false;
            uint16_t formatVersion = 0;
            std::string firmwareVersion;
        };
        bool readSnapshotInfo(unsigned slot, SnapshotInfo& info) const;
        SnapshotLoadResult loadSnapshot(unsigned slot,
                                        std::string* firmwareVersion = nullptr,
                                        uint16_t* fileFormatVersion = nullptr);
        static uint16_t snapshotFormatVersion();

        uint32_t snapshotSectionId() const override;
        uint16_t snapshotSectionVersion() const override;
        bool saveState(SnapshotWriter& writer) const override;
        bool loadState(SnapshotReader& reader, uint16_t version) override;
        void postLoad() override;

        Cpu8080Compatible* getCpu();

        // Смена ядра на ходу: старое разрушается, новое создаётся в том же
        // буфере и получает всю обвязку заново. Вызывающий обязан после этого
        // выполнить полный сброс машины.
        KorvetCpuType getCpuType() const;
        void setCpuType(KorvetCpuType type);

        // Состояние для пунктов меню (отметки/радиогруппы), зеркалящих горячие
        // клавиши. Раскладка: 0=QWERTY, 1=ЙЦУКЕН, 2=Smart.
        bool getColorMode() const;
        bool getCroppedToVisible() const;
        uint8_t getVideoDisplayPage() const;
        uint8_t getVideoFontNumber() const;
        bool getVideoWideCharMode() const;
        uint16_t getCpuPc() const;
        uint16_t getCpuAf() const;
        uint16_t getCpuBc() const;
        uint16_t getCpuDe() const;
        uint16_t getCpuHl() const;
        uint16_t getCpuSp() const;
        uint8_t getCpuMemoryByte(uint16_t addr) const;
        uint8_t getMemoryConfig() const;
        int getKbdLayoutModeIndex() const;

        // Аппаратные сбросы Вектор-06Ц, дублирующие клавиши F11/F12, чтобы их
        // можно было вызвать из меню:
        //   resetTurnOnRom  — F11 (БЛК+ВВОД): сброс, ПЗУ включено (в монитор);
        //   resetTurnOffRom — F12 (БЛК+СБР):  сброс, ПЗУ выключено (из ОЗУ).
        void resetTurnOnRom();
        void resetTurnOffRom();
        unsigned getCpuFrequency() const {return m_cpuFrequency;}
        void setCpuFrequency(unsigned frequency);
        Keyboard* getKeyboard();
        KorvetAddrSpace* getAddrSpace() {return m_addrSpace;}
        bool assignDiskAFileName(const std::string& fileName, bool readOnly = false);
        bool floppyImagePresent(KorvetFloppyDrive drive) const;
        bool floppyImageReadOnly(KorvetFloppyDrive drive) const;
        bool floppyReadOnlyMode(KorvetFloppyDrive drive) const;
        bool canSetFloppyReadOnly(KorvetFloppyDrive drive, bool readOnly) const;
        void setFloppyReadOnly(KorvetFloppyDrive drive, bool readOnly);
        std::string getFloppyFileName(KorvetFloppyDrive drive) const;
        void chooseFloppyImage(KorvetFloppyDrive drive);
        void ejectFloppyImage(KorvetFloppyDrive drive);
        bool hddImagePresent() const;
        std::string getHddFileName() const;
        void chooseHddImage();
        void ejectHddImage();

        bool tapeHooksEnabled() const;
        void setTapeHooksEnabled(bool enabled);
        void chooseTapeInput();
        void chooseTapeOutput();
        void ejectTapeFiles();
        bool tapeFilePresent() const;
        std::string getTapeInputFileName() const;
        std::string getTapeOutputFileName() const;

        bool ramDiskEnabled(int diskNum) const;
        void setRamDiskEnabled(int diskNum, bool enabled);

        void vrtc(bool isActive);
        void hrtc(bool isActive);
        void int4(bool isActive);
        void inte(bool isActive);
        void tapeOut(bool isActive) {m_tapeOut = isActive;}
        bool getTapeOut() const {return m_tapeOut;}
        WavWriter* getWavWriter() {return m_wavWriter;}
        bool getPsgEnabled() const;
        void setPsgEnabled(bool enabled);
        bool getPsgStereo() const;
        void setPsgStereo(bool stereo);
        bool getPsgAcbOrder() const;
        void setPsgAcbOrder(bool acbOrder);
        bool getHddEnabled() const;
        void setHddEnabled(bool enabled);

    private:
        Ram* m_ram = nullptr;
        Rom* m_rom = nullptr;
        Rom* m_rom2 = nullptr;
        Rom* m_rom3 = nullptr;
        bool m_z80_installed = false;
        Cpu8080Compatible* m_cpu = nullptr;
        unsigned m_cpuFrequency = 2500000;
        KorvetAddrSpace* m_addrSpace = nullptr;
        KorvetAddrSpaceSelector* m_addrSpaceSelector = nullptr;
        AddrSpace* m_ioAddrSpace = nullptr;
        KorvetRenderer* m_renderer = nullptr;
        KorvetVideoPpiCircuit* m_videoPpiCircuit = nullptr;
        KorvetKeyboard* m_keyboard = nullptr;
        KorvetKbdLayout* m_kbdLayout = nullptr;
        KbdTapper* m_kbdTapper = nullptr;
        KorvetPpi8255Circuit* m_ppiCircuit = nullptr;
        GeneralSoundSource* m_tapeSoundSource = nullptr;
        Ppi8255* m_ppi = nullptr;
        KorvetColorRegister* m_colorReg = nullptr;
        Covox* m_covox = nullptr;
        KorvetPpi8255Circuit2* m_covoxCircuit = nullptr;
        Ppi8255* m_ppi2 = nullptr;
        Pit8253* m_pit = nullptr;
        Pit8253SoundSource* m_sndSource = nullptr;
        Psg3910* m_ay = nullptr;
        Psg3910SoundSource* m_psgSoundSource = nullptr;
        Fdc1793* m_fdc = nullptr;
        KorvetFddControlRegister* m_fddReg = nullptr;
        AtaDrive* m_ataDrive = nullptr;
        KorvetHddRegisters* m_hddRegisters = nullptr;
        FdImage* m_diskA = nullptr;
        FdImage* m_diskB = nullptr;
        FdImage* m_diskC = nullptr;
        FdImage* m_diskD = nullptr;
        bool m_floppyReadOnlyMode[4] = {false, false, false, false};
        DiskImage* m_hdd = nullptr;
        KorvetFileLoader* m_loader = nullptr;
        TapeRedirector* m_tapeInFile = nullptr;
        TapeRedirector* m_tapeOutFile = nullptr;
        WavWriter* m_wavWriter = nullptr;
        RkTapeInHook* m_tapeInHookBas = nullptr;
        RkTapeOutHook* m_tapeOutHookBas = nullptr;
        CloseFileHook* m_closeFileHookBas = nullptr;
        RkTapeInHook* m_tapeInHookMon = nullptr;
        RkTapeOutHook* m_tapeOutHookMon = nullptr;
        Ret8080Hook* m_skipHookMon = nullptr;
        CloseFileHook* m_closeFileHookMon = nullptr;
        RkTapeInHook* m_tapeInHookEmuRk = nullptr;
        RkTapeOutHook* m_tapeOutHookEmuRk = nullptr;
        CloseFileHook* m_closeFileHookEmuRk = nullptr;
        SRam* m_ramDiskMem = nullptr;
        RamDisk* m_ramDisk = nullptr;
        KorvetRamDiskSelector* m_ramDiskSelector = nullptr;
        SRam* m_ramDiskMem2 = nullptr;
        RamDisk* m_ramDisk2 = nullptr;
        KorvetRamDiskSelector* m_ramDiskSelector2 = nullptr;
        CpuHook* m_tapeHooks[10] = {};

        bool m_intReq = false;
        bool m_intsEnabled = false;
        bool m_curVrtc = false;
        bool m_curHrtc = false;
        bool m_tapeOut = false;
};

class KorvetAddrSpace : public AddressableDevice, public SnapshotSerializable
{
    public:

        void reset() override;

        void writeByte(int addr, uint8_t value) override;
        uint8_t readByte(int addr) override;

        void attachRam(Ram* mem) {m_mainMemory = mem; rebuildPageMap();}
        void attachRom(Rom* rom) {m_rom = rom; rebuildPageMap();}
        void attachCpu(Cpu8080Compatible* cpu) {m_cpu = cpu; rebuildPageMap();}
        void attachSelector(KorvetAddrSpaceSelector* selector) {m_addrSpaceSelector = selector;}
        void setPage(int pageNum, AddressableDevice* page) {m_pages[pageNum] = page;}
        void attachRamDisk(int diskNum, SRam* ramDisk);
        void attachCrtRenderer(KorvetRenderer* crtRenderer) {m_crtRenderer = crtRenderer; rebuildPageMap();}
        void enableRom();
        void disableRom();

        // Перестроить быструю карту страниц в CPU по текущей конфигурации памяти.
        // Вызывается из всех операций, меняющих раскладку; на горячем пути не лежит.
        void rebuildPageMap();
        void ramDiskControl(int diskNum, int inRamPagesMask, bool stackEnabled, int inRamPage, int stackPage);
        void eramControl(int eramSegment, int eramPageStartAddr, int eramPageEndAddr);

        uint32_t snapshotSectionId() const override;
        uint16_t snapshotSectionVersion() const override;
        bool saveState(SnapshotWriter& writer) const override;
        bool loadState(SnapshotReader& reader, uint16_t version) override;
        void postLoad() override;


    private:
        Ram* m_mainMemory = nullptr;
        Rom* m_rom = nullptr;
        SRam* m_ramDisk = nullptr;
        SRam* m_ramDisk2 = nullptr;
        Cpu8080Compatible* m_cpu = nullptr;
        KorvetRenderer* m_crtRenderer = nullptr;
        KorvetAddrSpaceSelector* m_addrSpaceSelector = nullptr;
        AddressableDevice* m_pages[9] = {};

        bool m_romEnabled = true;

        int m_inRamPagesMask = 0;
        bool m_stackDiskEnabled = false;
        int m_inRamDiskPage = 0;
        int m_stackDiskPage = 0;

        int m_inRamPagesMask2 = 0;
        bool m_stackDiskEnabled2 = false;
        int m_inRamDiskPage2 = 0;
        int m_stackDiskPage2 = 0;

        int m_eramSegment = 0;
        uint16_t m_eramPageStartAddr = 0xA000;
        uint16_t m_eramPageEndAddr = 0xDFFF;
        bool m_eram = false;

        // Пересекается ли страница [first..last] с окном RAM-диска
        bool pageHitsRamDisk(int first, int last) const;
};


class KorvetAddrSpaceSelector : public AddressableDevice
{
    public:
        void reset() override {m_memCfg = 0;}
        void writeByte(int, uint8_t value) override {m_memCfg = value & 0x7C;}
        uint8_t getMemoryConfig() const {return m_memCfg;}

    private:
        uint8_t m_memCfg = 0;
};


class KorvetFileLoader : public EmuObject
{
    public:
        bool loadFile(const std::string& fileName, bool run = false, bool readOnly = false);
        bool chooseAndLoadFile(bool run = false);
        void attachAddrSpace(Ram* addrSpace) {m_addrSpace = addrSpace;}
        void setFilter(const std::string& filter) {m_filter = filter;}

    private:
        Ram* m_addrSpace = nullptr;
        std::string m_filter;
        int m_skipTicks = 2000000;
};


class KorvetKeyboard : public Keyboard
{
    public:
        KorvetKeyboard();

        void resetKeys() override;
        void processKey(EmuKey key, bool isPressed) override;

        void setMatrix1Mask(uint8_t mask) {m_mask1 = mask;}
        void setMatrix2Mask(uint8_t mask) {m_mask2 = mask;}
        uint8_t getMatrix1Data();
        uint8_t getMatrix2Data();

        // Compatibility for the disabled Vector PPI circuit.
        void setMatrixMask(uint8_t mask) {m_mask1 = static_cast<uint8_t>(~mask);}
        uint8_t getMatrixData() {return static_cast<uint8_t>(~getMatrix1Data());}
        uint8_t getCtrlKeys() const {return 0xFF;}

    private:
        const EmuKey m_keyMatrix1[8][8] = {
            { EK_AT,    EK_A,     EK_B,     EK_C,         EK_D,       EK_E,        EK_F,      EK_G      },
            { EK_H,     EK_I,     EK_J,     EK_K,         EK_L,       EK_M,        EK_N,      EK_O      },
            { EK_P,     EK_Q,     EK_R,     EK_S,         EK_T,       EK_U,        EK_V,      EK_W      },
            { EK_X,     EK_Y,     EK_Z,     EK_LBRACKET,  EK_BKSLASH, EK_RBRACKET, EK_CARET,  EK_UNDSCR },
            { EK_0,     EK_1,     EK_2,     EK_3,         EK_4,       EK_5,        EK_6,      EK_7      },
            { EK_8,     EK_9,     EK_COLON, EK_SEMICOLON, EK_COMMA,   EK_MINUS,    EK_PERIOD, EK_SLASH  },
            { EK_CR,    EK_CLEAR, EK_STOP,  EK_INS,       EK_DEL,     EK_BSP,      EK_TAB,    EK_SPACE  },
            { EK_SHIFT, EK_LANG,  EK_GRAPH, EK_ESC,       EK_SEL,     EK_CTRL,     EK_FIX,    EK_SHIFT  }
        };

        const EmuKey m_keyMatrix2[3][8] = {
            { EK_PHOME, EK_SHOME, EK_DOWN, EK_SEND, EK_LEFT, EK_MENU, EK_RIGHT, EK_HOME },
            { EK_UP,    EK_END,   EK_NONE, EK_NONE, EK_NONE, EK_NONE, EK_PEND,  EK_NONE },
            { EK_F1,    EK_F2,    EK_F3,   EK_F4,   EK_F5,   EK_NONE, EK_NONE,  EK_NONE }
        };

        uint8_t m_keys1[8];
        uint8_t m_keys2[3];
        uint8_t m_mask1;
        uint8_t m_mask2;
};

class KorvetKeyboardRegisters : public AddressableDevice
{
    public:
        void attachKeyboard(KorvetKeyboard* keyboard) {m_keyboard = keyboard;}
        void writeByte(int, uint8_t) override {}
        uint8_t readByte(int addr) override;

    private:
        KorvetKeyboard* m_keyboard = nullptr;
};


class KorvetPpi8255Circuit : public Ppi8255Circuit
{
    public:

        // derived from Ppi8255Circuit
        uint8_t getPortB() override; // port 02
        uint8_t getPortC() override; // port 01
        void setPortA(uint8_t value) override; // port 03
        void setPortB(uint8_t value) override; // port 02
        void setPortC(uint8_t value) override; // port 01

        void attachKeyboard(KorvetKeyboard* kbd) {m_kbd = kbd;}
        void attachRenderer(KorvetRenderer* renderer) {m_renderer = renderer;}
        void attachTapeSoundSource(GeneralSoundSource* source) {m_tapeSoundSource = source;}


    private:
        // Источник звука - вывод на магнитофон
        GeneralSoundSource* m_tapeSoundSource;

        KorvetKeyboard* m_kbd = nullptr;
        KorvetRenderer* m_renderer = nullptr;
};


class KorvetPit8253SoundSource : public Pit8253SoundSource
{
    public:
        int calcValue() override;
        void tuneupPit() override;
        void setGate(bool gate);

    private:
        void updateStats();

        bool m_gate = false;
        int m_sumValue = 0;
};


class KorvetPpi8255Circuit2 : public Ppi8255Circuit
{
    public:

        // derived from Ppi8255Circuit
        void setPortA(uint8_t value) override; // port 03
        uint8_t getPortA() override {return 0x00;} // dummy USPID joystick
        uint8_t getPortC() override {return 0x0E;} // unset printer busy bit
        void setPortC(uint8_t value) override; // port 03

        void attachCovox(Covox* covox) {m_covox = covox;}
        void attachPitSoundSource(KorvetPit8253SoundSource* source) {m_pitSoundSource = source;}


    private:
        // Источник звука - ковокс
        Covox* m_covox = nullptr;
        KorvetPit8253SoundSource* m_pitSoundSource = nullptr;

        uint8_t m_printerData = 0;
        bool m_printerStrobe = true;
};


class KorvetPpiPsgAdapter : public Ppi8255Circuit
{
    public:
        void attachPsg(Psg3910* psg) {m_psg = psg;}

        uint8_t getPortA() override;
        void setPortA(uint8_t value) override;
        void setPortB(uint8_t value) override;

    private:
        Psg3910* m_psg = nullptr;
        bool m_strobe = false;
        uint8_t m_read = 0;
        uint8_t m_write = 0;
};


class KorvetColorRegister : public AddressableDevice
{
    public:

        void attachRenderer(KorvetRenderer* renderer) {m_renderer = renderer;}
        void attachGraphicsAdapter(KorvetGraphicsAdapter* adapter) {m_graphicsAdapter = adapter;}

        void writeByte(int addr, uint8_t value) override;


    private:
        KorvetRenderer* m_renderer = nullptr;
        KorvetGraphicsAdapter* m_graphicsAdapter = nullptr;
};



class KorvetKbdLayout : public KbdLayout
{
    protected:
        EmuKey translateKey(PalKeyCode keyCode) override;
        EmuKey translateUnicodeKey(unsigned unicodeKey, PalKeyCode keyCode, bool& shift, bool& lang) override;
        bool processSpecialKeys(PalKeyCode keyCode) override;

    private:
        bool m_downAsNumpad5 = false;
};


class KorvetRamDiskSelector : public AddressableDevice
{
    public:

        void attachKorvetAddrSpace(KorvetAddrSpace* vectorAddrSpace) {m_korvetAddrSpace = vectorAddrSpace;}
        void setDiskNum(int diskNum) {m_diskNum = diskNum;}
        bool getEnabled() const {return m_enabled;}
        void setEnabled(bool enabled);

        void writeByte(int, uint8_t value) override;
        uint8_t readByte(int)  override {return 0xff;}


    private:
        KorvetAddrSpace* m_korvetAddrSpace = nullptr;
        int m_diskNum = 0;
        bool m_enabled = true;
};


class KorvetFddControlRegister : public AddressableDevice
{
    public:

        inline void attachFdc1793(Fdc1793* fdc) {m_fdc = fdc;}

        void writeByte(int addr, uint8_t value) override;
        uint8_t readByte(int)  override {return 0xff;}


    private:
        Fdc1793* m_fdc = nullptr;
};


class KorvetHddRegisters : public AddressableDevice
{
    public:

        void attachAtaDrive(AtaDrive* ataDrive) {m_ataDrive = ataDrive;}
        bool getEnabled() const {return m_enabled;}
        void setEnabled(bool enabled);

        void writeByte(int addr, uint8_t value) override;
        uint8_t readByte(int) override;


    private:
        AtaDrive* m_ataDrive = nullptr;
        bool m_enabled = true;

        uint8_t m_highR = 0;
        uint8_t m_highW = 0;
};


#endif // VECTOR_H
