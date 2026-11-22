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

#ifndef SPECIALIST_H
#define SPECIALIST_H

#include <memory>

#include "CloseFileHook.h"
#include "DiskImage.h"
#include "Fdc1793.h"
#include "Pit8253.h"
#include "Pit8253Sound.h"
#include "Cpu8080.h"
#include "CpuZ80.h"
#include "EmuTypes.h"
#include "Keyboard.h"
#include "Ppi8255.h"
#include "Ppi8255Circuit.h"
#include "RkTapeHooks.h"
#include "SoundMixer.h"
#include "TapeRedirector.h"
#include "WavWriter.h"

enum class SpecCpuType { I8080, Z80 };
enum class SpecColorMode { Mono, Color4, Color8, Mx };
enum class SpecModel { Classic, Sp580, Mx };

class SpecRamDisk
{
    public:
        bool open();
        bool flush();
        uint8_t read(unsigned address);
        bool write(unsigned address, uint8_t value);
    private:
        bool cache(unsigned address);
        static constexpr unsigned blockSize = 4096;
        static constexpr unsigned diskSize = 8 * 65536;
        PalFile m_file;
        uint8_t m_data[blockSize] = {};
        unsigned m_block = diskSize;
        bool m_dirty = false;
};

class SpecTapeHook : public CpuHook
{
    public:
        enum class Operation { InputHeader, Input, OutputHeader, Output };
        SpecTapeHook(uint16_t addr, TapeRedirector& tape, Operation operation)
            : CpuHook(addr), m_operation(operation) {setTapeRedirector(&tape);}
        bool hookProc() override;
    private:
        Operation m_operation;
};

class SpecPitSound : public Pit8253SoundSource
{
    public:
        int calcValue() override;
        void setMode(SpecModel model);
        void setGate(bool gate);
    private:
        void updateMx();
        SpecModel m_model = SpecModel::Classic;
        bool m_gate = false;
        uint64_t m_total = 0;
        uint64_t m_high = 0;
};

class SpecKeyboard : public Keyboard
{
    public:
        void resetKeys() override;
        void processKey(EmuKey key, bool isPressed) override;
        void setVMatrixMask(uint16_t mask) {m_vMask = ~mask;}
        void setHMatrixMask(uint8_t mask) {m_hMask = ~mask;}
        uint8_t getVMatrixData();
        uint16_t getHMatrixData();
        void setModel(SpecModel model) {m_model = model;}
        bool getShift() const {return m_shift;}

    private:
        static constexpr EmuKey m_keyMatrix[12][6] = {
            { EK_CR,    EK_BSP,   EK_PERIOD,  EK_COLON,    EK_MINUS,     EK_CLEAR },
            { EK_LF,    EK_SLASH, EK_BKSLASH, EK_H,        EK_0,         EK_F11   },
            { EK_RIGHT, EK_COMMA, EK_V,       EK_Z,        EK_9,         EK_F10   },
            { EK_RPT,   EK_AT,    EK_D,       EK_RBRACKET, EK_8,         EK_F9    },
            { EK_LEFT,  EK_B,     EK_L,       EK_LBRACKET, EK_7,         EK_F8    },
            { EK_SPACE, EK_X,     EK_O,       EK_G,        EK_6,         EK_F7    },
            { EK_ESC,   EK_T,     EK_R,       EK_N,        EK_5,         EK_F6    },
            { EK_TAB,   EK_I,     EK_P,       EK_E,        EK_4,         EK_F5    },
            { EK_DOWN,  EK_M,     EK_A,       EK_K,        EK_3,         EK_F4    },
            { EK_UP,    EK_S,     EK_W,       EK_U,        EK_2,         EK_F3    },
            { EK_HOME,  EK_CARET, EK_Y,       EK_C,        EK_1,         EK_F2    },
            { EK_LANG,  EK_Q,     EK_F,       EK_J,        EK_SEMICOLON, EK_F1    }
        };

