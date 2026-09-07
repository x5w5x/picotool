add_library(pio_manager STATIC
    pio_manager.c
)

target_include_directories(pio_manager PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(pio_manager PUBLIC
    pico_stdlib
    hardware_pio
)
