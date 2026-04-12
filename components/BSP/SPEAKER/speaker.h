#ifndef _SPEAKER_H_
#define _SPEAKER_H_

#include "esp_err.h"

/**
 * @brief 初始化MAX98357扬声器
 */
void speaker_init(void);

/**
 * @brief 开始播放音频（播放简单的蜂鸣音）
 */
void speaker_start_beep(void);

/**
 * @brief 停止播放音频
 */
void speaker_stop(void);

/**
 * @brief 播放提示音（短促的蜂鸣）
 */
void speaker_play_prompt(void);

/**
 * @brief 播放PCM音频数据
 * @param data PCM16音频数据（单声道，16kHz）
 * @param len 数据长度（字节）
 */
void speaker_play_pcm(const int16_t *data, size_t len);

/**
 * @brief 开启流式播放模式（创建环形缓冲区 + 播放任务）
 *        在此模式下 speaker_play_pcm 写入缓冲区而非直接阻塞 I2S
 */
void speaker_start_stream(void);

/**
 * @brief 停止流式播放模式，立即清空缓冲区
 */
void speaker_stop_stream(void);

/**
 * @brief 等待流式缓冲区播放完毕（含 DMA drain），然后自动停止流式模式
 * @param timeout_ms 最大等待时间
 * @return ESP_OK 正常播完, ESP_ERR_TIMEOUT 超时
 */
esp_err_t speaker_drain(uint32_t timeout_ms);

/**
 * @brief 静音（立即停止当前音频输出并忽略后续 play_pcm 调用）
 */
void speaker_mute(void);

/**
 * @brief 取消静音
 */
void speaker_unmute(void);

#endif // _SPEAKER_H_
