#include "picoMenu.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <iterator>
#include <string>

#include <pico/time.h>

#include "graphics.h"
#include "../Globals.h"
#include "../Emulation.h"
#include "../Vector.h"
#include "../SoundMixer.h"

namespace {

struct MenuPage;

struct MenuItem {
    const char* title;
    const char* (*getTitle)();
    const MenuPage* submenu;
    void (*action)();
    bool (*isEnabled)();
    bool (*isChecked)();
};

struct MenuPage {
    const char* title;
    // Для страниц, чей заголовок зависит от состояния машины
    const char* (*getTitle)();
    const MenuItem* items;
    int itemCount;
    // Радиогруппа: текущее значение и его установка. Для обычных страниц null.
    int (*getValue)();
    void (*setValue)(int);
};

// --- Processor -------------------------------------------------------------

int cpuGetValue()
{
    if (!g_emulation || !g_emulation->getVector())
        return 0;
    return static_cast<int>(g_emulation->getVector()->getCpuType());
}

void cpuSetValue(int value)
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (!core)
        return;
    core->setCpuType(static_cast<VectorCpuType>(value));
}

static const MenuItem processorItems[] = {
    {"KR580VM80A (i8080)", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"Zilog Z80", nullptr, nullptr, nullptr, nullptr, nullptr},
};

static const MenuPage processorPage {
    "Processor",
    nullptr,
    processorItems,
    static_cast<int>(sizeof(processorItems) / sizeof(processorItems[0])),
    cpuGetValue,
    cpuSetValue
};

// --- Storage / floppy drives ----------------------------------------------

char driveTitleBuffer[2][96];

const char* driveTitle(VectorFloppyDrive drive)
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    const std::string fileName = core ? core->getFloppyFileName(drive) : std::string();
    char* buffer = driveTitleBuffer[static_cast<int>(drive)];
    constexpr char prefix[] = "Drive A: ";
    std::memcpy(buffer, prefix, sizeof(prefix));
    buffer[6] = drive == VectorFloppyDrive::A ? 'A' : 'B';
    if (fileName.empty()) {
        constexpr char empty[] = "empty";
        std::memcpy(buffer + sizeof(prefix) - 1, empty, sizeof(empty));
        return buffer;
    }
    const size_t slash = fileName.find_last_of("/\\");
    const char* base = fileName.c_str() + (slash == std::string::npos ? 0 : slash + 1);
    const size_t prefixLen = sizeof(prefix) - 1;
    constexpr char readOnlySuffix[] = " (ro)";
    const bool readOnly = core && core->floppyImageReadOnly(drive);
    const size_t suffixLen = readOnly ? sizeof(readOnlySuffix) - 1 : 0;
    const size_t baseLen = std::min(std::strlen(base), sizeof(driveTitleBuffer[0]) - prefixLen - suffixLen - 1);
    std::memcpy(buffer + prefixLen, base, baseLen);
    if (readOnly)
        std::memcpy(buffer + prefixLen + baseLen, readOnlySuffix, suffixLen);
    buffer[prefixLen + baseLen + suffixLen] = '\0';
    return buffer;
}

bool driveHasImage(VectorFloppyDrive drive)
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    return core && core->floppyImagePresent(drive);
}
void driveInsert(VectorFloppyDrive drive) { if (g_emulation && g_emulation->getVector()) g_emulation->getVector()->chooseFloppyImage(drive); }
void driveEject(VectorFloppyDrive drive) { if (g_emulation && g_emulation->getVector()) g_emulation->getVector()->ejectFloppyImage(drive); }
bool driveReadOnly(VectorFloppyDrive drive)
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    return core && core->floppyImageReadOnly(drive);
}
bool driveReadOnlyEnabled(VectorFloppyDrive drive)
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (!core || !core->floppyImagePresent(drive))
        return false;
    return core->floppyImageReadOnly(drive)
        ? core->canSetFloppyReadOnly(drive, false)
        : core->canSetFloppyReadOnly(drive, true);
}
void driveToggleReadOnly(VectorFloppyDrive drive)
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (core)
        core->setFloppyReadOnly(drive, !core->floppyImageReadOnly(drive));
}

