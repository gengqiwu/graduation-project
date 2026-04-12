#ifndef __XFYUN_IMAGE_H__
#define __XFYUN_IMAGE_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_websocket_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 讯飞图片理解配置
 */
typedef struct {
    const char *app_id;
    const char *api_key;
    const char *api_secret;
} xfyun_image_config_t;

/**
 * @brief 初始化讯飞图片理解模块
 */

typedef void (*xfyun_image_chunk_cb_t)(const char *text, bool is_final, void *user_ctx);
esp_err_t xfyun_image_init(const xfyun_image_config_t *config);

/**
 * @brief 对图片进行理解分析
 *
 * @param image_data    图片原始二进制数据（jpg/png）
 * @param image_len     图片数据长度（字节）
 * @param question      对图片的提问（例如 "描述一下这张图片"）
 * @param out_reply     输出：堆分配的UTF-8回复字符串，调用者需 free()
 * @param timeout_ms    超时时间（毫秒）
 * @return ESP_OK 成功
 */
esp_err_t xfyun_image_understand(const uint8_t *image_data, size_t image_len,
                                  const char *question,
                                  xfyun_image_chunk_cb_t chunk_cb, void *user_ctx,
                                  char **out_reply, uint32_t timeout_ms);

/**
 * @brief 中断正在进行的图片理解请求（可在任意任务中调用）
 */
void xfyun_image_abort(void);



esp_websocket_client_handle_t xfyun_image_preconnect(void);
esp_err_t xfyun_image_understand_with_conn(esp_websocket_client_handle_t client,
                                   const uint8_t *image_data, size_t image_len,
                                   const char *question,
                                   xfyun_image_chunk_cb_t chunk_cb, void *user_ctx,
                                   char **out_reply, uint32_t timeout_ms);


#ifdef __cplusplus
}
#endif

#endif /* __XFYUN_IMAGE_H__ */
