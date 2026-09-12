/*
 * @Author: 轩
 * @Date: 2026-09-10 19:49:11
 * @LastEditTime: 2026-09-12 14:29:14
 * @FilePath: \pico\fun\fun_pio_uart.c
 */
#include "fun_pio_uart.h"
#include "hardware/clocks.h"
#include <stddef.h>
#include <string.h>
#include "hardware/gpio.h"
#include "uart.pio.h"

#define FUN_PIO_UART_CYCLES_PER_BIT 8u
#define FUN_PIO_UART_TX_NAME "uart_tx"
#define FUN_PIO_UART_RX_NAME "uart_rx"
static bool fun_pio_uart_calc_clkdiv(uint32_t baudrate,float *clkdiv){
      if (baudrate == 0 || clkdiv == NULL) 
        return false;
    uint32_t system_clock =clock_get_hz(clk_sys);
    float result = (float)system_clock/((float)baudrate*(float)FUN_PIO_UART_CYCLES_PER_BIT);
    if(result <1.0f||result>65535.0f) return false;
    *clkdiv =result;
    return true;
    
}

void fun_pio_uart_tx_init(fun_pio_uart_tx_t *tx)
{
    if(tx == NULL) return ;
    memset(tx,0,sizeof(*tx));
}

bool fun_pio_uart_tx_open(fun_pio_uart_tx_t *tx,pio_manager_t *manager,uint8_t pio_index, uint8_t tx_gpio,uint32_t baudrate)
{
    if (tx == NULL || manager == NULL) 
        return false;
    if (tx->opened) 
        return false;
    if (pio_index >= PIO_BLOCK_COUNT) 
        return false;
    if (tx_gpio >= NUM_BANK0_GPIOS) 
        return false;
    float clkdiv;
    if (!fun_pio_uart_calc_clkdiv(baudrate, &clkdiv)) 
        return false;
    int sm_id = pio_manager_claim_sm_from( manager, pio_index,FUN_PIO_UART_TX_NAME);
    if (sm_id < 0) {
        return false;
    }
    uint8_t sm =(uint8_t)(sm_id % PIO_SM_COUNT);

    int offset = pio_manager_load_program(manager,pio_index,FUN_PIO_UART_TX_NAME,&uart_tx_program);
    if (offset < 0) {
        pio_manager_release_sm(manager,pio_index,sm);
        return false;
    }
    PIO pio =pio_manager_get_hardware(manager, pio_index);
    if (pio == NULL) {
        pio_manager_release_program(manager,pio_index,FUN_PIO_UART_TX_NAME);
        pio_manager_release_sm( manager,pio_index,sm);
        return false;
    }
    pio_sm_config config =uart_tx_program_get_default_config((uint)offset);
    sm_config_set_out_pins(&config,tx_gpio,1);
    sm_config_set_sideset_pins(&config,tx_gpio);
    sm_config_set_out_shift( &config, true, false, 32);
    sm_config_set_clkdiv( &config, clkdiv);
    pio_gpio_init(pio, tx_gpio);
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_init(pio, sm, (uint)offset, &config);
    uint32_t pin_mask =1u << tx_gpio;
    pio_sm_set_pins_with_mask( pio,sm,pin_mask,pin_mask);
    pio_sm_set_pindirs_with_mask(pio, sm, pin_mask, pin_mask);
    pio_sm_set_enabled(pio, sm, true);
    tx->opened = true;
    tx->manager = manager;
    tx->pio = pio;
    tx->pio_index = pio_index;
    tx->sm = sm;
    tx->offset = (uint)offset;
    tx->tx_gpio = tx_gpio;
    tx->baudrate = baudrate;
    return true;
}


bool fun_pio_uart_tx_putc(fun_pio_uart_tx_t *tx,uint8_t value){
    if(tx==NULL||!tx->opened||tx->pio==NULL) return false;
    pio_sm_put_blocking(tx->pio,tx->sm,(uint32_t)value);
    return true;
}

