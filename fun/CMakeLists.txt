add_library(fun STATIC
    fun_uart.c
    fun_uart_profile.c
    fun_esp.c
    fun_pio_uart.c
    pin_manager.c
    fun_swd.c
    fun_swd_config.c
    fun_pio_swd.c
)

target_include_directories(fun PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(fun PUBLIC
    pico_stdlib
    hardware_uart
    hardware_gpio
    hardware_pio
    hardware_clocks
    pio_manager
)

pico_generate_pio_header(fun
    ${CMAKE_CURRENT_LIST_DIR}/../pio/uart.pio
)

pico_generate_pio_header(fun
    ${CMAKE_CURRENT_LIST_DIR}/../pio/swd.pio
)
