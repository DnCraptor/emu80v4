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
#include <algorithm>
static std::string fdir = "/vector06c";
std::string palOpenFileDialog(const std::string& title, const std::string& filter, bool write) {
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
    uint32_t yb = y + fnth + 5;
    const uint32_t scrollW = 4;
    const uint32_t listW = w - 2 - scrollW;
    const uint32_t scrollX = x + w - 1 - scrollW;
    const char back[] = "..";

    graphics_rect(x, y, w, h, RGB888(0, 0, 0));
    graphics_fill(x + 1, y + 1, w - 2, fnth + 2, 0b000101);
    graphics_rect(x, y, w, fnth + 4, RGB888(0, 0, 0));

    string t2;
    if (write) {
        t2 = "Type a name of ext: " + filter;
        graphics_fill(x + 1, yb, w - 2, fnth + 1, RGB888(207, 255, 255));
        yb += fnth + 1;
    }

    DIR f_dir;
    if (f_opendir(&f_dir, fdir.c_str()) != FR_OK)
        fdir = "/";
    else
        f_closedir(&f_dir);
    if (f_opendir(&f_dir, fdir.c_str()) != FR_OK)
        return "";
    f_closedir(&f_dir);

    // Раньше здесь был static-массив: он занимал 14336 байт SRAM постоянно,
    // хотя нужен только пока открыт диалог. Хуже того, строки имён в нём
    // сохраняли свои блоки в куче и после закрытия диалога — сбрасывались они
    // лишь при следующем открытии.
    // Диалог модальный, поэтому список живёт ровно столько, сколько нужно.
    std::unique_ptr<PalFileInfo[]> fileListHolder(
        new (std::nothrow) PalFileInfo[MAX_FILE_DIALOG_ITEMS]);
    if (!fileListHolder)
        return "";
    PalFileInfo* fileList = fileListHolder.get();
    int fileCount = 0;
    int selected_file_n = 0;
    int shift_j = 0;
    int visibleRows = (int)((y + h - fnth - yb) / msi) + 1;
    if (visibleRows < 1) visibleRows = 1;
    PalFileInfo* selected_fi = nullptr;

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
        uint32_t xt = x + 1;
        if (t.length() * fntw < w - 2)
            xt = x + 1 + (w - 2 - t.length() * fntw) / 2;
        graphics_type(xt, y + 3, 0b101010, t.c_str(), t.length());
    };

    auto drawInput = [&]() {
        if (!write) return;
        graphics_fill(x + 1, y + fnth + 5, w - 2, fnth + 1, RGB888(207, 255, 255));
        graphics_type(x + 2, y + fnth + 6, RGB888(0, 0, 0), t2.c_str(), t2.length());
    };

    auto drawRow = [&](int itemIndex) {
        if (itemIndex < shift_j || itemIndex >= shift_j + visibleRows) return;
        int row = itemIndex - shift_j;
        uint32_t rowY = yb + row * msi;
        bool selected = itemIndex == selected_file_n;
        uint32_t bg = selected ? RGB888(114, 114, 224) : RGB888(255, 255, 255);
        uint32_t fg = selected ? RGB888(255, 255, 255) : RGB888(0, 0, 0);
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
        uint32_t trackH = y + h - 1 - trackY;
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

    string res;
    while (1) {
        sleep_ms(100);
        PalKeyCodeAction pk = getKey();

        if (write && pk.pressed) {
            bool changed = true;
            if (pk.vk >= PK_1 && pk.vk <= PK_0) t2 += '1' + pk.vk - PK_1;
            else if (pk.vk >= PK_A && pk.vk <= PK_Z) t2 += 'a' + pk.vk - PK_A;
            else if (pk.vk == PK_SPACE) t2 += ' ';
            else if (pk.vk == PK_PERIOD) t2 += '.';
            else if (pk.vk == PK_BSP) t2.clear();
            else changed = false;
            if (changed) drawInput();
        }

        int oldSelected = selected_file_n;
        int oldShift = shift_j;
        int count = fileCount;
        if (count == 0) {
            if (pk.vk == PK_ESC && pk.pressed) break;
            continue;
        }

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
        } else if ((pk.vk == PK_ENTER || pk.vk == PK_KP_ENTER) && pk.pressed) {
            selected_fi = &fileList[selected_file_n];
            if (write) {
                res = fdir + "/" + t2;
                break;
            }
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
            res = fdir + "/" + selected_fi->fileName;
            break;
        } else if (pk.vk == PK_ESC && pk.pressed) {
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
bool palAudioIsI2S() {return s_audioI2S;}

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

// Оба канала в одном слове: младшая половина — левый, старшая — правый
static uint32_t s_audioRing[c_audioRingSize];
static volatile unsigned s_audioWrite = 0;
static volatile unsigned s_audioRead = 0;
static uint32_t s_audioLast = (uint32_t(128) << 16) | 128;
static bool s_audioPaced = false;
static repeating_timer_t s_audioTimer;

// Остаток квантования, переносимый в следующий отсчёт (только для ШИМ)
static int s_audioErrL = 0;
static int s_audioErrR = 0;

static bool __not_in_flash_func(audioTimerCb)(repeating_timer_t*)
{
    if (s_audioRead != s_audioWrite) {
        s_audioLast = s_audioRing[s_audioRead];
        s_audioRead = (s_audioRead + 1) & c_audioRingMask;
    }
    // При опустошении буфера удерживается последний уровень: эмуляция могла
    // встать (диалог, пауза), и повторять по кругу старое содержимое нельзя.
    pwm_set_gpio_level(PWM_PIN0, uint16_t(s_audioLast & 0xFFFF));
    pwm_set_gpio_level(PWM_PIN1, uint16_t(s_audioLast >> 16));
    return true;
}

static void audioStartPacedOutput(int sampleRate)
{
#if !defined(AUDIO_FORCE_I2S)
    // i2s_init() внутри вызывает pio_claim_unused_sm(..., true) и
    // pio_add_program(..., true) — оба при нехватке ресурсов делают panic().
    // Пока определение типа платы не подтверждено на железе, тракт I2S
    // поднимается только по явному требованию AUDIO_FORCE_I2S.
    s_audioI2S = false;
#endif
    if (sampleRate <= 0)
        return;

    const int periodUs = 1000000 / sampleRate;
    if (periodUs <= 0)
        return;

    s_audioRead = 0;
    s_audioWrite = 0;
    s_audioLast = (uint32_t(128) << 16) | 128;
    for (unsigned i = 0; i < c_audioRingSize; i++)
        s_audioRing[i] = s_audioLast;

    // Отрицательная задержка — период отсчитывается от предыдущего срабатывания,
    // а не от конца обработчика, поэтому частота не уплывает
    if (!add_repeating_timer_us(-periodUs, audioTimerCb, nullptr, &s_audioTimer))
        return;

    s_audioPaced = true;
}

void __not_in_flash_func(palPlaySample)(int16_t left, int16_t right) {
    // Перенос ошибки квантования в следующий отсчёт. ЦАП восьмибитный, простое
    // отбрасывание младших восьми бит даёт равномерный шум по всей полосе.
    // Накопление остатка сдвигает шум вверх по спектру и возвращает около 5 дБ
    // в полосе до 8 кГц. Приём взят из pico-spec (pwm_audio.cpp).
    int xL = int(left) + 32768 + s_audioErrL;
    if (xL < 0) xL = 0; else if (xL > 0xFFFF) xL = 0xFFFF;
    const uint16_t outL = uint16_t(unsigned(xL) >> 8);
    s_audioErrL = xL - (int(outL) << 8);

    int xR = int(right) + 32768 + s_audioErrR;
    if (xR < 0) xR = 0; else if (xR > 0xFFFF) xR = 0xFFFF;
    const uint16_t outR = uint16_t(unsigned(xR) >> 8);
    s_audioErrR = xR - (int(outR) << 8);

    if (!s_audioPaced) {
        // Запасной путь: таймер не поднялся. По джиттеру это прежнее поведение,
        // но звук есть — молчать здесь нельзя ни при каких обстоятельствах.
        pwm_set_gpio_level(PWM_PIN0, outL);
        pwm_set_gpio_level(PWM_PIN1, outR);
        return;
    }

    const unsigned next = (s_audioWrite + 1) & c_audioRingMask;

    // Буфер полон: эмулятор ушёл вперёд реального времени (перемотка ленты,
    // догон после провала главного цикла). Отсчёт отбрасываем — это правильнее,
    // чем затирать ещё не воспроизведённое.
    if (next == s_audioRead)
        return;

    s_audioRing[s_audioWrite] = (uint32_t(outR) << 16) | outL;
    s_audioWrite = next;
}

bool isRunning = true;
int sampleRate = 48000;

bool palSetSampleRate(int sampleRate)
{
    // ВНИМАНИЕ: isRunning инициализируется значением true и нигде в дереве не
    // сбрасывается, поэтому проверка ниже срабатывает всегда, а весь остаток
    // функции недостижим. Запуск вывода поэтому вынесен ВЫШЕ неё — раньше он
    // стоял под ней и не выполнялся ни разу.
    if (!s_audioPaced)
        audioStartPacedOutput(sampleRate);

    if (isRunning)
        return false;
    ::sampleRate = sampleRate;
    return true;
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
