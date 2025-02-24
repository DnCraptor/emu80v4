#include <cstdio>
#include <cstring>
#include <cstdarg>

#include <pico/multicore.h>
#include <pico/stdlib.h>
#include <hardware/pio.h>
#include <hardware/i2c.h>
#include <hardware/vreg.h>
#include <hardware/sync.h>
#include <hardware/flash.h>

#include "audio.h"
#include "ff.h"
#include "psram_spi.h"
#ifdef KBDUSB
    #include "ps2kbd_mrmltr.h"
#else
    #include "ps2.h"
#endif

#if USE_NESPAD
#include "nespad.h"
#endif

#include "Pal.h"
#include "CmdLine.h"
#include "Emulation.h"

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
#include <queue>
static std::queue<PalKeyCodeAction*> actions;

inline static void addKey(PalKeyCode vk, bool pressed) {
    actions.push(new PalKeyCodeAction(vk, pressed));
}

PalKeyCodeAction getKey() {
    if (actions.empty()) return PalKeyCodeAction();
    PalKeyCodeAction* p = actions.front();
    actions.pop();
    if (!p) return PalKeyCodeAction();
    PalKeyCodeAction res = *p;
    delete p;
    return res;
}

void processKeys() {
    if (!actions.empty()) {
        PalKeyCodeAction* p = actions.front();
        actions.pop();
        if (p && g_emulation) {
            g_emulation->activePlatformKey(p->vk, p->pressed);
            delete p;
        }
    }
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
        g_emulation->activePlatformKey(lastKey.vk, lastKey.pressed);
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
 // TODO:
        case HID_KEY_GUI_LEFT: return PalKeyCode::PK_F1;
        case HID_KEY_GUI_RIGHT: return PalKeyCode::PK_F1;
        default: break;
    }
    return PalKeyCode::PK_NONE;
}

