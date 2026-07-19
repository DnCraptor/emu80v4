# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
#set(PICO_PLATFORM "rp2040" CACHE STRING "Chip type")
set(PICO_PLATFORM "rp2350" CACHE STRING "Chip type")
#set(PICO_BOARD "murmulator" CACHE STRING "Board type")
set(PICO_BOARD "murmulator2" CACHE STRING "Board type")
#set(PICO_BOARD "olimex-pico-pc" CACHE STRING "Board type")
#set(PICO_BOARD "waveshare_rp2040_pizero" CACHE STRING "Board type")
#set(PICO_BOARD "waveshare_rp2350_pizero" CACHE STRING "Board type")

#set(m1p2-launcher ON)
#set(I2S ON)

#set(SOFTTV ON)
#set(TFT ON)
#set(ILI9341 ON)
set(VGA_DRV ON)
#set(HDMI ON)

#set(CPU_MHZ 366)
#set(CPU_MHZ 372)
#set(CPU_MHZ 378)
#set(CPU_MHZ 440)
#set(CPU_MHZ 444)
#set(CPU_MHZ 456)
#set(CPU_MHZ 460)
#set(CPU_MHZ 512)
set(CPU_MHZ 520)
set (PORT_VERSION "0.4.9")


set(KBDUSB ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

SET(PROJECT_NAME v06c)
set(FLASH_SIZE 4096)
set(BUILD_NAME "${PROJECT_NAME}")

set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${OUTPUT_DIR}")

project(v06c C CXX ASM)

message(STATUS ">>> PICO_PLATFORM=${PICO_PLATFORM}")
message(STATUS ">>> PICO_BOARD=${PICO_BOARD}")
message(STATUS ">>> PICO_BOARD_HEADER_DIRS=${PICO_BOARD_HEADER_DIRS}")

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

add_subdirectory(drivers/audio)
add_subdirectory(drivers/picomp3lib)
add_subdirectory(drivers/nespad)
add_subdirectory(drivers/fatfs)
add_subdirectory(drivers/sdcard)
add_subdirectory(drivers/vga-nextgen)
add_subdirectory(drivers/st7789)
add_subdirectory(drivers/hdmi)
add_subdirectory(drivers/tv)
add_subdirectory(drivers/tv-software)
add_subdirectory(drivers/graphics)
add_subdirectory(drivers/psram)

#target_compile_definitions(${PROJECT_NAME} PRIVATE VGA)

# INCLUDE FILES THAT SHOULD BE COMPILED:
#GLOB_RECURSE
file(GLOB SRC
    "src/**.cpp"
    "src/**.c"
    "src/pico/**.cpp"
    "src/pico/**.c"
)

# The firmware contains a single statically constructed Vector-06C machine.
# Keep shared device implementations, but do not compile other platform models.
message(STATUS "Add source files:")
foreach (SRC_FILE IN LISTS SRC)
    message(STATUS "${SRC_FILE}")
endforeach ()
message(STATUS "")

add_executable(${PROJECT_NAME} ${SRC})

# pull in common dependencies
target_link_libraries(${PROJECT_NAME} PRIVATE
    psram
    audio
    graphics
    picomp3lib
    
    nespad
    sdcard
    fatfs
    
    pico_runtime
    pico_stdlib
    pico_multicore
    hardware_dma
    hardware_pio
    hardware_i2c
    hardware_interp
    hardware_timer
    hardware_clocks
    hardware_pwm
    hardware_flash
    hardware_exception
    stdc++
    
    tinyusb_host tinyusb_board
)

target_include_directories(${PROJECT_NAME} PRIVATE
        src
)

if (PICO_CYW43_SUPPORTED)
    target_link_libraries(${PROJECT_NAME} pico_cyw43_arch_none)
endif()

pico_enable_stdio_uart(${PROJECT_NAME} 0)
pico_enable_stdio_usb(${PROJECT_NAME} 0)

# create map/bin/hex file etc.
pico_add_extra_outputs(${PROJECT_NAME})

# add url via pico_set_program_url
if (m1p2-launcher)
    pico_set_linker_script(${PROJECT_NAME} "${CMAKE_SOURCE_DIR}/memmap.ld")
endif()
target_link_options(${PROJECT_NAME} PRIVATE -Xlinker --print-memory-usage --data-sections)
target_compile_definitions(${PROJECT_NAME} PRIVATE FLASH_SIZE=${FLASH_SIZE})

if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
target_compile_options(${PROJECT_NAME} PUBLIC
        -Wno-error=suggest-attribute=format
        -Wno-error=cast-qual
        -Wno-error=unused-parameter
        -Wno-error=conversion
        -Wno-error=format=
        -Wno-error=sign-compare
        -Wno-error=missing-field-initializers
        -Wno-error=switch
        -Wno-error=implicit-fallthrough=
        -Wno-error=stringop-truncation
        -Wno-error=restrict
        -w
)
endif ()

# performance:
target_compile_options(${PROJECT_NAME} PRIVATE
    -O3 -ffunction-sections -fdata-sections
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fno-threadsafe-statics>
)
target_link_options(${PROJECT_NAME} PRIVATE -Wl,--gc-sections)

include(CheckIPOSupported)
check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR LANGUAGES C CXX)
if (IPO_SUPPORTED)
    set_property(TARGET ${PROJECT_NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

    # pico_stdio provides the __wrap_printf/__wrap_puts/__wrap_putchar/
    # __wrap_getchar symbols used by Pico SDK linker options.  GCC 14 LTO can
    # lose those plugin-defined wrapper symbols during ltrans, producing both
    # undefined and duplicate definitions.  Keep only this SDK translation
    # unit outside LTO; the emulator and all hot paths remain LTO-enabled.
    set_source_files_properties(
        "${PICO_SDK_PATH}/src/rp2_common/pico_stdio/stdio.c"
        PROPERTIES COMPILE_OPTIONS "-fno-lto"
    )
else()
    message(WARNING "IPO/LTO is not supported: ${IPO_ERROR}")
endif()

IF(KBDUSB)
    add_subdirectory(drivers/ps2kbd)
    target_link_libraries(${PROJECT_NAME} PRIVATE ps2kbd)
    target_compile_definitions(${PROJECT_NAME} PRIVATE KBDUSB)
ELSE()
    add_subdirectory(drivers/ps2)
    target_link_libraries(${PROJECT_NAME} PRIVATE ps2)
    target_compile_definitions(${PROJECT_NAME} PRIVATE KBDPS2)
ENDIF()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    PORT_VERSION="${PORT_VERSION}"
    CPU_MHZ=${CPU_MHZ}
)

if(${PICO_PLATFORM} MATCHES "rp2040")
    pico_define_boot_stage2(slower_boot2 ${PICO_DEFAULT_BOOT_STAGE2_FILE})
    target_compile_definitions(slower_boot2 PRIVATE PICO_FLASH_SPI_CLKDIV=4)
    pico_set_boot_stage2(${PROJECT_NAME} slower_boot2)
    if(${PICO_BOARD} MATCHES "murmulator2")
        SET(BUILD_NAME "m2p1-${PROJECT_NAME}-${CPU_MHZ}")
    elseif(${PICO_BOARD} MATCHES "olimex-pico-pc")
        SET(BUILD_NAME "PCp1-${PROJECT_NAME}-${CPU_MHZ}")
    elseif(${PICO_BOARD} MATCHES "waveshare_rp2040_pizero")
        SET(BUILD_NAME "z0p1-${PROJECT_NAME}-${CPU_MHZ}")
    else()
        SET(BUILD_NAME "m1p1-${PROJECT_NAME}-${CPU_MHZ}")
    endif()
else()
    if(${PICO_BOARD} MATCHES "murmulator2")
        SET(BUILD_NAME "m2p2-${PROJECT_NAME}-${CPU_MHZ}")
    elseif(${PICO_BOARD} MATCHES "olimex-pico-pc")
        SET(BUILD_NAME "PCp2-${PROJECT_NAME}-${CPU_MHZ}")
    elseif(${PICO_BOARD} MATCHES "waveshare_rp2350_pizero")
        SET(BUILD_NAME "z0p2-${PROJECT_NAME}-${CPU_MHZ}")
    else()
        SET(BUILD_NAME "m1p2-${PROJECT_NAME}-${CPU_MHZ}")
    endif()
endif()


# Звук. Одна прошивка обслуживает и ШИМ-плату, и I2S-плату: тип выхода
# определяется электрической прозвонкой выводов при старте (см.
# palProbeAudioOutput). AUDIO_PWM определять НЕЛЬЗЯ — иначе в драйвере audio
# включается ветка AUDIO_PWM_PIN и i2s_init() не настраивает PIO.
IF(CS4334)
    target_compile_definitions(${PROJECT_NAME} PRIVATE I2S_CS4334)
    SET(BUILD_NAME "${BUILD_NAME}-CS4334")
ENDIF()
# Принудительный выбор в обход прозвонки, если она ошибается на какой-то плате
IF(AUDIO_FORCE_PWM)
    target_compile_definitions(${PROJECT_NAME} PRIVATE AUDIO_FORCE_PWM)
    SET(BUILD_NAME "${BUILD_NAME}-forcePWM")
ELSEIF(AUDIO_FORCE_I2S)
    target_compile_definitions(${PROJECT_NAME} PRIVATE AUDIO_FORCE_I2S)
    SET(BUILD_NAME "${BUILD_NAME}-forceI2S")
ENDIF()

IF(TFT)
    target_link_libraries(${PROJECT_NAME} PRIVATE st7789)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TFT)
    IF(ILI9341)
        SET(BUILD_NAME "${BUILD_NAME}-ILI9341-${PORT_VERSION}")
        target_compile_definitions(${PROJECT_NAME} PRIVATE ILI9341)
    ELSE()
        SET(BUILD_NAME "${BUILD_NAME}-ST7789-${PORT_VERSION}")
    ENDIF()