        static constexpr EmuKey m_keyMatrixMx[12][6] = {
            { EK_CR,    EK_BSP,   EK_PERIOD,  EK_COLON,    EK_MINUS,     EK_CLEAR },
            { EK_LF,    EK_SLASH, EK_BKSLASH, EK_H,        EK_0,         EK_F9    },
            { EK_RIGHT, EK_COMMA, EK_V,       EK_Z,        EK_9,         EK_F8    },
            { EK_TAB,   EK_AT,    EK_D,       EK_RBRACKET, EK_8,         EK_F7    },
            { EK_LEFT,  EK_B,     EK_L,       EK_LBRACKET, EK_7,         EK_F6    },
            { EK_SPACE, EK_X,     EK_O,       EK_G,        EK_6,         EK_F5    },
            { EK_NONE,  EK_T,     EK_R,       EK_N,        EK_5,         EK_F4    },
            { EK_NONE,  EK_I,     EK_P,       EK_E,        EK_4,         EK_F3    },
            { EK_DOWN,  EK_M,     EK_A,       EK_K,        EK_3,         EK_F2    },
            { EK_UP,    EK_S,     EK_W,       EK_U,        EK_2,         EK_F1    },
            { EK_HOME,  EK_CARET, EK_Y,       EK_C,        EK_1,         EK_F10   },
            { EK_LANG,  EK_Q,     EK_F,       EK_J,        EK_SEMICOLON, EK_ESC   }
        };

        static constexpr EmuKey m_keyMatrixSp580[12][6] = {
            { EK_CR,    EK_BSP,   EK_PERIOD,  EK_COLON,    EK_MINUS,     EK_CLEAR },
            { EK_LF,    EK_SLASH, EK_BKSLASH, EK_H,        EK_0,         EK_F10   },
            { EK_RIGHT, EK_COMMA, EK_V,       EK_Z,        EK_9,         EK_F11   },
            { EK_RPT,   EK_AT,    EK_D,       EK_RBRACKET, EK_8,         EK_F7    },
            { EK_LEFT,  EK_B,     EK_L,       EK_LBRACKET, EK_7,         EK_F6    },
            { EK_SPACE, EK_X,     EK_O,       EK_G,        EK_6,         EK_F5    },
            { EK_NONE,  EK_T,     EK_R,       EK_N,        EK_5,         EK_F4    },
            { EK_NONE,  EK_I,     EK_P,       EK_E,        EK_4,         EK_F3    },
            { EK_DOWN,  EK_M,     EK_A,       EK_K,        EK_3,         EK_F2    },
            { EK_UP,    EK_S,     EK_W,       EK_U,        EK_2,         EK_F1    },
            { EK_HOME,  EK_CARET, EK_Y,       EK_C,        EK_1,         EK_TAB   },
            { EK_LANG,  EK_Q,     EK_F,       EK_J,        EK_SEMICOLON, EK_ESC   }
        };

        SpecModel m_model = SpecModel::Classic;
        uint8_t m_vKeys[12] = {};
        uint16_t m_hKeys[6] = {};
        uint16_t m_vMask = 0;
        uint8_t m_hMask = 0;
        bool m_shift = false;
};

class SpecKbdLayout : public RkKbdLayout
{
    public:
        void setModel(SpecModel model) {m_model = model;}
    private:
        SpecModel m_model = SpecModel::Classic;
    protected:
        EmuKey translateKey(PalKeyCode keyCode) override;
        EmuKey translateUnicodeKey(unsigned unicodeKey, PalKeyCode, bool& shift, bool& lang) override
        {
            const auto key = translateCommonUnicodeKeys(unicodeKey, shift, lang);
            if (m_model == SpecModel::Sp580 && (key == EK_SEMICOLON || key == EK_AT))
                shift = !shift;
            return key;
        }
};

class SpecMemory : public AddressableDevice
{
    public:
        static constexpr unsigned romStart = 0xc000;
        static constexpr unsigned videoStart = 0x9000;
        static constexpr unsigned videoSize = romStart - videoStart;
        static constexpr unsigned width = 384;
        static constexpr unsigned height = 256;

        SpecMemory(Ppi8255& expansion, Ppi8255& keyboard, Pit8253& pit, Fdc1793& fdc)
            : m_expansion(expansion), m_keyboard(keyboard), m_pit(pit), m_fdc(fdc) {}
        void setModel(SpecModel model);
        SpecModel getModel() const {return m_model;}
        bool loadRom(const std::string& path, bool high);
        void restoreRom();
        void selectMxPage(unsigned page);
        uint8_t readMxRamDisk(unsigned addr) {return m_ramDisk.read(addr);}
        bool writeMxRamDisk(unsigned addr, uint8_t value) {return m_ramDisk.write(addr, value);}
        bool openMxRamDisk() {return m_ramDisk.open();}
        bool flushMxRamDisk() {return m_ramDisk.flush();}
        void mapCpuPages(Cpu& cpu);
        uint8_t readByte(int addr) override;
        void writeByte(int addr, uint8_t value) override;
        void setColorLatch(uint8_t value);
#ifndef SPECIALIST_SCANLINES
        uint32_t takeDirtyRows(unsigned block);
        void invalidateVideo();
#endif
        void reset() override;