void ///__not_in_flash_func(
    process_kbd_report(
    hid_keyboard_report_t const *report,
    hid_keyboard_report_t const *prev_report
) {
    static bool numlock = false;
    for (uint8_t pkc: prev_report->keycode) {
        if (!pkc) continue;
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
            pressed_key[pkc] = PalKeyCode::PK_NONE;
        }
    }
    for (uint8_t kc: report->keycode) {
        if (!kc) continue;
        PalKeyCode vk = pressed_key[kc];
        if (vk == PalKeyCode::PK_NONE) { // it was not yet pressed
            vk = map_key(kc);
            if (vk != PalKeyCode::PK_NONE) {
                pressed_key[kc] = vk;
                if (g_emulation) {
                    addKey(vk, true);
                }
                if (vk == PK_KP_PLUS) graphics_inc_y();
                else if (vk == PK_KP_MINUS) graphics_dec_y();
                else if (vk == PK_KP_MUL) graphics_inc_x();
                else if (vk == PK_KP_DIV) graphics_dec_x();
                else if (vk == PK_NUMLOCK) {
                    numlock = !numlock;
                    uint8_t m = ((uint8_t)graphics_get_mode() + 1);
                    if (m >= UNSUPPORTED_MODE) m = GRAPHICSMODE_DEFAULT;
                    graphics_set_mode((enum graphics_mode_t)m);
                }
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

uint8_t* tmp_screen = nullptr;

void __scratch_x("render") render_core() {
    multicore_lockout_victim_init();
    graphics_init();
    graphics_set_buffer((uint8_t*)tmp_screen, DISP_WIDTH, DISP_HEIGHT);
    graphics_set_bgcolor(0x000000);
    graphics_set_flashmode(false, false);
    sem_acquire_blocking(&vga_start_semaphore);
    while (true) {
        repeat_me_for_input();
        tight_loop_contents();
    }
    __unreachable();
}

#if SOFTTV
typedef struct tv_out_mode_t {
    // double color_freq;
    float color_index;
    COLOR_FREQ_t c_freq;
    enum graphics_mode_t mode_bpp;
    g_out_TV_t tv_system;
    NUM_TV_LINES_t N_lines;
    bool cb_sync_PI_shift_lines;
    bool cb_sync_PI_shift_half_frame;
} tv_out_mode_t;
extern tv_out_mode_t tv_out_mode;

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

#ifdef I2S_SOUND
i2s_config_t i2s_config = {
		.sample_freq = I2S_FREQUENCY, 
		.channel_count = 2,
		.data_pin = PWM_PIN0,
		.clock_pin_base = PWM_PIN1,
		.pio = pio1,
		.sm = 0,
        .dma_channel = 0,
        .dma_trans_count = 0,
        .dma_buf = NULL,
        .volume = 0
	};
#endif

#ifdef LOAD_WAV_PIO
inline static void inInit(uint gpio) {
    gpio_init(gpio);
    gpio_set_dir(gpio, GPIO_IN);
    gpio_pull_up(gpio);
}
#endif

#ifdef AUDIO_PWM_PIN
#include "hardware/pwm.h"
#endif

void init_sound() {
#ifndef I2S_SOUND
    pwm_config config = pwm_get_default_config();
    gpio_set_function(PWM_PIN0, GPIO_FUNC_PWM);
    gpio_set_function(PWM_PIN1, GPIO_FUNC_PWM);
    pwm_config_set_clkdiv(&config, 1.0f);
    pwm_config_set_wrap(&config, (1 << 8) - 1); // MAX PWM value
    pwm_init(pwm_gpio_to_slice_num(PWM_PIN0), &config, true);
    pwm_init(pwm_gpio_to_slice_num(PWM_PIN1), &config, true);
    #if BEEPER_PIN
        gpio_set_function(BEEPER_PIN, GPIO_FUNC_PWM);
        pwm_config_set_clkdiv(&config, 127);
        pwm_init(pwm_gpio_to_slice_num(BEEPER_PIN), &config, true);
    #endif
#else
    i2s_config.sample_freq = I2S_FREQUENCY;
    i2s_config.channel_count = 2;
    i2s_config.dma_trans_count = 1;
    i2s_init(&i2s_config);
#endif
#ifdef LOAD_WAV_PIO
    //пин ввода звука
    inInit(LOAD_WAV_PIO);
///	add_repeating_timer_us(-1000000ll / hz, timer_callback, NULL, &m_timer);
#endif
}

static const char* const platforms[] = {
    "vector",
    "vector.z80",
    "spec.m1",
    "spec",
    "sp580",
    "spec.z80",
    "spec.lik",
    "spmx",
    "spmx.turbo",
    "spmx.mxos",
    "spmx.com",
    "korvet",
    "partner",
    "bashkiria",
    "lvov",
    "mikro80",
    "mikro80.rk",
#if ORION
    "orion.1",
    "orion",
    "orion.dsdos",
    "orion.31",
    "orion.32",
#endif
#if PK86
    "apogey",
    "apogey.sd",
    "mikrosha",
    "mikrosha.krista",
    "rk86",
    "rk86.sd",
    "rk86.sdos",
    "rk86.z80",
    "rk86.kr03",
#endif
    "kr04",
    "eureka",
    "ut88"
#if PK8000
    "pk8000",
    "pk8000.fdc",
    "pk8000.hdd",
#endif
};
/**
Вектор-06Ц
Вектор-06Ц Z80 CPU
Специалист (Монитор 1)
Специалист (Монитор 2)
Специалист SP580
Специалист Z80 CPU
Лик (промышленный вариант "Специалиста")
Специалист-MX (RAMFOS)
Специалист-MX Turbo Z80 CPU (RAMFOS)
Специалист-MX (MXOS)
Специалист-MX (Commander)
ПК8020 Корвет
Партнер-01.01
Башкирия-2М
ПК-01 Львов
Микро-80
Микро-80 (Монитор РК)
Орион-128 (Монитор 1)
Орион-128 (Монитор 2)
Орион-128 (Монитор 2) DSDOS
Орион-128 (Монитор 3.1)
Орион-128 (Монитор 3.2) Z80 CPU (Московский вариант)
Апогей БК-01
Апогей БК-01 (SD card)
Микроша
Криста
Радио-86РК
Радио-86РК (SD card / vinxru)
Радио-86РК (SDOS)
Радио-86РК Z80 CPU
Электроника КР-02/03
Электроника КР-04
Эврика
ЮТ-88
 */
static const unsigned char platforms_list_cp866[] = {
	0x82, 0xa5, 0xaa, 0xe2, 0xae, 0xe0, 0x2d, 0x30, 0x36, 0x96, 0x00, 0x0a,
	0x82, 0xa5, 0xaa, 0xe2, 0xae, 0xe0, 0x2d, 0x30, 0x36, 0x96, 0x20, 0x5a,
	0x38, 0x30, 0x20, 0x43, 0x50, 0x55, 0x00, 0x0a, 0x91, 0xaf, 0xa5, 0xe6,
	0xa8, 0xa0, 0xab, 0xa8, 0xe1, 0xe2, 0x20, 0x28, 0x8c, 0xae, 0xad, 0xa8,
	0xe2, 0xae, 0xe0, 0x20, 0x31, 0x29, 0x00, 0x0a, 0x91, 0xaf, 0xa5, 0xe6,
	0xa8, 0xa0, 0xab, 0xa8, 0xe1, 0xe2, 0x20, 0x28, 0x8c, 0xae, 0xad, 0xa8,
	0xe2, 0xae, 0xe0, 0x20, 0x32, 0x29, 0x00, 0x0a, 0x91, 0xaf, 0xa5, 0xe6,
	0xa8, 0xa0, 0xab, 0xa8, 0xe1, 0xe2, 0x20, 0x53, 0x50, 0x35, 0x38, 0x30,
	0x00, 0x0a, 0x91, 0xaf, 0xa5, 0xe6, 0xa8, 0xa0, 0xab, 0xa8, 0xe1, 0xe2,
	0x20, 0x5a, 0x38, 0x30, 0x20, 0x43, 0x50, 0x55, 0x00, 0x0a, 0x8b, 0xa8,
	0xaa, 0x20, 0x28, 0xaf, 0xe0, 0xae, 0xac, 0xeb, 0xe8, 0xab, 0xa5, 0xad,
	0xad, 0xeb, 0xa9, 0x20, 0xa2, 0xa0, 0xe0, 0xa8, 0xa0, 0xad, 0xe2, 0x20,
	0x22, 0x91, 0xaf, 0xa5, 0xe6, 0xa8, 0xa0, 0xab, 0xa8, 0xe1, 0xe2, 0xa0,
	0x22, 0x29, 0x00, 0x0a, 0x91, 0xaf, 0xa5, 0xe6, 0xa8, 0xa0, 0xab, 0xa8,
	0xe1, 0xe2, 0x2d, 0x4d, 0x58, 0x20, 0x28, 0x52, 0x41, 0x4d, 0x46, 0x4f,
	0x53, 0x29, 0x00, 0x0a, 0x91, 0xaf, 0xa5, 0xe6, 0xa8, 0xa0, 0xab, 0xa8,
	0xe1, 0xe2, 0x2d, 0x4d, 0x58, 0x20, 0x54, 0x75, 0x72, 0x62, 0x6f, 0x20,
	0x5a, 0x38, 0x30, 0x20, 0x43, 0x50, 0x55, 0x20, 0x28, 0x52, 0x41, 0x4d,
	0x46, 0x4f, 0x53, 0x29, 0x00, 0x0a, 0x91, 0xaf, 0xa5, 0xe6, 0xa8, 0xa0,
	0xab, 0xa8, 0xe1, 0xe2, 0x2d, 0x4d, 0x58, 0x20, 0x28, 0x4d, 0x58, 0x4f,
	0x53, 0x29, 0x00, 0x0a, 0x91, 0xaf, 0xa5, 0xe6, 0xa8, 0xa0, 0xab, 0xa8,
	0xe1, 0xe2, 0x2d, 0x4d, 0x58, 0x20, 0x28, 0x43, 0x6f, 0x6d, 0x6d, 0x61,
	0x6e, 0x64, 0x65, 0x72, 0x29, 0x00, 0x0a, 0x8f, 0x8a, 0x38, 0x30, 0x32,
	0x30, 0x20, 0x8a, 0xae, 0xe0, 0xa2, 0xa5, 0xe2, 0x00, 0x0a, 0x8f, 0xa0,
	0xe0, 0xe2, 0xad, 0xa5, 0xe0, 0x2d, 0x30, 0x31, 0x2e, 0x30, 0x31, 0x00,
	0x0a, 0x81, 0xa0, 0xe8, 0xaa, 0xa8, 0xe0, 0xa8, 0xef, 0x2d, 0x32, 0x8c,
	0x00, 0x0a, 0x8f, 0x8a, 0x2d, 0x30, 0x31, 0x20, 0x8b, 0xec, 0xa2, 0xae,
	0xa2, 0x00, 0x0a, 0x8c, 0xa8, 0xaa, 0xe0, 0xae, 0x2d, 0x38, 0x30, 0x00,
	0x0a, 0x8c, 0xa8, 0xaa, 0xe0, 0xae, 0x2d, 0x38, 0x30, 0x20, 0x28, 0x8c,
	0xae, 0xad, 0xa8, 0xe2, 0xae, 0xe0, 0x20, 0x90, 0x8a, 0x29, 0x00, 0x0a,
	0x8e, 0xe0, 0xa8, 0xae, 0xad, 0x2d, 0x31, 0x32, 0x38, 0x20, 0x28, 0x8c,
	0xae, 0xad, 0xa8, 0xe2, 0xae, 0xe0, 0x20, 0x31, 0x29, 0x00, 0x0a, 0x8e,
	0xe0, 0xa8, 0xae, 0xad, 0x2d, 0x31, 0x32, 0x38, 0x20, 0x28, 0x8c, 0xae,
	0xad, 0xa8, 0xe2, 0xae, 0xe0, 0x20, 0x32, 0x29, 0x00, 0x0a, 0x8e, 0xe0,
	0xa8, 0xae, 0xad, 0x2d, 0x31, 0x32, 0x38, 0x20, 0x28, 0x8c, 0xae, 0xad,
	0xa8, 0xe2, 0xae, 0xe0, 0x20, 0x32, 0x29, 0x20, 0x44, 0x53, 0x44, 0x4f,
	0x53, 0x00, 0x0a, 0x8e, 0xe0, 0xa8, 0xae, 0xad, 0x2d, 0x31, 0x32, 0x38,
	0x20, 0x28, 0x8c, 0xae, 0xad, 0xa8, 0xe2, 0xae, 0xe0, 0x20, 0x33, 0x2e,
	0x31, 0x29, 0x00, 0x0a, 0x8e, 0xe0, 0xa8, 0xae, 0xad, 0x2d, 0x31, 0x32,
	0x38, 0x20, 0x28, 0x8c, 0xae, 0xad, 0xa8, 0xe2, 0xae, 0xe0, 0x20, 0x33,
	0x2e, 0x32, 0x29, 0x20, 0x5a, 0x38, 0x30, 0x20, 0x43, 0x50, 0x55, 0x20,
	0x28, 0x8c, 0xae, 0xe1, 0xaa, 0xae, 0xa2, 0xe1, 0xaa, 0xa8, 0xa9, 0x20,
	0xa2, 0xa0, 0xe0, 0xa8, 0xa0, 0xad, 0xe2, 0x29, 0x00, 0x0a, 0x80, 0xaf,
	0xae, 0xa3, 0xa5, 0xa9, 0x20, 0x81, 0x8a, 0x2d, 0x30, 0x31, 0x00, 0x0a,
	0x80, 0xaf, 0xae, 0xa3, 0xa5, 0xa9, 0x20, 0x81, 0x8a, 0x2d, 0x30, 0x31,
	0x20, 0x28, 0x53, 0x44, 0x20, 0x63, 0x61, 0x72, 0x64, 0x29, 0x00, 0x0a,
	0x8c, 0xa8, 0xaa, 0xe0, 0xae, 0xe8, 0xa0, 0x00, 0x0a, 0x8a, 0xe0, 0xa8,
	0xe1, 0xe2, 0xa0, 0x00, 0x0a, 0x90, 0xa0, 0xa4, 0xa8, 0xae, 0x2d, 0x38,
	0x36, 0x90, 0x8a, 0x00, 0x0a, 0x90, 0xa0, 0xa4, 0xa8, 0xae, 0x2d, 0x38,
	0x36, 0x90, 0x8a, 0x20, 0x28, 0x53, 0x44, 0x20, 0x63, 0x61, 0x72, 0x64,
	0x20, 0x2f, 0x20, 0x76, 0x69, 0x6e, 0x78, 0x72, 0x75, 0x29, 0x00, 0x0a,
	0x90, 0xa0, 0xa4, 0xa8, 0xae, 0x2d, 0x38, 0x36, 0x90, 0x8a, 0x20, 0x28,
	0x53, 0x44, 0x4f, 0x53, 0x29, 0x00, 0x0a, 0x90, 0xa0, 0xa4, 0xa8, 0xae,
	0x2d, 0x38, 0x36, 0x90, 0x8a, 0x20, 0x5a, 0x38, 0x30, 0x20, 0x43, 0x50,
	0x55, 0x00, 0x0a, 0x9d, 0xab, 0xa5, 0xaa, 0xe2, 0xe0, 0xae, 0xad, 0xa8,
	0xaa, 0xa0, 0x20, 0x8a, 0x90, 0x2d, 0x30, 0x32, 0x2f, 0x30, 0x33, 0x00,
	0x0a, 0x9d, 0xab, 0xa5, 0xaa, 0xe2, 0xe0, 0xae, 0xad, 0xa8, 0xaa, 0xa0,
	0x20, 0x8a, 0x90, 0x2d, 0x30, 0x34, 0x00, 0x0a, 0x9d, 0xa2, 0xe0, 0xa8,
	0xaa, 0xa0, 0x00, 0x0a, 0x9e, 0x92, 0x2d, 0x38, 0x38, 0x00
};

static const char* argv[3] = {
    "emu80",
    "--platform",
    0
};

int main() {
    static FATFS fs;
#if !PICO_RP2040
    volatile uint32_t *qmi_m0_timing=(uint32_t *)0x400d000c;
    vreg_disable_voltage_limit();
    vreg_set_voltage(VREG_VOLTAGE_1_60);
    sleep_ms(33);
    *qmi_m0_timing = 0x60007204;
    set_sys_clock_khz(CPU_MHZ * KHZ, 0);
    *qmi_m0_timing = 0x60007303;
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
    tmp_screen = new uint8_t[DISP_WIDTH*DISP_HEIGHT];
    memset((void*)tmp_screen, RGB(0xFFFF00), DISP_WIDTH*DISP_HEIGHT);

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
    init_psram();
#endif
    // send kbd reset only after initial process passed
#ifndef KBDUSB
    keyboard_send(0xFF);
#endif
    f_mount(&fs, "SD", 1);
    f_mkdir("/emu80");
#if LOG
    f_unlink("/emu80.log");
#endif
    {
        int selected_file_n = 0;
        int shift_j = 0;
        uint32_t height_in_j = 0;
        uint32_t sw = graphics_get_width();
        uint32_t sh = graphics_get_height();
        uint32_t w = sw - 10;
        uint32_t h = sh - 6;
        int32_t x = (sw - w) / 2;
        int32_t y = (sh - h) / 2;
        uint32_t fntw = graphics_get_font_width();
        uint32_t fnth = graphics_get_font_height();
        graphics_rect(x, y, w, h, RGB888(0, 0, 0)); // outer rect

        uint32_t yt = y+2+1;
        graphics_fill(x+1, y+1, w-2, fnth+2, RGB888(107, 216, 231)); // Title background
        graphics_rect(x, y, w, fnth+4, RGB888(0, 0, 0));
        string t = "Select platform";
        uint32_t xt = (w-2 - t.length() * fntw) / 2; // center title
        graphics_type(xt, yt, RGB888(0, 0, 0), t.c_str(), t.length()); // print title
        
    again:
        int j = 0;
        uint32_t xb = x + 2;
        uint32_t yb = y + fnth + 5;
        graphics_fill(x + 1, yb, w-2, h-2-fnth-2-2, RGB888(0xFF, 0xFF, 0xFF)); // cleanup (prpepare background) in the rect
        uint32_t msi = fnth + 1; // height of one file line
        size_t lines = sizeof(platforms) / sizeof(char*);
        const char* pls = (const char*)platforms_list_cp866;
        height_in_j = 0;
        for (auto i = 0; i < lines; ++i, ++j) {
            const char* name = pls; ///pls[i].c_str();
            while(*pls++); ++pls;
            if (j < shift_j) continue;
            uint32_t ybj = yb + (j - shift_j) * msi;
            if (ybj > y + h - fnth) break;
            height_in_j++;
            if (selected_file_n == j) {
                graphics_fill(xb-1, ybj, w-2, fnth, RGB888(114, 114, 224));
                graphics_type(xb, ybj, RGB888(0xFF, 0xFF, 0xFF), name, strlen(name));
            } else {
                graphics_type(xb, ybj, RGB888(0, 0, 0), name, strlen(name));
            }
        }
        while(1) {
            sleep_ms(100);
            if (pressed_key[HID_KEY_ARROW_UP] || pressed_key[HID_KEY_KEYPAD_8]) {
                selected_file_n--;
                if (selected_file_n < 0) {
                    selected_file_n = lines - 1;
                    while (selected_file_n >= shift_j + height_in_j) {
                        shift_j += 10;
                    }
                }
                while (selected_file_n < shift_j) {
                    shift_j--;
                }
                if (shift_j < 0) shift_j = lines - 1;
                goto again;
            }
            if (pressed_key[HID_KEY_PAGE_UP] || pressed_key[HID_KEY_KEYPAD_9]) {
                selected_file_n -= 10;
                if (selected_file_n < 0) {
                    selected_file_n = 0;
                }
                while (selected_file_n < shift_j) {
                    shift_j -= 10;
                }
                if (shift_j < 0) shift_j = 0;
                goto again;
            }
            if (pressed_key[HID_KEY_ARROW_DOWN] || pressed_key[HID_KEY_KEYPAD_2]) {
                selected_file_n++;
                if (selected_file_n >= lines) {
                    selected_file_n = 0;
                    shift_j = 0;
                }
                while (selected_file_n >= shift_j + height_in_j) {
                    shift_j++;
                }
                goto again;
            }
            if (pressed_key[HID_KEY_PAGE_DOWN] || pressed_key[HID_KEY_KEYPAD_3]) {
                selected_file_n += 10;
                if (selected_file_n >= lines) {
                    selected_file_n = lines - 1;
                }
                while (selected_file_n >= shift_j + height_in_j) {
                    shift_j += 10;
                }
                goto again;
            }
            if (pressed_key[HID_KEY_HOME] || pressed_key[HID_KEY_KEYPAD_7]) {
                selected_file_n = 0;
                shift_j = 0;
                goto again;
            }
            if (pressed_key[HID_KEY_END] || pressed_key[HID_KEY_KEYPAD_1]) {
                selected_file_n = lines - 1;
                while (selected_file_n >= shift_j + height_in_j) {
                    shift_j += 10;
                }
                goto again;
            }
            if (pressed_key[HID_KEY_ENTER] || pressed_key[HID_KEY_KEYPAD_ENTER]) {
                break;
            }
        }
        graphics_set_buffer(NULL, DISP_WIDTH, DISP_HEIGHT);
        if (selected_file_n != 11) { // korvet W/A
            delete[] tmp_screen;
        }
        argv[2] = platforms[selected_file_n];
    }
    int argc = 3;
    palInit(argc, (char**)argv);
    CmdLine cmdLine(argc, (char**)argv);
    new Emulation(cmdLine); // g_emulation присваивается в конструкторе
    palExecute();
    __unreachable();
}
