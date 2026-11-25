#include <cstring>
#include <cstdarg>
#include <algorithm>
#include <atomic>

#include <pico/multicore.h>
#include <pico/stdlib.h>
#include <hardware/pio.h>
#include <hardware/i2c.h>
#include <hardware/vreg.h>
#include <hardware/sync.h>
#include <hardware/flash.h>
#include <hardware/watchdog.h>
#ifdef PICO_RP2350
#include <hardware/regs/qmi.h>
#include <hardware/exception.h>
#include <hardware/structs/qmi.h>
#endif
#include "audio.h"
#include "ff.h"
#ifdef KBDUSB
    #include "ps2kbd_mrmltr.h"
#else
    #include "ps2.h"
#endif

#if USE_NESPAD
#include "nespad.h"
#endif

#include "Pal.h"
#include "Emulation.h"
#include "pico/picoMenu.h"
#ifdef HDMI_DVI
#include "hdmi-dvi.h"
#endif

#pragma GCC optimize("Ofast")

struct semaphore vga_start_semaphore;

static FATFS fs;

struct input_bits_t {
    bool a: true;
    bool b: true;
    bool select: true;
    bool start: true;
    bool right: true;
    bool left: true;
    bool up: true;
    bool down: true;
};

input_bits_t gamepad1_bits = { false, false, false, false, false, false, false, false };
static input_bits_t gamepad2_bits = { false, false, false, false, false, false, false, false };


static PalKeyCode joystickMappings[2][PAL_JOY_CONTROL_COUNT] = {
    { PK_UP, PK_DOWN, PK_LEFT, PK_RIGHT, PK_LALT, PK_TAB, PK_SPACE, PK_ENTER },
    { PK_UP, PK_DOWN, PK_LEFT, PK_RIGHT, PK_LALT, PK_TAB, PK_SPACE, PK_ENTER }
};

PalKeyCode palGetJoystickMapping(unsigned joystick, unsigned control)
{
    if (joystick >= 2 || control >= PAL_JOY_CONTROL_COUNT)
        return PK_NONE;
    return joystickMappings[joystick][control];
}

void palSetJoystickMapping(unsigned joystick, unsigned control, PalKeyCode key)
{
    if (joystick < 2 && control < PAL_JOY_CONTROL_COUNT
        && key > PK_NONE && key <= PK_JOY_RIGHT)
        joystickMappings[joystick][control] = key;
}

void repeat_handler(void);

#define JPAD (Config::secondJoy == 3 ? back2joy2: joyPushData)