        const uint8_t* video() const {return m_ram + videoStart;}
        const uint8_t* colors() const {return m_colors;}

    private:
        Ppi8255& m_expansion;
        Ppi8255& m_keyboard;
        Pit8253& m_pit;
        Fdc1793& m_fdc;
        SpecRamDisk m_ramDisk;
        SpecModel m_model = SpecModel::Classic;
        Cpu* m_cpu = nullptr;
        unsigned m_mxPage = 0;
        std::unique_ptr<uint8_t[]> m_romOverride;
        std::unique_ptr<uint8_t[]> m_highRomOverride;
        const uint8_t* m_rom = nullptr;
        const uint8_t* m_highRom = nullptr;
        unsigned m_romSize = 0;
        uint8_t m_ram[0xffe0] = {};
        uint8_t m_colors[videoSize] = {};
#ifndef SPECIALIST_SCANLINES
        uint32_t m_dirtyRows[height / 32] = {};
#endif
        uint8_t m_color = 0;
        void writeVideo(unsigned addr, uint8_t value);
        void mapPages();
        void setBuiltInRom();
};

class SpecRenderer : public ActiveDevice
{
    public:
        explicit SpecRenderer(SpecMemory& memory) : m_memory(memory) {}
        void init() override;
        void operate() override;
        void redraw();
        SpecColorMode getColorMode() const
        {
            return static_cast<SpecColorMode>(__atomic_load_n(&m_mode, __ATOMIC_RELAXED));
        }
        void setColorMode(SpecColorMode mode);

    private:
        void renderRow(unsigned row, uint32_t* dst) const;
#ifdef SPECIALIST_SCANLINES
        static const uint8_t* scanline(void* context, unsigned row);
#else
        void renderFrame();
#endif
        SpecMemory& m_memory;
        uint8_t m_mode = static_cast<uint8_t>(SpecColorMode::Color8);
};

class SpecPpiCircuit : public Ppi8255Circuit
{
    public:
        SpecPpiCircuit(SpecKeyboard& keyboard, SpecMemory& memory,
                       GeneralSoundSource& speaker, GeneralSoundSource& tape, SpecPitSound& pit)
            : m_keyboard(keyboard), m_memory(memory), m_speaker(speaker), m_tape(tape), m_pit(pit) {}
        uint8_t getPortA() override;
        uint8_t getPortB() override;
        uint8_t getPortC() override;
        void setPortA(uint8_t value) override;
        void setPortB(uint8_t value) override;
        void setPortC(uint8_t value) override;
        void setPortAMode(bool input) override;
        void setPortBMode(bool input) override;
        void setPortCLoMode(bool input) override;
        void reset() override;
        bool getTapeOut() const {return m_tapeOut;}

    private:
        SpecKeyboard& m_keyboard;
        SpecMemory& m_memory;
        GeneralSoundSource& m_speaker;
        GeneralSoundSource& m_tape;
        SpecPitSound& m_pit;
        uint16_t m_kbdMask = 0xfff;
        bool m_portAInput = true;
        bool m_portBInput = true;
        bool m_portCLoInput = true;
        bool m_tapeOut = false;
};

class SpecialistCore
{
    public:
        SpecialistCore();
        ~SpecialistCore();
        void init();
        void reset();
        void processKey(PalKeyCode key, bool pressed, unsigned unicode);
        void resetKeys();
        void sysReq(SysReq request);

        Cpu8080Compatible* getCpu() const {return m_cpu;}
        SpecCpuType getCpuType() const {return m_cpuType;}
        void setCpuType(SpecCpuType type);
        SpecModel getModel() const {return m_memory.getModel();}
        bool setModel(SpecModel model);
        bool flushStorage() {return m_memory.flushMxRamDisk();}
        void chooseRom(bool high = false);
        void restoreRom();
        void mountFloppy(unsigned drive);
        void ejectFloppy(unsigned drive);
        const std::string& floppyPath(unsigned drive) const {return m_disks[drive].getFileName();}
        unsigned getCpuFrequency() const {return m_cpuFrequency;}
        void setCpuFrequency(unsigned frequency);
        Keyboard* getKeyboard() {return &m_keyboard;}
        int getKbdLayoutModeIndex() const;

