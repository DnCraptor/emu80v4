#include "picoPal.h"
#include "ffPalFile.h"

#include <sstream>
#include <iostream>
#include <memory>
#include <new>

#include <pico/stdlib.h>
#include <hardware/pio.h>

int palReadFromFile(const string& fileName, int offset, int sizeToRead, uint8_t* buffer, bool useBasePath)
{
#if LOG
    emuLog << "palReadFromFile: " << fileName << "\n";
#endif
    string fullFileName;
    if (useBasePath)
        fullFileName = palMakeFullFileName(fileName);
    else
        fullFileName = fileName;

    FIL file;
    if (f_open(&file, fullFileName.c_str(), FA_READ) == FR_OK) {
        f_lseek(&file, offset);
        UINT nBytesRead;
        f_read(&file, buffer, sizeToRead, &nBytesRead);
        f_close(&file);
        return nBytesRead;
    }
    return 0;
}

void palLog(const std::string& s) {
#if LOG
    static FIL pl;
    gpio_put(PICO_DEFAULT_LED_PIN, true);
    f_open(&pl, "/emu80.log", FA_WRITE | FA_OPEN_APPEND);
    UINT bw;
    f_write(&pl, s.c_str(), s.length(), &bw);
    f_close(&pl);
    gpio_put(PICO_DEFAULT_LED_PIN, false);
#endif
}

EmuLog& EmuLog::operator<<(const string& s)
{
#if LOG
    palLog(s);
#endif
    return *this;
}


EmuLog& EmuLog::operator<<(const char* sz)
{
#if LOG
    string s = sz;
    palLog(s);
#endif
    return *this;
}


EmuLog& EmuLog::operator<<(int n)
{
#if LOG
    ostringstream oss;
    oss << n;
    string s = oss.str();
    palLog(s);
#endif
    return *this;
}

EmuLog emuLog;

string palMakeFullFileName(string fileName)
{
    if (fileName[0] == '\0' || fileName[0] == '/' || fileName[0] == '\\')
        return fileName;
    string fullFileName("/vector06c/");
    fullFileName += fileName;
    return fullFileName;
}

static constexpr int MAX_FILE_DIALOG_ITEMS = 256;

static int palGetDirContent(const string& d, PalFileInfo* fileList, int maxItems)
{
    DIR dir;
    FILINFO entry;
    if (f_opendir(&dir, d.c_str()) != FR_OK)
        return 0;

    int count = 0;
    while (count < maxItems && f_readdir(&dir, &entry) == FR_OK && entry.fname[0] != '\0') {
        PalFileInfo& file = fileList[count++];
        file.fileName = entry.fname;
        file.isDir = (entry.fattrib & AM_DIR) != 0;
    }
    f_closedir(&dir);
    return count;
}

/// TODO: .h
extern PalKeyCodeAction getKey();
extern PalKeyCode pressed_key[256];
#include "ps2kbd_mrmltr.h"
#include "picoMenu.h"
#include <algorithm>
static std::string fdir = "/vector06c";

// Состояние карты. Монтирование при старте могло не удаться (карта не
// вставлена), поэтому результат запоминается, а повторная попытка делается
// при каждом обращении к файловому диалогу.
static FATFS s_sdFs;
static bool s_sdMounted = false;

void palSetSdMounted(bool mounted)
{
    s_sdMounted = mounted;
}

bool palEnsureSdMounted()
{
    if (s_sdMounted)
        return true;
    if (f_mount(&s_sdFs, "SD", 1) != FR_OK)
        return false;
    s_sdMounted = true;
    f_mkdir("/vector06c");
    return true;
}

