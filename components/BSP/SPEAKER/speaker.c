#include "speaker.h"
#include "driver/i2s.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>

#define TAG "SPEAKER"

// MAX98357 引脚定义
#define MAX98357_LRC_IO   41  // LRC (WS)
#define MAX98357_BCLK_IO  40  // BCLK
#define MAX98357_DIN_IO   39   // DIN (DATA)

// 音频参数
#define SAMPLE_RATE       16000
#define BEEP_FREQUENCY    800   // 蜂鸣音频率 (Hz)
#define BUFFER_SIZE       512

// 流式播放缓冲区参数
#define STREAM_RINGBUF_SIZE  (96 * 1024)   // 96KB ≈ 3s 音频缓冲，桥接分段TTS间的网络等待
#define STREAM_PREBUF_SIZE   (4 * 1024)    // 预缓冲 4KB（125ms）后开始播放，兼顾低延迟与防抖
#define PLAYBACK_LOCAL_SIZE  (8 * 1024)    // 播放任务每次最多取 8KB 到本地缓冲

static bool is_playing = false;
static TaskHandle_t play_task_handle = NULL;
static volatile bool g_muted = false;   // 静音标志

// 流式播放相关
static RingbufHandle_t g_stream_ringbuf = NULL;
static TaskHandle_t g_stream_task_handle = NULL;
static volatile bool g_stream_active = false;
static volatile bool g_stream_eof = false;    // 生产者已结束
static SemaphoreHandle_t g_drain_sem = NULL;
static SemaphoreHandle_t g_stream_mutex = NULL;  // 保护 ringbuf 生命周期与并发写入
static volatile size_t g_stream_buffered = 0; // 已缓冲字节数（用于预缓冲判断）

/**
 * @brief 生成正弦波音频数据
 */
static void generate_sine_wave(int16_t *buffer, size_t samples, float frequency) {
    static float phase = 0.0f;
    float phase_increment = 2.0f * M_PI * frequency / SAMPLE_RATE;
    
    for (size_t i = 0; i < samples; i++) {
        buffer[i] = (int16_t)(sin(phase) * 8000.0f);  // 幅度约为最大值的25%
        phase += phase_increment;
        if (phase >= 2.0f * M_PI) {
            phase -= 2.0f * M_PI;
        }
    }
}

/**
 * @brief 音频播放任务
 */
static void play_task(void *pvParam) {
    int16_t buffer[BUFFER_SIZE];
    size_t bytes_written;
    
    ESP_LOGI(TAG, "Audio playback started");
    
    while (is_playing) {
        // 生成正弦波数据
        generate_sine_wave(buffer, BUFFER_SIZE, BEEP_FREQUENCY);
        
        // 写入I2S
        i2s_write(I2S_NUM_1, buffer, BUFFER_SIZE * sizeof(int16_t), &bytes_written, portMAX_DELAY);
    }
    
    ESP_LOGI(TAG, "Audio playback stopped");
    play_task_handle = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief 初始化MAX98357扬声器
 */
void speaker_init(void) {
    ESP_LOGI(TAG, "Initializing MAX98357 speaker...");
    
    // I2S配置 - 输出模式
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),  // 主模式，发送
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // 单声道（左声道）
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,         // 8个DMA缓冲区：总8×512×2=8KB≈256ms，减少drain等待
        .dma_buf_len = 512,         // 每个缓冲区512样本，减少播放拖尾延迟
        .use_apll = false,
        .tx_desc_auto_clear = true  // 自动清除发送描述符
    };
    
    // 安装I2S驱动（使用I2S_NUM_1，因为I2S_NUM_0被麦克风占用）
    esp_err_t ret = i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install I2S driver: %s", esp_err_to_name(ret));
        return;
    }
    
    // I2S引脚配置
    i2s_pin_config_t pin_config = {
        .bck_io_num = MAX98357_BCLK_IO,    // BCLK -> GPIO15
        .ws_io_num = MAX98357_LRC_IO,      // LRC -> GPIO16
        .data_out_num = MAX98357_DIN_IO,   // DIN -> GPIO7
        .data_in_num = I2S_PIN_NO_CHANGE   // 不使用输入
    };
    
    ret = i2s_set_pin(I2S_NUM_1, &pin_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set I2S pins: %s", esp_err_to_name(ret));
        return;
    }
    
    // 清零DMA缓冲区
    i2s_zero_dma_buffer(I2S_NUM_1);

    if (!g_stream_mutex) {
        g_stream_mutex = xSemaphoreCreateMutex();
        if (!g_stream_mutex) {
            ESP_LOGE(TAG, "Failed to create stream mutex");
        }
    }
    
    ESP_LOGI(TAG, "MAX98357 speaker initialized successfully");
    ESP_LOGI(TAG, "Pin config: LRC=GPIO%d, BCLK=GPIO%d, DIN=GPIO%d", 
             MAX98357_LRC_IO, MAX98357_BCLK_IO, MAX98357_DIN_IO);
}

