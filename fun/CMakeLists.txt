add_library(fun STATIC
    fun_uart.c
    fun_esp.c
)

target_include_directories(fun PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(fun PUBLIC
    pico_stdlib
    hardware_uart
    hardware_gpio
    hardware_pio
)