// Сообщение в стиле меню с ожиданием клавиши
void palModalMessage(const char* title, const char* text)
{
    palMessageBox(title, text);
    while (1) {
        sleep_ms(100);
        const PalKeyCodeAction k = getKey();
        if (k.pressed && (k.vk == PK_ESC || k.vk == PK_ENTER || k.vk == PK_KP_ENTER))
            break;
    }
}
std::string palOpenFileDialog(const std::string& title, const std::string& filter, bool write, bool* readOnly) {
    uint32_t sw = graphics_get_width();
    uint32_t sh = graphics_get_height();
    uint32_t w = sw - 10;
    uint32_t h = sh - 6;
    int32_t x = (sw - w) / 2;
    int32_t y = (sh - h) / 2;
    uint32_t fntw = graphics_get_font_width();
    uint32_t fnth = graphics_get_font_height();
    uint32_t msi = fnth + 1;
    uint32_t xb = x + 2;
    const uint32_t contentTop = y + fnth + 5;
    const uint32_t inputBoxH = write ? fnth + 4 : 0;
    const uint32_t hintLineH = write ? fnth + 2 : 0;
    const uint32_t bottomAreaH = inputBoxH + hintLineH + (write ? 2 : 0);
    const uint32_t listBottom = y + h - 1 - bottomAreaH;
    uint32_t yb = contentTop;
    const uint32_t inputY = listBottom + 2;
    const uint32_t hintY = inputY + inputBoxH;
    const uint32_t scrollW = 4;
    const uint32_t listW = w - 2 - scrollW;
    const uint32_t scrollX = x + w - 1 - scrollW;
    const char back[] = "..";

    // Карта могла быть не смонтирована при старте или вынута позже.
    // Пробуем ещё раз здесь, при каждом открытии диалога.
    if (!palEnsureSdMounted()) {
        palModalMessage("No SD-card", "Insert a card and try again");
        return "";
    }

    // Каталог проверяется ДО отрисовки: раньше рамка рисовалась первой, и при
    // неудаче диалог закрывался сразу после неё — на экране это выглядело
    // как мелькание без объяснений.
    // Отката на корень тут нет намеренно: он затирал текущий каталог, и в
    // сообщении об ошибке всегда оказывался корень вместо реального пути.
    DIR f_dir;
    if (f_opendir(&f_dir, fdir.c_str()) != FR_OK) {
        // Сначала сообщаем про реальный путь, и только потом откатываемся
        // на корень. Если делать наоборот, в сообщении всегда оказывается
        // корень; если не откатываться вовсе, исчезнувший с карты каталог
        // блокирует диалог навсегда.
        const string msg = "Cannot open " + fdir;
        palModalMessage("Error", msg.c_str());
        fdir = "/";
        if (f_opendir(&f_dir, fdir.c_str()) != FR_OK)
            return "";   // и корень недоступен — выходим молча
    }
    f_closedir(&f_dir);

    graphics_rect(x, y, w, h, RGB888(0, 0, 0));
    graphics_fill(x + 1, y + 1, w - 2, fnth + 2, 0b000101);
    graphics_rect(x, y, w, fnth + 4, RGB888(0, 0, 0));

    string t2;

    // Список статический. В патче J он был переведён в кучу ради экономии
    // 14 КБ, но выделение может не удаться, и тогда диалог молча закрывался
    // сразу после отрисовки рамки. Диалог модальный и существует ровно один,
    // так что память ему полагается фиксированная: 7168 байт в .bss вместо
    // прежних 14336 — сокращение из патча J (обрезанная структура) осталось.
    static PalFileInfo fileList[MAX_FILE_DIALOG_ITEMS];
    int fileCount = 0;
    int selected_file_n = 0;
    int shift_j = 0;
    int visibleRows = (int)((listBottom - yb) / msi);
    if (visibleRows < 1) visibleRows = 1;
    PalFileInfo* selected_fi = nullptr;
    bool openReadOnly = readOnly && *readOnly;

    auto loadDirectory = [&]() {
        fileCount = 0;
        if (fdir.length() > 1) {
            fileList[fileCount].fileName = back;
            fileList[fileCount].isDir = true;
            fileCount++;
        }
        fileCount += palGetDirContent(fdir, fileList + fileCount, MAX_FILE_DIALOG_ITEMS - fileCount);
        sort(fileList, fileList + fileCount, [](const PalFileInfo& a, const PalFileInfo& b) {
            if (a.isDir == b.isDir) return a.fileName < b.fileName;
            return a.isDir > b.isDir;
        });
    };

    auto drawTitle = [&]() {
        graphics_fill(x + 1, y + 1, w - 2, fnth + 2, 0b000101);
        string t = title + ": " + fdir;
        if (readOnly)
            t += openReadOnly ? " [RO]" : " [RW]";
        uint32_t xt = x + 1;
        if (t.length() * fntw < w - 2)
            xt = x + 1 + (w - 2 - t.length() * fntw) / 2;
        graphics_type(xt, y + 3, 0b101010, t.c_str(), t.length());
    };

    bool inputFocused = write;
    bool inputCursorVisible = true;
    int inputCursorTicks = 0;
    auto drawInput = [&]() {
        if (!write) return;
        const char label[] = "File name: ";
        const size_t labelLen = sizeof(label) - 1;
        const uint32_t bg = inputFocused ? RGB888(207, 255, 255) : RGB888(224, 224, 224);
        graphics_fill(x + 1, inputY, w - 2, inputBoxH, RGB888(0, 0, 0));
        graphics_fill(x + 2, inputY + 1, w - 4, inputBoxH - 2, bg);
        graphics_type(x + 4, inputY + 2, RGB888(0, 0, 0), label, labelLen);
        const uint32_t textX = x + 4 + labelLen * fntw;
        graphics_type(textX, inputY + 2, RGB888(0, 0, 0), t2.c_str(), t2.length());
        if (inputFocused && inputCursorVisible) {
            const uint32_t cursorX = textX + t2.length() * fntw;
            if (cursorX < x + w - 3)
                graphics_fill(cursorX, inputY + 2, 1, fnth, RGB888(0, 0, 0));
        }
        const char hint[] = "Tab: switch  Enter: open/save  Esc: cancel";
        graphics_fill(x + 1, hintY, w - 2, hintLineH, RGB888(0, 0, 0));
        size_t hintLen = sizeof(hint) - 1;
        const size_t maxHintChars = (w - 4) / fntw;
        if (hintLen > maxHintChars) hintLen = maxHintChars;
        graphics_type(x + 3, hintY + 1, RGB888(192, 192, 192), hint, hintLen);
    };

    auto drawRow = [&](int itemIndex) {
        if (itemIndex < shift_j || itemIndex >= shift_j + visibleRows) return;
        int row = itemIndex - shift_j;
        uint32_t rowY = yb + row * msi;
        bool selected = itemIndex == selected_file_n;
        bool activeSelection = selected && (!write || !inputFocused);
        uint32_t bg = activeSelection ? RGB888(114, 114, 224)
                                      : selected ? RGB888(208, 208, 208) : RGB888(255, 255, 255);
        uint32_t fg = activeSelection ? RGB888(255, 255, 255) : RGB888(0, 0, 0);
        graphics_fill(x + 1, rowY, listW, fnth, bg);
        if (itemIndex >= 0 && itemIndex < fileCount) {
            const PalFileInfo& fi = fileList[itemIndex];
            string name = fi.isDir ? "<" + fi.fileName + ">" : fi.fileName;
            size_t maxChars = listW > 2 ? (listW - 2) / fntw : 0;
            if (name.length() > maxChars) name.resize(maxChars);
            graphics_type(xb, rowY, fg, name.c_str(), name.length());
        }
    };

    auto drawScrollbar = [&]() {
        uint32_t trackY = yb;
        uint32_t trackH = visibleRows * msi;
        graphics_fill(scrollX, trackY, scrollW, trackH, RGB888(224, 224, 224));
        int total = fileCount;
        if (total <= visibleRows || trackH == 0) return;
        uint32_t thumbH = (uint32_t)((uint64_t)trackH * visibleRows / total);
        if (thumbH < 4) thumbH = 4;
        if (thumbH > trackH) thumbH = trackH;
        int maxShift = total - visibleRows;
        uint32_t thumbY = trackY + (uint32_t)((uint64_t)(trackH - thumbH) * shift_j / maxShift);
        graphics_fill(scrollX, thumbY, scrollW, thumbH, RGB888(96, 96, 96));
    };

    auto drawWindow = [&]() {
        for (int row = 0; row < visibleRows; ++row)
            drawRow(shift_j + row);
        drawScrollbar();
    };

    loadDirectory();
    drawTitle();
    drawInput();
    drawWindow();

    auto makePath = [&](const string& name) {
        return fdir == "/" ? "/" + name : fdir + "/" + name;
    };

    auto redrawDialog = [&]() {
        graphics_rect(x, y, w, h, RGB888(0, 0, 0));
        graphics_rect(x, y, w, fnth + 4, RGB888(0, 0, 0));
        drawTitle();
        drawInput();
        drawWindow();
    };

    auto confirmOverwrite = [&](const string& path) {
        const size_t slash = path.find_last_of('/');
        const string name = slash == string::npos ? path : path.substr(slash + 1);
        const string message = "Replace " + name + "?  Enter=yes  Esc=no";
        palMessageBox("Overwrite file", message.c_str());
        while (1) {
            sleep_ms(100);
            const PalKeyCodeAction k = getKey();
            if (!k.pressed) continue;
            if (k.vk == PK_ENTER || k.vk == PK_KP_ENTER) {
                redrawDialog();
                return true;
            }
            if (k.vk == PK_ESC) {
                redrawDialog();
                return false;
            }
        }
    };

    string res;
    while (1) {
        sleep_ms(100);
        PalKeyCodeAction pk = getKey();

        if (write && inputFocused && ++inputCursorTicks >= 5) {
            inputCursorTicks = 0;
            inputCursorVisible = !inputCursorVisible;
            drawInput();
        }

        if (write && pk.pressed && pk.vk == PK_TAB) {
            inputFocused = !inputFocused;
            inputCursorVisible = true;
            inputCursorTicks = 0;
            drawInput();
            if (fileCount > 0) drawRow(selected_file_n);
            continue;
        }

        if (write && inputFocused && pk.pressed) {
            bool changed = true;
            if (pk.vk >= PK_1 && pk.vk <= PK_0) t2 += '1' + pk.vk - PK_1;
            else if (pk.vk >= PK_A && pk.vk <= PK_Z) t2 += 'a' + pk.vk - PK_A;
            else if (pk.vk == PK_SPACE) t2 += ' ';
            else if (pk.vk == PK_PERIOD) t2 += '.';
            else if (pk.vk == PK_BSP && !t2.empty()) t2.pop_back();
            else changed = false;
            if (changed) {
                inputCursorVisible = true;
                inputCursorTicks = 0;
                drawInput();
                continue;
            }
        }

        if (readOnly && pk.vk == PK_SCRLOCK && pk.pressed) {
            openReadOnly = !openReadOnly;
            drawTitle();
            continue;
        }

        const bool enterPressed = (pk.vk == PK_ENTER || pk.vk == PK_KP_ENTER) && pk.pressed;
        if (write && inputFocused && enterPressed) {
            if (t2.empty()) continue;
            const string path = makePath(t2);
            FILINFO existing;
            if (f_stat(path.c_str(), &existing) == FR_OK) {
                if (existing.fattrib & AM_DIR) {
                    palModalMessage("Cannot save", "A directory has this name");
                    redrawDialog();
                    continue;
                }
                if (!confirmOverwrite(path))
                    continue;
            }
            res = path;
            break;
        }

        if (pk.vk == PK_ESC && pk.pressed)
            break;

        if (write && inputFocused)
            continue;

        int oldSelected = selected_file_n;
        int oldShift = shift_j;
        int count = fileCount;
        if (count == 0) continue;

        if (pressed_key[HID_KEY_ARROW_UP] || pressed_key[HID_KEY_KEYPAD_8]) {
            selected_file_n = selected_file_n > 0 ? selected_file_n - 1 : count - 1;
        } else if (pressed_key[HID_KEY_PAGE_UP] || pressed_key[HID_KEY_KEYPAD_9]) {
            selected_file_n = std::max(0, selected_file_n - visibleRows);
        } else if (pressed_key[HID_KEY_ARROW_DOWN] || pressed_key[HID_KEY_KEYPAD_2]) {
            selected_file_n = selected_file_n + 1 < count ? selected_file_n + 1 : 0;
        } else if (pressed_key[HID_KEY_PAGE_DOWN] || pressed_key[HID_KEY_KEYPAD_3]) {
            selected_file_n = std::min(count - 1, selected_file_n + visibleRows);
        } else if ((pk.vk == PK_HOME || pk.vk == PK_KP_7) && pk.pressed) {
            selected_file_n = 0;
        } else if (pressed_key[HID_KEY_END] || pressed_key[HID_KEY_KEYPAD_1]) {
            selected_file_n = count - 1;
        } else if (enterPressed) {
            selected_fi = &fileList[selected_file_n];
            if (selected_fi->isDir) {
                if (selected_fi->fileName == back) {
                    fdir = fdir.substr(0, fdir.find_last_of('/'));
                    if (fdir.empty()) fdir = "/";
                } else {
                    if (fdir.length() > 1) fdir += "/";
                    fdir += selected_fi->fileName;
                }
                selected_file_n = 0;
                shift_j = 0;
                loadDirectory();
                drawTitle();
                drawWindow();
                continue;
            }
            if (write) {
                t2 = selected_fi->fileName;
                inputFocused = true;
                inputCursorVisible = true;
                inputCursorTicks = 0;
                drawInput();
                drawRow(selected_file_n);
                continue;
            }
            res = makePath(selected_fi->fileName);
            if (readOnly) *readOnly = openReadOnly;
            break;
        } else {
            continue;
        }

        if (selected_file_n < shift_j) shift_j = selected_file_n;
        if (selected_file_n >= shift_j + visibleRows) shift_j = selected_file_n - visibleRows + 1;
        int maxShift = std::max(0, count - visibleRows);
        if (shift_j > maxShift) shift_j = maxShift;

        if (shift_j != oldShift) {
            drawWindow();
        } else if (selected_file_n != oldSelected) {
            drawRow(oldSelected);
            drawRow(selected_file_n);
        }
    }

    // Строки имён держат блоки в куче; список статический, поэтому чистим сами
    for (int i = 0; i < fileCount; i++)
        fileList[i].fileName.clear();

    return res;
}

