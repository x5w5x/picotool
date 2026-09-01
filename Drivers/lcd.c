#include "lcd.h"
#include "board_pins.h"
#include "spi_bus.h"

#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "pico/time.h"
#include "pico/platform.h"

#include <stdlib.h>
#include <string.h>

#define ST7796_SWRESET   0x01
#define ST7796_SLPOUT    0x11
#define ST7796_INVON     0x21
#define ST7796_DISPON    0x29
#define ST7796_CASET     0x2A
#define ST7796_RASET     0x2B
#define ST7796_RAMWR     0x2C
#define ST7796_MADCTL    0x36
#define ST7796_COLMOD    0x3A

#define LCD_SPI          spi1
#define LCD_SPI_BAUD     SPI_BUS_BAUD_HIGH 

#define UI_TILE_H        32 //切片高度

#define LCD_CS_LOW()     gpio_put(PIN_LCD_CS, 0)
#define LCD_CS_HIGH()    gpio_put(PIN_LCD_CS, 1)
#define LCD_DC_LOW()     gpio_put(PIN_LCD_DC, 0)
#define LCD_DC_HIGH()    gpio_put(PIN_LCD_DC, 1)
#define LCD_RST_LOW()    gpio_put(PIN_LCD_RST, 0)
#define LCD_RST_HIGH()   gpio_put(PIN_LCD_RST, 1)

static int lcd_dma_chan = -1;  //DMA通道号
static dma_channel_config dma_cfg; 
static inline void LCD_WaitDMA_Auto(void)
{
    if (lcd_dma_chan >= 0 && dma_channel_is_busy(lcd_dma_chan)) {
        dma_channel_wait_for_finish_blocking(lcd_dma_chan);
    }
    while (spi_is_busy(LCD_SPI)) {
        tight_loop_contents();
    }
    while (spi_is_readable(LCD_SPI)) {
        (void)spi_get_hw(LCD_SPI)->dr;
    }
}

void LCD_WaitDMA(void)
{
    LCD_WaitDMA_Auto();
    LCD_CS_HIGH();
}

bool LCD_IsDMABusy(void)
{
    if (lcd_dma_chan >= 0) {
        return dma_channel_is_busy(lcd_dma_chan) || spi_is_busy(LCD_SPI);
    }
    return spi_is_busy(LCD_SPI);
}

static void LCD_WriteCmdData(uint8_t cmd, const uint8_t *data, size_t len)
{
    LCD_WaitDMA_Auto();
    LCD_DC_LOW();
    LCD_CS_LOW();
    spi_write_blocking(LCD_SPI, &cmd, 1);
    if (len > 0 && data != NULL) {
        LCD_DC_HIGH();
        spi_write_blocking(LCD_SPI, data, len);
    }
    LCD_CS_HIGH();
}

static inline void LCD_WriteCmd(uint8_t cmd) { LCD_WriteCmdData(cmd, NULL, 0); }

static inline void LCD_WriteData16(uint16_t data)
{
    uint8_t buf[2] = { (uint8_t)(data >> 8), (uint8_t)(data & 0xFF) };
    LCD_WaitDMA_Auto();
    LCD_DC_HIGH();
    LCD_CS_LOW();
    spi_write_blocking(LCD_SPI, buf, 2);
    LCD_CS_HIGH();
}

void LCD_BulkWriteDMA(const uint8_t *buf, uint32_t len)
{
    if (len == 0) return;
    LCD_WaitDMA_Auto();
    LCD_DC_HIGH();
    LCD_CS_LOW();
    dma_channel_set_read_addr(lcd_dma_chan, buf, false);
    dma_channel_set_trans_count(lcd_dma_chan, len, true);
}

void LCD_BulkWrite(const uint8_t *buf, uint32_t len)
{
    LCD_BulkWriteDMA(buf, len);
    LCD_WaitDMA();
}

void LCD_PushTileDMA_Async(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t *buf)
{
    if (w == 0 || h == 0 || !buf) return;
    
    uint32_t count = (uint32_t)w * h;
    if (count > LCD_WIDTH * UI_TILE_H) count = LCD_WIDTH * UI_TILE_H;

    LCD_WaitDMA();
    // ⚡ 核心优化 2：利用 GCC 内建 rev16 硬件单指令加速大小端翻转
    for (uint32_t i = 0; i < count; i++) {

         buf[i] = __builtin_bswap16(buf[i]);
    }

    LCD_SetWindow(x, y, x + w - 1, y + h - 1);
    LCD_DC_HIGH();
    LCD_CS_LOW();
    dma_channel_set_read_addr(lcd_dma_chan, buf, false);
    dma_channel_set_trans_count(lcd_dma_chan, count * 2, true);
}

void LCD_PushSubTile(uint16_t screen_x, uint16_t screen_y, 
                     uint16_t sub_w, uint16_t sub_h, 
                     const uint16_t *tile_buf, uint16_t tile_stride,
                     uint16_t src_offset_x, uint16_t src_offset_y)
{
   if (sub_w == 0 || sub_h == 0 || !tile_buf) return;

    // 边界裁剪[cite: 4]
    if (screen_x >= LCD_WIDTH || screen_y >= LCD_HEIGHT) return;
    if (screen_x + sub_w > LCD_WIDTH)  sub_w = LCD_WIDTH - screen_x;
    if (screen_y + sub_h > LCD_HEIGHT) sub_h = LCD_HEIGHT - screen_y;

    LCD_WaitDMA();

    // 1. 原地逐行完成大小端翻转
    for (uint16_t row = 0; row < sub_h; row++) {
        uint16_t *src = tile_buf + (src_offset_y + row) * tile_stride + src_offset_x;
        for (uint16_t col = 0; col < sub_w; col++) {
            src[col] = __builtin_bswap16(src[col]);
        }
    }

    // 2. 设置屏幕目标窗口
    LCD_SetWindow(screen_x, screen_y, screen_x + sub_w - 1, screen_y + sub_h - 1);
    LCD_DC_HIGH();
    LCD_CS_LOW();

    // 3. 逐行使用 DMA 发送（正确处理 tile_stride 内存跨度）
    for (uint16_t row = 0; row < sub_h; row++) {
        uint16_t *src = tile_buf + (src_offset_y + row) * tile_stride + src_offset_x;
        dma_channel_set_read_addr(lcd_dma_chan, src, false);
        dma_channel_set_trans_count(lcd_dma_chan, sub_w * 2, true);
        LCD_WaitDMA_Auto();
    }

    LCD_CS_HIGH();
}

static void LCD_GPIO_Init(void)
{
    gpio_init(PIN_W25_CS); gpio_set_dir(PIN_W25_CS, GPIO_OUT); gpio_put(PIN_W25_CS, 1);
    gpio_init(PIN_TF_CS);  gpio_set_dir(PIN_TF_CS,  GPIO_OUT); gpio_put(PIN_TF_CS,  1);

    gpio_set_function(PIN_SPI_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_MISO, GPIO_FUNC_SPI);
    gpio_pull_up(PIN_SPI_MISO);

    gpio_init(PIN_LCD_CS);  gpio_set_dir(PIN_LCD_CS, GPIO_OUT);
    gpio_init(PIN_LCD_DC);  gpio_set_dir(PIN_LCD_DC, GPIO_OUT);
    gpio_init(PIN_LCD_RST); gpio_set_dir(PIN_LCD_RST, GPIO_OUT);

    LCD_CS_HIGH(); LCD_DC_HIGH(); LCD_RST_HIGH();
}

void LCD_Init(void)
{
    LCD_GPIO_Init();
    spi_init(LCD_SPI, LCD_SPI_BAUD);
    spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    if (lcd_dma_chan < 0) {
        lcd_dma_chan = dma_claim_unused_channel(true);
        dma_cfg = dma_channel_get_default_config(lcd_dma_chan);
        channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_8);
        channel_config_set_dreq(&dma_cfg, spi_get_dreq(LCD_SPI, true));
        channel_config_set_read_increment(&dma_cfg, true);
        channel_config_set_write_increment(&dma_cfg, false);

        dma_channel_configure(lcd_dma_chan, &dma_cfg, &spi_get_hw(LCD_SPI)->dr, NULL, 0, false);
    }

    LCD_RST_LOW(); sleep_ms(20); LCD_RST_HIGH(); sleep_ms(120);
    LCD_WriteCmd(ST7796_SWRESET); sleep_ms(120);
    LCD_WriteCmd(ST7796_SLPOUT);  sleep_ms(100);

    const uint8_t madctl = 0xE8; LCD_WriteCmdData(ST7796_MADCTL, &madctl, 1);  //显示方向
    const uint8_t colmod = 0x55; LCD_WriteCmdData(ST7796_COLMOD, &colmod, 1);  //像素格式
    const uint8_t pwr1 = 0x23;   LCD_WriteCmdData(0xC0, &pwr1, 1);
    const uint8_t pwr2 = 0x10;   LCD_WriteCmdData(0xC1, &pwr2, 1);
    const uint8_t vcom[2] = {0x3E, 0x28}; LCD_WriteCmdData(0xC5, vcom, 2);

    LCD_WriteCmd(ST7796_INVON); //开启反色
    LCD_WriteCmd(ST7796_DISPON); //开启显示
    sleep_ms(20);
}

void LCD_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    if (x0 >= LCD_WIDTH)  x0 = LCD_WIDTH - 1;
    if (x1 >= LCD_WIDTH)  x1 = LCD_WIDTH - 1;
    if (y0 >= LCD_HEIGHT) y0 = LCD_HEIGHT - 1;
    if (y1 >= LCD_HEIGHT) y1 = LCD_HEIGHT - 1;

    uint8_t caset[4] = { (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF), (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF) };
    uint8_t raset[4] = { (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF), (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF) };

    LCD_WriteCmdData(ST7796_CASET, caset, 4);
    LCD_WriteCmdData(ST7796_RASET, raset, 4);
    LCD_WriteCmd(ST7796_RAMWR);
}

void LCD_Clear(uint16_t color)
{
    LCD_FillRect(0, 0, LCD_WIDTH, LCD_HEIGHT, color);
}

void LCD_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT || w == 0 || h == 0) return;
    if (x + w > LCD_WIDTH)  w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;

    LCD_SetWindow(x, y, x + w - 1, y + h - 1);

    uint8_t line_buf[w * 2];
    uint8_t hi = (uint8_t)(color >> 8);
    uint8_t lo = (uint8_t)(color & 0xFF);
    for (uint16_t i = 0; i < w; i++) {
        line_buf[2 * i]     = hi;
        line_buf[2 * i + 1] = lo;
    }

    for (uint16_t row = 0; row < h; row++) {
        LCD_BulkWriteDMA(line_buf, w * 2);
        LCD_WaitDMA_Auto();
    }
    LCD_CS_HIGH();
}

void LCD_DrawFastHLine(uint16_t x, uint16_t y, uint16_t w, uint16_t color)
{
    LCD_FillRect(x, y, w, 1, color);
}

void LCD_DrawFastVLine(uint16_t x, uint16_t y, uint16_t h, uint16_t color)
{
    LCD_FillRect(x, y, 1, h, color);
}

void LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color)
{
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return;
    LCD_SetWindow(x, y, x, y);
    LCD_WriteData16(color);
}

void LCD_DrawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    if (y0 == y1) {
        if (x0 <= x1) LCD_DrawFastHLine(x0, y0, x1 - x0 + 1, color);
        else LCD_DrawFastHLine(x1, y0, x0 - x1 + 1, color);
        return;
    }
    if (x0 == x1) {
        if (y0 <= y1) LCD_DrawFastVLine(x0, y0, y1 - y0 + 1, color);
        else LCD_DrawFastVLine(x0, y1, y0 - y1 + 1, color);
        return;
    }

    int dx = abs((int)x1 - (int)x0);
    int dy = abs((int)y1 - (int)y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (1) {
        LCD_SetWindow(x0, y0, x0, y0);
        LCD_WriteData16(color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void LCD_DrawRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (w == 0 || h == 0) return;
    LCD_DrawFastHLine(x, y, w, color);
    LCD_DrawFastHLine(x, y + h - 1, w, color);
    LCD_DrawFastVLine(x, y, h, color);
    LCD_DrawFastVLine(x + w - 1, y, h, color);
}

void LCD_DrawCircle(uint16_t x0, uint16_t y0, uint16_t r, uint16_t color)
{
    int x = (int)r;
    int y = 0;
    int err = 0;

    while (x >= y) {
        LCD_DrawPoint(x0 + x, y0 + y, color);
        LCD_DrawPoint(x0 + y, y0 + x, color);
        LCD_DrawPoint(x0 - y, y0 + x, color);
        LCD_DrawPoint(x0 - x, y0 + y, color);
        LCD_DrawPoint(x0 - x, y0 - y, color);
        LCD_DrawPoint(x0 - y, y0 - x, color);
        LCD_DrawPoint(x0 + y, y0 - x, color);
        LCD_DrawPoint(x0 + x, y0 - y, color);

        y++;
        if (err <= 0) err += 2 * y + 1;
        if (err > 0)  { x--; err -= 2 * x + 1; }
    }
}

const uint8_t Font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14}, {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00}, {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02}, {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00}, {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06}, {0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x40,0x00},
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, {0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40}, {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00},
    {0x7F,0x10,0x28,0x44,0x00}, {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, {0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C}, {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44}
};

void LCD_DrawChar(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg, uint8_t size)
{
    char str[2] = { c, '\0' };
    LCD_DrawString(x, y, str, color, bg, size);
}

void LCD_DrawString(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg, uint8_t size)
{
    if (!str || *str == '\0' || y >= LCD_HEIGHT) return;

    size_t len = strlen(str);
    uint16_t total_w = len * 6 * size;
    uint16_t total_h = 8 * size;

    if (x + total_w > LCD_WIDTH) total_w = (x < LCD_WIDTH) ? (LCD_WIDTH - x) : 0;
    if (total_w == 0 || y + total_h > LCD_HEIGHT) return;

    LCD_SetWindow(x, y, x + total_w - 1, y + total_h - 1);

    uint8_t c_hi = (uint8_t)(color >> 8);
    uint8_t c_lo = (uint8_t)(color & 0xFF);
    uint8_t b_hi = (uint8_t)(bg >> 8);
    uint8_t b_lo = (uint8_t)(bg & 0xFF);

    uint8_t row_buf[total_w * 2];

    for (uint8_t row = 0; row < 8; row++) {
        uint32_t buf_idx = 0;
        for (size_t k = 0; k < len; k++) {
            char ch = str[k];
            if (ch < ' ' || ch > '~') ch = ' ';
            const uint8_t *p = Font5x7[ch - ' '];

            for (uint8_t col = 0; col < 5; col++) {
                bool is_fg = (p[col] & (1 << row)) != 0;
                uint8_t hi = is_fg ? c_hi : b_hi;
                uint8_t lo = is_fg ? c_lo : b_lo;
                for (uint8_t sx = 0; sx < size; sx++) {
                    if ((k * 6 * size + col * size + sx) >= total_w) break;
                    row_buf[buf_idx++] = hi;
                    row_buf[buf_idx++] = lo;
                }
            }
            for (uint8_t sx = 0; sx < size; sx++) {
                if ((k * 6 * size + 5 * size + sx) >= total_w) break;
                row_buf[buf_idx++] = b_hi;
                row_buf[buf_idx++] = b_lo;
            }
        }

        for (uint8_t sy = 0; sy < size; sy++) {
            LCD_BulkWriteDMA(row_buf, buf_idx);
            LCD_WaitDMA_Auto();
        }
    }
    LCD_CS_HIGH();
}