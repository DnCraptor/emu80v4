/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2016-2018
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

#include <string>

#include "Pal.h"

#include "CmdLine.h"
#include "Emulation.h"

using namespace std;

Emulation* g_emulation = nullptr;

void displayCmdLineHelp()
{
    palMsgBox("Usage: " EXE_NAME " [options]\n\n"\
            "Options are:\n"\
            " --platform <platform_name>\n"\
            " --conf-file <conf_file>\n"\
            " --post-conf <post_conf_file>\n"\
            " --run <file_to_run>\n"\
            " --load <file_to_load>\n"\
            " --disk-a <image_file>\n"\
            " --disk-b <image_file>\n"\
            " --disk-c <image_file>\n"\
            " --disk-d <image_file>\n"\
            " --hdd <image_file>\n"\
            " --edd <image_file>\n"\
            " --edd2 <image_file>\n\n"\
            "For more help see \"Emu80 v4 Manual.rtf\"\n");
}

#include <hardware/structs/vreg_and_chip_reset.h>
#include <hardware/clocks.h>
#include <hardware/pwm.h>
#include <pico/multicore.h>
#include <pico/bootrom.h>
#include <pico/stdlib.h>
#include "psram_spi.h"
#include "graphics.h"
#include "ff.h"

extern "C" {
    #include "ps2.h"
    #include "util_Wii_Joy.h"
    #include "nespad.h"
}

static FATFS fs;
semaphore vga_start_semaphore;

pwm_config config = pwm_get_default_config();
void PWM_init_pin(uint8_t pinN, uint16_t max_lvl) {
    gpio_set_function(pinN, GPIO_FUNC_PWM);
    pwm_config_set_clkdiv(&config, 1.0);
    pwm_config_set_wrap(&config, max_lvl); // MAX PWM value
    pwm_init(pwm_gpio_to_slice_num(pinN), &config, true);
}

void inInit(uint gpio) {
    gpio_init(gpio);
    gpio_set_dir(gpio, GPIO_IN);
    gpio_pull_up(gpio);
}

extern "C" bool __time_critical_func(handleScancode)(const uint32_t ps2scancode) {
    return true;
}

void nespad_update() {
    // TODO:
}

#define Screen_WIDTH 800
#define Screen_HEIGHT 600
///static uint8_t __screen[Screen_WIDTH * Screen_HEIGHT] = { 0 };

void __time_critical_func(render_core)() {
    uint8_t* buffer = 0;// __screen;
    graphics_set_buffer(buffer, Screen_WIDTH, Screen_HEIGHT);
    multicore_lockout_victim_init();
    graphics_init();
    graphics_set_textbuffer(buffer);
    graphics_set_bgcolor(0x000000);
    graphics_set_offset(0, 0);
    graphics_set_flashmode(false, false);
    sem_acquire_blocking(&vga_start_semaphore);
    // 60 FPS loop
#define frame_tick (16666)
    uint64_t tick = time_us_64();
#ifdef TFT
    uint64_t last_renderer_tick = tick;
#endif
    uint64_t last_input_tick = tick;
    while (true) {
#ifdef TFT
        if (tick >= last_renderer_tick + frame_tick) {
            refresh_lcd();
            last_renderer_tick = tick;
        }
#endif
        // Every 5th frame
        if (tick >= last_input_tick + frame_tick * 5) {
            nespad_read();
            last_input_tick = tick;
            nespad_update();
        }
        tick = time_us_64();
        tight_loop_contents();
    }
    __unreachable();
}

#ifdef SOUND
static repeating_timer_t timer;
static int snd_channels = 2;
static bool __not_in_flash_func(snd_timer_callback)(repeating_timer_t *rt) {
    static uint16_t outL = 0;  
    static uint16_t outR = 0;

    pwm_set_gpio_level(PWM_PIN0, outR); // Право
    pwm_set_gpio_level(PWM_PIN1, outL); // Лево
    outL = outR = 0;

    pwm_set_gpio_level(BEEPER_PIN, 0);
    return true;
}
#endif

#include "f_util.h"
static FATFS fatfs;
bool SD_CARD_AVAILABLE = false;
static void init_fs() {
    FRESULT result = f_mount(&fatfs, "", 1);
    if (FR_OK != result) {
        printf("Unable to mount SD-card: %s (%d)", FRESULT_str(result), result);
    } else {
        SD_CARD_AVAILABLE = true;
    }
}

inline static void init_wii() {
    if (Init_Wii_Joystick()) {
        Wii_decode_joy();
        printf("Found WII joystick");
    }
}

int main() {
    hw_set_bits(&vreg_and_chip_reset_hw->vreg, VREG_AND_CHIP_RESET_VREG_VSEL_BITS);
    sleep_ms(10);
    set_sys_clock_khz(378 * KHZ, true);
    stdio_init_all();
    keyboard_init();
    keyboard_send(0xFF);
    nespad_begin(clock_get_hz(clk_sys) / 1000, NES_GPIO_CLK, NES_GPIO_DATA, NES_GPIO_LAT);

    nespad_read();
    sleep_ms(50);

    // F12 Boot to USB FIRMWARE UPDATE mode
    if (nespad_state & DPAD_START /*|| input_map.keycode == 0x58*/) { // F12
        printf("reset_usb_boot");
        reset_usb_boot(0, 0);
    }

    init_fs(); // TODO: psram replacement (pagefile)
    init_psram();

    sem_init(&vga_start_semaphore, 0, 1);
    multicore_launch_core1(render_core);
    sem_release(&vga_start_semaphore);

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    for (int i = 0; i < 6; i++) {
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }

    PWM_init_pin(BEEPER_PIN, (1 << 8) - 1);
#ifdef SOUND
    PWM_init_pin(PWM_PIN0, (1 << 8) - 1);
    PWM_init_pin(PWM_PIN1, (1 << 8) - 1);
#endif
#if LOAD_WAV_PIO
    //пин ввода звука
    inInit(LOAD_WAV_PIO);
#endif

#ifdef SOUND
    int hz = 48000; // TODO:
    snd_channels = 1; // TODO:
    // negative timeout means exact delay (rather than delay between callbacks)
    if (!add_repeating_timer_us(-1000000 / hz, snd_timer_callback, NULL, &timer)) {
        printf("Failed to add timer");
    }
#endif

    int argc = 0;
    char** argv = 0;
    CmdLine cmdLine(argc, argv);

    if (!palInit(argc, argv))
        return 1;

    if (cmdLine.checkParam("help")) {
        displayCmdLineHelp();
        return 0;
    }

    auto warnings = cmdLine.getWarnings();
    if (!warnings.empty())
        palMsgBox("Warnings:\n\n" + warnings + "\nFor brief help: " EXE_NAME " --help");
    while(true) {
        new Emulation(cmdLine); // g_emulation присваивается в конструкторе
        palStart();
        palExecute();
        delete g_emulation;
    }
    __unreachable();
}