#include "../EmuCalls.h"

void palExecute() {
    while(1) {
        emuEmulationCycle();
    }
    __unreachable();
}

#include "audio.h"

extern i2s_config_t i2s_config;
#include "hardware/pwm.h"


// ---------------------------------------------------------------------------
// Вывод звука через PWM с аппаратной тактовой сеткой.
//
// Эмулятор считает отсчёты в ВИРТУАЛЬНОМ времени: за один проход главного цикла
// их появляется столько, сколько виртуального времени прошло, и все они
// выдаются подряд за единицы микросекунд. Прежняя реализация писала каждый
// отсчёт прямо в регистр сравнения PWM, поэтому промежуточные уровни держались
// доли микросекунды, а последний — до следующего прохода цикла. Физическая
// частота дискретизации получалась рваной: пачки, джиттер, искажение спектра.
//
// Теперь отсчёты складываются в кольцевой буфер, а извлекает их DMA, темп
// которому задаёт аппаратный делитель DMA. Интервал между отсчётами строго
// одинаков и от загрузки процессора не зависит.
//
// Оба канала пишутся одним 32-битным словом в регистр CC среза PWM (канал A —
// младшая половина, канал B — старшая), поэтому режим требует, чтобы оба вывода
// сидели на одном срезе PWM (соседние GPIO вида 2n / 2n+1). Если это не так или
// не досталось DMA, остаётся прежняя прямая запись — она хуже, но работает.
// ---------------------------------------------------------------------------

