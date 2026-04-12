#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LCD SPI 接线定义
 *   GND  -> GND
 *   3.3V -> VCC
 *   GPIO19 -> SCL  (SPI时钟)
 *   GPIO20 -> SDA  (SPI数据/MOSI)
 *   GPIO21 -> RES  (复位)
 *   GPIO47 -> DC   (数据/命令选择)
 *   GPIO45 -> CS   (片选)
 *   GPIO38 -> BLK  (背光)
 */
#define LCD_PIN_SCK   19
#define LCD_PIN_MOSI  20
#define LCD_PIN_RST   21
#define LCD_PIN_DC    47
#define LCD_PIN_CS    45
#define LCD_PIN_BLK   38

#define LCD_H_RES     240
#define LCD_V_RES     240
#define LCD_BITS_PER_PIXEL  16   // RGB565

/* SPI 总线频率：40 MHz 对杜邦线/跳线最稳定；如走短 PCB 走线可尝试 60~80 MHz */
#define LCD_SPI_CLOCK_HZ  (40 * 1000 * 1000)

/**
 * @brief 初始化 ST7789 SPI LCD 屏幕
 * @return ESP_OK 成功
 */
esp_err_t lcd_init(void);

/**
 * @brief 将一帧 RGB565 数据绘制到屏幕
 * @param data  RGB565 格式像素数组，大小需为 LCD_H_RES * LCD_V_RES * 2 字节
 */
void lcd_draw_frame(const uint16_t *data);

/**
 * @brief 控制背光开关
 * @param on  true 开背光，false 关背光
 */
void lcd_set_backlight(bool on);

#ifdef __cplusplus
}
#endif
