# app — 应用管理器 + 业务 App 静态库
# 当前包含:
#   - app_manager:通用 App 调度 / 切换 / 触控路由 / 渲染分发(无 SDK 直依赖)
#   - app_led:GPIO LED 控制示例 App(直接调 Pico SDK hardware/gpio.h)
# 通过 app_t 接口接入具体业务 App(GPIO 矩阵 / 示波器 / DAP 烧录 / UART 助手 等)。
# 依赖 ui/(ui_tile.h / ui_geometry.h / ui_btn.h → chinese.h → Drivers/) + Pico SDK。

cmake_minimum_required(VERSION 3.13)

add_library(appmgr STATIC
    app_manager.c
    app_led.c
    app_template.c
    app_uart_term.c
    app_swd_probe.c
    cmsis_dap.c         
    usb_descriptors.c   
    msc_disk.c 
    app_sd_msc.c
    swd_flasher.c
    app_adc.c
    app_pwm.c
    app_i2c_spi.c
    app_pinout.c
    audio_i2s.c
    app_audio.c
    app_loader.c
    app_settings.c
)

target_include_directories(appmgr PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    # 项目根目录包含 config.h 等全局配置头文件。
    ${CMAKE_CURRENT_LIST_DIR}/..
    # ui/ 是 app/ 的兄弟目录,app_base.h 直接 #include "ui_tile.h"
    ${CMAKE_CURRENT_LIST_DIR}/../ui
    # app_led.c 通过 ui_btn.h / ui_draw.h 链上 chinese.h → Drivers/
    ${CMAKE_CURRENT_LIST_DIR}/../Drivers
     ${CMAKE_CURRENT_LIST_DIR}/../fatfs
     ${CMAKE_CURRENT_LIST_DIR}/../fatfs/ff15/source
     ${CMAKE_CURRENT_LIST_DIR}/../fatfs/include
     ${CMAKE_CURRENT_LIST_DIR}/../fatfs/src
)

# 业务 App(app_led)需要 Pico SDK 的 hardware/gpio.h / pico/stdlib.h。
# 用 INTERFACE_INCLUDE_DIRECTORIES 顺着 link 关系自动加进来,
#   IntelliSense / 编译器都能拿到 SDK 头文件路径。
# 注意:pico_stdlib 是 INTERFACE 库,它会 PUBLIC 暴露所有 SDK include 路径。
target_link_libraries(appmgr PUBLIC pico_stdlib hardware_gpio pico_multicore 
pico_stdio_usb 
pico_unique_id
hardware_dma
 hardware_pio 
hardware_adc
 hardware_pwm
 hardware_i2c
 hardware_spi

)

# ARM GCC 自带的 stdint.h / stdbool.h 路径。
# 与 ui/CMakeLists.txt 完全相同的检测块 —— 不能简单依赖 uigeom 的 PUBLIC 选项传播,
# 因为 PUBLIC compile_options 不会跨"无 link 依赖"的边界。
# 直接在本目录再跑一次 detect + -I,既保证 IntelliSense 能解析 stdint.h,
# 也保证本模块自给自足、不被 ui/ 的内部选项污染。
if(CMAKE_C_COMPILER AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    get_filename_component(_appmgr_gcc_dir "${CMAKE_C_COMPILER}" DIRECTORY)
    get_filename_component(_appmgr_gcc_root "${_appmgr_gcc_dir}/.." ABSOLUTE)
    execute_process(
        COMMAND "${CMAKE_C_COMPILER}" "-dumpversion"
        OUTPUT_VARIABLE _appmgr_gcc_version
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    set(_appmgr_gcc_include
        "${_appmgr_gcc_root}/lib/gcc/arm-none-eabi/${_appmgr_gcc_version}/include")
    if(EXISTS "${_appmgr_gcc_include}/stdint.h")
        target_compile_options(appmgr PRIVATE "-I${_appmgr_gcc_include}")
        message(STATUS "appmgr: ARM GCC builtin include = ${_appmgr_gcc_include}")
    else()
        message(WARNING "appmgr: ${_appmgr_gcc_include} not found, stdint.h unresolved")
    endif()
endif()

# 与 ui/ 保持一致的字符集(注释/字符串走 GBK 编码)
target_compile_options(appmgr PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable
    -finput-charset=UTF-8
    -fexec-charset=GBK
)

# 接入方式(在主工程的 CMakeLists.txt):
#   add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/app)
#   target_link_libraries(<your_target> PRIVATE appmgr)
#
# 注意:appmgr 不依赖 Pico SDK,但它的依赖链上 ui/ 依赖 Drivers/lcd.h,
#      所以最终可执行目标链接时仍需要 drivelib(lcd.h 来自 Drivers)。