#include <hardware/gpio.h>
#include <pico/time.h>

// ===========================================================================
//  Определение типа звукового выхода и общий кольцевой буфер
//
//  ШИМ-плата и I2S-плата садятся на одни и те же выводы (PWM_PIN0 / PWM_PIN1),
//  поэтому одна прошивка обслуживает обе: тип выхода определяется электрически
//  при старте. Приём и обе поправки к нему взяты из pico-spec (pwm_audio.cpp).
// ===========================================================================

static bool s_audioI2S = false;
static bool s_audioOutputInitialized = false;
bool palAudioIsI2S() {return s_audioI2S;}

bool palAudioOutputCanSwitch()
{
#if defined(AUDIO_FORCE_PWM) || defined(AUDIO_FORCE_I2S)
    return false;
#else
    return true;
#endif
}

// Результаты прозвонки, доступны снаружи для отладки:
// [0], [1] — время нарастания на PWM_PIN0 / PWM_PIN1, мкс
// [2]      — признак внешней запитки (вывод читается единицей при подтяжке вниз)
static uint32_t s_audioProbe[3] = {0, 0, 0};
uint32_t palAudioProbe(int i) {return (i >= 0 && i < 3) ? s_audioProbe[i] : 0;}

// Время нарастания на выводе при внутренней подтяжке вверх (около 50 кОм).
//
// Это и есть различающий признак. Вход ЦАП на I2S-плате — просто вход КМОП,
// нагрузка порядка единиц пикофарад, уровень поднимается за микросекунды.
// Вывод ШИМ-платы нагружен конденсатором RC-фильтра, и на это уходят сотни
// микросекунд, а то и больше.
//
// Прежний вариант, перенесённый из pico-spec, различал не это: там достаточно
// было, чтобы вывод не оказался прижат к земле. Но конденсатор RC-фильтра за
// отведённые 33 мс успевает зарядиться через ту же подтяжку, и ШИМ-плата
// читается ровно как свободный вход. Отсюда и ложное определение I2S.
static uint32_t audioRiseTimeUs(unsigned pin)
{
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 0);
    sleep_ms(2);                       // гарантированно разряжаем ёмкость
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);

    const uint32_t t0 = time_us_32();
    uint32_t dt = 0;
    while (!gpio_get(pin)) {
        dt = time_us_32() - t0;
        if (dt >= 5000)                // потолок: заведомо ёмкостная нагрузка
            break;
    }
    gpio_deinit(pin);
    return dt;
}