/**
 * @brief 开始播放音频（播放持续的蜂鸣音）
 */
void speaker_start_beep(void) {
    if (is_playing) {
        ESP_LOGW(TAG, "Already playing");
        return;
    }
    
    is_playing = true;
    
    // 创建播放任务
    xTaskCreate(play_task, "speaker_play", 4096, NULL, 5, &play_task_handle);
    ESP_LOGI(TAG, "Beep playback started");
}

/**
 * @brief 停止播放音频
 */
void speaker_stop(void) {
    if (!is_playing) {
        ESP_LOGW(TAG, "Not playing");
        return;
    }
    
    is_playing = false;
    
    // 等待播放任务结束
    int retry = 0;
    while (play_task_handle != NULL && retry < 20) {
        vTaskDelay(pdMS_TO_TICKS(50));
        retry++;
    }
    
    // 清零DMA缓冲区，避免残留噪音
    i2s_zero_dma_buffer(I2S_NUM_1);
    
    ESP_LOGI(TAG, "Playback stopped");
}

/**
 * @brief 播放提示音（短促的蜂鸣）
 */
void speaker_play_prompt(void) {
    ESP_LOGI(TAG, "Playing prompt tone");
    
    int16_t buffer[BUFFER_SIZE];
    size_t bytes_written;
    
    // 播放200ms的提示音
    int samples_to_play = (SAMPLE_RATE * 200) / 1000;  // 200ms
    int buffers_to_play = samples_to_play / BUFFER_SIZE;
    
    for (int i = 0; i < buffers_to_play; i++) {
        generate_sine_wave(buffer, BUFFER_SIZE, 1000);  // 1kHz提示音
        i2s_write(I2S_NUM_1, buffer, BUFFER_SIZE * sizeof(int16_t), &bytes_written, portMAX_DELAY);
    }
    
    // 清零DMA缓冲区
    i2s_zero_dma_buffer(I2S_NUM_1);
}

// ========== 流式播放任务 ==========

