#pragma once

#include "esp_err.h"
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief OV2640 摄像头引脚定义（ESP32-S3-CAM 标准接法）
 *        如与实际接线不符，请修改下方宏定义。
 */
#define CAMERA_PIN_PWDN    (-1)
#define CAMERA_PIN_RESET   (-1)
#define CAMERA_PIN_XCLK    15
#define CAMERA_PIN_SIOD    4       // I2C SDA
#define CAMERA_PIN_SIOC    5       // I2C SCL
#define CAMERA_PIN_D7      16
#define CAMERA_PIN_D6      17
#define CAMERA_PIN_D5      18
#define CAMERA_PIN_D4      12
#define CAMERA_PIN_D3      10
#define CAMERA_PIN_D2      8
#define CAMERA_PIN_D1      9
#define CAMERA_PIN_D0      11
#define CAMERA_PIN_VSYNC   6
#define CAMERA_PIN_HREF    7
#define CAMERA_PIN_PCLK    13

#define CAMERA_XCLK_FREQ_HZ  (20 * 1000 * 1000)  // 20 MHz

/**
 * @brief 初始化 OV2640 摄像头，输出格式 RGB565，分辨率 240x240
 * @return ESP_OK 成功
 */
esp_err_t camera_init(void);

/**
 * @brief 反初始化摄像头，释放资源
 */
void camera_deinit(void);

#ifdef __cplusplus
}
#endif