// Читается ли вывод единицей при подтяжке вниз — значит, запитан извне
static bool audioBackFed(unsigned pin)
{
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_down(pin);
    sleep_ms(10);
    const bool v = gpio_get(pin);
    gpio_deinit(pin);
    return v;
}

// Возвращает true, если обнаружена I2S-плата.
//
// Правило намеренно осторожное: I2S выбирается только при положительном
// признаке, во всех неоднозначных случаях остаётся ШИМ. Ошибка в эту сторону
// даёт лишь чуть худший звук, а в обратную — поток PIO прямо в аналоговый
// усилитель, то есть громкий хрип при старте.
bool palProbeAudioOutput()
{
#if defined(AUDIO_FORCE_PWM)
    s_audioI2S = false;
#elif defined(AUDIO_FORCE_I2S)
    s_audioI2S = true;
#else
    // Порог с большим запасом: свободный вход поднимается за единицы
    // микросекунд, ёмкость RC-фильтра — минимум за сотни.
    static constexpr uint32_t c_bareInputUs = 100;

    const bool backFed = audioBackFed(PWM_PIN0) || audioBackFed(PWM_PIN1);
    s_audioProbe[0] = audioRiseTimeUs(PWM_PIN0);
    s_audioProbe[1] = audioRiseTimeUs(PWM_PIN1);
    s_audioProbe[2] = backFed ? 1 : 0;

    s_audioI2S = !backFed
              && s_audioProbe[0] < c_bareInputUs
              && s_audioProbe[1] < c_bareInputUs;
#endif
    return s_audioI2S;
}

