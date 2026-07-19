#include "picoMenu.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <iterator>

#include "graphics.h"
#include "../Globals.h"
#include "../Emulation.h"
#include "../SoundMixer.h"

namespace {

struct MenuPage;

struct MenuItem {
    const char* title;
    const MenuPage* submenu;
};

struct MenuPage {
    const char* title;
    const MenuItem* items;
    int itemCount;
};

static const MenuPage processorPage {"Processor", nullptr, 0};
static const MenuPage storagePage   {"Storage", nullptr, 0};
static const MenuPage romPage       {"ROM", nullptr, 0};
static const MenuPage soundPage     {"Sound", nullptr, 0};
static const MenuPage tapePage      {"Tape", nullptr, 0};
static const MenuPage snapshotPage  {"Snapshots", nullptr, 0};
static const MenuPage videoPage     {"Video", nullptr, 0};
static const MenuPage systemPage    {"System", nullptr, 0};
static const MenuPage aboutPage     {"About", nullptr, 0};

static const MenuItem rootItems[] = {
    {"Processor", &processorPage},
    {"Storage", &storagePage},
    {"ROM", &romPage},
    {"Sound", &soundPage},
    {"Tape", &tapePage},
    {"Snapshots", &snapshotPage},
    {"Video", &videoPage},
    {"System", &systemPage},
    {"About", &aboutPage},
};

static const MenuPage rootPage {
    "Vector-06C 80A",
    rootItems,
    static_cast<int>(sizeof(rootItems) / sizeof(rootItems[0]))
};

int pageHeight(const MenuPage& page)
{
    const int fontH = graphics_get_font_height();
    const int rowH = fontH + 3;
    const int rows = page.itemCount > 0 ? page.itemCount : 1;
    return fontH + 6 + rows * rowH + 2;
}

void drawPage(const MenuPage& page, int selected, int x, int y, int w, int h)
{
    const int fontW = graphics_get_font_width();
    const int fontH = graphics_get_font_height();
    const int rowH = fontH + 3;

    graphics_fill(x + 4, y + 4, w, h, RGB888(32, 32, 32));
    graphics_fill(x, y, w, h, RGB888(232, 232, 232));
    graphics_rect(x, y, w, h, RGB888(0, 0, 0));
    graphics_fill(x + 1, y + 1, w - 2, fontH + 4, RGB888(0, 48, 128));

    graphics_type(x + 5, y + 3, RGB888(255, 255, 255), page.title, std::strlen(page.title));

    if (!page.items || page.itemCount == 0) {
        const char emptyText[] = "Not implemented yet";
        graphics_type(x + 6, y + fontH + 6, RGB888(0, 0, 0), emptyText, sizeof(emptyText) - 1);
    } else {
        for (int i = 0; i < page.itemCount; ++i) {
            const int rowY = y + fontH + 5 + i * rowH;
            const bool current = i == selected;
            graphics_fill(x + 2, rowY, w - 4, rowH,
                          current ? RGB888(64, 96, 192) : RGB888(232, 232, 232));
            graphics_type(x + 6, rowY + 1,
                          current ? RGB888(255, 255, 255) : RGB888(0, 0, 0),
                          page.items[i].title, std::strlen(page.items[i].title));
            if (page.items[i].submenu) {
                const char arrow[] = ">";
                graphics_type(x + w - fontW - 6, rowY + 1,
                              current ? RGB888(255, 255, 255) : RGB888(0, 0, 0),
                              arrow, 1);
            }
        }
    }
}

struct MenuState {
    bool open = false;
    bool wasPaused = false;
    bool wasMuted = false;
    SoundMixer* mixer = nullptr;
    const MenuPage* stack[8] = {};
    int selected[8] = {};
    int depth = 0;
    int screenH = 0;
    int menuW = 0;
};

MenuState menu;

void redrawMenu()
{
    if (!menu.open)
        return;
    const int menuH = std::min(pageHeight(*menu.stack[menu.depth]), menu.screenH);
    drawPage(*menu.stack[menu.depth], menu.selected[menu.depth], 0, 0, menu.menuW, menuH);
}

} // namespace

void palOpenMainMenu()
{
    if (menu.open || !g_emulation)
        return;

    menu.mixer = g_emulation->getSoundMixer();
    menu.wasPaused = g_emulation->getPausedState();
    menu.wasMuted = menu.mixer && menu.mixer->getMuted();

    g_emulation->setPaused(true);
    if (menu.mixer)
        menu.mixer->setMuted(true);

    const int screenW = graphics_get_width();
    menu.screenH = graphics_get_height();
    menu.menuW = std::min(300, screenW - 4);
    // Подложка экрана больше не сохраняется. Прежде под неё выделялось
    // 304 x 119 = 36176 байт из кучи — больше, чем весь остальной постоянный
    // расход, и это выделение соседствовало с выделением списка файлов.
    // Восстанавливать фон незачем: как только эмуляция снимается с паузы,
    // рендерер перерисовывает весь кадр целиком за один проход, то есть за 20 мс.

    menu.stack[0] = &rootPage;
    std::fill(std::begin(menu.selected), std::end(menu.selected), 0);
    menu.depth = 0;
    menu.open = true;
    redrawMenu();
}

void palCloseMainMenu()
{
    if (!menu.open)
        return;

    menu.open = false;
    if (menu.mixer)
        menu.mixer->setMuted(menu.wasMuted);
    if (g_emulation)
        g_emulation->setPaused(menu.wasPaused);
    menu.mixer = nullptr;
}

bool palMainMenuIsOpen()
{
    return menu.open;
}

bool palMainMenuHandleKey(PalKeyCode keyCode, bool isPressed)
{
    if (!menu.open)
        return false;

    if (!isPressed)
        return true;

    const MenuPage* page = menu.stack[menu.depth];
    if (keyCode == PK_ESC) {
        if (menu.depth == 0) {
            palCloseMainMenu();
            return true;
        }
        --menu.depth;
    } else if (page->itemCount > 0 && keyCode == PK_UP) {
        menu.selected[menu.depth] = menu.selected[menu.depth] > 0
            ? menu.selected[menu.depth] - 1 : page->itemCount - 1;
    } else if (page->itemCount > 0 && keyCode == PK_DOWN) {
        menu.selected[menu.depth] = menu.selected[menu.depth] + 1 < page->itemCount
            ? menu.selected[menu.depth] + 1 : 0;
    } else if (page->itemCount > 0 && (keyCode == PK_ENTER || keyCode == PK_KP_ENTER)) {
        const MenuPage* submenu = page->items[menu.selected[menu.depth]].submenu;
        if (submenu && menu.depth + 1 < static_cast<int>(sizeof(menu.stack) / sizeof(menu.stack[0]))) {
            menu.stack[++menu.depth] = submenu;
            menu.selected[menu.depth] = 0;
        }
    } else {
        return true;
    }

    if (menu.open)
        redrawMenu();
    return true;
}
