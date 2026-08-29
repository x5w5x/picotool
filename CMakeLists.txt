# # Generated Cmake Pico project file

# cmake_minimum_required(VERSION 3.13)

# set(CMAKE_C_STANDARD 11)
# set(CMAKE_CXX_STANDARD 17)
# set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# # Initialise pico_sdk from installed location
# # (note this can come from environment, CMake cache etc)

# # == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
# if(WIN32)
#     set(USERHOME $ENV{USERPROFILE})
# else()
#     set(USERHOME $ENV{HOME})
# endif()
# set(sdkVersion 2.2.0)
# set(toolchainVersion 14_2_Rel1)
# set(picotoolVersion 2.2.0-a4)
# set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
# if (EXISTS ${picoVscode})
#     include(${picoVscode})
# endif()
# # ====================================================================================
# set(PICO_BOARD pico CACHE STRING "Board type")

# # Pull in Raspberry Pi Pico SDK (must be before project)
# include(pico_sdk_import.cmake)

# project(pico C CXX ASM)

# # Initialise the Raspberry Pi Pico SDK
# pico_sdk_init()

# # Add executable. Default name is the project name, version 0.1

# add_executable(pico pico.cpp )

# pico_set_program_name(pico "pico")
# pico_set_program_version(pico "0.1")

# # Generate PIO header
# pico_generate_pio_header(pico ${CMAKE_CURRENT_LIST_DIR}/blink.pio)

# # Modify the below lines to enable/disable output over UART/USB
# # 关键:把 USB CDC stdout 超时从默认 500ms 缩短到 1ms。
# # Pico 接上 USB 但 host 没开串口监视器时,printf 默认阻塞 500ms
# # 等 DTR,这会让 RTOS 任务卡死。1ms 超时虽然数据会被丢弃,
# # 但 task 不会卡住,串口监视器打开后输出能立刻恢复。
# target_compile_definitions(pico PRIVATE
#     PICO_STDIO_USB_STDOUT_TIMEOUT_US=1000
# )

# pico_enable_stdio_uart(pico 1)
# pico_enable_stdio_usb(pico 1)

# # Add the standard library to the build
# target_link_libraries(pico PUBLIC
#         pico_stdlib
#         hardware_pio
#         hardware_spi
#         hardware_dma
#         hardware_i2c)

# # Add the standard include files to the build
# target_include_directories(pico PRIVATE
#         ${CMAKE_CURRENT_LIST_DIR}
# )

# # Pull in MiniRTOS (手搓 RTOS 内核,移植自 STM32F103)
# add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/rtos)

# # Pull in Drivers (手搓外设驱动: LCD ST7796 / Touch FT6336 / Flash W25Q64)
# add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/Drivers)
# # 关键:--whole-archive 让链接器整体拉取 minirtos.a 的所有 .o,
# # 否则 SDK 默认的 weak isr_pendsv/bkpt 会抢占 vector table,
# # 导致 PendSV_Handler 实际指向 BKPT(死锁)。后续 --no-whole-archive 恢复默认。
# target_link_libraries(pico PRIVATE
#     -Wl,--whole-archive
#     minirtos
#     drivelib
#     -Wl,--no-whole-archive
#     pico_stdlib
# )

# pico_add_extra_outputs(pico)


# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

set(PICO_BOARD pico CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK
include(pico_sdk_import.cmake)

project(pico C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(pico pico.cpp)

pico_set_program_name(pico "pico")
pico_set_program_version(pico "0.1")

# Generate PIO header
pico_generate_pio_header(pico ${CMAKE_CURRENT_LIST_DIR}/blink.pio)

# USB CDC 1ms 超时保护 + 关闭 SDK 默认描述符(让 app/usb_descriptors.c 接管 USB 复合设备描述符)
# 关闭后 SDK 不再提供 tud_descriptor_device_cb/configuration_cb/string_cb,
# 用户的 HID + CDC 复合描述符独占,链接冲突消除。
target_compile_definitions(pico PRIVATE
    PICO_STDIO_USB_STDOUT_TIMEOUT_US=1000
    PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS=0
)

pico_enable_stdio_uart(pico 1)
pico_enable_stdio_usb(pico 1)

# Add the standard include files to the build
target_include_directories(pico PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/fatfs/ff15/source
    ${CMAKE_CURRENT_LIST_DIR}/fatfs/include
    ${CMAKE_CURRENT_LIST_DIR}/fatfs/src
)

# 引入子模块
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/Drivers)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/ui)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/app)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/fatfs)

# 关键修复：仅对 minirtos 使用 --whole-archive，drivelib 和 SDK 库放在常规链接区
# 业务静态库(drivelib / uigeom / appmgr)用 --start-group / --end-group 圈起来,
# 让链接器反复扫描直到符号收敛 —— 否则 app_led.o 引用 ui_switch_set_state,
# 而 libuigeom.a 在 libappmgr.a 之前就被处理,ui_switch.o 拉不进来 → undefined reference。
target_link_libraries(pico PRIVATE
    -Wl,--start-group
    drivelib
    uigeom
    appmgr
    FatFs_SPI
    -Wl,--end-group
    pico_stdlib
    hardware_pio
    hardware_spi
    hardware_dma
    hardware_i2c
    hardware_adc
)

pico_add_extra_outputs(pico)