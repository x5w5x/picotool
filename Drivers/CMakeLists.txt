# Drivers 与 UI 统一驱动库
add_library(drivelib STATIC
    # 底层外设驱动
    lcd.c
    touch.c
    w25q64.c
    tf.c
    chinese.c
)

# 导出当前驱动目录的头文件路径
target_include_directories(drivelib PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    C:/Users/xwx/.pico-sdk/sdk/2.2.0/src/rp2_common/cmsis/stub/CMSIS/Device/RP2040/Include
    C:/Users/xwx/.pico-sdk/sdk/2.2.0/src/rp2_common/cmsis/stub/CMSIS/Core/Include
)

# 链接 Pico SDK 官方接口库（自动传递底层寄存器与 include 依赖）
target_link_libraries(drivelib PUBLIC
    pico_stdlib
    hardware_spi
    hardware_i2c
    hardware_gpio
    hardware_dma
    hardware_resets
    hardware_timer
)

# 编译选项与中文字符集转码保护
target_compile_options(drivelib PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -finput-charset=UTF-8
    -fexec-charset=GBK
)