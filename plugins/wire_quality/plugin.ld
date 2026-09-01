/* =============================================================
 * Wire Quality Tester 动态 App 专用链接脚本
 * -------------------------------------------------------------
 * 必须严格对应 app/app_api.h 里的两个常量:
 *   APP_RAM_BASE = 0x20038000
 *   APP_RAM_SIZE = 32K
 *
 * 入口 app_entry() 强制放在二进制 offset 0:
 *   - host 在 s_app_ram_pool 起始位置直接跳转 (Thumb 入口,LSB=1)
 *   - .entry 段必须排在 .text / .data / .bss 之前
 * ============================================================= */

MEMORY
{
    APP_RAM (rwx) : ORIGIN = 0x20038000, LENGTH = 32K
}

SECTIONS
{
    /* ⚡ 1. 入口函数必须位于二进制最前头第 0 字节 */
    .entry : ALIGN(4)
    {
        KEEP(*(.entry))
    } > APP_RAM

    /* ⚡ 2. 代码与常量 (.rodata 跟随 .text,函数内常量字符串能直接寻址) */
    .text : ALIGN(4)
    {
        *(.text*)
        *(.rodata*)
    } > APP_RAM

    /* ⚡ 3. 已初始化数据 */
    .data : ALIGN(4)
    {
        *(.data*)
    } > APP_RAM

    /* ⚡ 4. 零初始化数据 (运行前 host memset 池子,这部分会被清零) */
    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > APP_RAM

    /* 丢弃 SDK / GCC 在 freestanding 环境下不需要的元数据段 */
    /DISCARD/ :
    {
        *(.ARM.attributes)
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.comment)
        *(.note*)
    }
}