extern "C" bool handleScancode(const uint32_t ps2scancode) {
    static bool pause_detected = false;
    if (pause_detected) {
        pause_detected = false;
        if (ps2scancode == 0x1D) return true; // ignore next byte after 0x45, TODO: split with NumLock
    }
    if ( ((ps2scancode >> 8) & 0xFF) == 0xE0) { // E0 block
        uint8_t cd = ps2scancode & 0xFF;
        bool pressed = cd < 0x80;
        cd &= 0x7F;
        /**
        switch (cd) {
            case 0x5B: kbdPushData(PalKeyCode::PK_LCTRL, pressed); return true; /// L WIN
            case 0x1D: {
                if (Config::CursorAsJoy) joyPushData(PalKeyCode::PK_DPAD_ALTFIRE, pressed);
                kbdPushData(PalKeyCode::PK_RCTRL, pressed);
                return true;
            }
            case 0x38: kbdPushData(PalKeyCode::PK_RALT, pressed); return true;
            case 0x5C: {  /// R WIN
                if (Config::CursorAsJoy) joyPushData(PalKeyCode::PK_DPAD_ALTFIRE, pressed);
                kbdPushData(PalKeyCode::PK_RCTRL, pressed);
                return true;
            }
            case 0x5D: kbdPushData(PalKeyCode::PK_F1, pressed); return true; /// MENU
            case 0x37: kbdPushData(PalKeyCode::PK_PRINTSCREEN, pressed); return true;
            case 0x46: kbdPushData(PalKeyCode::PK_BREAK, pressed); return true;
            case 0x52: kbdPushData(PalKeyCode::PK_INSERT, pressed); return true;
            case 0x47: {
                joyPushData(PalKeyCode::PK_MENU_HOME, pressed);
                kbdPushData(PalKeyCode::PK_HOME, pressed);
                return true;
            }
            case 0x4F: kbdPushData(PalKeyCode::PK_END, pressed); return true;
            case 0x49: kbdPushData(PalKeyCode::PK_PAGEUP, pressed); return true;
            case 0x51: kbdPushData(PalKeyCode::PK_PAGEDOWN, pressed); return true;
            case 0x53: kbdPushData(PalKeyCode::PK_DELETE, pressed); return true;
            case 0x48: {
                if (Config::CursorAsJoy) joyPushData(PalKeyCode::PK_DPAD_UP, pressed);
                joyPushData(PalKeyCode::PK_MENU_UP, pressed);
                kbdPushData(PalKeyCode::PK_UP, pressed);
                return true;
            }
            case 0x50: {
                if (Config::CursorAsJoy) joyPushData(PalKeyCode::PK_DPAD_DOWN, pressed);
                joyPushData(PalKeyCode::PK_MENU_DOWN, pressed);
                kbdPushData(PalKeyCode::PK_DOWN, pressed);
                return true;
            }
            case 0x4B: {
                if (Config::CursorAsJoy) joyPushData(PalKeyCode::PK_DPAD_LEFT, pressed);
                joyPushData(PalKeyCode::PK_MENU_LEFT, pressed);
                kbdPushData(PalKeyCode::PK_LEFT, pressed);
                return true;
            }
            case 0x4D: {
                if (Config::CursorAsJoy) joyPushData(PalKeyCode::PK_DPAD_RIGHT, pressed);
                joyPushData(PalKeyCode::PK_MENU_RIGHT, pressed);
                kbdPushData(PalKeyCode::PK_RIGHT, pressed);
                return true;
            }
            case 0x35: kbdPushData(PalKeyCode::PK_SLASH, pressed); return true;
            case 0x1C: { // VK_KP_ENTER
                kbdPushData(Config::rightSpace ? PalKeyCode::PK_SPACE : PalKeyCode::PK_RETURN, pressed);
                return true;
            }
        }
        */
        return true;
    }
    uint8_t cd = ps2scancode & 0xFF;
    bool pressed = cd < 0x80;
    cd &= 0x7F;
    /*
    switch (cd) {
        case 0x1E: kbdPushData(PalKeyCode::PK_A, pressed); return true;
        case 0x30: kbdPushData(PalKeyCode::PK_B, pressed); return true;
        case 0x2E: kbdPushData(PalKeyCode::PK_C, pressed); return true;
        case 0x20: kbdPushData(PalKeyCode::PK_D, pressed); return true;
        case 0x12: kbdPushData(PalKeyCode::PK_E, pressed); return true;
        case 0x21: kbdPushData(PalKeyCode::PK_F, pressed); return true;
        case 0x22: kbdPushData(PalKeyCode::PK_G, pressed); return true;
        case 0x23: kbdPushData(PalKeyCode::PK_H, pressed); return true;
        case 0x17: kbdPushData(PalKeyCode::PK_I, pressed); return true;
        case 0x24: kbdPushData(PalKeyCode::PK_J, pressed); return true;
        case 0x25: kbdPushData(PalKeyCode::PK_K, pressed); return true;
        case 0x26: kbdPushData(PalKeyCode::PK_L, pressed); return true;
        case 0x32: kbdPushData(PalKeyCode::PK_M, pressed); return true;
        case 0x31: kbdPushData(PalKeyCode::PK_N, pressed); return true;
        case 0x18: kbdPushData(PalKeyCode::PK_O, pressed); return true;
        case 0x19: kbdPushData(PalKeyCode::PK_P, pressed); return true;
        case 0x10: kbdPushData(PalKeyCode::PK_Q, pressed); return true;
        case 0x13: kbdPushData(PalKeyCode::PK_R, pressed); return true;
        case 0x1F: kbdPushData(PalKeyCode::PK_S, pressed); return true;
        case 0x14: kbdPushData(PalKeyCode::PK_T, pressed); return true;
        case 0x16: kbdPushData(PalKeyCode::PK_U, pressed); return true;
        case 0x2F: kbdPushData(PalKeyCode::PK_V, pressed); return true;
        case 0x11: kbdPushData(PalKeyCode::PK_W, pressed); return true;
        case 0x2D: kbdPushData(PalKeyCode::PK_X, pressed); return true;
        case 0x15: kbdPushData(PalKeyCode::PK_Y, pressed); return true;
        case 0x2C: kbdPushData(PalKeyCode::PK_Z, pressed); return true;

        case 0x0B: kbdPushData(PalKeyCode::PK_0, pressed); return true;
        case 0x02: kbdPushData(PalKeyCode::PK_1, pressed); return true;
        case 0x03: kbdPushData(PalKeyCode::PK_2, pressed); return true;
        case 0x04: kbdPushData(PalKeyCode::PK_3, pressed); return true;
        case 0x05: kbdPushData(PalKeyCode::PK_4, pressed); return true;
        case 0x06: kbdPushData(PalKeyCode::PK_5, pressed); return true;
        case 0x07: kbdPushData(PalKeyCode::PK_6, pressed); return true;
        case 0x08: kbdPushData(PalKeyCode::PK_7, pressed); return true;
        case 0x09: kbdPushData(PalKeyCode::PK_8, pressed); return true;
        case 0x0A: kbdPushData(PalKeyCode::PK_9, pressed); return true;

        case 0x29: kbdPushData(PalKeyCode::PK_TILDE, pressed); return true;
        case 0x0C: kbdPushData(PalKeyCode::PK_MINUS, pressed); return true;
        case 0x0D: kbdPushData(PalKeyCode::PK_EQUALS, pressed); return true;
        case 0x2B: kbdPushData(PalKeyCode::PK_BACKSLASH, pressed); return true;
        case 0x1A: kbdPushData(PalKeyCode::PK_LEFTBRACKET, pressed); return true;
        case 0x1B: kbdPushData(PalKeyCode::PK_RIGHTBRACKET, pressed); return true;
        case 0x27: kbdPushData(PalKeyCode::PK_SEMICOLON, pressed); return true;
        case 0x28: kbdPushData(PalKeyCode::PK_QUOTE, pressed); return true;
        case 0x33: kbdPushData(PalKeyCode::PK_COMMA, pressed); return true;
        case 0x34: kbdPushData(PalKeyCode::PK_PERIOD, pressed); return true;
        case 0x35: kbdPushData(PalKeyCode::PK_SLASH, pressed); return true;

        case 0x0E: {
            joyPushData(PalKeyCode::PK_MENU_BS, pressed);
            kbdPushData(PalKeyCode::PK_BACKSPACE, pressed);
            return true;
        }
        case 0x39: {
            joyPushData(PalKeyCode::PK_MENU_ENTER, pressed);
            kbdPushData(PalKeyCode::PK_SPACE, pressed);
            return true;
        }
        case 0x0F: {
            if (Config::TABasfire1) JPAD(PalKeyCode::PK_DPAD_FIRE, pressed);
            kbdPushData(PalKeyCode::PK_TAB, pressed);
            return true;
        }
        case 0x3A: kbdPushData(PalKeyCode::PK_CAPSLOCK, pressed); return true; /// TODO: CapsLock
        case 0x2A: kbdPushData(PalKeyCode::PK_LSHIFT, pressed); return true;
        case 0x1D: kbdPushData(PalKeyCode::PK_LCTRL, pressed); return true;
        case 0x38: {
            if (Config::CursorAsJoy) JPAD(PalKeyCode::PK_DPAD_FIRE, pressed);
            kbdPushData(PalKeyCode::PK_LALT, pressed);
            return true;
        }
        case 0x36: kbdPushData(PalKeyCode::PK_RSHIFT, pressed); return true;
        case 0x1C: {
            joyPushData(PalKeyCode::PK_MENU_ENTER, pressed);
            kbdPushData(PalKeyCode::PK_RETURN, pressed);
            return true;
        }
        case 0x01: kbdPushData(PalKeyCode::PK_ESCAPE, pressed); return true;
        case 0x3B: kbdPushData(PalKeyCode::PK_F1, pressed); return true;
        case 0x3C: kbdPushData(PalKeyCode::PK_F2, pressed); return true;
        case 0x3D: kbdPushData(PalKeyCode::PK_F3, pressed); return true;
        case 0x3E: kbdPushData(PalKeyCode::PK_F4, pressed); return true;
        case 0x3F: kbdPushData(PalKeyCode::PK_F5, pressed); return true;
        case 0x40: kbdPushData(PalKeyCode::PK_F6, pressed); return true;
        case 0x41: kbdPushData(PalKeyCode::PK_F7, pressed); return true;
        case 0x42: kbdPushData(PalKeyCode::PK_F8, pressed); return true;
        case 0x43: kbdPushData(PalKeyCode::PK_F9, pressed); return true;
        case 0x44: kbdPushData(PalKeyCode::PK_F10, pressed); return true;
        case 0x57: kbdPushData(PalKeyCode::PK_F11, pressed); return true;
        case 0x58: kbdPushData(PalKeyCode::PK_F12, pressed); return true;

        case 0x46: kbdPushData(PalKeyCode::PK_SCROLLLOCK, pressed); return true; /// TODO:
        case 0x45: {
            kbdPushData(PalKeyCode::PK_PAUSE, pressed);
            pause_detected = pressed;
            return true;
        }
        case 0x37: {
            JPAD(PalKeyCode::PK_DPAD_START, pressed);
            kbdPushData(PalKeyCode::PK_KP_MULTIPLY, pressed);
            return true;
        }
        case 0x4A: {
            JPAD(PalKeyCode::PK_DPAD_SELECT, pressed);
            kbdPushData(PalKeyCode::PK_MINUS, pressed);
            return true;
        }
        case 0x4E: {
            JPAD(PalKeyCode::PK_DPAD_FIRE, pressed);
            kbdPushData(PalKeyCode::PK_PLUS, pressed);
            return true;
        }
        case 0x53: {
            JPAD(PalKeyCode::PK_DPAD_FIRE, pressed);
            kbdPushData(PalKeyCode::PK_KP_PERIOD, pressed);
            return true;
        }
        case 0x52: {
            JPAD(PalKeyCode::PK_DPAD_ALTFIRE, pressed);
            kbdPushData(PalKeyCode::PK_KP_0, pressed);
            return true;
        }
        case 0x4F: {
            JPAD(PalKeyCode::PK_DPAD_LEFT, pressed);
            JPAD(PalKeyCode::PK_DPAD_DOWN, pressed);
            kbdPushData(PalKeyCode::PK_KP_1, pressed);
            return true;
        }
        case 0x50: {
            JPAD(PalKeyCode::PK_DPAD_DOWN, pressed);
            kbdPushData(PalKeyCode::PK_KP_2, pressed);
            return true;
        }
        case 0x51: {
            JPAD(PalKeyCode::PK_DPAD_RIGHT, pressed);
            JPAD(PalKeyCode::PK_DPAD_DOWN, pressed);
            kbdPushData(PalKeyCode::PK_KP_3, pressed);
            return true;
        }
        case 0x4B: {
            JPAD(PalKeyCode::PK_DPAD_LEFT, pressed);
            kbdPushData(PalKeyCode::PK_KP_4, pressed);
            return true;
        }
        case 0x4C: {
            JPAD(PalKeyCode::PK_DPAD_DOWN, pressed);
            kbdPushData(PalKeyCode::PK_KP_5, pressed);
            return true;
        }
        case 0x4D: {
            JPAD(PalKeyCode::PK_DPAD_RIGHT, pressed);
            kbdPushData(PalKeyCode::PK_KP_6, pressed);
            return true;
        }
        case 0x47: {
            JPAD(PalKeyCode::PK_DPAD_LEFT, pressed);
            JPAD(PalKeyCode::PK_DPAD_UP, pressed);
            kbdPushData(PalKeyCode::PK_KP_7, pressed);
            return true;
        }
        case 0x48: {
            JPAD(PalKeyCode::PK_DPAD_UP, pressed);
            kbdPushData(PalKeyCode::PK_KP_8, pressed);
            return true;
        }
        case 0x49: {
            JPAD(PalKeyCode::PK_DPAD_RIGHT, pressed);
            JPAD(PalKeyCode::PK_DPAD_UP, pressed);
            kbdPushData(PalKeyCode::PK_KP_9, pressed);
            return true;
        }
    }
    */
    return true;
}


