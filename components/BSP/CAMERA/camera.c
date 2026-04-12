#include "camera.h"
#include "esp_log.h"
#include "esp_camera.h"

static const char *TAG = "CAMERA";

esp_err_t camera_init(void)
{
    /* 摄像头配置 */
    camera_config_t config = {
        .pin_pwdn       = CAMERA_PIN_PWDN,
        .pin_reset      = CAMERA_PIN_RESET,
        .pin_xclk       = CAMERA_PIN_XCLK,
        .pin_sccb_sda   = CAMERA_PIN_SIOD,
        .pin_sccb_scl   = CAMERA_PIN_SIOC,

        .pin_d7         = CAMERA_PIN_D7,
        .pin_d6         = CAMERA_PIN_D6,
        .pin_d5         = CAMERA_PIN_D5,
        .pin_d4         = CAMERA_PIN_D4,
        .pin_d3         = CAMERA_PIN_D3,
        .pin_d2         = CAMERA_PIN_D2,
        .pin_d1         = CAMERA_PIN_D1,
        .pin_d0         = CAMERA_PIN_D0,

        .pin_vsync      = CAMERA_PIN_VSYNC,
        .pin_href       = CAMERA_PIN_HREF,
        .pin_pclk       = CAMERA_PIN_PCLK,

        .xclk_freq_hz   = CAMERA_XCLK_FREQ_HZ,
        .ledc_timer     = LEDC_TIMER_0,
        .ledc_channel   = LEDC_CHANNEL_0,

        /* 输出 RGB565 格式，与 ST7789 屏幕原生格式一致，无需转换 */
        .pixel_format   = PIXFORMAT_RGB565,
        /* FRAMESIZE_240X240 需要 esp32-camera 组件支持 */
        .frame_size     = FRAMESIZE_240X240,

        .jpeg_quality   = 12,   // 仅 JPEG 格式时有效
        .fb_count       = 2,    // 双缓冲，减少撕裂
        .fb_location    = CAMERA_FB_IN_PSRAM,  // 帧缓冲放入 PSRAM
        .grab_mode      = CAMERA_GRAB_LATEST,  // 始终获取最新帧，丢弃未及时处理的旧帧
    };

    esp_err_t ret = esp_camera_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "摄像头初始化失败: %s (0x%x)", esp_err_to_name(ret), ret);
        return ret;
    }

    /* 调整摄像头传感器参数以获得更好画质 */
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_vflip(s, 0);         // 垂直翻转（0=正常，1=翻转，根据安装方向调整）
        s->set_hmirror(s, 0);       // 水平镜像（0=正常，1=镜像，根据安装方向调整）
        s->set_brightness(s, 0);    // 亮度 -2~2，0=中性（过亮会压缩动态范围）
        s->set_contrast(s, 0);      // 对比度 -2~2，0=中性（减少噪声放大）
        s->set_saturation(s, 0);    // 饱和度 -2~2，0=中性（高饱和会让噪点颜色更鲜艳显眼）
        s->set_sharpness(s, 0);     // 锐度 0~2，0=关闭（锐化会增强噪声边缘）
        if (s->set_denoise) s->set_denoise(s, 1); // 降噪（OV2640 部分版本不支持，安全检查）
        s->set_awb_gain(s, 1);      // 自动白平衡增益（开启）
        s->set_whitebal(s, 1);      // 自动白平衡（开启）
        s->set_wb_mode(s, 0);       // 白平衡模式：0=自动
        s->set_gain_ctrl(s, 1);     // 自动增益控制（开启）
        s->set_agc_gain(s, 0);      // AGC 基准增益
        s->set_exposure_ctrl(s, 1); // 自动曝光控制（开启）
        s->set_aec2(s, 1);          // AEC2（DSP 曝光算法，更稳定）
        s->set_ae_level(s, 0);      // 曝光等级补偿 -2~2
        s->set_bpc(s, 1);           // 坏点校正
        s->set_wpc(s, 1);           // 白点校正
        s->set_raw_gma(s, 1);       // RAW Gamma 使能
        s->set_lenc(s, 1);          // 镜头畸变补偿（改善边缘亮度均匀性）
        ESP_LOGI(TAG, "传感器参数配置完成");
    }

    ESP_LOGI(TAG, "OV2640 摄像头初始化成功，分辨率 240x240，格式 RGB565");
    return ESP_OK;
}

void camera_deinit(void)
{
    esp_camera_deinit();
    ESP_LOGI(TAG, "摄像头已释放");
}
