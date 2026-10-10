add_library(fun STATIC
    fun_uart.c
    fun_uart_profile.c
    fun_esp.c
    fun_pio_uart.c
    pin_manager.c
    fun_swd.c
    fun_swd_config.c
    fun_pio_swd.c
    thumb_disasm.c
    sym_loader.c
    fun_script_engine.c
    fun_ir.c
    fun_task_flow.c
    fun_i2c.c
    fun_spi.c

    
)

target_include_directories(fun PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/../app
    ${CMAKE_CURRENT_LIST_DIR}/../ui
    ${CMAKE_CURRENT_LIST_DIR}/../Drivers
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../fatfs
    ${CMAKE_CURRENT_LIST_DIR}/../fatfs/ff15/source
    ${CMAKE_CURRENT_LIST_DIR}/../fatfs/include
    ${CMAKE_CURRENT_LIST_DIR}/../fatfs/src
    
)

target_link_libraries(fun PUBLIC
    pico_stdlib
    hardware_uart
    hardware_gpio
    hardware_pio
    hardware_pwm
    hardware_i2c
    hardware_spi
    hardware_adc
    hardware_clocks
    pio_manager
   
)

pico_generate_pio_header(fun
    ${CMAKE_CURRENT_LIST_DIR}/../pio/uart.pio
)

pico_generate_pio_header(fun
    ${CMAKE_CURRENT_LIST_DIR}/../pio/swd.pio
     ${CMAKE_CURRENT_LIST_DIR}/../pio/ir.pio
)
