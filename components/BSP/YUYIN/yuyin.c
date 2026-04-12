#include "yuyin.h"
#include "esp_log.h"
#include "esp_mn_models.h"
#include "model_path.h"
#include "esp_mn_iface.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_mn_speech_commands.h"

#include "i2s.h"         // 自定义I2S驱动
#include "driver/i2s.h"  // ESP-IDF I2S驱动

#define TAG "YUYIN"

// --- 全局变量 (仅本文件可见) ---
static QueueHandle_t        g_result_que    = NULL; // 识别结果消息队列
static esp_afe_sr_iface_t   *afe_handle     = NULL; // AFE (音频前端) 接口句柄
static esp_afe_sr_data_t    *g_afe_data     = NULL; // AFE 数据句柄（全局）
static model_iface_data_t   *model_data     = NULL; // MultiNet 模型数据句柄
static const esp_mn_iface_t *multinet       = NULL; // MultiNet 接口句柄
static volatile int         task_flag       = 0;    // 任务运行标志
static bool                 sr_paused       = false; // SR是否暂停

/**
 * @brief 音频馈送任务 (Feed Task)
 */
static void feed_Task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;

    // 获取 AFE 每次需要的音频块大小和通道数
    int feed_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int feed_nch = afe_handle->get_feed_channel_num(afe_data);
    
    // 分配缓冲区
    int16_t *feed_buff = (int16_t *) malloc(feed_chunksize * feed_nch * sizeof(int16_t));
    assert(feed_buff);

    while (task_flag) {
        size_t bytesIn = 0;
        // 1. 从 I2S 读取音频数据 (阻塞模式)
        esp_err_t result = i2s_read(I2S_NUM_0, feed_buff, feed_chunksize * feed_nch * sizeof(int16_t), &bytesIn, portMAX_DELAY);
        
        if (result == ESP_OK) {
            // 2. 将数据“喂”给 AFE 引擎进行降噪和处理
            afe_handle->feed(afe_data, feed_buff);
        }
    }

    if (feed_buff) {
        free(feed_buff);
        feed_buff = NULL;
    }
    vTaskDelete(NULL);
}

// ===== 辅助函数：发送结果到队列 =====
static void send_sr_result(wakenet_state_t wake_state, esp_mn_state_t mn_state, int cmd_id) {
    sr_result_t result = {
        .wakenet_mode = wake_state,
        .state = mn_state,
        .command_id = cmd_id,
    };
    xQueueSend(g_result_que, &result, 10);
}

// ===== 辅助函数：处理唤醒 =====
static void handle_wake_detected(int model_idx, int word_idx) {
    ESP_LOGI(TAG, ">>> 唤醒检测 <<< (模型: %d, 词: %d)", model_idx, word_idx);
    send_sr_result(WAKENET_DETECTED, ESP_MN_STATE_DETECTING, 0);
}

/**
 * @brief 识别检测任务 (Detect Task)
 */
static void detect_Task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;
    int afe_chunksize = afe_handle->get_fetch_chunksize(afe_data);
    
    // AFE Fetch 结果缓冲区
    int16_t *buff = malloc(afe_chunksize * sizeof(int16_t)); 
    assert(buff);

    ESP_LOGI(TAG, "检测任务启动...");

    while (task_flag) {
        // 暂停检查
        if (sr_paused) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // 获取 AFE 结果
        afe_fetch_result_t* res = afe_handle->fetch(afe_data); 
        if (!res || res->ret_value == ESP_FAIL) {
            ESP_LOGE(TAG, "AFE获取失败");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // 处理唤醒：发送事件后立即重新开启唤醒词监听，不进入命令等待模式
        if (res->wakeup_state == WAKENET_DETECTED) {
            handle_wake_detected(res->wakenet_model_index, res->wake_word_index);
            afe_handle->enable_wakenet(afe_data);  /* 立即重置，可连续唤醒 */
        }
    }
    
    free(buff);
    vTaskDelete(NULL);
}