// ---------------------------------------------------------------------------
//  Кольцевой буфер и вывод по таймеру.
//
//  Эмулятор считает отсчёты в ВИРТУАЛЬНОМ времени: за один проход главного
//  цикла их появляется столько, сколько виртуального времени прошло, и все они
//  выдаются подряд за единицы микросекунд. Прежняя реализация писала каждый
//  отсчёт прямо в регистр PWM, поэтому промежуточные уровни держались доли
//  микросекунды, а последний — до следующего прохода цикла.
//
//  Теперь отсчёты складываются в кольцевой буфер, а достаёт их обработчик
//  повторяющегося таймера — ровно по одному за период. Способ взят из
//  pico-spec (pwm_audio.cpp): он проверен на этом железе.
//
//  Период задаётся как 1000000 / частота дискретизации в микросекундах, то есть
//  частота обязана быть делителем миллиона. Поэтому в Emulation выставлено
//  50000 Гц: 20 мкс ровно. При 48000 период округлился бы до тех же 20 мкс,
//  что дало бы воспроизведение на 4.17% быстрее — почти три четверти полутона.
// ---------------------------------------------------------------------------
static constexpr unsigned c_audioRingSize = 1024;
static constexpr unsigned c_audioRingMask = c_audioRingSize - 1;

// Оба знаковых 16-битных канала в одном слове: младшая половина — левый,
// старшая — правый. Квантование до 8 бит выполняется только в PWM callback.
static uint32_t s_audioRing[c_audioRingSize];
static volatile unsigned s_audioWrite = 0;
static volatile unsigned s_audioRead = 0;
static uint32_t s_audioLast = 0;
static bool s_audioPaced = false;
static repeating_timer_t s_audioTimer;