const char* driveATitle() { return driveTitle(VectorFloppyDrive::A); }
const char* driveBTitle() { return driveTitle(VectorFloppyDrive::B); }
bool driveAHasImage() { return driveHasImage(VectorFloppyDrive::A); }
bool driveBHasImage() { return driveHasImage(VectorFloppyDrive::B); }
void driveAInsert() { driveInsert(VectorFloppyDrive::A); }
void driveBInsert() { driveInsert(VectorFloppyDrive::B); }
void driveAEject() { driveEject(VectorFloppyDrive::A); }
void driveBEject() { driveEject(VectorFloppyDrive::B); }
bool driveAReadOnly() { return driveReadOnly(VectorFloppyDrive::A); }
bool driveBReadOnly() { return driveReadOnly(VectorFloppyDrive::B); }
bool driveAReadOnlyEnabled() { return driveReadOnlyEnabled(VectorFloppyDrive::A); }
bool driveBReadOnlyEnabled() { return driveReadOnlyEnabled(VectorFloppyDrive::B); }
void driveAToggleReadOnly() { driveToggleReadOnly(VectorFloppyDrive::A); }
void driveBToggleReadOnly() { driveToggleReadOnly(VectorFloppyDrive::B); }

static const MenuItem driveAItems[] = {
    {"Insert image [Alt+A]...", nullptr, nullptr, driveAInsert, nullptr, nullptr},
    {"Read only", nullptr, nullptr, driveAToggleReadOnly, driveAReadOnlyEnabled, driveAReadOnly},
    {"Eject", nullptr, nullptr, driveAEject, driveAHasImage, nullptr},
};
static const MenuItem driveBItems[] = {
    {"Insert image [Alt+B]...", nullptr, nullptr, driveBInsert, nullptr, nullptr},
    {"Read only", nullptr, nullptr, driveBToggleReadOnly, driveBReadOnlyEnabled, driveBReadOnly},
    {"Eject", nullptr, nullptr, driveBEject, driveBHasImage, nullptr},
};
char hddTitleBuffer[96];

const char* hddTitle()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    const std::string fileName = core ? core->getHddFileName() : std::string();
    constexpr char prefix[] = "HDD: ";
    std::memcpy(hddTitleBuffer, prefix, sizeof(prefix));
    if (fileName.empty()) {
        constexpr char empty[] = "empty";
        std::memcpy(hddTitleBuffer + sizeof(prefix) - 1, empty, sizeof(empty));
        return hddTitleBuffer;
    }
    const size_t slash = fileName.find_last_of("/\\");
    const char* base = fileName.c_str() + (slash == std::string::npos ? 0 : slash + 1);
    const size_t prefixLen = sizeof(prefix) - 1;
    const size_t baseLen = std::min(std::strlen(base), sizeof(hddTitleBuffer) - prefixLen - 1);
    std::memcpy(hddTitleBuffer + prefixLen, base, baseLen);
    hddTitleBuffer[prefixLen + baseLen] = '\0';
    return hddTitleBuffer;
}

bool hddHasImage()
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    return core && core->hddImagePresent();
}
void hddInsert() { if (g_emulation && g_emulation->getVector()) g_emulation->getVector()->chooseHddImage(); }
void hddEject() { if (g_emulation && g_emulation->getVector()) g_emulation->getVector()->ejectHddImage(); }

static const MenuItem hddItems[] = {
    {"Insert image [Alt+F4]...", nullptr, nullptr, hddInsert, nullptr, nullptr},
    {"Eject", nullptr, nullptr, hddEject, hddHasImage, nullptr},
};
static const MenuPage driveAPage {"Drive A", driveATitle, driveAItems, static_cast<int>(sizeof(driveAItems) / sizeof(driveAItems[0])), nullptr, nullptr};
static const MenuPage driveBPage {"Drive B", driveBTitle, driveBItems, static_cast<int>(sizeof(driveBItems) / sizeof(driveBItems[0])), nullptr, nullptr};
static const MenuPage hddPage {"HDD", hddTitle, hddItems, static_cast<int>(sizeof(hddItems) / sizeof(hddItems[0])), nullptr, nullptr};

