#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Xunfei TTS credentials
 */
typedef struct {
    const char *app_id;
    const char *api_key;
    const char *api_secret;
} xfyun_tts_config_t;

/**
 * @brief TTS parameters
 */
typedef struct {
    const char *voice_name;     // 发音人，默认 "xiaoyan"
    int speed;                  // 语速，0-100，默认50
    int volume;                 // 音量，0-100，默认50
    int pitch;                  // 音高，0-100，默认50
} xfyun_tts_params_t;

/**
 * @brief 音频数据回调函数
 * @param audio_data PCM16 音频数据
 * @param audio_len 数据长度（字节）
 * @param user_ctx 用户上下文
 */
typedef void (*xfyun_tts_audio_callback_t)(const uint8_t *audio_data, size_t audio_len, void *user_ctx);

/**
 * @brief 初始化讯飞TTS客户端
 */
esp_err_t xfyun_tts_init(const xfyun_tts_config_t *config);

/**
 * @brief 流式合成文本并通过回调返回音频
 * @param text 要合成的文本
 * @param params TTS参数（可为NULL使用默认值）
 * @param audio_callback 音频数据回调
 * @param user_ctx 用户上下文
 * @param timeout_ms 超时时间
 */
esp_err_t xfyun_tts_synthesize(const char *text,
                                const xfyun_tts_params_t *params,
                                xfyun_tts_audio_callback_t audio_callback,
                                void *user_ctx,
                                uint32_t timeout_ms);

/**
 * @brief 临时断开TTS持久连接以释放内存
 */
void xfyun_tts_disconnect(void);

/**
 * @brief 重新连接TTS持久连接
 */
esp_err_t xfyun_tts_reconnect(void);

/**
 * @brief 中断当前正在进行的TTS合成，立即停止播放
 */
void xfyun_tts_abort(void);

/* ===== TTS 流水线 API =====
 * 用于 Spark 流式输出同步合成：发现句末即将该句送入流水线，
 * 流水线任务并发运行、逐句合成音频。
 * 常规用法：
 *   1. xfyun_tts_pipeline_start()
 *   2. xfyun_tts_pipeline_feed() × N    (在 Spark 流式回调中调用)
 *   3. xfyun_tts_pipeline_finish()       (等待全部完成)
 * 如需中断：调用 xfyun_tts_pipeline_abort()，再调用 _finish() 清理资源。
 */
typedef void *xfyun_tts_pipeline_handle_t;

/**
 * @brief 启动流水线任务：占领 TTS WS 连接，等待句子入队。
 */
esp_err_t xfyun_tts_pipeline_start(const xfyun_tts_params_t *params,
                                    xfyun_tts_audio_callback_t audio_cb,
                                    void *user_ctx,
                                    xfyun_tts_pipeline_handle_t *out_handle);

/**
 * @brief 将一个句子投入流水线队列（非阻塞，全内部 strdup）。
 */
esp_err_t xfyun_tts_pipeline_feed(xfyun_tts_pipeline_handle_t handle, const char *sentence);

/**
 * @brief 结束流水线：发送哨兵、等待任务完成并释放资源。
 */
esp_err_t xfyun_tts_pipeline_finish(xfyun_tts_pipeline_handle_t handle, uint32_t wait_ms, volatile bool *abort_flag);

/**
 * @brief 立即中断流水线（中断正在合成的句子）。
 *        调用后仍需调用 _finish() 清理资源。
 */
void xfyun_tts_pipeline_abort(xfyun_tts_pipeline_handle_t handle);

#ifdef __cplusplus
}
#endif