#if USE_NESPAD

static void nespad_tick1(void) {
    nespad_read();
    gamepad1_bits.a = (nespad_state & DPAD_A) != 0;
    gamepad1_bits.b = (nespad_state & DPAD_B) != 0;
    gamepad1_bits.select = (nespad_state & DPAD_SELECT) != 0;
    gamepad1_bits.start = (nespad_state & DPAD_START) != 0;
    gamepad1_bits.up = (nespad_state & DPAD_UP) != 0;
    gamepad1_bits.down = (nespad_state & DPAD_DOWN) != 0;
    gamepad1_bits.left = (nespad_state & DPAD_LEFT) != 0;
    gamepad1_bits.right = (nespad_state & DPAD_RIGHT) != 0;
}

static void nespad_tick2(void) {
    gamepad2_bits.a = (nespad_state2 & DPAD_A) != 0;
    gamepad2_bits.b = (nespad_state2 & DPAD_B) != 0;
    gamepad2_bits.select = (nespad_state2 & DPAD_SELECT) != 0;
    gamepad2_bits.start = (nespad_state2 & DPAD_START) != 0;
    gamepad2_bits.up = (nespad_state2 & DPAD_UP) != 0;
    gamepad2_bits.down = (nespad_state2 & DPAD_DOWN) != 0;
    gamepad2_bits.left = (nespad_state2 & DPAD_LEFT) != 0;
    gamepad2_bits.right = (nespad_state2 & DPAD_RIGHT) != 0;
}
#endif