void invokeSysReq(SysReq request)
{
    VectorCore* core = g_emulation ? g_emulation->getVector() : nullptr;
    if (core)
        core->sysReq(request);
}

void eddOpen() { invokeSysReq(SR_OPENRAMDISK); }
void eddSaveAs() { invokeSysReq(SR_SAVERAMDISKAS); }
void edd2Open() { invokeSysReq(SR_OPENRAMDISK2); }
void edd2SaveAs() { invokeSysReq(SR_SAVERAMDISK2AS); }

static const MenuItem eddItems[] = {
    {"Load image [Alt+E]...", nullptr, nullptr, eddOpen, nullptr, nullptr},
    {"Save image as [Alt+O]...", nullptr, nullptr, eddSaveAs, nullptr, nullptr},
};
static const MenuItem edd2Items[] = {
    {"Load image [Alt+Shift+E]...", nullptr, nullptr, edd2Open, nullptr, nullptr},
    {"Save image as [Alt+Shift+O]...", nullptr, nullptr, edd2SaveAs, nullptr, nullptr},
};
static const MenuPage eddPage {"EDD", nullptr, eddItems, static_cast<int>(sizeof(eddItems) / sizeof(eddItems[0])), nullptr, nullptr};
static const MenuPage edd2Page {"EDD2", nullptr, edd2Items, static_cast<int>(sizeof(edd2Items) / sizeof(edd2Items[0])), nullptr, nullptr};

static const MenuItem storageItems[] = {
    {"Drive A", driveATitle, &driveAPage, nullptr, nullptr, nullptr},
    {"Drive B", driveBTitle, &driveBPage, nullptr, nullptr, nullptr},
    {"HDD", hddTitle, &hddPage, nullptr, nullptr, nullptr},
    {"EDD", nullptr, &eddPage, nullptr, nullptr, nullptr},
    {"EDD2", nullptr, &edd2Page, nullptr, nullptr, nullptr},
};
static const MenuPage storagePage {"Storage", nullptr, storageItems, static_cast<int>(sizeof(storageItems) / sizeof(storageItems[0])), nullptr, nullptr};
static const MenuPage romPage       {"ROM", nullptr, nullptr, 0, nullptr, nullptr};
static const MenuPage soundPage     {"Sound", nullptr, nullptr, 0, nullptr, nullptr};
static const MenuPage tapePage      {"Tape", nullptr, nullptr, 0, nullptr, nullptr};
static const MenuPage snapshotPage  {"Snapshots", nullptr, nullptr, 0, nullptr, nullptr};
static const MenuPage videoPage     {"Video", nullptr, nullptr, 0, nullptr, nullptr};
static const MenuPage systemPage    {"System", nullptr, nullptr, 0, nullptr, nullptr};
static const MenuPage aboutPage     {"About", nullptr, nullptr, 0, nullptr, nullptr};

static const MenuItem rootItems[] = {
    {"Processor", nullptr, &processorPage, nullptr, nullptr, nullptr},
    {"Storage", nullptr, &storagePage, nullptr, nullptr, nullptr},
    {"ROM", nullptr, &romPage, nullptr, nullptr, nullptr},
    {"Sound", nullptr, &soundPage, nullptr, nullptr, nullptr},
    {"Tape", nullptr, &tapePage, nullptr, nullptr, nullptr},
    {"Snapshots", nullptr, &snapshotPage, nullptr, nullptr, nullptr},
    {"Video", nullptr, &videoPage, nullptr, nullptr, nullptr},
    {"System", nullptr, &systemPage, nullptr, nullptr, nullptr},
    {"About", nullptr, &aboutPage, nullptr, nullptr, nullptr},
};

