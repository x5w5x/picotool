# ============================================================
# 独立 plugin 编译: wire_quality (杜邦线质量测量仪)
# 编译产物 wire_quality.papp 是 raw binary,塞到 TF 卡 0:/wire_quality.papp
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
project(wire_quality C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# ⚠️ plugin 不能加进 pico 主可执行文件,只能编译成独立 .papp 二进制
# main.c 只放 app_entry() 入口(锚定 .entry 段 offset 0),
# plugin_wire_quality.c 放状态 + 生命周期 + UI 渲染
add_executable(wire_quality
    main.c
    plugin_wire_quality.c
)

# 头文件路径:让 plugin 拿到 host 的类型定义 (无链接)
target_include_directories(wire_quality PRIVATE
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/ui
    ${CMAKE_SOURCE_DIR}/Drivers
)

# ⚡ 编译参数: cortex-m0plus + Thumb + freestanding,函数和 data 分别 section 让
#    --gc-sections 能砍掉真正无引用的符号
target_compile_options(wire_quality PRIVATE
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
    # 所以源里写的 "杜邦线质量" 必须编译成 GBK 字节才能正确出字
    -finput-charset=UTF-8
    -fexec-charset=GBK
)

# ⚡ 链接参数: 不进 SDK / libc / libuigeom / drivelib,
#    只用 plugin 自己的 .o + 自定义 linker 脚本
# ⚠️ 唯一例外:target_link_libraries 显式链 libgcc,提供 cortex-m0+ 没有的软除法子程
#    (__aeabi_uidiv / __aeabi_idivmod / __aeabi_ldivmod),
#    BER 计算 (bit_errors * 1000 / bits_sent) 离不开它们。
#    注意 -nodefaultlibs 默认不链 libgcc,必须显式链入。
#    target_link_libraries 比 target_link_options 的优势在于把 -lgcc 放到 object 之后,
#    ld 在处理 object 时积累未解析符号,然后扫到 -lgcc 才能补齐。
target_link_options(wire_quality PRIVATE
    -mcpu=cortex-m0plus
    -mthumb
    -nostartfiles
    -nodefaultlibs
    -nostdlib
    -Wl,--gc-sections
    -Wl,--build-id=none
    -Wl,-T${CMAKE_CURRENT_SOURCE_DIR}/plugin.ld
)
target_link_libraries(wire_quality PRIVATE gcc)

# post-build: 抽 raw binary → wire_quality.papp
# host 的 app_loader_load_papp 直接 f_read 整个 32KB,头部必须含 app_entry 入口
add_custom_command(TARGET wire_quality POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary
        $<TARGET_FILE:wire_quality>
        ${CMAKE_CURRENT_BINARY_DIR}/wire_quality.papp
    COMMENT ">>> 生成 wire_quality.papp <<<"
    VERBATIM
)
