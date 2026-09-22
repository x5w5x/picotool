#include "debug_symbols.h"
#include <stddef.h>

const debug_symbol_t g_debug_symbols[DEBUG_SYMBOL_COUNT] = {
    { 0x080000ECu,    8, "__main" },
    { 0x080000F4u,   44, "__scatterload_rt2" },
    { 0x08000128u,   28, "__scatterload_zeroinit" },
    { 0x0800016Cu,    8, "Reset_Handler" },
    { 0x080001ACu,    2, "__use_two_region_memory" },
    { 0x080001AEu,    2, "__rt_heap_escrow$2region" },
    { 0x080001B0u,    2, "__rt_heap_expand$2region" },
    { 0x080001B2u,   74, "__user_setup_stackheap" },
    { 0x080001FCu,   18, "exit" },
    { 0x08000210u,    8, "__user_libspace" },
    { 0x08000218u,    8, "_sys_exit" },
    { 0x08000224u,    2, "__use_no_semihosting_swi" },
    { 0x08000226u,    4, "BusFault_Handler" },
    { 0x0800022Au,    2, "DebugMon_Handler" },
    { 0x0800022Cu,   24, "Delay_ms" },
    { 0x08000244u,   46, "Delay_us" },
    { 0x08000272u,  278, "GPIO_Init" },
    { 0x08000388u,    4, "GPIO_ResetBits" },
    { 0x0800038Cu,    4, "GPIO_SetBits" },
    { 0x08000390u,    4, "HardFault_Handler" },
    { 0x08000394u,    4, "MemManage_Handler" },
    { 0x08000398u,    2, "NMI_Handler" },
    { 0x0800039Au,    2, "PendSV_Handler" },
    { 0x0800039Cu,   26, "RCC_APB2PeriphClockCmd" },
    { 0x080003BCu,    2, "SVC_Handler" },
    { 0x080003BEu,    8, "SetSysClock" },
    { 0x080003C8u,  214, "SetSysClockTo72" },
    { 0x080004A8u,    2, "SysTick_Handler" },
    { 0x080004ACu,   78, "SystemInit" },
    { 0x0800050Cu,    4, "UsageFault_Handler" },
    { 0x08000510u,   86, "main" },
};

const char *debug_symbol_find(uint32_t pc, uint32_t *offset)
{
    pc &= ~1u;
    int low = 0, high = DEBUG_SYMBOL_COUNT - 1, hit = -1;
    while (low <= high) {
        int mid = low + (high - low) / 2;
        if (g_debug_symbols[mid].addr <= pc) {
            hit = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    if (hit >= 0) {
        const debug_symbol_t *sym = &g_debug_symbols[hit];
        if (pc < (sym->addr + sym->size)) {
            if (offset) *offset = pc - sym->addr;
            return sym->name;
        }
    }
    return NULL;
}
