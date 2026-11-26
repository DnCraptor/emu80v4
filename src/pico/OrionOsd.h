#ifndef ORION_OSD_H
#define ORION_OSD_H

#include "PalKeys.h"
#include "graphics.h"
#include <pico/stdlib.h>
#include <algorithm>
#include <string_view>

extern PalKeyCodeAction getKey();

namespace OrionOsd {

constexpr uint8_t panel = RGB(0xe8e8e8);
constexpr uint8_t title = RGB(0x003080);
constexpr uint8_t selection = RGB(0x40c0ff);
constexpr uint8_t shadow = RGB(0x202020);
constexpr uint8_t black = RGB(0);
constexpr uint8_t white = RGB(0xffffff);
constexpr uint8_t gray = RGB(0x808080);

struct Screen {
    int fw = graphics_get_font_width(), fh = graphics_get_font_height();
    int cols = int(graphics_get_width()) / fw;
    int rows = int(graphics_get_visible_height()) / fh;

    void fill(int x, int y, int w, int h, uint8_t color) const
    {
        if (w > 0 && h > 0) graphics_fill(x * fw, y * fh, w * fw, h * fh, color);
    }

    void text(int x, int y, std::string_view str, int limit, uint8_t fg = black) const
    {
        const auto len = std::min(str.size(), size_t(std::max(0, limit)));
        if (len) graphics_type(x * fw, y * fh, fg, str.data(), len);
    }

    void window(int x, int y, int w, int h, std::string_view caption) const
    {
        fill(x + 1, y + 1, w, h, shadow);
        fill(x, y, w, h, panel);
        fill(x, y, w, 1, title);
        text(x + 1, y, caption, w - 2, white);
    }
};

inline bool closeKey(PalKeyCode key)
{
    return key == PK_F12 || key == PK_LALT || key == PK_RALT;
}

inline bool enterKey(PalKeyCode key)
{
    return key == PK_ENTER || key == PK_KP_ENTER;
}

class NavigationKeys {
public:
    PalKeyCode next()
    {
        const auto event = getKey();
        const uint64_t now = time_us_64();
        if (event.vk != PK_NONE) {
            if (!event.pressed) {
                if (event.vk == held) clear();
                return PK_NONE;
            }
            switch (event.vk) {
            case PK_UP: case PK_DOWN: case PK_KP_8: case PK_KP_2:
            case PK_PGUP: case PK_PGDN: case PK_KP_9: case PK_KP_3:
                held = event.vk;
                due = now + 400000;
                break;
            default:
                clear();
                break;
            }
            return event.vk;
        }
        if (held != PK_NONE && now >= due) {
            due = now + 60000;
            return held;
        }
        return PK_NONE;
    }

    void clear() { held = PK_NONE; }

private:
    PalKeyCode held = PK_NONE;
    uint64_t due = 0;
};

}
#endif
