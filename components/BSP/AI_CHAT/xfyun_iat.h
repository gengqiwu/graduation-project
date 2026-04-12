#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Credential bundle for Xunfei IAT WebSocket service.
 */
typedef struct {
    const char *app_id;
    const char *api_key;
    const char *api_secret;
} xfyun_iat_config_t;

/**
 * @brief Initialize the Xunfei IAT client with credential strings.
 *
 * The provided pointers are copied into internal buffers, so the caller
 * does not need to preserve them after this call.
 */
esp_err_t xfyun_iat_init(const xfyun_iat_config_t *config);

/**
 * @brief Transcribe PCM16 mono audio using Xunfei IAT over WebSocket.
 *
 * @param pcm_samples   Pointer to 16 kHz, 16-bit, mono PCM samples.
 * @param sample_count  Number of samples in the buffer.
 * @param out_text      On success, set to a heap-allocated UTF-8 string; caller frees with free().
 * @param timeout_ms    Overall timeout for recognition (including network and server processing).
 * @return ESP_OK on success; ESP_ERR_* on failure.
 */
esp_err_t xfyun_iat_transcribe(const int16_t *pcm_samples, size_t sample_count, char **out_text, uint32_t timeout_ms);

/**
 * @brief 流式识别会话句柄
 */
typedef void* xfyun_iat_session_handle_t;

/**
 * @brief 流式识别进度回调
 * @param partial_text 当前已识别的部分文本（可能随后续帧更新）
 * @param user_ctx 用户上下文
 */
typedef void (*xfyun_iat_partial_callback_t)(const char *partial_text, void *user_ctx);

/**
 * @brief 开始流式识别会话
 * @param out_session 输出会话句柄
 * @param partial_cb 可选的部分识别结果回调（可为NULL）
 * @param user_ctx 用户上下文，传递给回调
 * @return ESP_OK成功；否则失败
 */
esp_err_t xfyun_iat_streaming_start(xfyun_iat_session_handle_t *out_session, 
                                     xfyun_iat_partial_callback_t partial_cb, 
                                     void *user_ctx);

/**
 * @brief 发送音频分片到流式会话
 * @param session 会话句柄
 * @param pcm_chunk 音频分片（16kHz, 16bit, mono）
 * @param sample_count 分片样本数（建议200ms = 3200 samples）
 * @return ESP_OK成功；否则失败
 */
esp_err_t xfyun_iat_streaming_feed(xfyun_iat_session_handle_t session,
                                    const int16_t *pcm_chunk,
                                    size_t sample_count);

/**
 * @brief 结束流式会话并获取最终识别结果
 * @param session 会话句柄
 * @param out_text 输出最终识别文本（heap分配，调用者释放）
 * @param timeout_ms 等待最终结果的超时时间
 * @return ESP_OK成功；否则失败
 */
esp_err_t xfyun_iat_streaming_finish(xfyun_iat_session_handle_t session,
                                      char **out_text,
                                      uint32_t timeout_ms);

/**
 * @brief 关闭流式会话并释放资源
 * @param session 会话句柄
 * @return ESP_OK成功；否则失败
 */
esp_err_t xfyun_iat_streaming_close(xfyun_iat_session_handle_t session);

#ifdef __cplusplus
}
#endif
