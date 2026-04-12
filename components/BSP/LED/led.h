#ifndef __LED_H__
#define __LED_H__

#include <stdint.h>
#include "driver/gpio.h"

// WS2812 RGB LED Pin
#define LED_GPIO_PIN GPIO_NUM_48

/**
 * @brief 初始化 WS2812 LED
 */
void led_init(void);

/**
 * @brief 设置 LED 颜色
 * @param r 红色分量 (0-255)
 * @param g 绿色分量 (0-255)
 * @param b 蓝色分量 (0-255)
 */
void led_set_color(uint8_t r, uint8_t g, uint8_t b);

#endif