size_t fun_pio_uart_tx_write(fun_pio_uart_tx_t *tx,const uint8_t *data,size_t len)
{
    if(tx==NULL||data==NULL) return 0;
    size_t written=0;
    while(written<len){
        if(!fun_pio_uart_tx_putc(tx,data[written])){
            break;
        }
        ++written;
    }
    return written;
}

bool fun_pio_uart_tx_is_writable(const fun_pio_uart_tx_t *tx)
{
    if(tx==NULL||!tx->opened || tx->pio==NULL)
    return false;
    return !pio_sm_is_tx_fifo_full(tx->pio,tx->sm);
}

bool fun_pio_uart_tx_try_putc(fun_pio_uart_tx_t *tx,uint8_t value)
{
    if(!fun_pio_uart_tx_is_writable(tx))
    return false;
    pio_sm_put(tx->pio,tx->sm,(uint32_t)value);
    return true;
}

bool fun_pio_uart_tx_close(fun_pio_uart_tx_t *tx)
{
    if (tx == NULL || !tx->opened ||tx->manager == NULL || tx->pio == NULL) {
        return false;
    }

    pio_sm_set_enabled(tx->pio, tx->sm, false);
    pio_sm_clear_fifos(tx->pio, tx->sm);
    bool program_released = pio_manager_release_program(tx->manager,tx->pio_index, FUN_PIO_UART_TX_NAME);
    bool sm_released = pio_manager_release_sm(tx->manager, tx->pio_index,tx->sm);
    fun_pio_uart_tx_init(tx);

    return program_released && sm_released;
}
void fun_pio_uart_rx_init(fun_pio_uart_rx_t *rx)
{
    if (rx == NULL) {
        return;
    }
    memset(rx, 0, sizeof(*rx));
}

bool fun_pio_uart_rx_open(fun_pio_uart_rx_t *rx,pio_manager_t *manager,uint8_t pio_index,uint8_t rx_gpio,uint32_t baudrate)
{
    if (rx == NULL || manager == NULL) 
        return false;
    if (rx->opened) 
        return false;
    if (pio_index >= PIO_BLOCK_COUNT) 
        return false;
    if (rx_gpio >= NUM_BANK0_GPIOS) 
        return false;
     float clkdiv;
      if (!fun_pio_uart_calc_clkdiv(baudrate, &clkdiv)) 
        return false;
    int sm_id = pio_manager_claim_sm_from(manager,pio_index,FUN_PIO_UART_RX_NAME);
    if (sm_id < 0) {
        return false;
    }
    uint8_t sm = (uint8_t)(sm_id % PIO_SM_COUNT);
    int offset = pio_manager_load_program(manager,pio_index,FUN_PIO_UART_RX_NAME,&uart_rx_program);
    if (offset < 0) {
        pio_manager_release_sm(manager, pio_index, sm);
        return false;
    }
    PIO pio = pio_manager_get_hardware(manager, pio_index);
    if (pio == NULL) {
        pio_manager_release_program(manager,pio_index,FUN_PIO_UART_RX_NAME);
        pio_manager_release_sm( manager,pio_index,sm);
        return false;
    }
    pio_sm_config config =uart_rx_program_get_default_config((uint)offset);
    sm_config_set_in_pins(&config, rx_gpio);
    sm_config_set_in_shift(&config, true, false, 32);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&config, clkdiv);

    pio_gpio_init(pio,rx_gpio);
    gpio_pull_up(rx_gpio);
    pio_sm_set_consecutive_pindirs(pio,sm,rx_gpio,1,false);
    pio_sm_set_enabled(pio,sm,false);
    pio_sm_clear_fifos(pio,sm);
    pio_sm_init(pio,sm,(uint)offset,&config);

    rx->manager =manager;
    rx->pio=pio;
    rx->pio_index=pio_index;
    rx->sm=sm;
    rx->offset=offset;
    rx->rx_gpio=rx_gpio;
    rx->baudrate=baudrate;
    pio_sm_set_enabled(pio,sm,true);
    rx->opened=true;

    return true;
}

bool fun_pio_uart_rx_is_readable(const fun_pio_uart_rx_t *rx)
{
    if(rx==NULL||!rx->opened||rx->pio==NULL)
        return false;
    return !pio_sm_is_rx_fifo_empty(rx->pio,rx->sm);
}

