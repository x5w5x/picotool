# MiniRTOS 子模块构建
# 生成一个静态库 minirtos,链接到顶层可执行文件。
# 注意:不要在这里 link 到 pico_stdlib 等 INTERFACE 库,
# 否则 SDK 的 startup (crt0.S.obj, 含 weak isr_pendsv BKPT 默认) 会
# 被合并到 libminirtos.a,我们的强定义 isr_pendsv 会被链接器优先匹配
# SDK weak,导致 PendSV_Handler 实际指向 BKPT!

add_library(minirtos STATIC
    OS_Core/os_core.c
    OS_Port/os_port.c
    OS_Port/os_port.s
)

# 暴露头文件路径
target_include_directories(minirtos PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/OS_Core
    ${CMAKE_CURRENT_LIST_DIR}/OS_Port
    C:/Users/xwx/.pico-sdk/sdk/2.2.0/src/rp2_common/cmsis/stub/CMSIS/Device/RP2040/Include
    C:/Users/xwx/.pico-sdk/sdk/2.2.0/src/rp2_common/cmsis/stub/CMSIS/Core/Include
    # 整个 SDK src 路径(SYSTEM 包含,只用于查找头文件,不传链接依赖)
    C:/Users/xwx/.pico-sdk/sdk/2.2.0/src
)
target_include_directories(minirtos SYSTEM PUBLIC
    C:/Users/xwx/.pico-sdk/sdk/2.2.0/src
)

# RTOS 用 C99 写,C 标准保持 11
target_compile_options(minirtos PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable
)

# 只 link 直接需要的硬件接口(不传递依赖)
target_link_libraries(minirtos PUBLIC
    hardware_structs
    hardware_regs
)