// Остаток квантования, переносимый в следующий отсчёт (только для ШИМ)
static int s_audioErrL = 0;
static int s_audioErrR = 0;

extern i2s_config_t i2s_config;

static void audioWritePwm(uint32_t sample)
{
    int xL = int(int16_t(sample & 0xFFFF)) + 32768 + s_audioErrL;
    if (xL < 0) xL = 0; else if (xL > 0xFFFF) xL = 0xFFFF;
    const uint16_t outL = uint16_t(unsigned(xL) >> 8);
    s_audioErrL = xL - (int(outL) << 8);

    int xR = int(int16_t(sample >> 16)) + 32768 + s_audioErrR;
    if (xR < 0) xR = 0; else if (xR > 0xFFFF) xR = 0xFFFF;
    const uint16_t outR = uint16_t(unsigned(xR) >> 8);
    s_audioErrR = xR - (int(outR) << 8);

    pwm_set_gpio_level(PWM_PIN0, outL);
    pwm_set_gpio_level(PWM_PIN1, outR);
}

static bool __not_in_flash_func(audioTimerCb)(repeating_timer_t*)
{
    if (s_audioRead != s_audioWrite) {
        s_audioLast = s_audioRing[s_audioRead];
        s_audioRead = (s_audioRead + 1) & c_audioRingMask;
    }
    if (s_audioI2S) {
        if (!pio_sm_is_tx_fifo_full(i2s_config.pio, i2s_config.sm))
            pio_sm_put(i2s_config.pio, i2s_config.sm, s_audioLast);
    } else {
        audioWritePwm(s_audioLast);
    }
    return true;
}

static void audioStopPacedOutput()
{
    if (s_audioPaced) {
        cancel_repeating_timer(&s_audioTimer);
        s_audioPaced = false;
    }
}