bool fun_pio_uart_rx_try_getc(fun_pio_uart_rx_t *rx,uint8_t *value)
{
    if(value==NULL||!fun_pio_uart_rx_is_readable(rx))
    return false;
    uint32_t raw = pio_sm_get(rx->pio,rx->sm);
    *value =(uint8_t)(raw >> 24);
    return true;
}

size_t fun_pio_uart_rx_read(fun_pio_uart_rx_t *rx,uint8_t *data,size_t len)
{
    if(rx==NULL||data==NULL) return 0;
    size_t read_count=0;
    while(read_count<len){
        if(!fun_pio_uart_rx_try_getc(rx,&data[read_count]))
            break;
        ++read_count;
    }
    return read_count;
}

bool fun_pio_uart_rx_close(fun_pio_uart_rx_t *rx)
{
    if(rx==NULL||!rx->opened||rx->manager==NULL||rx->pio==NULL) return false;
    pio_sm_set_enabled(rx->pio,rx->sm,false);
    pio_sm_clear_fifos(rx->pio,rx->sm);
    bool ret_pro = pio_manager_release_program(rx->manager,rx->pio_index,FUN_PIO_UART_RX_NAME);
    bool ret_sm = pio_manager_release_sm(rx->manager,rx->pio_index,rx->sm);
    gpio_disable_pulls(rx->rx_gpio);
    gpio_set_function(rx->rx_gpio,GPIO_FUNC_SIO);
    gpio_set_dir(rx->rx_gpio,GPIO_IN);
    fun_pio_uart_rx_init(rx);
    return ret_pro&&ret_sm;

}

void fun_pio_uart_init(fun_pio_uart_t *uart)
{
    if(uart==NULL)
    return ;
    uart->opened=false;
    fun_pio_uart_rx_init(&uart->rx);
    fun_pio_uart_tx_init(&uart->tx);
}

bool fun_pio_uart_open(fun_pio_uart_t *uart,pio_manager_t *manager,uint8_t pio_index,uint8_t tx_gpio,uint8_t rx_gpio,uint32_t baudrate)
{
    if(uart==NULL||manager==NULL||uart->opened||pio_index>=PIO_BLOCK_COUNT||tx_gpio>=NUM_BANK0_GPIOS||rx_gpio>=NUM_BANK0_GPIOS||baudrate==0||rx_gpio==tx_gpio)
        return false;
    bool ret_tx=fun_pio_uart_tx_open(&uart->tx,manager,pio_index,tx_gpio,baudrate);
    if(!ret_tx) {
        return false;
    } 
    bool ret_rx=fun_pio_uart_rx_open(&uart->rx,manager,pio_index,rx_gpio,baudrate);
    if(!ret_rx)  {
        fun_pio_uart_tx_close(&uart->tx);
        return false;
    } 
    uart->opened=true;
    return true;
}

bool fun_pio_uart_close(fun_pio_uart_t *uart)
{
    if (uart == NULL || !uart->opened) 
        return false;
    bool rx_closed = fun_pio_uart_rx_close(&uart->rx);
    bool tx_closed = fun_pio_uart_tx_close(&uart->tx);
    fun_pio_uart_init(uart);
    return rx_closed && tx_closed;
}

bool fun_pio_uart_putc(fun_pio_uart_t *uart,uint8_t value)
{
    if(uart==NULL||!uart->opened)
    return false;
    return fun_pio_uart_tx_putc(&uart->tx,value);
}
bool fun_pio_uart_try_putc(fun_pio_uart_t *uart,uint8_t value)
{
    if(uart==NULL||!uart->opened)
    return false;
    return fun_pio_uart_tx_try_putc(&uart->tx,value);
}

size_t fun_pio_uart_write(fun_pio_uart_t *uart,const uint8_t *data,size_t len)
{
    if(uart==NULL||!uart->opened||data==NULL) return 0;
    return fun_pio_uart_tx_write(&uart->tx,data,len);
}
size_t fun_pio_uart_write_nonblocking(fun_pio_uart_t *uart,const uint8_t *data,size_t len)
{
    if(uart==NULL||!uart->opened||data==NULL) return 0;
    size_t written = 0;
    while(written<len){
        if(!fun_pio_uart_try_putc(uart,data[written]))
        break;
        ++written;
    }
    return written;
}

bool fun_pio_uart_is_readable(const fun_pio_uart_t *uart)
{
    if(uart==NULL||!uart->opened) return false;
    return fun_pio_uart_rx_is_readable(&uart->rx);
}

bool fun_pio_uart_try_getc(fun_pio_uart_t *uart,uint8_t *value)
{
    if(uart==NULL||!uart->opened||value==NULL) return false;
    return fun_pio_uart_rx_try_getc(&uart->rx,value);

}
size_t fun_pio_uart_read(fun_pio_uart_t *uart,uint8_t *data,size_t len)
{
    if(uart==NULL||!uart->opened||data==NULL)
    return 0;
    return fun_pio_uart_rx_read(&uart->rx,data,len);
}
bool fun_pio_uart_is_open(const fun_pio_uart_t *uart)
{
    return uart!=NULL&&uart->opened;
}

bool fun_pio_uart_is_writable(const fun_pio_uart_t *uart)
{
    if(uart==NULL||!uart->opened)
    return false;
    return fun_pio_uart_tx_is_writable(&uart->tx);
}

bool fun_pio_uart_can_open_on(const pio_manager_t *manager,uint8_t pio_index)
{
    if(manager==NULL||pio_index>=PIO_BLOCK_COUNT) return false;
    if(pio_manager_get_free_sm_count(manager,pio_index)<2) return false;
     if (!pio_manager_can_use_program( manager,pio_index,FUN_PIO_UART_TX_NAME,&uart_tx_program)) {
        return false;
    }
    if (!pio_manager_can_use_program(manager,pio_index, FUN_PIO_UART_RX_NAME, &uart_rx_program)) {
        return false;
    }
    return true;
}
int fun_pio_uart_select_pio(const pio_manager_t *manager)
{
     if (manager == NULL) 
        return -1;
    int best_pio = -1;
    uint8_t best_reuse_score = 0;
    uint8_t best_free_sm = 0;

    for (uint8_t pio_index = 0; pio_index < PIO_BLOCK_COUNT;++pio_index) {
        if (!fun_pio_uart_can_open_on( manager,pio_index)) 
            continue;
        uint8_t reuse_score = 0;
        if (pio_manager_find_program(manager,pio_index,FUN_PIO_UART_TX_NAME) >= 0) 
            ++reuse_score;
        if (pio_manager_find_program(manager,pio_index,FUN_PIO_UART_RX_NAME) >= 0) 
            ++reuse_score;
        uint8_t free_sm =pio_manager_get_free_sm_count(manager,pio_index);
        if (best_pio < 0 ||reuse_score > best_reuse_score ||(reuse_score == best_reuse_score &&free_sm > best_free_sm)) {
            best_pio = (int)pio_index;
            best_reuse_score = reuse_score;
            best_free_sm = free_sm;
        }
    }

    return best_pio;
}

bool fun_pio_uart_open_auto(fun_pio_uart_t *uart,pio_manager_t *manager,uint8_t tx_gpio,uint8_t rx_gpio,uint32_t baudrate,uint8_t *selected_pio)
{
    if (uart == NULL || manager == NULL || uart->opened) 
        return false;
    int preferred = fun_pio_uart_select_pio(manager);
    if (preferred < 0) {
        return false;
    }

    uint8_t candidates[PIO_BLOCK_COUNT] = {
        (uint8_t)preferred,
        (uint8_t)(preferred == 0 ? 1 : 0)
    };

    for (uint8_t i = 0;i < PIO_BLOCK_COUNT; ++i) {
        uint8_t pio_index = candidates[i];
        if (!fun_pio_uart_can_open_on(manager,pio_index)) {
            continue;
        }
        if (fun_pio_uart_open( uart, manager, pio_index, tx_gpio, rx_gpio,baudrate)) {
            if (selected_pio != NULL) {
                *selected_pio = pio_index;
            }
            return true;
        }
    }

    return false;
}