ELSEIF(HDMI)
    target_link_libraries(${PROJECT_NAME} PRIVATE hdmi)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HDMI)
    SET(BUILD_NAME "${BUILD_NAME}-HDMI-${PORT_VERSION}")
ELSEIF(TV)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TV)
    target_link_libraries(${PROJECT_NAME} PRIVATE tv)
    SET(BUILD_NAME "${BUILD_NAME}-TV-${PORT_VERSION}")
ELSEIF(SOFTTV)
	target_compile_definitions(${PROJECT_NAME} PRIVATE SOFTTV)
	target_link_libraries(${PROJECT_NAME} PRIVATE tv-software)
	SET(BUILD_NAME "${BUILD_NAME}-TV-SOFT-${PORT_VERSION}")
ELSE()
    target_compile_definitions(${PROJECT_NAME} PRIVATE VGA_DRV)
    target_link_libraries(${PROJECT_NAME} PRIVATE vga-nextgen)
    SET(BUILD_NAME "${BUILD_NAME}-VGA-${PORT_VERSION}")
ENDIF()

if(${PICO_BOARD} MATCHES "waveshare_rp2350_pizero")
    pico_set_program_name(${PROJECT_NAME} "Vector-06C for RP2350-PiZero")
elseif(${PICO_BOARD} MATCHES "olimex-pico-pc")
    pico_set_program_name(${PROJECT_NAME} "Vector-06C for Olimex RP2040-PICO-PC")
elseif(${PICO_BOARD} MATCHES "murmulator2")
    pico_set_program_name(${PROJECT_NAME} "Vector-06C for MURMULATOR 2.0")
else()
    pico_set_program_name(${PROJECT_NAME} "Vector-06C for MURMULATOR 1.x")
endif()
pico_set_program_version(${PROJECT_NAME} "${PORT_VERSION}")

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${BUILD_NAME}")
