cmake_minimum_required(VERSION 3.13)
project(game2048 C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

add_executable(game2048 main.c game2048_plugin.c)
target_include_directories(game2048 PRIVATE
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/ui
    ${CMAKE_SOURCE_DIR}/Drivers
)
target_compile_options(game2048 PRIVATE
    -mcpu=cortex-m0plus -mthumb -O2 -ffreestanding -fno-builtin
    -fdata-sections -ffunction-sections -Wall -Wno-unused-parameter
    -finput-charset=UTF-8 -fexec-charset=GBK
)
target_link_options(game2048 PRIVATE
    -mcpu=cortex-m0plus -mthumb -nostartfiles -nodefaultlibs -nostdlib
    -Wl,--gc-sections -Wl,--build-id=none
    -Wl,-T${CMAKE_CURRENT_SOURCE_DIR}/plugin.ld
)
target_link_libraries(game2048 PRIVATE gcc)
add_custom_command(TARGET game2048 POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:game2048>
        ${CMAKE_CURRENT_BINARY_DIR}/2048.papp
    COMMENT "Generating 2048.papp"
    VERBATIM
)
