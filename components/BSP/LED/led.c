#include "led.h"
#include "led_strip.h"
#include "esp_log.h"

#define TAG "LED"

static led_strip_handle_t led_strip;

void led_init(void)
{
    ESP_LOGI(TAG, "Initializing WS2812 LED on GPIO %d", LED_GPIO_PIN);

    /* LED strip initialization with the GPIO and pixel number */
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO_PIN,
        .max_leds = 1, // 开发板上通常只有 1 颗 RGB 灯
        // .led_pixel_format = LED_PIXEL_FORMAT_GRB, // WS2812B 常用格式 (旧版本库支持，新版本可能已移除或默认)
        // .led_model = LED_MODEL_WS2812, // LED 型号 
        .flags.invert_out = false, // 是否反相
    };
    
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT, // 使用默认时钟
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false, // 不需要DMA
    };
    
    // 创建 RMT 后端的 LED Strip 对象
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    
    // 初始化后关闭 (清空)
    led_strip_clear(led_strip);
}

void led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (led_strip) {
        // 亮度降至10%
        uint8_t r10 = r / 10;
        uint8_t g10 = g / 10;
        uint8_t b10 = b / 10;
        led_strip_set_pixel(led_strip, 0, r10, g10, b10);
        led_strip_refresh(led_strip);
    }
}
