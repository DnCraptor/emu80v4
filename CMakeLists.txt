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
set(ORION_VIDEO "HDMI" CACHE STRING "Video output: HDMI, VGA, RGB, PAL, NTSC")
set_property(CACHE ORION_VIDEO PROPERTY STRINGS HDMI VGA RGB PAL NTSC)
set(CPU_MHZ 440 CACHE STRING "System clock in MHz")
set(PORT_VERSION "0.5.1")

if(ORION_VIDEO STREQUAL "HDMI" OR ORION_VIDEO STREQUAL "RGB")
    # 800x600 DVI uses 400 MHz TMDS; RGB TV divides 400 MHz to 8 MHz.
    set(CPU_MHZ 400)
elseif(NOT ORION_VIDEO MATCHES "^(VGA|PAL|NTSC)$")
    message(FATAL_ERROR "Unknown ORION_VIDEO: ${ORION_VIDEO}")
endif()
if(PICO_BOARD STREQUAL "olimex-pico-pc" AND NOT ORION_VIDEO STREQUAL "HDMI")
    message(FATAL_ERROR "${PICO_BOARD} requires ORION_VIDEO=HDMI")
endif()

set(KBDUSB ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

SET(PROJECT_NAME orion128)
set(FLASH_SIZE 4096)
set(BUILD_NAME "${PROJECT_NAME}")

set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${OUTPUT_DIR}")

project(orion128 C CXX ASM)

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
add_subdirectory(drivers/tv)
add_subdirectory(drivers/tv-software)
add_subdirectory(drivers/graphics)
add_subdirectory(drivers/psram)
if(ORION_VIDEO STREQUAL "HDMI")
    add_subdirectory(drivers/libdvi)
    add_subdirectory(drivers/hdmi-dvi)
endif()

#target_compile_definitions(${PROJECT_NAME} PRIVATE VGA)

set(SRC
    src/AddrSpace.cpp
    src/AtaDrive.cpp
    src/CloseFileHook.cpp
    src/CmdLine.cpp
    src/Cpu.cpp
    src/Cpu8080.cpp
    src/Cpu8080dasm.cpp
    src/CpuHook.cpp
    src/CpuZ80.cpp
    src/CpuZ80dasm.cpp
    src/CrtRenderer.cpp
    src/DiskImage.cpp
    src/Dma8257.cpp
    src/EmuCalls.cpp
    src/EmuConfig.cpp
    src/EmuObjects.cpp
    src/EmuWindow.cpp
    src/Emulation.cpp
    src/Fdc1793.cpp
    src/FileLoader.cpp
    src/KbdLayout.cpp
    src/KbdTapper.cpp
    src/Main.cpp
    src/Memory.cpp
    src/Orion.cpp
    src/OrionMenu.cpp
    src/OrionMachine.cpp
    src/Pal.cpp
    src/Parameters.cpp
    src/Platform.cpp
    src/PlatformCore.cpp
    src/Ppi8255.cpp
    src/PpiAtaAdapter.cpp
    src/PrnWriter.cpp
    src/RamDisk.cpp
    src/RkKeyboard.cpp
    src/RkPpi8255Circuit.cpp
    src/RkRomDisk.cpp
    src/RkTapeHooks.cpp
    src/Shortcuts.cpp
    src/SoundMixer.cpp
    src/TapeRedirector.cpp
    src/WavReader.cpp
    src/WavWriter.cpp
    src/hid_app.cpp
    src/joy_hid.cpp
    src/xinput_host.c
    src/pico/ffPalFile.cpp
    src/pico/picoPal.cpp
    src/pico/picoPalWindow.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/OrionRomDisks.S
)

# ROM disks already tracked in dist/orion; embed without generated C arrays.
configure_file(src/OrionRomDisks.S.in OrionRomDisks.S @ONLY)

message(STATUS "Add source files:")
foreach (SRC_FILE IN LISTS SRC)
    message(STATUS "${SRC_FILE}")
endforeach ()
message(STATUS "")

add_executable(${PROJECT_NAME} ${SRC})
set_property(SOURCE ${CMAKE_CURRENT_BINARY_DIR}/OrionRomDisks.S PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/orion/Orion128_RomDisk4.rom;${CMAKE_CURRENT_SOURCE_DIR}/dist/orion/romdisk2.bin;${CMAKE_CURRENT_SOURCE_DIR}/dist/orion/dsdos/dsdos-128_s1024.bin")
target_compile_options(${PROJECT_NAME} PRIVATE -O3 -ffunction-sections -fdata-sections)
target_link_options(${PROJECT_NAME} PRIVATE -Wl,--gc-sections)
target_compile_definitions(${PROJECT_NAME} PRIVATE ORION_SCANLINES)

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


# Automatic PWM / I2S detection; explicit overrides are optional.
option(AUDIO_FORCE_PWM "Force PWM output" OFF)
option(AUDIO_FORCE_I2S "Force I2S output" OFF)
if(PICO_BOARD STREQUAL "olimex-pico-pc")
    set(AUDIO_FORCE_PWM ON)
endif()
if(AUDIO_FORCE_PWM AND AUDIO_FORCE_I2S)
    message(FATAL_ERROR "Select at most one forced audio output")
endif()
if(AUDIO_FORCE_PWM)
    target_compile_definitions(${PROJECT_NAME} PRIVATE AUDIO_FORCE_PWM)
    set(BUILD_NAME "${BUILD_NAME}-PWM")
elseif(AUDIO_FORCE_I2S)
    target_compile_definitions(${PROJECT_NAME} PRIVATE AUDIO_FORCE_I2S)
    set(BUILD_NAME "${BUILD_NAME}-I2S")
endif()
if(CS4334)
    target_compile_definitions(${PROJECT_NAME} PRIVATE I2S_CS4334)
endif()

if(ORION_VIDEO STREQUAL "HDMI")
    if(PICO_BOARD STREQUAL "murmulator2")
        set(DVI_PIN_CFG murmulator2_cfg)
    elseif(PICO_BOARD STREQUAL "olimex-pico-pc")
        set(DVI_PIN_CFG Olimex_RP2040_PICO_PC_cfg)
        target_compile_definitions(${PROJECT_NAME} PRIVATE DVI_USE_PIO_CLOCK)
    elseif(PICO_BOARD STREQUAL "waveshare_rp2040_pizero")
        set(DVI_PIN_CFG waveshare_rp2040_pizero_hdmi_cfg)
    elseif(PICO_BOARD STREQUAL "waveshare_rp2350_pizero")
        set(DVI_PIN_CFG waveshare_rp2350_pizero_hdmi_cfg)
    else()
        set(DVI_PIN_CFG murmulator_cfg)
    endif()
    target_compile_definitions(${PROJECT_NAME} PRIVATE HDMI_DVI
        DVI_DEFAULT_SERIAL_CONFIG=${DVI_PIN_CFG} DVI_N_TMDS_BUFFERS=3)
    target_link_libraries(${PROJECT_NAME} PRIVATE hdmi-dvi)
    set(BUILD_NAME "${BUILD_NAME}-HDMI-DVI-${PORT_VERSION}")
elseif(ORION_VIDEO STREQUAL "RGB")
    target_compile_definitions(${PROJECT_NAME} PRIVATE RGB_TV)
    target_link_libraries(${PROJECT_NAME} PRIVATE tv)
    set(BUILD_NAME "${BUILD_NAME}-TV-RGB-${PORT_VERSION}")
elseif(ORION_VIDEO MATCHES "^(PAL|NTSC)$")
    target_compile_definitions(${PROJECT_NAME} PRIVATE SOFTTV)
    if(ORION_VIDEO STREQUAL "NTSC")
        target_compile_definitions(${PROJECT_NAME} PRIVATE TV_NTSC)
    endif()
    target_link_libraries(${PROJECT_NAME} PRIVATE tv-software)
    set(BUILD_NAME "${BUILD_NAME}-TV-SOFT-${ORION_VIDEO}-${PORT_VERSION}")
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE VGA_DRV)
    target_link_libraries(${PROJECT_NAME} PRIVATE vga-nextgen)
    set(BUILD_NAME "${BUILD_NAME}-VGA-${PORT_VERSION}")
endif()

if(${PICO_BOARD} MATCHES "waveshare_rp2350_pizero")
    pico_set_program_name(${PROJECT_NAME} "Orion-128 for RP2350-PiZero")
elseif(${PICO_BOARD} MATCHES "olimex-pico-pc")
    pico_set_program_name(${PROJECT_NAME} "Orion-128 for Olimex RP2040-PICO-PC")
elseif(${PICO_BOARD} MATCHES "murmulator2")
    pico_set_program_name(${PROJECT_NAME} "Orion-128 for MURMULATOR 2.0")
else()
    pico_set_program_name(${PROJECT_NAME} "Orion-128 for MURMULATOR 1.x")
endif()
pico_set_program_version(${PROJECT_NAME} "${PORT_VERSION}")

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${BUILD_NAME}")
