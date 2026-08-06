#include "picoPal.h"
#include "ffPalFile.h"

#include <sstream>
#include <iostream>

#include <pico/stdlib.h>
#include <hardware/pio.h>

uint8_t* palReadFile(const string& fileName, int &fileSize, bool useBasePath)
{
    string fullFileName;
    if (useBasePath)
        fullFileName = palMakeFullFileName(fileName);
    else
        fullFileName = fileName;
#if LOG
    emuLog << "fullFileName: '" << fullFileName << "'\n";
#endif
    FIL file;
    if (f_open(&file, fullFileName.c_str(), FA_READ) == FR_OK) {
        fileSize = f_size(&file);
        uint8_t* buf = new uint8_t[fileSize];
        UINT br;
        f_read(&file, buf, fileSize, &br);
        f_close(&file);
        return buf;
    }
    return nullptr;
}

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

void palLog(std::string s) {
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

EmuLog& EmuLog::operator<<(string s)
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
    string fullFileName("/emu80/");
    fullFileName += fileName;
    return fullFileName;
}

static std::string clipboard;
std::string palGetTextFromClipboard() {
    return clipboard;
}

void palCopyTextToClipboard(const char* text) {
    clipboard = text;
}

void palGetDirContent(const string& d, vector<PalFileInfo*>& fileList)
{
    DIR dir;
    FILINFO entry;
    if (f_opendir(&dir, d.c_str()) != FR_OK) {
        return;
    }
    while (f_readdir(&dir, &entry) == FR_OK && entry.fname[0] != '\0') {
        PalFileInfo* newFile = new PalFileInfo;
        newFile->fileName = entry.fname;
        newFile->isDir = (entry.fattrib & AM_DIR) != 0;
        newFile->size = entry.fsize;
        newFile->year = 1980 + (entry.fdate >> 9);
        newFile->month = (entry.fdate >> 5) & 0b111;
        newFile->day = entry.fdate & 0b11111;
        newFile->hour = entry.ftime >> 11;
        newFile->minute =  (entry.ftime >> 5) & 0b1111111;
        newFile->second = entry.ftime & 0b11111;
        fileList.push_back(newFile);
    }
    f_closedir(&dir);
}