// Заголовок корневой страницы отражает установленное ядро
const char* rootTitle()
{
    if (g_emulation && g_emulation->getVector()
        && g_emulation->getVector()->getCpuType() == VECTOR_CPU_Z80)
        return "Vector-06C Z80";
    return "Vector-06C 80A";
}

static const MenuPage rootPage {
    "Vector-06C 80A",
    rootTitle,
    rootItems,
    static_cast<int>(sizeof(rootItems) / sizeof(rootItems[0])),
    nullptr,
    nullptr
};

// ---------------------------------------------------------------------------
//  Подложка под каскад подменю.
//
//  Главное меню фон не сохраняет: закрывается оно вместе со снятием паузы,
//  и рендерер перерисовывает кадр целиком за один проход.
//
//  А вот подменю раскрываются вбок, вправо от родителя, поверх изображения
//  машины, и по «влево» область под ними должна вернуться к прежнему виду.
//  Поэтому фон запоминается для каждого уровня отдельно, стеком: вход на
//  уровень кладёт прямоугольник в пул, возврат снимает его оттуда.
// ---------------------------------------------------------------------------
// Задержка до начала автоповтора и период повторов
static constexpr uint64_t c_repeatDelayUs = 400000;
static constexpr uint64_t c_repeatRateUs  = 60000;

static constexpr int c_menuBackupPool = 32768;
static constexpr int c_menuMaxDepth = 8;

static uint8_t s_menuBackup[c_menuBackupPool];
static int s_backupUsed = 0;

struct BackupRec {
    int x, y, w, h, offset;
    bool valid;
};
static BackupRec s_backup[c_menuMaxDepth];

void pushBackground(int depth, int x, int y, int w, int h)
{
    BackupRec& r = s_backup[depth];
    r.valid = false;
    uint8_t* frame = graphics_get_frame();
    if (!frame || depth < 0 || depth >= c_menuMaxDepth)
        return;

    const int screenW = graphics_get_width();
    const int screenH = graphics_get_height();
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > screenW) w = screenW - x;
    if (y + h > screenH) h = screenH - y;
    if (w <= 0 || h <= 0 || s_backupUsed + w * h > c_menuBackupPool)
        return;   // не влезло — просто не восстановим, кадр всё равно перерисуется

    r = {x, y, w, h, s_backupUsed, true};
    for (int row = 0; row < h; ++row)
        std::memcpy(s_menuBackup + r.offset + row * w,
                    frame + (y + row) * screenW + x, w);
    s_backupUsed += w * h;
}

void popBackground(int depth)
{
    if (depth < 0 || depth >= c_menuMaxDepth)
        return;
    BackupRec& r = s_backup[depth];
    uint8_t* frame = graphics_get_frame();
    if (r.valid && frame) {
        const int screenW = graphics_get_width();
        for (int row = 0; row < r.h; ++row)
            std::memcpy(frame + (r.y + row) * screenW + r.x,
                        s_menuBackup + r.offset + row * r.w, r.w);
        s_backupUsed = r.offset;
    }
    r.valid = false;
}

int pageHeight(const MenuPage& page)
{
    const int fontH = graphics_get_font_height();
    const int rowH = fontH + 3;
    const int rows = page.itemCount > 0 ? page.itemCount : 1;
    return fontH + 6 + rows * rowH + 2;
}

int backupW = 0;
int backupH = 0;
bool backupValid = false;

// Геометрия строки списка внутри страницы
int rowTop(int y, int index)
{
    const int fontH = graphics_get_font_height();
    return y + fontH + 5 + index * (fontH + 3);
}