        SpecColorMode getColorMode() const {return m_renderer.getColorMode();}
        void setColorMode(SpecColorMode mode)
        {
            m_renderer.setColorMode(getModel() == SpecModel::Mx ? SpecColorMode::Mx
                : mode == SpecColorMode::Mx ? SpecColorMode::Color8 : mode);
        }
        void redrawVideo() {m_renderer.redraw();}

        bool tapeHooksEnabled() const {return m_tapeHooksEnabled;}
        void setTapeHooksEnabled(bool enabled);
        void chooseTapeInput() {m_tapeIn.openFile();}
        void chooseTapeOutput() {m_tapeOut.openFile();}
        void ejectTapeFiles();
        bool tapeFilePresent() const {return m_tapeIn.hasFile() || m_tapeOut.hasFile();}
        const std::string& getTapeInputFileName() const {return m_tapeIn.getFileName();}
        const std::string& getTapeOutputFileName() const {return m_tapeOut.getFileName();}
        WavWriter* getWavWriter() {return &m_wavWriter;}
        bool getTapeOut() const {return m_ppiCircuit.getTapeOut();}

    private:
        void attachCpu();
        bool loadFile(const std::string& path, bool run);
        bool loadSp580(const std::string& path, bool run);
        bool loadMx(const std::string& path, bool run);
        bool loadProgram(PalFile& file, unsigned first, unsigned last, unsigned entry,
                         bool run, const std::string* continuation = nullptr, int nextBlock = -1);
        void configureTape();

        // Hooks outlive the CPU, including while its storage is reused.
        TapeRedirector m_tapeIn;
        TapeRedirector m_tapeOut;
        WavWriter m_wavWriter;
        RkTapeInHook m_tapeInHook{0xc377, 200};
        RkTapeOutHook m_tapeOutHook{0xc3d0};
        CloseFileHook m_closeHook{0xc337, &m_tapeIn, &m_tapeOut};
        CloseFileHook m_mxClose{0xce78, &m_tapeIn, &m_tapeOut};
        SpecTapeHook m_spInHead{0xc463, m_tapeIn, SpecTapeHook::Operation::InputHeader};
        SpecTapeHook m_spIn{0xc4d9, m_tapeIn, SpecTapeHook::Operation::Input};
        SpecTapeHook m_spOutHead{0xfe40, m_tapeOut, SpecTapeHook::Operation::OutputHeader};
        SpecTapeHook m_spOut{0xfe56, m_tapeOut, SpecTapeHook::Operation::Output};
        SpecTapeHook m_mxInHead{0xc66c, m_tapeIn, SpecTapeHook::Operation::InputHeader};
        SpecTapeHook m_mxIn{0xc6be, m_tapeIn, SpecTapeHook::Operation::Input};
        SpecTapeHook m_mxOutHead{0xc70f, m_tapeOut, SpecTapeHook::Operation::OutputHeader};
        SpecTapeHook m_mxOut{0xc728, m_tapeOut, SpecTapeHook::Operation::Output};
        static constexpr size_t cpuSize = sizeof(Cpu8080) > sizeof(CpuZ80)
            ? sizeof(Cpu8080) : sizeof(CpuZ80);
        alignas(Cpu8080) alignas(CpuZ80) uint8_t m_cpuStorage[cpuSize];
        Cpu8080Compatible* m_cpu;
        SpecCpuType m_cpuType = SpecCpuType::I8080;
        unsigned m_cpuFrequency = 2000000;
        bool m_tapeHooksEnabled = true;

        Ppi8255 m_expansionPpi;
        Ppi8255 m_keyboardPpi;
        Pit8253 m_pit;
        Fdc1793 m_fdc;
        FdImage m_disks[2] = {{80, 2, 5, 1024}, {80, 2, 5, 1024}};
        SpecMemory m_memory{m_expansionPpi, m_keyboardPpi, m_pit, m_fdc};
        SpecRenderer m_renderer{m_memory};
        SpecKeyboard m_keyboard;
        SpecKbdLayout m_layout;
        GeneralSoundSource m_speaker;
        GeneralSoundSource m_tapeSound;
        SpecPitSound m_pitSound;
        SpecPpiCircuit m_ppiCircuit{m_keyboard, m_memory, m_speaker, m_tapeSound, m_pitSound};
};

#endif
