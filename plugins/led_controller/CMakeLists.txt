# ============================================================
# @file   plugins/led_controller/CMakeLists.txt
# @brief  led_controller.papp 独立 freestanding 编译脚本
#
# @section 角色
#   - 编译产物 led_controller.papp 是 raw binary (≤32KB)
#   - 由 host 在 app_sd_msc 文件浏览器里点 .papp 触发加载
#   - 与 novel_reader / wire_quality 的 CMakeLists 模板一致
#
# @section 关键约束
#   - bare-metal: -nostartfiles -nodefaultlibs -nostdlib -fno-builtin
#   - 不链 libuigeom.a / drivelib / pico_stdlib
#   - UI 函数全部走 host_api 函数指针
#   - post-build: arm-none-eabi-objcopy -O binary 抽 raw binary
#
# @section 头文件依赖
#   app_api.h → ui_tile.h → ui_geometry.h
#             → ui_btn.h  → ui_tile.h / chinese.h
#             → chinese.h
#   include path 必须覆盖: ${CMAKE_SOURCE_DIR}/app, /ui, /Drivers
#
# @section 待重构 (P2)
#   - 三 plugin (led_controller / novel_reader / wire_quality) 的 CMakeLists
#     大同小异,只差 plugin 名 + list 源 → 抽成 cmake/plugin.cmake include
#   - link gcc 用 target_link_libraries;wire_quality 已显式链,本 plugin
#     实际不链 (无除法),未来若加整数运算要补
# ============================================================
# 独立 plugin 编译: led_controller
# 编译产物 led_controller.papp 是 raw binary,塞到 TF 卡 0:/led_controller.papp
# 由 host 在 app_sd_msc 文件浏览器里点 .papp 触发加载
#
# 关键约束:
#   - 必须 bare-metal: -nostartfiles -nodefaultlibs -nostdlib -fno-builtin
#   - 不链 libuigeom.a / drivelib / pico_stdlib: 所有 UI 函数都走 host_api 函数指针
#   - include path 只用来拿 struct / typedef 定义,不链 SDK 任何 .o / .a
#   - post-build 用 arm-none-eabi-objcopy -O binary 抽 raw binary
#
# 头文件依赖图 (必须全部覆盖到):
#   app_api.h → ui_tile.h → ui_geometry.h
#             → ui_btn.h  → ui_tile.h / chinese.h
#             → chinese.h
# 所以 include path 至少需要:
#   ${CMAKE_SOURCE_DIR}/app      (app_api.h 所在)
#   ${CMAKE_SOURCE_DIR}/ui       (ui_*.h 所在)
#   ${CMAKE_SOURCE_DIR}/Drivers  (chinese.h / FontSize_t 所在)
# ============================================================

cmake_minimum_required(VERSION 3.13)
project(led_controller C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# ⚠️ plugin 不能加进 pico 主可执行文件,只能编译成独立 .papp 二进制
# main.c 只放 app_entry() 入口(锚定 .entry 段 offset 0),
# plugin_led_controller.c 放状态 + 生命周期 + UI 渲染(镜像 app_led.c 风格)
add_executable(led_controller
    main.c
    plugin_led_controller.c
)

# 头文件路径:让 plugin 拿到 host 的类型定义 (无链接)
target_include_directories(led_controller PRIVATE
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/ui
    ${CMAKE_SOURCE_DIR}/Drivers
)

# ⚡ 编译参数: cortex-m0plus + Thumb + freestanding,函数和 data 分别 section 让
#    --gc-sections 能砍掉真正无引用的符号
target_compile_options(led_controller PRIVATE
    -mcpu=cortex-m0plus
    -mthumb
    -O2
    -ffreestanding
    -fno-builtin
    -fdata-sections
    -ffunction-sections
    -Wall
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    # ⚡ 源文件 UTF-8 → 目标二进制 GBK (与主系统字库 GB2312 兼容)
    # ui_draw_string 看到 ≥0x80 的字节会按 GB2312 双字节解释并查字库,
    # 所以源里写的 "LED 控制器" 必须编译成 GBK 字节才能正确出字
    -finput-charset=UTF-8
    -fexec-charset=GBK
)

# ⚡ 链接参数: 不进 SDK / libc / libgcc / libuigeom / drivelib,
#    只用 plugin 自己的 .o + 自定义 linker 脚本
target_link_options(led_controller PRIVATE
    -mcpu=cortex-m0plus
    -mthumb
    -nostartfiles
    -nodefaultlibs
    -nostdlib
    -Wl,--gc-sections
    -Wl,--build-id=none
    -Wl,-T${CMAKE_CURRENT_SOURCE_DIR}/plugin.ld
)

# post-build: 抽 raw binary → led_controller.papp
# host 的 app_loader_load_papp 直接 f_read 整个 32KB,头部必须含 app_entry 入口
add_custom_command(TARGET led_controller POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary
        $<TARGET_FILE:led_controller>
        ${CMAKE_CURRENT_BINARY_DIR}/led_controller.papp
    COMMENT ">>> 生成 led_controller.papp <<<"
    VERBATIM
)