// 初始化函数
QueueHandle_t yuyin_init(void)
{
    // 1. 初始化 I2S
    Init_i2s();
    ESP_LOGI(TAG, "I2S Initialized");
    
    // 2. 加载模型
    srmodel_list_t *models = esp_srmodel_init("model"); 
    if (models == NULL) {
        ESP_LOGE(TAG, "Error: 'model' partition not found!");
        return NULL;
    }

    // 3. 配置 AFE
    afe_config_t *afe_config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    afe_config->wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL); 
    afe_config->aec_init = false;
    
    afe_handle = esp_afe_handle_from_config(afe_config);
    g_afe_data = afe_handle->create_from_config(afe_config);
    
    ESP_LOGI(TAG, "AFE Initialized");

    // 4. 配置 MultiNet
    char *mn_name = esp_srmodel_filter(models, ESP_MN_CHINESE, NULL);
    if (NULL == mn_name) {
        ESP_LOGE(TAG, "Error: No Multinet Model Found!");
        return NULL;
    }
    
    multinet = esp_mn_handle_from_name(mn_name);
    // 配置 MultiNet，并将超时时间设置为 10000 毫秒 (10秒)
    model_data = multinet->create(mn_name, 10000);
    
    ESP_LOGI(TAG, "MultiNet Loaded: %s", mn_name);

    /* 不注册自定义命令词 —— 仅使用唤醒词直接触发，MultiNet 保留默认状态即可。
     * 注意：esp_mn_commands_update() 在命令数为 1 时 build_fsts 会崩溃，务必不调用。 */

    afe_config_free(afe_config);

    // 6. 创建任务
    task_flag = 1;
    xTaskCreatePinnedToCore(&feed_Task, "SR_Feed", 4 * 1024, (void*)g_afe_data, 5, NULL, 0);
    xTaskCreatePinnedToCore(&detect_Task, "SR_Detect", 6 * 1024, (void*)g_afe_data, 5, NULL, 1);

    // 7. 创建结果队列
    g_result_que = xQueueCreate(3, sizeof(sr_result_t));
    return g_result_que;
}

/**
 * @brief 暂停语音识别
 */
void yuyin_pause(void)
{
    sr_paused = true;
    if (g_afe_data && afe_handle) {
        afe_handle->disable_wakenet(g_afe_data);
    }
    // 等待 detect_Task 退出可能正在阻塞的 fetch() 调用
    // 避免 detect_Task 和 afe_reader_task 同时调用 fetch() 导致数据丢失
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "SR Paused (detect_Task synced)");
}

/**
 * @brief 恢复语音识别
 */
void yuyin_resume(void)
{
    sr_paused = false;
    if (g_afe_data && afe_handle) {
        afe_handle->enable_wakenet(g_afe_data);
    }
    ESP_LOGI(TAG, "SR Resumed");
}

/**
 * @brief 获取AFE的chunk大小
 */
int yuyin_get_afe_chunksize(void)
{
    if (!g_afe_data || !afe_handle) return 0;
    return afe_handle->get_fetch_chunksize(g_afe_data);
}

/**
 * @brief 从AFE获取音频数据
 */
int yuyin_get_afe_audio(int16_t *buffer, size_t size, uint32_t timeout_ms)
{
    if (!buffer || !g_afe_data || !afe_handle) {
        return -1;
    }

    int afe_chunksize = afe_handle->get_fetch_chunksize(g_afe_data);
    size_t total_read = 0;
    uint32_t start_time = xTaskGetTickCount();

    while (total_read < size) {
        // 检查超时
        if ((xTaskGetTickCount() - start_time) > pdMS_TO_TICKS(timeout_ms)) {
            break;
        }

        // 从AFE获取数据
        afe_fetch_result_t* res = afe_handle->fetch(g_afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            vTaskDelay(pdMS_TO_TICKS(1));  // 减少延时，更快读取
            continue;
        }

        // 复制数据到缓冲区
        size_t to_copy = (size - total_read) < afe_chunksize ? (size - total_read) : afe_chunksize;
        memcpy(buffer + total_read, res->data, to_copy * sizeof(int16_t));
        total_read += to_copy;
        
        // 不延时，尽快读取下一块以清空ringbuffer
    }

    return total_read * sizeof(int16_t);
}
