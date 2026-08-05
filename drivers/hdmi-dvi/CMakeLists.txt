add_library(hdmi-dvi INTERFACE)

target_sources(hdmi-dvi INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/hdmi-dvi.c
)

target_include_directories(hdmi-dvi INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/../graphics
)

target_link_libraries(hdmi-dvi INTERFACE
        libdvi
        hardware_clocks
)