static void stream_playback_task(void *pvParam)
{
    ESP_LOGI(TAG, "Stream playback task started");
    bool prebuf_done = false;

    // 本地缓冲区：从 ring buffer 拷贝后立即归还 item，
    // 这样 i2s_write 阻塞期间 ring buffer 空间仍可被生产者使用
    uint8_t *local_buf = (uint8_t *)malloc(PLAYBACK_LOCAL_SIZE);
    if (!local_buf) {
        ESP_LOGE(TAG, "Failed to alloc playback buffer");
        g_stream_active = false;
        if (g_drain_sem) xSemaphoreGive(g_drain_sem);
        g_stream_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (g_stream_active) {
        // 预缓冲：等到缓冲区积累足够数据或生产者已结束，再开始输出
        if (!prebuf_done && !g_stream_eof) {
            if (g_stream_buffered < STREAM_PREBUF_SIZE) {
                vTaskDelay(pdMS_TO_TICKS(15));
                continue;
            }
            prebuf_done = true;
            ESP_LOGI(TAG, "Pre-buffer filled (%d bytes), starting playback", (int)g_stream_buffered);
        }

        if (g_muted) {
            size_t item_size;
            void *item = xRingbufferReceive(g_stream_ringbuf, &item_size, pdMS_TO_TICKS(50));
            if (item) vRingbufferReturnItem(g_stream_ringbuf, item);
            continue;
        }

        // 使用 ReceiveUpTo 限制单次最大读取量，减少持锁时间
        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(g_stream_ringbuf, &item_size,
                                            pdMS_TO_TICKS(100), PLAYBACK_LOCAL_SIZE);

        if (item && item_size > 0) {
            // 立即拷贝到本地缓冲区并归还 ring buffer item
            memcpy(local_buf, item, item_size);
            vRingbufferReturnItem(g_stream_ringbuf, item);

            // 用本地缓冲区写 I2S（可以安全阻塞，ring buffer 已释放）
            size_t offset = 0;
            while (offset < item_size && !g_muted && g_stream_active) {
                size_t written;
                i2s_write(I2S_NUM_1, local_buf + offset,
                          item_size - offset, &written, pdMS_TO_TICKS(200));
                offset += written;
            }
        } else {
            if (item) vRingbufferReturnItem(g_stream_ringbuf, item);
            if (g_stream_eof && item == NULL) break;
        }
    }

    free(local_buf);
    ESP_LOGI(TAG, "Stream playback task ended");
    if (g_drain_sem) {
        xSemaphoreGive(g_drain_sem);
    }
    g_stream_active = false;
    g_stream_task_handle = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief 播放PCM音频数据
 *        流式模式下写入环形缓冲区（快速、不阻塞WebSocket）
 *        非流式模式下直接写 I2S（兼容旧逻辑）
 */
void speaker_play_pcm(const int16_t *data, size_t len) {
    if (!data || len == 0) return;
    if (g_muted) return;

    // 流式模式：写入环形缓冲区（短超时，不长时间阻塞 WebSocket 事件线程）
    if (g_stream_active && g_stream_ringbuf) {
        const uint8_t *src = (const uint8_t *)data;
        size_t offset = 0;
        while (offset < len && !g_muted && g_stream_active) {
            size_t to_write = len - offset;
            if (to_write > 4096) to_write = 4096;  // 增大单次写入到 4KB（原 1KB）
            bool sent = false;
            if (g_stream_mutex && xSemaphoreTake(g_stream_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                RingbufHandle_t rb = g_stream_ringbuf;
                bool stream_ok = g_stream_active;
                if (stream_ok && rb && xRingbufferSend(rb, src + offset, to_write, 0) == pdTRUE) {
                    sent = true;
                }
                xSemaphoreGive(g_stream_mutex);
            }

            if (sent) {
                offset += to_write;
                g_stream_buffered += to_write;
            } else {
                // 缓冲区满，短暂让出 CPU（播放任务在消费）
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        }
        return;
    }

    // 非流式：直接写 I2S（短超时）
    size_t offset = 0;
    while (offset < len && !g_muted) {
        size_t written;
        esp_err_t ret = i2s_write(I2S_NUM_1,
                                  (const char *)data + offset,
                                  len - offset, &written,
                                  pdMS_TO_TICKS(100));
        if (ret != ESP_OK) break;
        offset += written;
    }
}

// ========== 流式播放管理 ==========

void speaker_start_stream(void)
{
    if (g_stream_active) {
        ESP_LOGW(TAG, "Stream already active");
        return;
    }

    if (g_stream_task_handle) {
        ESP_LOGW(TAG, "Previous stream task still running, skip start");
        return;
    }

    if (g_stream_ringbuf) {
        if (g_stream_mutex && xSemaphoreTake(g_stream_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            vRingbufferDelete(g_stream_ringbuf);
            g_stream_ringbuf = NULL;
            xSemaphoreGive(g_stream_mutex);
        } else {
            ESP_LOGW(TAG, "Stream mutex busy, cannot safely reset ringbuf");
            return;
        }
    }

    g_stream_ringbuf = xRingbufferCreate(STREAM_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!g_stream_ringbuf) {
        ESP_LOGE(TAG, "Failed to create stream ring buffer");
        return;
    }

    if (!g_drain_sem) {
        g_drain_sem = xSemaphoreCreateBinary();
    }

    g_stream_active = true;
    g_stream_eof = false;
    g_stream_buffered = 0;
    g_muted = false;

    // 播放任务优先级 8（高于 WebSocket 任务），绑定到 Core 0
    xTaskCreatePinnedToCore(stream_playback_task, "spk_stream", 4096,
                            NULL, 8, &g_stream_task_handle, 0);
    ESP_LOGI(TAG, "Audio stream started (buf=%dKB, prebuf=%dB)",
             STREAM_RINGBUF_SIZE / 1024, STREAM_PREBUF_SIZE);
}

void speaker_stop_stream(void)
{
    if (!g_stream_active && !g_stream_ringbuf) return;

    g_stream_active = false;
    g_stream_eof = true;

    // 等待播放任务退出
    int retry = 0;
    while (g_stream_task_handle && retry < 20) {
        vTaskDelay(pdMS_TO_TICKS(50));
        retry++;
    }

    if (g_stream_ringbuf) {
        if (g_stream_mutex && xSemaphoreTake(g_stream_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            vRingbufferDelete(g_stream_ringbuf);
            g_stream_ringbuf = NULL;
            xSemaphoreGive(g_stream_mutex);
        } else {
            ESP_LOGW(TAG, "Failed to lock stream mutex for stop cleanup");
        }
    }

    i2s_zero_dma_buffer(I2S_NUM_1);
    ESP_LOGI(TAG, "Audio stream stopped");
}

esp_err_t speaker_drain(uint32_t timeout_ms)
{
    if (!g_stream_active) return ESP_OK;

    // 通知播放任务：数据已全部送入
    g_stream_eof = true;

    // 等待播放任务消费完毕
    esp_err_t ret = ESP_OK;
    if (g_drain_sem) {
        if (xSemaphoreTake(g_drain_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
            ESP_LOGW(TAG, "Drain timeout");
            ret = ESP_ERR_TIMEOUT;
        } else {
            // 播放任务虽然已经排空了 ringbuf，并且退出。
            // 但底层 I2S DMA 硬件缓冲区中仍有最后约 512ms 的数据（16KB）正在播放。
            // 物理等待这批数据播放完毕，避免被静音截断或漏入下一轮麦克风（自问自答）。
            vTaskDelay(pdMS_TO_TICKS(550));
        }
    }

    if (ret == ESP_ERR_TIMEOUT) {
        // 超时后先停止生产/消费，再等待任务退出，避免并发删除 ringbuf。
        g_stream_active = false;
        int retry = 0;
        while (g_stream_task_handle && retry < 20) {
            vTaskDelay(pdMS_TO_TICKS(50));
            retry++;
        }
    }

    // 等 DMA 播放完最后的数据
    // DMA total = dma_buf_count(8) * dma_buf_len(512) * 2 bytes = 8KB
    // 8KB / 32000 B/s ≈ 256ms，加余量取 350ms
    if (ret == ESP_OK && !g_muted) {
        vTaskDelay(pdMS_TO_TICKS(350));
    }

    // 清理
    g_stream_active = false;
    if (g_stream_ringbuf) {
        if (g_stream_mutex && xSemaphoreTake(g_stream_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            vRingbufferDelete(g_stream_ringbuf);
            g_stream_ringbuf = NULL;
            xSemaphoreGive(g_stream_mutex);
        } else {
            ESP_LOGW(TAG, "Failed to lock stream mutex for drain cleanup");
        }
    }
    i2s_zero_dma_buffer(I2S_NUM_1);
    ESP_LOGI(TAG, "Audio stream drained");
    return ret;
}

void speaker_mute(void)
{
    g_muted = true;

    // 清空流式缓冲区
    if (g_stream_ringbuf) {
        size_t item_size;
        void *item;
        while ((item = xRingbufferReceive(g_stream_ringbuf, &item_size, 0)) != NULL) {
            vRingbufferReturnItem(g_stream_ringbuf, item);
        }
    }

    i2s_zero_dma_buffer(I2S_NUM_1);
    // i2s_zero_dma_buffer 会在后续推入 0 帧，但当前DMA缓冲区中如果已推满的旧数据可能要等它们被消耗完（约512ms）。
    // 我们等待它排空完毕以确保物理静音绝对生效，避免重入或切换时出现声音残留漏入麦克风。
    vTaskDelay(pdMS_TO_TICKS(550));
    ESP_LOGI(TAG, "Speaker muted");
}

void speaker_unmute(void)
{
    g_muted = false;
    ESP_LOGI(TAG, "Speaker unmuted");
}