// Одна строка списка. Вынесена отдельно, чтобы при перемещении выделения
// перерисовывать только две строки — ту, с которой ушли, и ту, на которую
// пришли. Полная перерисовка страницы на каждое нажатие давала заметное
// мигание.
void drawItem(const MenuPage& page, int index, int selected, int x, int y, int w)
{
    if (!page.items || index < 0 || index >= page.itemCount)
        return;

    const int fontW = graphics_get_font_width();
    const int fontH = graphics_get_font_height();
    const int rowH = fontH + 3;
    const int rowY = rowTop(y, index);
    const bool current = index == selected;
    const bool enabled = !page.items[index].isEnabled || page.items[index].isEnabled();
    const uint8_t fg = !enabled ? RGB888(128, 128, 128)
                     : current ? RGB888(255, 255, 255) : RGB888(0, 0, 0);

    graphics_fill(x + 2, rowY, w - 4, rowH,
                  current ? RGB888(64, 96, 192) : RGB888(232, 232, 232));
    const char* itemTitle = page.items[index].getTitle
                          ? page.items[index].getTitle()
                          : page.items[index].title;
    graphics_type(x + 6, rowY + 1, fg, itemTitle, std::strlen(itemTitle));

    if ((page.getValue && page.getValue() == index)
        || (page.items[index].isChecked && page.items[index].isChecked())) {
        const char mark[] = "*";
        graphics_type(x + w - fontW - 6, rowY + 1, fg, mark, 1);
    }
    if (page.items[index].submenu) {
        const char arrow[] = ">";
        graphics_type(x + w - fontW - 6, rowY + 1, fg, arrow, 1);
    }
}

void drawPage(const MenuPage& page, int selected, int x, int y, int w, int h)
{
    const int fontH = graphics_get_font_height();

    graphics_fill(x + 4, y + 4, w, h, RGB888(32, 32, 32));
    graphics_fill(x, y, w, h, RGB888(232, 232, 232));
    graphics_rect(x, y, w, h, RGB888(0, 0, 0));
    graphics_fill(x + 1, y + 1, w - 2, fontH + 4, RGB888(0, 48, 128));

    const char* title = page.getTitle ? page.getTitle() : page.title;
    graphics_type(x + 5, y + 3, RGB888(255, 255, 255), title, std::strlen(title));

    if (!page.items || page.itemCount == 0) {
        const char emptyText[] = "Not implemented yet";
        graphics_type(x + 6, y + fontH + 6, RGB888(0, 0, 0), emptyText, sizeof(emptyText) - 1);
        return;
    }

    for (int i = 0; i < page.itemCount; ++i)
        drawItem(page, i, selected, x, y, w);
}

struct MenuState {
    bool open = false;
    // Автоповтор навигации. Ни HID-слой, ни PS/2-слой повторов не шлют:
    // события идут только на изменение состояния клавиши, а repeat_handler()
    // в Main.cpp — пустая заглушка. Файловый диалог обходится опросом
    // pressed_key[] в собственном цикле, а меню событийное, поэтому повтор
    // отсчитывается здесь, по тикам основного цикла.
    PalKeyCode repeatKey = PK_NONE;
    uint64_t repeatAt = 0;
    bool wasPaused = false;
    bool wasMuted = false;
    SoundMixer* mixer = nullptr;
    const MenuPage* stack[c_menuMaxDepth] = {};
    int selected[c_menuMaxDepth] = {};
    int pageX[c_menuMaxDepth] = {};
    int pageY[c_menuMaxDepth] = {};
    int pageW[c_menuMaxDepth] = {};
    int pageH[c_menuMaxDepth] = {};
    int depth = 0;
    int screenH = 0;
    int menuW = 0;
};

MenuState menu;

// Перерисовывает только текущий уровень: родительские страницы остаются
// на экране, каскад никуда не двигается
void redrawMenu()
{
    if (!menu.open)
        return;
    const int d = menu.depth;
    drawPage(*menu.stack[d], menu.selected[d],
             menu.pageX[d], menu.pageY[d], menu.pageW[d], menu.pageH[d]);
}


// Перемещение выделения: перерисовываются ровно две строки
void redrawSelection(int oldSel, int newSel)
{
    if (!menu.open || oldSel == newSel)
        return;
    const int d = menu.depth;
    const MenuPage& page = *menu.stack[d];
    drawItem(page, oldSel, newSel, menu.pageX[d], menu.pageY[d], menu.pageW[d]);
    drawItem(page, newSel, newSel, menu.pageX[d], menu.pageY[d], menu.pageW[d]);
}


// Шаг выделения с заворотом. Возвращает true, если позиция изменилась.
bool moveSelection(int delta)
{
    const int d = menu.depth;
    const MenuPage* page = menu.stack[d];
    if (page->itemCount <= 0)
        return false;
    const int oldSel = menu.selected[d];
    int sel = oldSel + delta;
    if (sel < 0) sel = page->itemCount - 1;
    else if (sel >= page->itemCount) sel = 0;
    menu.selected[d] = sel;
    redrawSelection(oldSel, sel);
    return sel != oldSel;
}


// Ширина страницы по самому длинному пункту
int pageWidth(const MenuPage& page)
{
    const int fontW = graphics_get_font_width();
    size_t longest = std::strlen(page.getTitle ? page.getTitle() : page.title);
    for (int i = 0; i < page.itemCount; ++i) {
        const char* itemTitle = page.items[i].getTitle
                              ? page.items[i].getTitle()
                              : page.items[i].title;
        longest = std::max(longest, std::strlen(itemTitle));
    }
    if (page.itemCount == 0)
        longest = std::max(longest, sizeof("Not implemented yet") - 1);
    return static_cast<int>(longest + 4) * fontW;
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

    // Главное меню фон не сохраняет: закрывается оно вместе со снятием паузы,
    // и рендерер перерисует кадр целиком за один проход
    s_backupUsed = 0;
    for (int i = 0; i < c_menuMaxDepth; ++i)
        s_backup[i].valid = false;

    menu.menuW = std::min(pageWidth(rootPage), screenW - 4);
    menu.pageX[0] = 0;
    menu.pageY[0] = 0;
    menu.pageW[0] = menu.menuW;
    menu.pageH[0] = std::min(pageHeight(rootPage), menu.screenH);

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

    // Уровни снимаются в обратном порядке, чтобы фон под каскадом вернулся
    for (int d = menu.depth; d >= 1; --d)
        popBackground(d);
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

    if (!isPressed) {
        // Отпускание клавиши гасит автоповтор
        if (keyCode == menu.repeatKey)
            menu.repeatKey = PK_NONE;
        return true;
    }

    // Новое нажатие всегда перезапускает отсчёт
    menu.repeatKey = PK_NONE;

    const MenuPage* page = menu.stack[menu.depth];
    const bool enter = keyCode == PK_ENTER || keyCode == PK_KP_ENTER
                    || keyCode == PK_SPACE;

    if (keyCode == PK_ESC || keyCode == PK_LEFT) {
        // Влево и Esc возвращают на уровень выше, с верхнего — закрывают меню
        if (menu.depth == 0) {
            if (keyCode == PK_ESC)
                palCloseMainMenu();
            return true;
        }
        // Фон под закрываемым подменю возвращается на место
        popBackground(menu.depth);
        --menu.depth;
        return true;   // родительская страница уже на экране, перерисовка не нужна
    } else if (page->itemCount > 0 && (keyCode == PK_UP || keyCode == PK_DOWN)) {
        moveSelection(keyCode == PK_UP ? -1 : 1);
        menu.repeatKey = keyCode;
        menu.repeatAt = time_us_64() + c_repeatDelayUs;
        return true;   // перерисованы только две строки, полная не нужна
    } else if (page->itemCount > 0 && (keyCode == PK_RIGHT || enter)) {
        const int sel = menu.selected[menu.depth];

        if (page->setValue) {
            // Страница-радиогруппа: выбор пункта меняет значение.
            // Меню закрывается, машина сбрасывается — так требует смена ядра.
            if (page->getValue && page->getValue() != sel) {
                page->setValue(sel);
                palCloseMainMenu();
                if (g_emulation && g_emulation->getVector())
                    g_emulation->getVector()->reset();
                return true;
            }
            if (keyCode == PK_RIGHT)
                return true;   // значение уже выбрано, вправо ничего не делает
            palCloseMainMenu();
            return true;
        }

        const MenuItem& item = page->items[sel];
        if (item.isEnabled && !item.isEnabled())
            return true;
        if (item.action) {
            item.action();
            palCloseMainMenu();
            return true;
        }

        const MenuPage* submenu = item.submenu;
        if (!submenu)
            return true;
        if (menu.depth + 1 >= static_cast<int>(sizeof(menu.stack) / sizeof(menu.stack[0])))
            return true;
        // Каскад: подменю раскрывается вправо от родителя, верх выравнивается
        // по выбранному пункту
        const int d = menu.depth;
        const int fontH = graphics_get_font_height();
        const int rowH = fontH + 3;
        const int screenW = graphics_get_width();

        int nx = menu.pageX[d] + menu.pageW[d] + 2;
        int ny = menu.pageY[d] + fontH + 5 + sel * rowH - 2;
        int nw = std::min(pageWidth(*submenu), screenW - 4);
        int nh = std::min(pageHeight(*submenu), menu.screenH);

        if (nx + nw + 4 > screenW) nx = std::max(0, screenW - nw - 4);
        if (ny + nh + 4 > menu.screenH) ny = std::max(0, menu.screenH - nh - 4);
        if (ny < 0) ny = 0;

        // graphics_fill и graphics_rect работают по включительным границам:
        // x1 = x0 + width, цикл xi <= x1, то есть закрашивается width + 1
        // пиксель. Страница вместе с тенью занимает nw + 5 на nh + 5, и
        // сохранять надо именно столько — иначе от тени остаётся полоска
        // в один пиксель.
        pushBackground(d + 1, nx, ny, nw + 5, nh + 5);

        menu.stack[++menu.depth] = submenu;
        menu.pageX[menu.depth] = nx;
        menu.pageY[menu.depth] = ny;
        menu.pageW[menu.depth] = nw;
        menu.pageH[menu.depth] = nh;
        // В радиогруппе изначально выделено текущее значение
        menu.selected[menu.depth] = submenu->getValue ? submenu->getValue() : 0;
        if (menu.selected[menu.depth] < 0 || menu.selected[menu.depth] >= submenu->itemCount)
            menu.selected[menu.depth] = 0;
    } else {
        return true;
    }

    if (menu.open)
        redrawMenu();
    return true;
}


