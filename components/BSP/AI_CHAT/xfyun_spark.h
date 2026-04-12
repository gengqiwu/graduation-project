#ifndef __XFYUN_SPARK_H__
#define __XFYUN_SPARK_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *app_id;
    const char *api_key;
    const char *api_secret;
    const char *domain; // e.g. "lite"
} xfyun_spark_config_t;

esp_err_t xfyun_spark_init(const xfyun_spark_config_t *config);

esp_err_t xfyun_spark_chat(const char *user_text, char **out_reply, uint32_t timeout_ms);

/**
 * @brief 获取上一次 Spark 调用的错误码（0=成功）
 */
int xfyun_spark_get_last_error(void);

/**
 * @brief Spark 流式文本回调。从 WebSocket 事件任务调用，必须快速非阻塞。
 * @param accumulated_text 已收到的全部文本（含本次新增部分）
 * @param is_final         true = Spark 已完成本次回复
 * @param user_ctx         用户上下文
 */
typedef void (*spark_stream_cb_t)(const char *accumulated_text, bool is_final, void *user_ctx);

/**
 * @brief 流式版 xfyun_spark_chat：每次收到文字片段即调用 on_chunk。
 *        out_reply 返回完整回复文本（调用者负责 free）。
 */
esp_err_t xfyun_spark_chat_stream(const char *user_text,
                                   spark_stream_cb_t on_chunk,
                                   void *on_chunk_ctx,
                                   char **out_reply,
                                   uint32_t timeout_ms);

/* ===== 预连接 API：TLS 握手与其他操作并行，减少等待时间 ===== */

/**
 * @brief Spark 预连接句柄（opaque）
 */
typedef void* xfyun_spark_conn_t;

/**
 * @brief 仅完成 TLS 握手，不发送任何数据。
 *        可在录音等待期间后台调用，当问题文本就绪后立即发送。
 * @param out_conn 输出句柄（使用完毕或出错后需调用 xfyun_spark_conn_close）
 * @return ESP_OK 握手成功；其他值失败
 */
esp_err_t xfyun_spark_preconnect(xfyun_spark_conn_t *out_conn);

/**
 * @brief 使用已握手的连接发起聊天请求（省去 TLS 握手时间）。
 *        调用完成后句柄自动释放，无需再调用 xfyun_spark_conn_close。
 */
esp_err_t xfyun_spark_chat_stream_with_conn(xfyun_spark_conn_t conn,
                                             const char *user_text,
                                             spark_stream_cb_t on_chunk,
                                             void *on_chunk_ctx,
                                             char **out_reply,
                                             uint32_t timeout_ms);

/**
 * @brief 释放未使用的预连接句柄（如被中断时调用）。
 */
void xfyun_spark_conn_close(xfyun_spark_conn_t conn);

/**
 * @brief 如果当前有活跌的 Spark 流式请求，立即中断并返回 ESP_FAIL。
 * 可在任意任务中调用（不需持锁）。
 */
void xfyun_spark_abort(void);

#ifdef __cplusplus
}
#endif

#endif /* __XFYUN_SPARK_H__ */