inline static void addKey(PalKeyCode vk, bool pressed);

static bool joystickControlState(const input_bits_t& bits, unsigned control)
{
    switch (control) {
        case PAL_JOY_UP:     return bits.up;
        case PAL_JOY_DOWN:   return bits.down;
        case PAL_JOY_LEFT:   return bits.left;
        case PAL_JOY_RIGHT:  return bits.right;
        case PAL_JOY_START:  return bits.start;
        case PAL_JOY_SELECT: return bits.select;
        case PAL_JOY_A:      return bits.a;
        case PAL_JOY_B:      return bits.b;
        default:             return false;
    }
}

static bool mappedKeyActive(const input_bits_t bits[2],
                            const PalKeyCode maps[2][PAL_JOY_CONTROL_COUNT],
                            PalKeyCode key)
{
    for (unsigned joystick = 0; joystick < 2; ++joystick)
        for (unsigned control = 0; control < PAL_JOY_CONTROL_COUNT; ++control)
            if (maps[joystick][control] == key
                && joystickControlState(bits[joystick], control))
                return true;
    return false;
}

static void gamepad_to_keyboard_tick()
{
    static input_bits_t previousBits[2] = {};
    static PalKeyCode previousMappings[2][PAL_JOY_CONTROL_COUNT] = {
        { PK_UP, PK_DOWN, PK_LEFT, PK_RIGHT, PK_LALT, PK_TAB, PK_SPACE, PK_ENTER },
        { PK_UP, PK_DOWN, PK_LEFT, PK_RIGHT, PK_LALT, PK_TAB, PK_SPACE, PK_ENTER }
    };
    const input_bits_t currentBits[2] = { gamepad1_bits, gamepad2_bits };

    // A key may be assigned to several controls and/or both joysticks. Emit a
    // release only when no mapped control still holds it. Include both old and
    // new mappings so remapping a held control cannot leave a stuck key.
    bool checked[PK_JOY_RIGHT + 1] = {};
    for (unsigned joystick = 0; joystick < 2; ++joystick) {
        for (unsigned control = 0; control < PAL_JOY_CONTROL_COUNT; ++control) {
            const PalKeyCode candidates[2] = {
                previousMappings[joystick][control],
                joystickMappings[joystick][control]
            };
            for (PalKeyCode key : candidates) {
                if (key <= PK_NONE || key > PK_JOY_RIGHT || checked[key])
                    continue;
                checked[key] = true;
                const bool wasPressed = mappedKeyActive(previousBits, previousMappings, key);
                const bool isPressed = mappedKeyActive(currentBits, joystickMappings, key);
                if (wasPressed != isPressed)
                    addKey(key, isPressed);
            }
        }
    }

    previousBits[0] = currentBits[0];
    previousBits[1] = currentBits[1];
    std::memcpy(previousMappings, joystickMappings, sizeof(previousMappings));
}

#ifdef KBDUSB
inline static bool isInReport(hid_keyboard_report_t const *report, const unsigned char keycode) {
    for (unsigned char i: report->keycode) {
        if (i == keycode) {
            return true;
        }
    }
    return false;
}


PalKeyCode pressed_key[256] = { PalKeyCode::PK_NONE };

Emulation* g_emulation = nullptr;

// Аппаратный сброс по Ctrl+Alt+Del. Модификаторы приходят отдельными событиями
// (USB — по маске report->modifier, гейпад Start → PK_LALT), поэтому их
// состояние отслеживается здесь, а не берётся из machineKey: комбинация должна
// работать даже при открытом меню/модальном диалоге, когда machineKey не
// вызывается вовсе. Сброс делаем немедленным watchdog'ом.
static void checkHardResetCombo(PalKeyCode vk, bool pressed) {
    static bool ctrlDown = false;
    static bool altDown = false;
    switch (vk) {
        case PalKeyCode::PK_LCTRL:
        case PalKeyCode::PK_RCTRL:
            ctrlDown = pressed;
            break;
        case PalKeyCode::PK_LALT:
        case PalKeyCode::PK_RALT:
            altDown = pressed;
            break;
        case PalKeyCode::PK_DEL:
            if (pressed && ctrlDown && altDown) {
                watchdog_enable(1, true); // перезагрузка через ~1 мс
                while (true) tight_loop_contents();
            }
            break;
        default:
            break;
    }
}

// PS/2, USB and joysticks are polled on one producer core (core 1 on
// VGA, core 0 on HDMI). The menu and emulation consume on core 0.
static constexpr unsigned KEY_ACTION_QUEUE_SIZE = 64;
static PalKeyCodeAction keyActions[KEY_ACTION_QUEUE_SIZE];
static std::atomic<unsigned> keyActionHead{0};
static std::atomic<unsigned> keyActionTail{0};

inline static void addKey(PalKeyCode vk, bool pressed) {
    // Ctrl+Alt+Del — аппаратный сброс через watchdog. Ловится на самом нижнем
    // уровне ввода: до очереди клавиш, до меню и модальных диалогов (которые
    // читают клавиши сами, минуя machineKey). Поэтому срабатывает всегда,
    // независимо от того, что сейчас на экране, и Alt/меню не рвут цепочку.
    checkHardResetCombo(vk, pressed);

    const unsigned tail = keyActionTail.load(std::memory_order_relaxed);
    if (tail - keyActionHead.load(std::memory_order_acquire) == KEY_ACTION_QUEUE_SIZE)
        return;
    auto& action = keyActions[tail % KEY_ACTION_QUEUE_SIZE];
    action.vk = vk;
    action.pressed = pressed;
    keyActionTail.store(tail + 1, std::memory_order_release);
}

static PalKeyCodeAction __not_in_flash_func(popKeyAction)() {
    const unsigned head = keyActionHead.load(std::memory_order_relaxed);
    if (head == keyActionTail.load(std::memory_order_acquire))
        return {};
    const auto& slot = keyActions[head % KEY_ACTION_QUEUE_SIZE];
    PalKeyCodeAction action(slot.vk, slot.pressed);
    keyActionHead.store(head + 1, std::memory_order_release);
    return action;
}

PalKeyCodeAction getKey() {
    return popKeyAction();
}

void __not_in_flash_func(processKeys)() {
    if (!g_emulation)
        return;
    const auto action = popKeyAction();
    if (action.vk != PK_NONE)
        g_emulation->machineKey(action.vk, action.pressed);
}

void repeat_handler() {
    /// TODO
}

static PalKeyCode map_key(uint8_t kc) {
    switch(kc) {
        case HID_KEY_SPACE: return PalKeyCode::PK_SPACE;

        case HID_KEY_A: return PalKeyCode::PK_A;
        case HID_KEY_B: return PalKeyCode::PK_B;
        case HID_KEY_C: return PalKeyCode::PK_C;
        case HID_KEY_D: return PalKeyCode::PK_D;
        case HID_KEY_E: return PalKeyCode::PK_E;
        case HID_KEY_F: return PalKeyCode::PK_F;
        case HID_KEY_G: return PalKeyCode::PK_G;
        case HID_KEY_H: return PalKeyCode::PK_H;
        case HID_KEY_I: return PalKeyCode::PK_I;
        case HID_KEY_J: return PalKeyCode::PK_J;
        case HID_KEY_K: return PalKeyCode::PK_K;
        case HID_KEY_L: return PalKeyCode::PK_L;
        case HID_KEY_M: return PalKeyCode::PK_M;
        case HID_KEY_N: return PalKeyCode::PK_N;
        case HID_KEY_O: return PalKeyCode::PK_O;
        case HID_KEY_P: return PalKeyCode::PK_P;
        case HID_KEY_Q: return PalKeyCode::PK_Q;
        case HID_KEY_R: return PalKeyCode::PK_R;
        case HID_KEY_S: return PalKeyCode::PK_S;
        case HID_KEY_T: return PalKeyCode::PK_T;
        case HID_KEY_U: return PalKeyCode::PK_U;
        case HID_KEY_V: return PalKeyCode::PK_V;
        case HID_KEY_W: return PalKeyCode::PK_W;
        case HID_KEY_X: return PalKeyCode::PK_X;
        case HID_KEY_Y: return PalKeyCode::PK_Y;
        case HID_KEY_Z: return PalKeyCode::PK_Z;

        case HID_KEY_0: return PalKeyCode::PK_0;
        case HID_KEY_1: return PalKeyCode::PK_1;
        case HID_KEY_2: return PalKeyCode::PK_2;
        case HID_KEY_3: return PalKeyCode::PK_3;
        case HID_KEY_4: return PalKeyCode::PK_4;
        case HID_KEY_5: return PalKeyCode::PK_5;
        case HID_KEY_6: return PalKeyCode::PK_6;
        case HID_KEY_7: return PalKeyCode::PK_7;
        case HID_KEY_8: return PalKeyCode::PK_8;
        case HID_KEY_9: return PalKeyCode::PK_9;
        
        case HID_KEY_KEYPAD_0: return PalKeyCode::PK_KP_0;
        case HID_KEY_KEYPAD_1: return PalKeyCode::PK_KP_1;
        case HID_KEY_KEYPAD_2: return PalKeyCode::PK_KP_2;
        case HID_KEY_KEYPAD_3: return PalKeyCode::PK_KP_3;
        case HID_KEY_KEYPAD_4: return PalKeyCode::PK_KP_4;
        case HID_KEY_KEYPAD_5: return PalKeyCode::PK_KP_5;
        case HID_KEY_KEYPAD_6: return PalKeyCode::PK_KP_6;
        case HID_KEY_KEYPAD_7: return PalKeyCode::PK_KP_7;
        case HID_KEY_KEYPAD_8: return PalKeyCode::PK_KP_8;
        case HID_KEY_KEYPAD_9: return PalKeyCode::PK_KP_9;
        case HID_KEY_NUM_LOCK: return PalKeyCode::PK_NUMLOCK;
        case HID_KEY_KEYPAD_DIVIDE: return PalKeyCode::PK_KP_DIV;
        case HID_KEY_KEYPAD_MULTIPLY: return PalKeyCode::PK_KP_MUL;
        case HID_KEY_KEYPAD_SUBTRACT: return PalKeyCode::PK_KP_MINUS;
        case HID_KEY_KEYPAD_ADD: return PalKeyCode::PK_KP_PLUS;
        case HID_KEY_KEYPAD_ENTER: return PalKeyCode::PK_KP_ENTER;
        case HID_KEY_KEYPAD_DECIMAL: return PalKeyCode::PK_KP_PERIOD;
        
        case HID_KEY_PRINT_SCREEN: return PalKeyCode::PK_PRSCR;
        case HID_KEY_SCROLL_LOCK: return PalKeyCode::PK_SCRLOCK;
        case HID_KEY_PAUSE: return PalKeyCode::PK_PAUSEBRK;

        case HID_KEY_INSERT: return PalKeyCode::PK_INS;
        case HID_KEY_HOME: return PalKeyCode::PK_HOME;
        case HID_KEY_PAGE_UP: return PalKeyCode::PK_PGUP;
        case HID_KEY_PAGE_DOWN: return PalKeyCode::PK_PGDN;
        case HID_KEY_DELETE: return PalKeyCode::PK_DEL;
        case HID_KEY_END: return PalKeyCode::PK_END;

        case HID_KEY_F1: return PalKeyCode::PK_F1;
        case HID_KEY_F2: return PalKeyCode::PK_F2;
        case HID_KEY_F3: return PalKeyCode::PK_F3;
        case HID_KEY_F4: return PalKeyCode::PK_F4;
        case HID_KEY_F5: return PalKeyCode::PK_F5;
        case HID_KEY_F6: return PalKeyCode::PK_F6;
        case HID_KEY_F7: return PalKeyCode::PK_F7;
        case HID_KEY_F8: return PalKeyCode::PK_F8;
        case HID_KEY_F9: return PalKeyCode::PK_F9;
        case HID_KEY_F10: return PalKeyCode::PK_F10;
        case HID_KEY_F11: return PalKeyCode::PK_F11;
        case HID_KEY_F12: return PalKeyCode::PK_F12;

        case HID_KEY_ALT_LEFT: return PalKeyCode::PK_LALT;
        case HID_KEY_ALT_RIGHT: return PalKeyCode::PK_RALT;
        case HID_KEY_CONTROL_LEFT: return PalKeyCode::PK_LCTRL;
        case HID_KEY_CONTROL_RIGHT: return PalKeyCode::PK_RCTRL;
        case HID_KEY_SHIFT_LEFT: return PalKeyCode::PK_LSHIFT;
        case HID_KEY_SHIFT_RIGHT: return PalKeyCode::PK_RSHIFT;
        case HID_KEY_CAPS_LOCK: return PalKeyCode::PK_CAPSLOCK;

        case HID_KEY_TAB: return PalKeyCode::PK_TAB;
        case HID_KEY_ENTER: return PalKeyCode::PK_ENTER;
        case HID_KEY_ESCAPE: return PalKeyCode::PK_ESC;

        case HID_KEY_GRAVE: return PalKeyCode::PK_TILDE;
        case HID_KEY_MINUS: return PalKeyCode::PK_MINUS;
        case HID_KEY_EQUAL: return PalKeyCode::PK_EQU;
        case HID_KEY_BACKSLASH: return PalKeyCode::PK_BSLASH;
        case HID_KEY_EUROPE_1: return PalKeyCode::PK_BSLASH; // ???
        case HID_KEY_BRACKET_LEFT: return PalKeyCode::PK_LBRACKET;
        case HID_KEY_BRACKET_RIGHT: return PalKeyCode::PK_RBRACKET;
        case HID_KEY_SEMICOLON: return PalKeyCode::PK_SEMICOLON;
        case HID_KEY_APOSTROPHE: return PalKeyCode::PK_APOSTROPHE;
        case HID_KEY_COMMA: return PalKeyCode::PK_COMMA;
        case HID_KEY_PERIOD: return PalKeyCode::PK_PERIOD;
        case HID_KEY_SLASH: return PalKeyCode::PK_SLASH;
        case HID_KEY_BACKSPACE: return PalKeyCode::PK_BSP;

        case HID_KEY_ARROW_UP: return PalKeyCode::PK_UP;
        case HID_KEY_ARROW_DOWN: return PalKeyCode::PK_DOWN;
        case HID_KEY_ARROW_LEFT: return PalKeyCode::PK_LEFT;
        case HID_KEY_ARROW_RIGHT: return PalKeyCode::PK_RIGHT;
        // Win-клавиши — модификатор снимков (Left Win+F1..F12 — сохранить,
        // Right Win+F1..F12 — загрузить), поэтому доводятся как есть.
        case HID_KEY_GUI_LEFT: return PalKeyCode::PK_LWIN;
        case HID_KEY_GUI_RIGHT: return PalKeyCode::PK_RWIN;
        default: break;
    }
    return PalKeyCode::PK_NONE;
}

struct HidModifierMapping {
    uint8_t mask;
    PalKeyCode key;
};

static constexpr HidModifierMapping hidModifierMappings[] = {
    { KEYBOARD_MODIFIER_LEFTCTRL,   PalKeyCode::PK_LCTRL  },
    { KEYBOARD_MODIFIER_LEFTSHIFT,  PalKeyCode::PK_LSHIFT },
    { KEYBOARD_MODIFIER_LEFTALT,    PalKeyCode::PK_LALT   },
    { KEYBOARD_MODIFIER_LEFTGUI,    PalKeyCode::PK_LWIN   },
    { KEYBOARD_MODIFIER_RIGHTCTRL,  PalKeyCode::PK_RCTRL  },
    { KEYBOARD_MODIFIER_RIGHTSHIFT, PalKeyCode::PK_RSHIFT },
    { KEYBOARD_MODIFIER_RIGHTALT,   PalKeyCode::PK_RALT   },
    { KEYBOARD_MODIFIER_RIGHTGUI,   PalKeyCode::PK_RWIN   },
};

static inline bool isHidModifierKey(uint8_t keycode) {
    return keycode >= HID_KEY_CONTROL_LEFT && keycode <= HID_KEY_GUI_RIGHT;
}

// Сдвиг картинки и меню серыми клавишами цифрового блока.
//
// Повторов при удержании ни HID-, ни PS/2-слой не присылают: события идут
// только на изменение состояния клавиши. Поэтому отсчёт ведётся здесь, по
// тикам основного цикла, так же как это сделано для навигации в меню.
static PalKeyCode s_shiftKey = PalKeyCode::PK_NONE;
static uint64_t s_shiftRepeatAt = 0;
static constexpr uint64_t c_shiftDelayUs = 400000;
static constexpr uint64_t c_shiftRateUs = 60000;

static bool isShiftKey(PalKeyCode vk) {
    return vk == PK_KP_PLUS || vk == PK_KP_MINUS || vk == PK_KP_MUL || vk == PK_KP_DIV;
}

static void doShift(PalKeyCode vk) {
    // Пока меню на экране, сдвиг применяется к нему, а не к картинке: иначе,
    // сдвинув изображение, часть меню и файлового диалога уезжает за пределы
    // видимой области.
    if (palMainMenuIsOpen()) {
        if (vk == PK_KP_PLUS) palMainMenuShift(0, 1);
        else if (vk == PK_KP_MINUS) palMainMenuShift(0, -1);
        else if (vk == PK_KP_MUL) palMainMenuShift(1, 0);
        else if (vk == PK_KP_DIV) palMainMenuShift(-1, 0);
        return;
    }
    if (vk == PK_KP_PLUS) graphics_inc_y();
    else if (vk == PK_KP_MINUS) graphics_dec_y();
    else if (vk == PK_KP_MUL) graphics_inc_x();
    else if (vk == PK_KP_DIV) graphics_dec_x();
}

static void applyShiftKey(PalKeyCode vk, bool pressed) {
    if (!isShiftKey(vk))
        return;
    if (!pressed) {
        if (vk == s_shiftKey)
            s_shiftKey = PalKeyCode::PK_NONE;
        return;
    }
    doShift(vk);
    s_shiftKey = vk;
    s_shiftRepeatAt = time_us_64() + c_shiftDelayUs;
}

// При HDMI ядро 1 занято кодированием, поэтому ввод обслуживается отсюда.
// Внутри repeat_me_for_input стоит собственное ограничение в 60 Гц, так что
// вызывать её на каждой итерации основного цикла не накладно.
void palInputTick() {
#ifdef HDMI_DVI
    extern void repeat_me_for_input();
    repeat_me_for_input();
#endif
}

void palShiftKeysTick() {
    if (s_shiftKey == PalKeyCode::PK_NONE)
        return;
    const uint64_t now = time_us_64();
    if (now < s_shiftRepeatAt)
        return;
    doShift(s_shiftKey);
    s_shiftRepeatAt = now + c_shiftRateUs;
}

void ///__not_in_flash_func(
    process_kbd_report(
    hid_keyboard_report_t const *report,
    hid_keyboard_report_t const *prev_report
) {
    static bool numlock = false;

    const uint8_t changedModifiers = report->modifier ^ prev_report->modifier;
    for (const HidModifierMapping &mapping: hidModifierMappings) {
        if (changedModifiers & mapping.mask) {
            addKey(mapping.key, (report->modifier & mapping.mask) != 0);
        }
    }

    for (uint8_t pkc: prev_report->keycode) {
        if (!pkc || isHidModifierKey(pkc)) continue;
        bool key_still_pressed = false;
        for (uint8_t kc: report->keycode) {
            if (kc == pkc) {
                key_still_pressed = true;
                break;
            }
        }
        if (!key_still_pressed) {
            if (g_emulation) {
                addKey(pressed_key[pkc], false);
            }
            applyShiftKey(pressed_key[pkc], false);
            pressed_key[pkc] = PalKeyCode::PK_NONE;
        }
    }
    for (uint8_t kc: report->keycode) {
        if (!kc || isHidModifierKey(kc)) continue;
        PalKeyCode vk = pressed_key[kc];
        if (vk == PalKeyCode::PK_NONE) { // it was not yet pressed
            vk = map_key(kc);
            if (vk != PalKeyCode::PK_NONE) {
                pressed_key[kc] = vk;
                if (g_emulation) {
                    addKey(vk, true);
                }
                applyShiftKey(vk, true);
            }
        }
    }
}

Ps2Kbd_Mrmltr ps2kbd(
        pio1,
        KBD_CLOCK_PIN,
        process_kbd_report
);
#endif

void /// __scratch_x("render")
 repeat_me_for_input() {
    static uint32_t tickKbdRep1 = time_us_32();
    // 60 FPS loop
#define frame_tick (16666)
    static uint64_t tick = time_us_64();
    static bool tick1 = true;
    static uint64_t last_input_tick = tick;
        if (tick >= last_input_tick + frame_tick) {
#ifdef KBDUSB
            ps2kbd.tick();
#endif
#ifdef USE_NESPAD
            (tick1 ? nespad_tick1 : nespad_tick2)(); // split call for joy1 and 2
            tick1 = !tick1;
#endif
            gamepad_to_keyboard_tick();
            last_input_tick = tick;
        }
        tick = time_us_64();
        uint32_t tickKbdRep2 = time_us_32();
        if (tickKbdRep2 - tickKbdRep1 > 150000) { // repeat each 150 ms
            repeat_handler();
            tickKbdRep1 = tickKbdRep2;
        }

#ifdef KBDUSB
        tuh_task();
#endif
}

void __not_in_flash_func(render_core)() {
    multicore_lockout_victim_init();
    graphics_init();
    graphics_set_bgcolor(0x000000);
    graphics_set_flashmode(false, false);
    sem_acquire_blocking(&vga_start_semaphore);
#ifdef HDMI_DVI
    // libdvi занимает ядро целиком: кодирование TMDS идёт непрерывно и
    // возврата из цикла нет. Опрос клавиатуры и USB переезжает на core0,
    // в palShiftKeysTick рядом с остальными тиками основного цикла.
    hdmi_dvi_core_loop();
#else
    while (true) {
        repeat_me_for_input();
        tight_loop_contents();
    }
#endif
    __unreachable();
}

#if SOFTTV
// tv_out_mode_t и сам tv_out_mode объявлены в tv-software.h,
// который подтягивается через graphics.h

bool color_mode=true;
bool toggle_color() {
    color_mode=!color_mode;
    if(color_mode) {
        tv_out_mode.color_index= 1.0f;
    } else {
        tv_out_mode.color_index= 0.0f;
    }

    return true;
}
#endif

i2s_config_t i2s_config = {
		.sample_freq = I2S_FREQUENCY, 
		.channel_count = 2,
		.data_pin = AUDIO_DATA_PIN,
		.clock_pin_base = AUDIO_CLOCK_PIN,
		.pio = pio1,
		.sm = 0,
        .dma_channel = 0,
        .dma_trans_count = 0,
        .dma_buf = NULL,
        .volume = 0
	};

#ifdef LOAD_WAV_PIO
inline static void inInit(uint gpio) {
    gpio_init(gpio);
    gpio_set_dir(gpio, GPIO_IN);
    gpio_pull_up(gpio);
}
#endif

#include "hardware/pwm.h"


#include "pico/picoPal.h"

void init_sound() {
    // Тип выхода определяется электрически: одна прошивка работает и с
    // ШИМ-платой, и с I2S-платой. Сама инициализация выбранного тракта
    // отложена до palSetSampleRate(), когда известна частота дискретизации.
    palProbeAudioOutput();
#ifdef LOAD_WAV_PIO
    //пин ввода звука
    inInit(LOAD_WAV_PIO);
///	add_repeating_timer_us(-1000000ll / hz, timer_callback, NULL, &m_timer);
#endif
}

static const char* argv[1] = {
    "emu80"
};

#ifdef PICO_RP2350
void __not_in_flash_func(sigbus)(void) {
    while(true) {
        sleep_ms(330);
#ifdef PICO_DEFAULT_LED_PIN
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(330);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
#endif
    }
}
#endif

#ifndef PICO_RP2040
void __not_in_flash() flash_timings(uint32_t systemClockMHz) {
    const int max_flash_freq = 88 * MHZ;
    const int clock_hz = systemClockMHz * MHZ;
    int divisor = (clock_hz + max_flash_freq - 1) / max_flash_freq;
    if (divisor == 1 && clock_hz > 100000000) {
        divisor = 2;
    }
    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000) {
        rxdelay += 1;
    }
    qmi_hw->m[0].timing = 0x60007000 |
                        rxdelay << QMI_M0_TIMING_RXDELAY_LSB |
                        divisor << QMI_M0_TIMING_CLKDIV_LSB;
}
#endif

#if !PICO_RP2040
static uint16_t s_coreVoltageMv = CPU_MHZ > 440 ? 1600 : 1500;

uint32_t palGetSystemClockMHz()
{
    return clock_get_hz(clk_sys) / MHZ;
}

uint16_t palGetCoreVoltageMv()
{
    return s_coreVoltageMv;
}

bool palSetCoreVoltageMv(uint16_t mv)
{
    if (mv == s_coreVoltageMv)
        return true;

    enum vreg_voltage voltage;
    switch (mv) {
        case 1300: voltage = VREG_VOLTAGE_1_30; break;
        case 1400: voltage = VREG_VOLTAGE_1_40; break;
        case 1500: voltage = VREG_VOLTAGE_1_50; break;
        case 1600: voltage = VREG_VOLTAGE_1_60; break;
        case 1650: voltage = VREG_VOLTAGE_1_65; break;
        default: return false;
    }
    vreg_set_voltage(voltage);
    sleep_ms(5);
    s_coreVoltageMv = mv;
    return true;
}

#else
uint32_t palGetSystemClockMHz() { return clock_get_hz(clk_sys) / MHZ; }
uint16_t palGetCoreVoltageMv() { return 1300; }
bool palSetCoreVoltageMv(uint16_t) { return false; }
#endif

bool __not_in_flash_func(palSetSystemClockMHz)(uint32_t mhz)
{
    if (mhz == palGetSystemClockMHz())
        return true;

    if (!graphics_system_clock_can_change())
        return mhz == palGetSystemClockMHz();

    uint32_t count = 0;
    const uint32_t* clocks = graphics_get_supported_system_clocks(&count);
    bool supported = false;
    for (uint32_t i = 0; i < count; ++i)
        supported = supported || clocks[i] == mhz;
    if (!supported)
        return false;

    const uint32_t irqState = save_and_disable_interrupts();
    multicore_lockout_start_blocking();
#if !PICO_RP2040
    const auto oldMHz = palGetSystemClockMHz();
    flash_timings(std::max(mhz, oldMHz));
#endif
    const bool changed = set_sys_clock_khz(mhz * KHZ, false);
#if !PICO_RP2040
    flash_timings(changed ? mhz : oldMHz);
#endif
    if (changed)
        graphics_system_clock_changed();
    multicore_lockout_end_blocking();
    restore_interrupts(irqState);
    if (changed)
        palAudioSystemClockChanged();
    return changed;
}


int main() {
#if !PICO_RP2040
    vreg_disable_voltage_limit();
#if CPU_MHZ > 440
    vreg_set_voltage(VREG_VOLTAGE_1_60); // TODO: dynamic per CPU freq.
#else
    vreg_set_voltage(VREG_VOLTAGE_1_50);
#endif
    sleep_ms(33);
    flash_timings(CPU_MHZ);
    set_sys_clock_khz(CPU_MHZ * KHZ, 0);
    exception_set_exclusive_handler(HARDFAULT_EXCEPTION, sigbus);
#else
    hw_set_bits(&vreg_and_chip_reset_hw->vreg, VREG_AND_CHIP_RESET_VREG_VSEL_BITS);
    sleep_ms(10);
    set_sys_clock_khz(CPU_MHZ * KHZ, true);
#endif
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    for (int i = 0; i < 6; i++) {
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }
#endif
#ifdef KBDUSB
    tuh_init(BOARD_TUH_RHPORT);
    ps2kbd.init_gpio();
#else
    keyboard_init();
#endif

#if USE_NESPAD
    nespad_begin(clock_get_hz(clk_sys) / 1000, NES_GPIO_CLK, NES_GPIO_DATA, NES_GPIO_LAT);
#endif
    sem_init(&vga_start_semaphore, 0, 1);
    multicore_launch_core1(render_core);
    sem_release(&vga_start_semaphore);

    init_sound();
    // send kbd reset only after initial process passed
#ifndef KBDUSB
    keyboard_send(0xFF);
#endif
    // Результат запоминается: при неудаче файловый диалог повторит попытку
    // сам и покажет понятное сообщение вместо пустого экрана.
    // Здесь же убран локальный FATFS, затенявший глобальный.
    bool res = f_mount(&fs, "SD", 1) == FR_OK;
    palSetSdMounted(res);
    if (res) {
        f_mkdir("/specialist");
        f_mkdir("/.config");
    }
#if LOG
    f_unlink("/emu80.log");
#endif
    int argc = 1;
    palInit(argc, (char**)argv);
    (new Emulation)->init(); // g_emulation присваивается в конструкторе
    palLoadMenuState();
    palExecute();
 //while(1);
    __unreachable();
}
