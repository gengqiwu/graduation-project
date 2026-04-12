#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "led.h"
#include "wifi.h"
#include "yuyin.h"
#include "ai_chat.h"
#include "speaker.h"
#include "xfyun_image.h"
#include "xfyun_tts.h"
#include "sdkconfig.h"
#include "esp_sntp.h"
#include <time.h>
#include "camera.h"
#include "lcd.h"

#define TAG "SR_MAIN"


// ===== 前向声明 =====
void ai_chat_status_callback(ai_chat_state_t state, const char *message);

static void sync_time_once(void)
{
    ESP_LOGI(TAG, "Starting time synchronization...");

    if (esp_sntp_enabled()) {
        // 已启用：检查时间是否已经有效
        time_t now = 0;
        time(&now);
        if (now > 1700000000) {
            ESP_LOGI(TAG, "SNTP already enabled & synced: %ld", (long)now);
            return;
        }
        ESP_LOGI(TAG, "SNTP enabled but not synced yet, waiting...");
    } else {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        // 优先使用国内 NTP 服务器，提高成功率
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_setservername(1, "ntp.ntsc.ac.cn");
        esp_sntp_setservername(2, "pool.ntp.org");
        esp_sntp_init();
        ESP_LOGI(TAG, "SNTP initialized (servers: aliyun / ntsc / pool.ntp.org)");
    }

    // 等待时间同步，最多 15 秒（150 × 100ms）
    for (int i = 0; i < 150; ++i) {
        time_t now = 0;
        time(&now);
        if (now > 1700000000) {
            struct tm t;
            localtime_r(&now, &t);
            ESP_LOGI(TAG, "Time synced: %04d-%02d-%02d %02d:%02d:%02d UTC+0",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec);
            return;
        }
        if (i % 20 == 19) {
            ESP_LOGW(TAG, "SNTP waiting... (%d s)", (i + 1) / 10);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    // 即使超时也打印当前时间，方便排查
    time_t now = 0;
    time(&now);
    ESP_LOGE(TAG, "SNTP sync timeout! current time=%ld  "
             "==> Xunfei API signatures WILL fail (403) until clock is correct!", (long)now);
}

/**
 * @brief AI对话状态回调函数
 */
void ai_chat_status_callback(ai_chat_state_t state, const char *message)
{
    switch (state) {
        case AI_CHAT_IDLE:
            ESP_LOGI(TAG, "AI Chat: IDLE - %s", message);
            led_set_color(0, 0, 0); // 关灯
            break;
        case AI_CHAT_RECORDING:
            ESP_LOGI(TAG, "AI Chat: RECORDING - %s", message);
            led_set_color(255, 0, 0); // 红灯（录音中）
            break;
        case AI_CHAT_PROCESSING:
            ESP_LOGI(TAG, "AI Chat: PROCESSING - %s", message);
            led_set_color(255, 255, 0); // 黄灯（处理中）
            break;
        case AI_CHAT_PLAYING:
            ESP_LOGI(TAG, "AI Chat: PLAYING - %s", message);
            led_set_color(0, 255, 0); // 绿灯（播放回复）
            break;
        case AI_CHAT_ERROR:
            ESP_LOGE(TAG, "AI Chat: ERROR - %s", message);
            led_set_color(255, 0, 255); // 紫灯（错误）
            break;
    }
}

/**
 * @brief 语音事件处理任务
 * 唤醒词"嗨，乐鑫"触发后直接启动 AI 语音对话，无需二次命令
 */
void sr_handler_task(void *pvParam)
{
    QueueHandle_t xQueue = (QueueHandle_t) pvParam;

    while (true) {
        sr_result_t result;
        if (xQueueReceive(xQueue, &result, portMAX_DELAY) != pdTRUE) continue;

        if (result.wakenet_mode == WAKENET_DETECTED) {
            ai_chat_state_t state = ai_chat_get_state();
            if (state == AI_CHAT_IDLE) {
                ESP_LOGI(TAG, "[唤醒] 开始 AI 语音对话");
                led_set_color(0, 0, 255);  /* 蓝灯：对话准备中 */
                ai_chat_start_conversation(ai_chat_status_callback);
            } else if (state == AI_CHAT_RECORDING) {
                ESP_LOGI(TAG, "[唤醒] 录音中，立即提交");
                ai_chat_stop_recording();
            } else {
                ESP_LOGW(TAG, "[唤醒] AI 对话进行中 (state=%d)，忽略重复唤醒", state);
            }
        }
    }
}

static void cam_display_task(void *pvParam)
{
    ESP_LOGI(TAG, "[摄像头预览] 任务启动");
    bool first_frame = true;
    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            if (first_frame) {
                ESP_LOGI(TAG, "[摄像头预览] 首帧: fmt=%d w=%d h=%d len=%d",
                         fb->format, (int)fb->width, (int)fb->height, (int)fb->len);
                first_frame = false;
            }
            /* OV2640 DVP 输出的 RGB565 高字节在前，与 ST7789 格式一致，直接发送即可。
             * 若帧尺寸恰好是 240x240，len == 115200，lcd_draw_frame 取前 240*240 像素正好填满屏幕。
             * 若摄像头实际输出更宽（如 320x240），画面会被压缩显示，可通过换 FRAMESIZE_240X240 解决。 */
            if (fb->format == PIXFORMAT_RGB565 && fb->len >= LCD_H_RES * LCD_V_RES * 2) {
                lcd_draw_frame((const uint16_t *)fb->buf);
            } else {
                ESP_LOGW(TAG, "[摄像头预览] 帧不可用: fmt=%d len=%d", fb->format, (int)fb->len);
            }
            esp_camera_fb_return(fb);
        } else {
            ESP_LOGW(TAG, "[摄像头预览] 获取帧失败");
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        /* 每帧建立最小等待：LCD SPI 写 240行在 40MHz 下约 23ms。
         * 加 5ms 延迟确保 Core1 上的 I2S DMA / speaker 任务得到运行机会，
         * 避免音频因要帧刘奉求源而静默。实际帧率捲 23+5=~28ms，即 ~36fps。 */
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void app_main(void)
{
    // 0. 优先初始化 LCD 和摄像头（不依赖网络）
    ESP_LOGI(TAG, "Initializing LCD...");
    if (lcd_init() == ESP_OK) {
        ESP_LOGI(TAG, "LCD initialized");
    } else {
        ESP_LOGE(TAG, "LCD init failed!");
    }

    ESP_LOGI(TAG, "Initializing OV2640 camera...");
    if (camera_init() == ESP_OK) {
        ESP_LOGI(TAG, "Camera initialized, starting preview task");
        /* 摄像头预览任务：Core 1，优先级 3（低于 AI/AFE 的 5），避免与 WiFi 竞争 Core 0 */
        xTaskCreatePinnedToCore(cam_display_task, "cam_disp", 4 * 1024, NULL, 3, NULL, 1);
    } else {
        ESP_LOGE(TAG, "Camera init failed!");
    }

    // 1. 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // 1.5 初始化 WiFi
    wifi_init_sta();
    
    // 等待WiFi连接
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    vTaskDelay(pdMS_TO_TICKS(5000));

    // 1.6 同步时间，避免讯飞签名因时钟错误返回401
    sync_time_once();
    
    ESP_LOGI(TAG, "Initializing LED...");
    // 2. 初始化 WS2812 LED
    led_init();
    ESP_LOGI(TAG, "LED initialized");
    
    ESP_LOGI(TAG, "Initializing Speaker...");
    // 2.5 初始化MAX98357扬声器
    speaker_init();
    ESP_LOGI(TAG, "Speaker initialized");

    // 3. 初始化AI对话模块
    ESP_LOGI(TAG, "Initializing AI Chat module...");
    
    ESP_LOGW(TAG, "========================================");
    ESP_LOGW(TAG, "Using Xunfei Spark Chat API");
    ESP_LOGW(TAG, "Endpoint: wss://spark-api.xf-yun.com/v3.1/chat");
    ESP_LOGW(TAG, "");
    ESP_LOGW(TAG, "IMPORTANT:");
    ESP_LOGW(TAG, "1. Spark uses the same XFYUN APPID/APIKey/APISecret as IAT/TTS");
    ESP_LOGW(TAG, "2. Voice-to-text uses Xunfei IAT WebSocket");
    ESP_LOGW(TAG, "3. Voice synthesis uses Xunfei TTS WebSocket");
    ESP_LOGW(TAG, "========================================");

    ret = ai_chat_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize AI Chat: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "AI Chat module initialized successfully");
    }

    // 4a. 初始化图片理解模块
    xfyun_image_config_t img_cfg = {
        .app_id     = CONFIG_XFYUN_APP_ID,
        .api_key    = CONFIG_XFYUN_API_KEY,
        .api_secret = CONFIG_XFYUN_API_SECRET,
    };
    ret = xfyun_image_init(&img_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Image Understanding: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Image Understanding module initialized");
    }

    ESP_LOGI(TAG, "Initializing Speech Recognition...");
    // 4. 初始化语音模块 (在 yuyin.c 中实现)
    // - 初始化 I2S
    // - 加载模型
    // - 启动 Feed 和 Detect 任务
    QueueHandle_t sr_queue = yuyin_init();
    ESP_LOGI(TAG, "Speech Recognition initialization completed");
    
    if (sr_queue != NULL) {
        // 5. 创建业务处理任务，监听 sr_queue
        xTaskCreatePinnedToCore(&sr_handler_task, "handler_task", 8 * 1024, sr_queue, 1, NULL, 1);
        ESP_LOGI(TAG, "系统就绪，说 '嗨，乐鑫' 直接开始 AI 语音对话");
    } else {
        ESP_LOGE(TAG, "Speech Recognition Init Failed!");
    }
}