void palMessageBox(const char* title, const char* text)
{
    const int screenW = graphics_get_width();
    const int screenH = graphics_get_height();
    const int fontW = graphics_get_font_width();
    const int fontH = graphics_get_font_height();

    const int textLen = static_cast<int>(std::strlen(text));
    const int titleLen = static_cast<int>(std::strlen(title));
    int w = (std::max(textLen, titleLen) + 3) * fontW;
    if (w > screenW - 8) w = screenW - 8;
    const int h = fontH * 2 + 12;
    const int x = (screenW - w) / 2;
    const int y = (screenH - h) / 2;

    graphics_fill(x + 4, y + 4, w, h, RGB888(32, 32, 32));            // тень
    graphics_fill(x, y, w, h, RGB888(232, 232, 232));
    graphics_rect(x, y, w, h, RGB888(0, 0, 0));
    graphics_fill(x + 1, y + 1, w - 2, fontH + 4, RGB888(0, 48, 128)); // заголовок
    graphics_type(x + 5, y + 3, RGB888(255, 255, 255), title, titleLen);
    graphics_type(x + 5, y + fontH + 8, RGB888(0, 0, 0), text, textLen);
}


void palMainMenuTick()
{
    if (!menu.open || menu.repeatKey == PK_NONE)
        return;

    const uint64_t now = time_us_64();
    if (now < menu.repeatAt)
        return;

    moveSelection(menu.repeatKey == PK_UP ? -1 : 1);
    menu.repeatAt = now + c_repeatRateUs;
}
