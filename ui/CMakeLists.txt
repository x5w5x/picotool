# ui — UI 辅助模块静态库
# 当前仅包含 ui_geometry(矩形/点几何基础),无任何 Pico SDK / RTOS 依赖。
# 后续添加 ui_*.c 时,只需追加到 add_library 列表即可。

cmake_minimum_required(VERSION 3.13)

add_library(uigeom STATIC
    ui_geometry.c
    ui_tile.c
    ui_draw.c
    ui_core.c
    ui_btn.c
    ui_slider.c
    ui_switch.c
    ui_dropdown.c
    ui_scrollview.c
    ui_modal.c
    ui_tabview.c
    ui_waveform.c
    ui_terminal.c
    ui_input.c
    ui_keyboard.c
    ui_progressbar.c
    ui_file_browser.c
    ui_numpad.c
    ui_packet_table.c
    ui_toast.c
    ui_drawer.c
    ui_stepper.c
    ui_checkbox_group.c
    ui_hex_view.c
    ui_status_bar.c
    ui_picture.c
    
   
    

    
)

target_include_directories(uigeom PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    # ui 与 Drivers 是兄弟目录,要从 ui 往上一级再进 Drivers
    ${CMAKE_CURRENT_LIST_DIR}/../Drivers
    ${CMAKE_CURRENT_LIST_DIR}/../app
)

# ARM GCC 自带的 stdint.h / stdbool.h 路径。
# 真编译时编译器会自动加,但 VSCode IntelliSense 不会自动挖,
# 必须显式写进 compile_commands.json,IntelliSense 才能解析 uint16_t 等类型。
#
# 坑点1:`${CMAKE_C_COMPILER_VERSION}` 在 CMakeCache 里没被缓存(裸 GCC 探测异常),
#        求出来是空串 → 拼出 .../arm-none-eabi//include 双斜杠,被静默吞。
# 坑点2:`find_program(arm-none-eabi-gcc)` 会抓到 eide 自带的 GCC(版本错),
#        拼出错的 stdint.h 路径。
# 坑点3:`target_include_directories` 加进去的 SYSTEM/PUBLIC 路径,如果已经
#        存在于 CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES,Ninja generator 会去重
#        掉(认为编译器默认就会搜),compile_commands.json 也跟着没了。
# 综合:从 CMAKE_C_COMPILER 反推根目录 + execute_process -dumpversion 拿真实版本
#       + target_compile_options -I(绕过去重)三件套搞定。
if(CMAKE_C_COMPILER AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    get_filename_component(_uigeom_gcc_dir "${CMAKE_C_COMPILER}" DIRECTORY)
    get_filename_component(_uigeom_gcc_root "${_uigeom_gcc_dir}/.." ABSOLUTE)
    execute_process(
        COMMAND "${CMAKE_C_COMPILER}" "-dumpversion"
        OUTPUT_VARIABLE _uigeom_gcc_version
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    set(_uigeom_gcc_include
        "${_uigeom_gcc_root}/lib/gcc/arm-none-eabi/${_uigeom_gcc_version}/include")
    if(EXISTS "${_uigeom_gcc_include}/stdint.h")
        # 用 -I 直接拼到命令行,不经过 target_include_directories 的去重逻辑
        # 用 PUBLIC 而非 PRIVATE —— 依赖 uigeom 的下游(如 appmgr)也得能解析 stdint.h,
        # PRIVATE 不传播,会导致 app/ 下的 .h 里 uint16_t / int16_t 飘红。
        target_compile_options(uigeom PUBLIC "-I${_uigeom_gcc_include}")
        message(STATUS "uigeom: ARM GCC builtin include = ${_uigeom_gcc_include}")
    else()
        message(WARNING "uigeom: ${_uigeom_gcc_include} not found, stdint.h unresolved")
    endif()
endif()

# 与 Drivers 保持一致的字符集(注释/字符串走 GBK 编码)
target_compile_options(uigeom PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable
    -finput-charset=UTF-8
    -fexec-charset=GBK
)

# 接入方式(在主工程的 CMakeLists.txt):
#   add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/ui)
#   target_link_libraries(<your_target> PRIVATE uigeom)