static bool audioInitOutput(int sampleRate)
{
    if (s_audioI2S) {
        i2s_config.sample_freq = sampleRate;
        i2s_config.dma_trans_count = 0;
        if (!i2s_init(&i2s_config))
            return false;
    } else {
        pwm_config config = pwm_get_default_config();
        pwm_config_set_clkdiv(&config, 1.0f);
        pwm_config_set_wrap(&config, (1 << 8) - 1);
        gpio_set_function(PWM_PIN0, GPIO_FUNC_PWM);
        gpio_set_function(PWM_PIN1, GPIO_FUNC_PWM);
        pwm_init(pwm_gpio_to_slice_num(PWM_PIN0), &config, true);
        pwm_init(pwm_gpio_to_slice_num(PWM_PIN1), &config, true);
    }
    s_audioOutputInitialized = true;
    return true;
}

static void audioDeinitOutput()
{
    if (!s_audioOutputInitialized)
        return;
    if (s_audioI2S) {
        i2s_deinit(&i2s_config);
    } else {
        const uint slice0 = pwm_gpio_to_slice_num(PWM_PIN0);
        const uint slice1 = pwm_gpio_to_slice_num(PWM_PIN1);
        pwm_set_enabled(slice0, false);
        if (slice1 != slice0)
            pwm_set_enabled(slice1, false);
        gpio_deinit(PWM_PIN0);
        gpio_deinit(PWM_PIN1);
    }
    s_audioOutputInitialized = false;
}

static bool audioStartPacedOutput(int sampleRate)
{
    if (sampleRate <= 0)
        return false;

    const int periodUs = 1000000 / sampleRate;
    if (periodUs <= 0)
        return false;

    s_audioRead = 0;
    s_audioWrite = 0;
    s_audioLast = 0;
    s_audioErrL = 0;
    s_audioErrR = 0;
    for (unsigned i = 0; i < c_audioRingSize; i++)
        s_audioRing[i] = 0;

    if (!add_repeating_timer_us(-periodUs, audioTimerCb, nullptr, &s_audioTimer))
        return false;

    s_audioPaced = true;
    return true;
}

void __not_in_flash_func(palPlaySample)(int16_t left, int16_t right) {
    const uint32_t sample = uint16_t(left) | (uint32_t(uint16_t(right)) << 16);

    if (!s_audioPaced) {
        if (s_audioI2S) {
            if (s_audioOutputInitialized
                && !pio_sm_is_tx_fifo_full(i2s_config.pio, i2s_config.sm))
                pio_sm_put(i2s_config.pio, i2s_config.sm, sample);
        } else if (s_audioOutputInitialized) {
            audioWritePwm(sample);
        }
        return;
    }

    const unsigned next = (s_audioWrite + 1) & c_audioRingMask;
    if (next == s_audioRead)
        return;

    s_audioRing[s_audioWrite] = sample;
    s_audioWrite = next;
}

int sampleRate = 50000;

bool palSetSampleRate(int newSampleRate)
{
    if (newSampleRate <= 0)
        return false;

    if (s_audioOutputInitialized && newSampleRate == sampleRate)
        return true;

    audioStopPacedOutput();
    audioDeinitOutput();
    sampleRate = newSampleRate;
    if (!audioInitOutput(sampleRate))
        return false;
    if (!audioStartPacedOutput(sampleRate)) {
        audioDeinitOutput();
        return false;
    }
    return true;
}

bool palSetAudioOutputI2S(bool i2s)
{
    if (!palAudioOutputCanSwitch())
        return i2s == s_audioI2S;
    if (i2s == s_audioI2S)
        return true;

    const bool previous = s_audioI2S;
    audioStopPacedOutput();
    audioDeinitOutput();

    s_audioI2S = i2s;
    if (audioInitOutput(sampleRate) && audioStartPacedOutput(sampleRate))
        return true;

    audioStopPacedOutput();
    audioDeinitOutput();
    s_audioI2S = previous;
    if (audioInitOutput(sampleRate))
        audioStartPacedOutput(sampleRate);
    return false;
}

int palGetSampleRate()
{
    return ::sampleRate;
}

bool palSetVsync(bool)
{
    return true;
}

void palMsgBox(const string& msg, bool)
{
#if LOG
    /// TODO:
    palLog(msg + "\n");
#endif
}


void palRequestForQuit() {  while(true); } /// TODO:
