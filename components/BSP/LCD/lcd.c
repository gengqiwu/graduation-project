#include "lcd.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "LCD";

/* ========== ST7789 命令定义 ========== */
#define ST7789_SWRESET      0x01
#define ST7789_SLPOUT       0x11
#define ST7789_NORON        0x13
#define ST7789_INVON        0x21
#define ST7789_DISPON       0x29
#define ST7789_CASET        0x2A
#define ST7789_RASET        0x2B
#define ST7789_RAMWR        0x2C
#define ST7789_COLMOD       0x3A
#define ST7789_MADCTL       0x36
#define ST7789_PORCTRL      0xB2
#define ST7789_GCTRL        0xB7
#define ST7789_VCOMS        0xBB
#define ST7789_LCMCTRL      0xC0
#define ST7789_VDVVRHEN     0xC2
#define ST7789_VRHS         0xC3
#define ST7789_VDVS         0xC4
#define ST7789_FRCTRL2      0xC6
#define ST7789_PWCTRL1      0xD0
#define ST7789_PVGAMCTRL    0xE0
#define ST7789_NVGAMCTRL    0xE1

static spi_device_handle_t s_spi = NULL;
/* 分块 DMA 缓冲区：每次传送 LCD_DMA_CHUNK_LINES 行，必须位于内部 DRAM
 * 保持 8 行 (3840 B) 以节省内部 DMA 池给 mbedTLS/AES 使用                */
#define LCD_DMA_CHUNK_LINES  8
#define LCD_DMA_CHUNK_BYTES  (LCD_H_RES * LCD_DMA_CHUNK_LINES * 2)   // 240*8*2 = 3840 B
static uint8_t *s_dma_buf = NULL;

/* ========== 底层 SPI 读写 ========== */
static void lcd_cmd(uint8_t cmd)
{
    gpio_set_level(LCD_PIN_DC, 0);  // DC=0: 命令
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    spi_device_polling_transmit(s_spi, &t);
}

static void lcd_data(const uint8_t *data, size_t len)
{
    if (len == 0) return;
    gpio_set_level(LCD_PIN_DC, 1);  // DC=1: 数据
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(s_spi, &t);
}

static void lcd_data1(uint8_t d)
{
    lcd_data(&d, 1);
}

/* ========== 设置绘制窗口 ========== */
static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t buf[4];

    lcd_cmd(ST7789_CASET);
    buf[0] = x0 >> 8; buf[1] = x0 & 0xFF;
    buf[2] = x1 >> 8; buf[3] = x1 & 0xFF;
    lcd_data(buf, 4);

    lcd_cmd(ST7789_RASET);
    buf[0] = y0 >> 8; buf[1] = y0 & 0xFF;
    buf[2] = y1 >> 8; buf[3] = y1 & 0xFF;
    lcd_data(buf, 4);

    lcd_cmd(ST7789_RAMWR);
}

/* ========== 初始化 ST7789 ========== */
static void st7789_init_sequence(void)
{
    /* 硬复位 */
    gpio_set_level(LCD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_cmd(ST7789_SWRESET);            // 软复位
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_cmd(ST7789_SLPOUT);             // 退出睡眠
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 颜色格式：16bit RGB565 */
    lcd_cmd(ST7789_COLMOD);
    lcd_data1(0x55);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 内存访问控制：BGR 模式关闭（RGB），行列正常方向 */
    lcd_cmd(ST7789_MADCTL);
    lcd_data1(0x00);

    /* Porch 控制（前/后廊+空闲模式） */
    lcd_cmd(ST7789_PORCTRL);
    { uint8_t d[] = {0x0C, 0x0C, 0x00, 0x33, 0x33}; lcd_data(d, 5); }

    /* 门控 */
    lcd_cmd(ST7789_GCTRL);
    lcd_data1(0x35);

    /* VCOM 设置 */
    lcd_cmd(ST7789_VCOMS);
    lcd_data1(0x19);

    /* LCM 控制 */
    lcd_cmd(ST7789_LCMCTRL);
    lcd_data1(0x2C);

    /* VDV/VRH 使能 */
    lcd_cmd(ST7789_VDVVRHEN);
    lcd_data1(0x01);

    /* VRH 设置 */
    lcd_cmd(ST7789_VRHS);
    lcd_data1(0x12);

    /* VDV 设置 */
    lcd_cmd(ST7789_VDVS);
    lcd_data1(0x20);

    /* 帧率控制：60Hz */
    lcd_cmd(ST7789_FRCTRL2);
    lcd_data1(0x0F);

    /* 电源控制 1 */
    lcd_cmd(ST7789_PWCTRL1);
    { uint8_t d[] = {0xA4, 0xA1}; lcd_data(d, 2); }

    /* 正 Gamma 校正 */
    lcd_cmd(ST7789_PVGAMCTRL);
    { uint8_t d[] = {0xD0,0x04,0x0D,0x11,0x13,0x2B,0x3F,0x54,
                     0x4C,0x18,0x0D,0x0B,0x1F,0x23}; lcd_data(d, 14); }

    /* 负 Gamma 校正 */
    lcd_cmd(ST7789_NVGAMCTRL);
    { uint8_t d[] = {0xD0,0x04,0x0C,0x11,0x13,0x2C,0x3F,0x44,
                     0x51,0x2F,0x1F,0x1F,0x20,0x23}; lcd_data(d, 14); }

    /* 显示区域：0,0 ~ 239,239 */
    lcd_cmd(ST7789_CASET);
    { uint8_t d[] = {0x00,0x00,0x00,0xEF}; lcd_data(d, 4); }

    lcd_cmd(ST7789_RASET);
    { uint8_t d[] = {0x00,0x00,0x00,0xEF}; lcd_data(d, 4); }

    lcd_cmd(ST7789_INVON);              // 反色（大部分 1.54" ST7789 模组需要）
    vTaskDelay(pdMS_TO_TICKS(10));

    lcd_cmd(ST7789_NORON);              // 正常显示模式
    vTaskDelay(pdMS_TO_TICKS(10));

    lcd_cmd(ST7789_DISPON);             // 开启显示
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* ========== 公开接口 ========== */

esp_err_t lcd_init(void)
{
    /* 1. 配置背光和 RST 为 GPIO 输出 */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LCD_PIN_RST) | (1ULL << LCD_PIN_BLK) | (1ULL << LCD_PIN_DC),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(LCD_PIN_BLK, 0);   // 先关背光
    gpio_set_level(LCD_PIN_RST, 1);

    /* 2. 初始化 SPI 总线 */
    spi_bus_config_t buscfg = {
        .sclk_io_num     = LCD_PIN_SCK,
        .mosi_io_num     = LCD_PIN_MOSI,
        .miso_io_num     = -1,            // 显示屏无需回读
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_DMA_CHUNK_BYTES + 64,  // 分块传输，只需一个块的大小
    };
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI 总线初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 3. 添加 ST7789 设备到 SPI 总线 */
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = LCD_SPI_CLOCK_HZ,
        .mode           = 0,               // SPI mode 0
        .spics_io_num   = LCD_PIN_CS,
        .queue_size     = 7,
        .pre_cb         = NULL,
    };
    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "添加 SPI 设备失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 4. 发送初始化序列 */
    st7789_init_sequence();

    /* 5. 预分配 DMA 分块缓冲区（必须在内部 DRAM，PSRAM 不能做 SPI DMA 源） */
    s_dma_buf = heap_caps_malloc(LCD_DMA_CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_dma_buf) {
        ESP_LOGE(TAG, "无法分配 DMA 缓冲区 (%d bytes)", LCD_DMA_CHUNK_BYTES);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "DMA 分块缓冲区已分配: %d bytes/块，共 %d 块",
             LCD_DMA_CHUNK_BYTES, (LCD_H_RES * LCD_V_RES * 2 + LCD_DMA_CHUNK_BYTES - 1) / LCD_DMA_CHUNK_BYTES);

    /* 6. 开启背光 */
    gpio_set_level(LCD_PIN_BLK, 1);

    ESP_LOGI(TAG, "ST7789 240x240 LCD 初始化成功");
    return ESP_OK;
}

void lcd_draw_frame(const uint16_t *data)
{
    if (!data || !s_dma_buf) return;

    /* 设置全屏写入窗口 */
    lcd_set_window(0, 0, LCD_H_RES - 1, LCD_V_RES - 1);

    /*
     * OV2640 通过 DVP 输出的 RGB565 高字节已在前（big-endian），
     * ST7789 SPI 同样需要高字节在前，无需字节交换，直接分块发送即可。
     */
    size_t total_bytes = LCD_H_RES * LCD_V_RES * 2;
    const uint8_t *src = (const uint8_t *)data;
    size_t offset = 0;

    gpio_set_level(LCD_PIN_DC, 1);
    while (offset < total_bytes) {
        size_t chunk = total_bytes - offset;
        if (chunk > LCD_DMA_CHUNK_BYTES) chunk = LCD_DMA_CHUNK_BYTES;

        /* 直接复制到 DMA 缓冲区（无字节交换） */
        memcpy(s_dma_buf, src + offset, chunk);

        spi_transaction_t t = {
            .length    = chunk * 8,
            .tx_buffer = s_dma_buf,
        };
        spi_device_polling_transmit(s_spi, &t);
        offset += chunk;
    }
}

void lcd_set_backlight(bool on)
{
    gpio_set_level(LCD_PIN_BLK, on ? 1 : 0);
}
