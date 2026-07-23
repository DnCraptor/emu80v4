add_library(hway INTERFACE)
target_sources(hway INTERFACE ${CMAKE_CURRENT_LIST_DIR}/hway.c)
target_include_directories(hway INTERFACE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(hway INTERFACE hardware_gpio hardware_pwm hardware_clocks)
