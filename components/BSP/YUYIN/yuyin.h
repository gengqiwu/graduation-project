#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_mn_models.h" // For wakenet_state_t, esp_mn_state_t

// --- 语音识别结果结构体 ---
typedef struct {
    wakenet_state_t     wakenet_mode;  // 唤醒状态 (检测到唤醒词/未检测到)
    esp_mn_state_t      state;         // 命令识别状态 (检测中/成功/超时)
    int                 command_id;    // 识别到的命令ID
} sr_result_t;

/**
 * @brief 初始化语音识别模块
 * 
 * 1. 初始化 I2S
 * 2. 加载 SR 模型 (WakeNet & MultiNet)
 * 3. 启动 Feed 和 Detect 任务
 * 
 * @return QueueHandle_t 返回识别结果队列，用户需创建一个任务监听此队列
 */
QueueHandle_t yuyin_init(void);

/**
 * @brief 暂停语音识别（用于AI对话录音）
 */
void yuyin_pause(void);

/**
 * @brief 恢复语音识别
 */
void yuyin_resume(void);

/**
 * @brief 获取AFE每次fetch的chunk大小（样本数）
 * @return chunk大小(样本数)，0表示未初始化
 */
int yuyin_get_afe_chunksize(void);

/**
 * @brief 从AFE获取音频数据（用于AI对话录音）
 * @param buffer 音频缓冲区
 * @param size 期望读取的样本数（建议使用yuyin_get_afe_chunksize的整数倍）
 * @param timeout_ms 超时时间（毫秒）
 * @return 实际读取的字节数，-1表示失败
 */
int yuyin_get_afe_audio(int16_t *buffer, size_t size, uint32_t timeout_ms);
