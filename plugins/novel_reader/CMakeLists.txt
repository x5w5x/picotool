# ============================================================
# 独立 plugin 编译: novel_reader (TF 卡 .txt 小说阅读器)
# 编译产物 novel_reader.papp 是 raw binary,塞到 TF 卡根目录
# 由 host 在 app_sd_msc 文件浏览器里点 .papp 触发加载
#
# freestanding: 不链 libuigeom.a / libc / libgcc 之外的 SDK
# (libgcc 通过 target_link_libraries 显式链,提供 cortex-m0+ 软除法)
# ============================================================

cmake_minimum_required(VERSION 3.13)
project(novel_reader C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

add_executable(novel_reader
    main.c
    plugin_novel_reader.c
)

target_include_directories(novel_reader PRIVATE
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/ui
    ${CMAKE_SOURCE_DIR}/Drivers
)

target_compile_options(novel_reader PRIVATE
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
    -finput-charset=UTF-8
    -fexec-charset=GBK
)

target_link_options(novel_reader PRIVATE
    -mcpu=cortex-m0plus
    -mthumb
    -nostartfiles
    -nodefaultlibs
    -nostdlib
    -Wl,--gc-sections
    -Wl,--build-id=none
    -Wl,-T${CMAKE_CURRENT_SOURCE_DIR}/plugin.ld
)
target_link_libraries(novel_reader PRIVATE gcc)

add_custom_command(TARGET novel_reader POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary
        $<TARGET_FILE:novel_reader>
        ${CMAKE_CURRENT_BINARY_DIR}/novel_reader.papp
    COMMENT ">>> 生成 novel_reader.papp <<<"
    VERBATIM
)