/// TODO: .h
extern PalKeyCodeAction getKey();
extern PalKeyCode pressed_key[256];
#include "ps2kbd_mrmltr.h"
#include <algorithm>
static std::string fdir = "/emu80";
std::string palOpenFileDialog(std::string title, std::string filter, bool write, PalWindow* window) {
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

    vector<PalFileInfo*> fileList;
    int selected_file_n = 0;
    int shift_j = 0;
    int visibleRows = (int)((y + h - fnth - yb) / msi) + 1;
    if (visibleRows < 1) visibleRows = 1;
    PalFileInfo* selected_fi = nullptr;

    auto freeFileList = [&]() {
        for (auto* fi : fileList) delete fi;
        fileList.clear();
    };

    auto loadDirectory = [&]() {
        freeFileList();
        if (fdir.length() > 1) {
            PalFileInfo* newFile = new PalFileInfo;
            newFile->fileName = back;
            newFile->isDir = 1;
            fileList.push_back(newFile);
        }
        palGetDirContent(fdir, fileList);
        sort(fileList.begin(), fileList.end(), [](const PPalFileInfo& a, const PPalFileInfo& b) {
            if (a->isDir == b->isDir) return a->fileName < b->fileName;
            return a->isDir > b->isDir;
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
        if (itemIndex >= 0 && itemIndex < (int)fileList.size()) {
            PalFileInfo* fi = fileList[itemIndex];
            string name = fi->isDir ? "<" + fi->fileName + ">" : fi->fileName;
            size_t maxChars = listW > 2 ? (listW - 2) / fntw : 0;
            if (name.length() > maxChars) name.resize(maxChars);
            graphics_type(xb, rowY, fg, name.c_str(), name.length());
        }
    };

    auto drawScrollbar = [&]() {
        uint32_t trackY = yb;
        uint32_t trackH = y + h - 1 - trackY;
        graphics_fill(scrollX, trackY, scrollW, trackH, RGB888(224, 224, 224));
        int total = (int)fileList.size();
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
        palInputTick();
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
        int count = (int)fileList.size();
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
            selected_fi = fileList[selected_file_n];
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

    freeFileList();
    return res;
}

void palGetDirContent(const string& d, list<PalFileInfo*>& fileList)
{
    DIR dir;
    FILINFO entry;
    if (f_opendir(&dir, d.c_str()) != FR_OK) {
        return;
    }
    while (f_readdir(&dir, &entry) == FR_OK && entry.fname[0] != '\0') {
        PalFileInfo* newFile = new PalFileInfo;
        newFile->fileName = entry.fname;
        newFile->isDir = (entry.fattrib & AM_DIR) != 0;
        newFile->size = entry.fsize;
        newFile->year = 1980 + (entry.fdate >> 9);
        newFile->month = (entry.fdate >> 5) & 0b111;
        newFile->day = entry.fdate & 0b11111;
        newFile->hour = entry.ftime >> 11;
        newFile->minute =  (entry.ftime >> 5) & 0b1111111;
        newFile->second = entry.ftime & 0b11111;
        fileList.push_back(newFile);
    }
    f_closedir(&dir);
}

#include "../EmuCalls.h"

void palExecute() {
    while(1) {
        emuEmulationCycle();
        sleep_ms(2);
    }
    __unreachable();
}

#include "audio.h"
#include "hardware/pwm.h"
#include <hardware/gpio.h>
#include <pico/time.h>
#ifdef HDMI_DVI
#include "hdmi-dvi.h"
#endif

// ===========================================================================
//  Звук ПК-01 «Львов» на Murmulator.
//
//  Перенесено из зрелого порта «Вектор-06Ц». Отличия для Львова:
//    * убран режим HWAY (реальный AY-3-8910 через 74595) — у ПК-01 нет
//      музыкального сопроцессора, только 1-битный бипер на ВВ55, поэтому
//      достаточно двух выходов: ШИМ и I2S;
//    * частота дискретизации 50 кГц (20 мкс ровно) — этого требует
//      ресемплер HDMI (50 кГц -> 48 кГц) в hdmi-dvi.c.
//
//  Тип выхода (ШИМ/I2S) определяется электрической прозвонкой пары DIN/BCK
//  при старте: одна прошивка обслуживает и ШИМ-плату, и I2S-плату.
// ===========================================================================

enum AudioOut : uint8_t { AUDIO_OUT_PWM = 0, AUDIO_OUT_I2S };
static AudioOut s_audioOut = AUDIO_OUT_PWM;
#define s_audioI2S  (s_audioOut == AUDIO_OUT_I2S)
static bool s_audioOutputInitialized = false;
bool palAudioIsI2S() { return s_audioI2S; }

bool palAudioOutputCanSwitch()
{
#if defined(AUDIO_FORCE_PWM) || defined(AUDIO_FORCE_I2S)
    return false;
#else
    return true;
#endif
}

// --- Прозвонка пары DIN/BCK (пассивные уровни + активная проверка связи) ----
// Результаты доступны снаружи для отладки:
// [0] — пассивная сигнатура пары DIN/BCK;
// [1] — результат активной проверки связи между линиями;
// [2] — полный код; ненулевой означает обнаруженный I2S-модуль.
static uint32_t s_audioProbe[3] = {0, 0, 0};
uint32_t palAudioProbe(int i) { return (i >= 0 && i < 3) ? s_audioProbe[i] : 0; }

static int audioTest0000(unsigned pin0, unsigned pin1, int res)
{
    gpio_init(pin0); gpio_set_dir(pin0, GPIO_OUT); sleep_ms(33); gpio_put(pin0, 1);
    gpio_init(pin1); gpio_set_dir(pin1, GPIO_IN);  gpio_pull_down(pin1); sleep_ms(33);
    if (gpio_get(pin1)) res |= (1 << 5) | 1;
    gpio_deinit(pin0); gpio_deinit(pin1);
    return res;
}

static int audioTest0101(unsigned pin0, unsigned pin1, int res)
{
    gpio_init(pin0); gpio_set_dir(pin0, GPIO_OUT); sleep_ms(33); gpio_put(pin0, 1);
    gpio_init(pin1); gpio_set_dir(pin1, GPIO_IN);  gpio_pull_down(pin1); sleep_ms(33);
    if (gpio_get(pin1)) res |= (1 << 5) | 1;
    gpio_deinit(pin0); gpio_deinit(pin1);
    return res;
}

static int audioTest1111(unsigned pin0, unsigned pin1, int res)
{
    gpio_init(pin0); gpio_set_dir(pin0, GPIO_OUT); sleep_ms(33); gpio_put(pin0, 0);
    gpio_init(pin1); gpio_set_dir(pin1, GPIO_IN);  gpio_pull_up(pin1); sleep_ms(33);
    if (!gpio_get(pin1)) res |= 1;
    gpio_deinit(pin0); gpio_deinit(pin1);
    return res;
}

// Полный testPins из pico-spec: пассивных уровней недостаточно, поэтому после
// них рабочий алгоритм возбуждает одну линию и проверяет отклик второй. Именно
// активный этап отличает установленный I2S-модуль.
static int audioTestPins(unsigned pin0, unsigned pin1)
{
    gpio_init(pin0); gpio_set_dir(pin0, GPIO_IN); gpio_pull_down(pin0);
    gpio_init(pin1); gpio_set_dir(pin1, GPIO_IN); gpio_pull_down(pin1);
    sleep_ms(33);
    const int pin0vPD = gpio_get(pin0);
    const int pin1vPD = gpio_get(pin1);
    gpio_deinit(pin0); gpio_deinit(pin1);

    gpio_init(pin0); gpio_set_dir(pin0, GPIO_IN); gpio_pull_up(pin0);
    gpio_init(pin1); gpio_set_dir(pin1, GPIO_IN); gpio_pull_up(pin1);
    sleep_ms(33);
    const int pin0vPU = gpio_get(pin0);
    const int pin1vPU = gpio_get(pin1);
    gpio_deinit(pin0); gpio_deinit(pin1);

    int res = (pin0vPD << 4) | (pin0vPU << 3) | (pin1vPD << 2) | (pin1vPU << 1);
    s_audioProbe[0] = uint32_t(res);

    if (pin0vPD == 1) {
        if (pin0vPU == 1 && pin1vPD == 1 && pin1vPU == 1)
            res = audioTest1111(pin0, pin1, res);
        else if (pin0vPU == 0 && pin1vPD == 1 && pin1vPU == 0)
            res |= (1 << 5) | 1;
    } else if (pin0vPU == 1) {
        if (pin1vPD == 0 && pin1vPU == 1)
            res = audioTest0101(pin0, pin1, res);
    } else if (pin1vPD == 0 && pin1vPU == 0) {
        res = audioTest0000(pin0, pin1, res);
    }

    s_audioProbe[1] = uint32_t(res & 0x21);
    s_audioProbe[2] = uint32_t(res);
    return res;
}

// Возвращает true, если полная проверка связи DIN/BCK обнаружила I2S-модуль.
bool palProbeAudioOutput()
{
#if defined(AUDIO_FORCE_PWM)
    s_audioOut = AUDIO_OUT_PWM;
#elif defined(AUDIO_FORCE_I2S)
    s_audioOut = AUDIO_OUT_I2S;
#else
    s_audioOut = audioTestPins(AUDIO_DATA_PIN, AUDIO_CLOCK_PIN) != 0
               ? AUDIO_OUT_I2S : AUDIO_OUT_PWM;
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
//  микросекунды. Теперь отсчёты складываются в кольцевой буфер, а достаёт их
//  обработчик повторяющегося таймера — ровно по одному за период.
//
//  Период = 1000000 / частота дискретизации в мкс, поэтому частота обязана
//  быть делителем миллиона. Выставлено 50000 Гц: 20 мкс ровно, и это же
//  значение ждёт ресемплер HDMI (50 кГц -> 48 кГц).
// ---------------------------------------------------------------------------
static constexpr unsigned c_audioRingSize = 1024;
static constexpr unsigned c_audioRingMask = c_audioRingSize - 1;

// Оба знаковых 16-битных канала в одном слове: младшая половина — левый,
// старшая — правый. Квантование до 12 бит выполняется только в PWM callback.
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
    if (s_audioOut != AUDIO_OUT_PWM)   // чужой режим — ноги не наши
        return;
    int xL = int(int16_t(sample & 0xFFFF)) + 32768 + s_audioErrL;
    if (xL < 0) xL = 0; else if (xL > 0xFFFF) xL = 0xFFFF;
    const uint16_t outL = uint16_t(unsigned(xL) >> 4);
    s_audioErrL = xL - (int(outL) << 4);

    int xR = int(int16_t(sample >> 16)) + 32768 + s_audioErrR;
    if (xR < 0) xR = 0; else if (xR > 0xFFFF) xR = 0xFFFF;
    const uint16_t outR = uint16_t(unsigned(xR) >> 4);
    s_audioErrR = xR - (int(outR) << 4);

    pwm_set_gpio_level(PWM_PIN0, outL);
    pwm_set_gpio_level(PWM_PIN1, outR);
}

static bool __not_in_flash_func(audioTimerCb)(repeating_timer_t*)
{
    if (s_audioRead != s_audioWrite) {
        s_audioLast = s_audioRing[s_audioRead];
        s_audioRead = (s_audioRead + 1) & c_audioRingMask;
    }
    #if defined(HDMI_DVI) && defined(PICO_RP2350)
    hdmi_dvi_push_audio_sample(
        int16_t(s_audioLast & 0xffffu),
        int16_t(s_audioLast >> 16));
    #endif
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
        pwm_config_set_wrap(&config, (1 << 12) - 1);
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

    const AudioOut previous = s_audioOut;
    audioStopPacedOutput();
    audioDeinitOutput();

    s_audioOut = i2s ? AUDIO_OUT_I2S : AUDIO_OUT_PWM;
    if (audioInitOutput(sampleRate) && audioStartPacedOutput(sampleRate))
        return true;

    audioStopPacedOutput();
    audioDeinitOutput();
    s_audioOut = previous;
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

void palMsgBox(string msg, bool)
{
#if LOG
    /// TODO:
    palLog(msg + "\n");
#endif
}

void palUpdateConfig() {
}

void palRemoveTabFromConfigWindow(int) {
}

void palRequestForQuit() {  while(true); } /// TODO:

