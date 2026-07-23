#include <cstring>
#include <cstdarg>
#include <algorithm>

#include <pico/multicore.h>
#include <pico/stdlib.h>
#include <hardware/pio.h>
#include <hardware/i2c.h>
#include <hardware/vreg.h>
#include <hardware/sync.h>
#include <hardware/flash.h>
#ifdef PICO_RP2350
#include <hardware/regs/qmi.h>
#include <hardware/exception.h>
#include <hardware/structs/qmi.h>
#include <hardware/structs/xip.h>
#include <hardware/regs/sysinfo.h>
#endif
#include "audio.h"
#include "ff.h"
#if PSRAM
    #include "psram_spi.h"
#endif
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

void repeat_handler(void);

#define JPAD (Config::secondJoy == 3 ? back2joy2: joyPushData)

extern "C" bool handleScancode(const uint32_t ps2scancode) {
    #if 0
    if (ps2scancode != 0x45 && ps2scancode != 0x1D && ps2scancode != 0xC5) {
        char tmp1[16];
        snprintf(tmp1, 16, "%08X", ps2scancode);
        OSD::osdCenteredMsg(tmp1, LEVEL_WARN, 500);
    }
    #endif
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

static void gamepad_to_keyboard_tick()
{
    static input_bits_t previous = { false, false, false, false, false, false, false, false };
    const input_bits_t current = {
        gamepad1_bits.a || gamepad2_bits.a,
        gamepad1_bits.b || gamepad2_bits.b,
        gamepad1_bits.select || gamepad2_bits.select,
        gamepad1_bits.start || gamepad2_bits.start,
        gamepad1_bits.right || gamepad2_bits.right,
        gamepad1_bits.left || gamepad2_bits.left,
        gamepad1_bits.up || gamepad2_bits.up,
        gamepad1_bits.down || gamepad2_bits.down
    };

    if (current.up != previous.up) {
        addKey(PalKeyCode::PK_UP, current.up);
    }
    if (current.down != previous.down) {
        addKey(PalKeyCode::PK_DOWN, current.down);
    }
    if (current.left != previous.left) {
        addKey(PalKeyCode::PK_LEFT, current.left);
    }
    if (current.right != previous.right) {
        addKey(PalKeyCode::PK_RIGHT, current.right);
    }
    if (current.a != previous.a) {
        addKey(PalKeyCode::PK_SPACE, current.a);
    }
    if (current.b != previous.b) {
        addKey(PalKeyCode::PK_ENTER, current.b);
    }
    if (current.select != previous.select)
        addKey(PalKeyCode::PK_TAB, current.select);
    if (current.start != previous.start)
        addKey(PalKeyCode::PK_LALT, current.start);

    previous = current;
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

#if 1
static constexpr unsigned KEY_ACTION_QUEUE_SIZE = 64;
static PalKeyCodeAction keyActions[KEY_ACTION_QUEUE_SIZE];
static unsigned keyActionHead = 0;
static unsigned keyActionTail = 0;
static unsigned keyActionCount = 0;

inline static void addKey(PalKeyCode vk, bool pressed) {
    if (keyActionCount >= KEY_ACTION_QUEUE_SIZE)
        return;
    keyActions[keyActionTail] = PalKeyCodeAction(vk, pressed);
    keyActionTail = (keyActionTail + 1) % KEY_ACTION_QUEUE_SIZE;
    keyActionCount++;
}

static PalKeyCodeAction __not_in_flash_func(popKeyAction)() {
    if (keyActionCount == 0)
        return PalKeyCodeAction();
    PalKeyCodeAction action = keyActions[keyActionHead];
    keyActionHead = (keyActionHead + 1) % KEY_ACTION_QUEUE_SIZE;
    keyActionCount--;
    return action;
}

PalKeyCodeAction getKey() {
    return popKeyAction();
}

void __not_in_flash_func(processKeys)() {
    if (keyActionCount == 0 || !g_emulation)
        return;
    PalKeyCodeAction action = popKeyAction();
    g_emulation->machineKey(action.vk, action.pressed);
}
#else
volatile static PalKeyCodeAction lastKey;
volatile static bool isLastKey;
inline static void addKey(PalKeyCode vk, bool pressed) {
    lastKey.vk = vk;
    lastKey.pressed = pressed;
    isLastKey = true;
}
PalKeyCodeAction getKey() {
    if (!isLastKey) return PalKeyCodeAction();
    isLastKey = false;
    return lastKey;
}
void processKeys() {
    if (isLastKey && g_emulation) {
        isLastKey = false;
        g_emulation->machineKey(lastKey.vk, lastKey.pressed);
    }
}
#endif

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

#ifdef HWAY
#include "hway.h"
#endif

void init_sound() {
#ifdef HWAY
    // Ноги I2S заняты сдвиговым регистром 595, обычный тракт не поднимаем.
    hway_init();
#else
    // Тип выхода определяется электрически: одна прошивка работает и с
    // ШИМ-платой, и с I2S-платой. Сама инициализация выбранного тракта
    // отложена до palSetSampleRate(), когда известна частота дискретизации.
    palProbeAudioOutput();
#if BEEPER_PIN
    pwm_config config = pwm_get_default_config();
    gpio_set_function(BEEPER_PIN, GPIO_FUNC_PWM);
    pwm_config_set_clkdiv(&config, 127);
    pwm_init(pwm_gpio_to_slice_num(BEEPER_PIN), &config, true);
#endif
#ifdef LOAD_WAV_PIO
    //пин ввода звука
    inInit(LOAD_WAV_PIO);
///	add_repeating_timer_us(-1000000ll / hz, timer_callback, NULL, &m_timer);
#endif
#endif // HWAY
}

static const char* argv[1] = {
    "emu80"
};

static int BUTTER_PSRAM_SIZE = -1;
#ifdef PICO_RP2350
#define MB16 (16ul << 20)
#define MB8 (8ul << 20)
#define MB4 (4ul << 20)
#define MB1 (1ul << 20)
uint8_t* PSRAM_DATA = (uint8_t*)0x11000000;
uint32_t __not_in_flash_func(butter_psram_size)() {
    if (BUTTER_PSRAM_SIZE != -1) return BUTTER_PSRAM_SIZE;
    for(register int i = MB8; i < MB16; i += 4096)
        PSRAM_DATA[i] = 16;
    for(register int i = MB4; i < MB8; i += 4096)
        PSRAM_DATA[i] = 8;
    for(register int i = MB1; i < MB4; i += 4096)
        PSRAM_DATA[i] = 4;
    for(register int i = 0; i < MB1; i += 4096)
        PSRAM_DATA[i] = 1;
    register uint32_t res = PSRAM_DATA[MB16 - 4096];
    for (register int i = MB16 - MB1; i < MB16; i += 4096) {
        if (res != PSRAM_DATA[i])
            return 0;
    }
    BUTTER_PSRAM_SIZE = res << 20;
    return BUTTER_PSRAM_SIZE;
}
void __no_inline_not_in_flash_func(psram_init)(uint cs_pin) {
    gpio_set_function(cs_pin, GPIO_FUNC_XIP_CS1);

    // Enable direct mode, PSRAM CS, clkdiv of 10.
    qmi_hw->direct_csr = 10 << QMI_DIRECT_CSR_CLKDIV_LSB | \
                               QMI_DIRECT_CSR_EN_BITS | \
                               QMI_DIRECT_CSR_AUTO_CS1N_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS)
        ;

    // Enable QPI mode on the PSRAM
    const uint CMD_QPI_EN = 0x35;
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | CMD_QPI_EN;

    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS)
        ;

    // Set PSRAM timing for APS6404
    //
    // Using an rxdelay equal to the divisor isn't enough when running the APS6404 close to 133MHz.
    // So: don't allow running at divisor 1 above 100MHz (because delay of 2 would be too late),
    // and add an extra 1 to the rxdelay if the divided clock is > 100MHz (i.e. sys clock > 200MHz).
    const int max_psram_freq = 166000000;
    const int clock_hz = clock_get_hz(clk_sys);
    int divisor = (clock_hz + max_psram_freq - 1) / max_psram_freq;
    if (divisor == 1 && clock_hz > 100000000) {
        divisor = 2;
    }
    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000) {
        rxdelay += 1;
    }

    // - Max select must be <= 8us.  The value is given in multiples of 64 system clocks.
    // - Min deselect must be >= 18ns.  The value is given in system clock cycles - ceil(divisor / 2).
    const int clock_period_fs = 1000000000000000ll / clock_hz;
    const int max_select = (125 * 1000000) / clock_period_fs;  // 125 = 8000ns / 64
    const int min_deselect = (18 * 1000000 + (clock_period_fs - 1)) / clock_period_fs - (divisor + 1) / 2;

    qmi_hw->m[1].timing = 1 << QMI_M1_TIMING_COOLDOWN_LSB |
                          QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
                          max_select << QMI_M1_TIMING_MAX_SELECT_LSB |
                          min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
                          rxdelay << QMI_M1_TIMING_RXDELAY_LSB |
                          divisor << QMI_M1_TIMING_CLKDIV_LSB;

    // Set PSRAM commands and formats
    qmi_hw->m[1].rfmt =
        QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB |\
        QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q   << QMI_M0_RFMT_ADDR_WIDTH_LSB |\
        QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB |\
        QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M0_RFMT_DUMMY_WIDTH_LSB |\
        QMI_M0_RFMT_DATA_WIDTH_VALUE_Q   << QMI_M0_RFMT_DATA_WIDTH_LSB |\
        QMI_M0_RFMT_PREFIX_LEN_VALUE_8   << QMI_M0_RFMT_PREFIX_LEN_LSB |\
        6                                << QMI_M0_RFMT_DUMMY_LEN_LSB;

    qmi_hw->m[1].rcmd = 0xEB;

    qmi_hw->m[1].wfmt =
        QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB |\
        QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q   << QMI_M0_WFMT_ADDR_WIDTH_LSB |\
        QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB |\
        QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M0_WFMT_DUMMY_WIDTH_LSB |\
        QMI_M0_WFMT_DATA_WIDTH_VALUE_Q   << QMI_M0_WFMT_DATA_WIDTH_LSB |\
        QMI_M0_WFMT_PREFIX_LEN_VALUE_8   << QMI_M0_WFMT_PREFIX_LEN_LSB;

    qmi_hw->m[1].wcmd = 0x38;

    // Disable direct mode
    qmi_hw->direct_csr = 0;

    // Enable writes to PSRAM
    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);

    butter_psram_size();
}
void __not_in_flash_func(sigbus)(void) {
    while(true) {
        sleep_ms(330);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(330);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }
}
void __attribute__((naked, noreturn)) __printflike(1, 0) dummy_panic(__unused const char *fmt, ...) {
    while (true) {
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }
}
#else
uint8_t* PSRAM_DATA = (uint8_t*)0;
uint32_t butter_psram_size() { return 0; }
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

bool palSetSystemClockMHz(uint32_t mhz)
{
    if (!graphics_system_clock_can_change())
        return mhz == palGetSystemClockMHz();

    uint32_t count = 0;
    const uint32_t* clocks = graphics_get_supported_system_clocks(&count);
    bool supported = false;
    for (uint32_t i = 0; i < count; ++i)
        supported = supported || clocks[i] == mhz;
    if (!supported)
        return false;

    multicore_lockout_start_blocking();
    flash_timings(mhz);
    const bool changed = set_sys_clock_khz(mhz * KHZ, false);
    graphics_system_clock_changed();
    multicore_lockout_end_blocking();
    if (changed)
        palAudioSystemClockChanged();
    return changed;
}

uint16_t palGetCoreVoltageMv()
{
    return s_coreVoltageMv;
}

bool palSetCoreVoltageMv(uint16_t mv)
{
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
bool palSetSystemClockMHz(uint32_t) { return false; }
uint16_t palGetCoreVoltageMv() { return 0; }
bool palSetCoreVoltageMv(uint16_t) { return false; }
#endif

int main() {
#if !PICO_RP2040
    vreg_disable_voltage_limit();
#if CPU_MHZ > 440
    vreg_set_voltage(VREG_VOLTAGE_1_60); // TODO: dynamic per CPU freq.
#else
    vreg_set_voltage(VREG_VOLTAGE_1_50);
#endif
    sleep_ms(33);
    bool rp2350a = (*((io_ro_32*)(SYSINFO_BASE + SYSINFO_PACKAGE_SEL_OFFSET)) & 1);
    flash_timings(CPU_MHZ);
    set_sys_clock_khz(CPU_MHZ * KHZ, 0);
    uint BUTTER_PSRAM_GPIO = rp2350a ? BUTTER_PSRAM_GPIO_RP2350A
                                      : BUTTER_PSRAM_GPIO_RP2350B;
    psram_init(BUTTER_PSRAM_GPIO);
    exception_set_exclusive_handler(HARDFAULT_EXCEPTION, sigbus);
#else
    hw_set_bits(&vreg_and_chip_reset_hw->vreg, VREG_AND_CHIP_RESET_VREG_VSEL_BITS);
    sleep_ms(10);
    set_sys_clock_khz(CPU_MHZ * KHZ, true);
#endif

#ifdef KBDUSB
    tuh_init(BOARD_TUH_RHPORT);
    ps2kbd.init_gpio();
#else
    keyboard_init();
#endif
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    for (int i = 0; i < 6; i++) {
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }

#if USE_NESPAD
    nespad_begin(clock_get_hz(clk_sys) / 1000, NES_GPIO_CLK, NES_GPIO_DATA, NES_GPIO_LAT);
#endif
    sem_init(&vga_start_semaphore, 0, 1);
    multicore_launch_core1(render_core);
    sem_release(&vga_start_semaphore);

    init_sound();
///    pcm_setup(SOUND_FREQUENCY, SOUND_FREQUENCY * 2 / 50); // 882 * 2  = 1764
#ifdef PSRAM
    if (BUTTER_PSRAM_SIZE <= 0)
        init_psram();
#endif
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
        f_mkdir("/vector06c");
        f_mkdir("/tmp");
        f_mkdir("/.config");
    }
#if LOG
    f_unlink("/emu80.log");
#endif
    int argc = 1;
    palInit(argc, (char**)argv);
    (new Emulation)->init(); // g_emulation присваивается в конструкторе
    palExecute();
 //while(1);
    __unreachable();
}
