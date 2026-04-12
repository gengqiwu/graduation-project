#include "xfyun_tts.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "cJSON.h"

#define TAG "XFYUN_TTS"

#define TTS_WS_URL "wss://tts-api.xfyun.cn/v2/tts"
#define TTS_KEEPALIVE_INTERVAL_MS 30000  // Send keepalive every 30s
#define TTS_PIPE_BATCH_TIMEOUT_MS 120000
#define TTS_FIRST_AUDIO_TIMEOUT_MS 4000
#define TTS_PROGRESS_TIMEOUT_MS 4000

static char g_app_id[64];
static char g_api_key[96];
static char g_api_secret[128];
static bool g_ready = false;

// Forward declaration
typedef struct xfyun_tts_session_s xfyun_tts_session_t;

// Persistent WebSocket connection
static esp_websocket_client_handle_t g_ws_client = NULL;
static esp_websocket_client_handle_t g_ws_next   = NULL; /* 后台预建的备用连接 */
static xfyun_tts_session_t *g_active_session = NULL;
static SemaphoreHandle_t g_ws_mutex = NULL;
static volatile bool g_tts_abort = false;  // 中断标志
static volatile int g_last_seg_audio_chunks = 0;
static volatile int g_last_seg_audio_bytes = 0;

struct xfyun_tts_session_s {
    xfyun_tts_audio_callback_t audio_cb;
    void *user_ctx;
    bool finished;
    bool error;
    bool connected;
    SemaphoreHandle_t sem;
    int audio_count;
    size_t total_audio_bytes;  // Total audio bytes received
    char *buffer;              // Buffer for accumulating WebSocket data
    size_t buffer_len;         // Current length of accumulated data
    size_t buffer_capacity;    // Allocated buffer size
};

// ========== 辅助函数 ==========

static char *base64_encode(const uint8_t *data, size_t len)
{
    size_t out_len = 0;
    (void)mbedtls_base64_encode(NULL, 0, &out_len, data, len);

    char *out = (char *)malloc(out_len + 1);
    if (!out) {
        return NULL;
    }

    if (mbedtls_base64_encode((unsigned char *)out, out_len, &out_len, data, len) != 0) {
        free(out);
        return NULL;
    }
    out[out_len] = '\0';
    return out;
}

static int base64_decode(const char *input, uint8_t **output, size_t *output_len)
{
    size_t decoded_len = 0;
    int ret = mbedtls_base64_decode(NULL, 0, &decoded_len, (const unsigned char *)input, strlen(input));
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        return -1;
    }

    uint8_t *decoded = (uint8_t *)malloc(decoded_len);
    if (!decoded) {
        return -1;
    }

    ret = mbedtls_base64_decode(decoded, decoded_len, &decoded_len, (const unsigned char *)input, strlen(input));
    if (ret != 0) {
        free(decoded);
        return -1;
    }

    *output = decoded;
    *output_len = decoded_len;
    return 0;
}

static esp_err_t hmac_sha256_base64(const char *key, const char *input, char **out_base64)
{
    esp_err_t ret = ESP_FAIL;
    unsigned char output[32];
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md_info) {
        return ESP_FAIL;
    }

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);

    do {
        if (mbedtls_md_setup(&ctx, md_info, 1) != 0) break;
        if (mbedtls_md_hmac_starts(&ctx, (const unsigned char *)key, strlen(key)) != 0) break;
        if (mbedtls_md_hmac_update(&ctx, (const unsigned char *)input, strlen(input)) != 0) break;
        if (mbedtls_md_hmac_finish(&ctx, output) != 0) break;

        char *encoded = base64_encode(output, sizeof(output));
        if (!encoded) break;
        *out_base64 = encoded;
        ret = ESP_OK;
    } while (0);

    mbedtls_md_free(&ctx);
    return ret;
}

static char *url_encode(const char *src)
{
    if (!src) return NULL;
    size_t len = strlen(src);
    char *out = (char *)malloc(len * 3 + 1);
    if (!out) return NULL;

    static const char *hex = "0123456789ABCDEF";
    char *pout = out;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            *pout++ = c;
        } else {
            *pout++ = '%';
            *pout++ = hex[c >> 4];
            *pout++ = hex[c & 15];
        }
    }
    *pout = '\0';
    return out;
}

static char *build_auth_url_tts(void)
{
    char date_str[64];
    {
        time_t now = time(NULL);
        struct tm tm_info;
        gmtime_r(&now, &tm_info);
        strftime(date_str, sizeof(date_str), "%a, %d %b %Y %H:%M:%S GMT", &tm_info);
    }

    const char *host = "tts-api.xfyun.cn";
    const char *request_line = "GET /v2/tts HTTP/1.1";

    char signature_origin[512];
    snprintf(signature_origin, sizeof(signature_origin),
             "host: %s\ndate: %s\n%s", host, date_str, request_line);

    char *signature_sha = NULL;
    if (hmac_sha256_base64(g_api_secret, signature_origin, &signature_sha) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate signature");
        return NULL;
    }

    char authorization_origin[1024];
    snprintf(authorization_origin, sizeof(authorization_origin),
             "api_key=\"%s\", algorithm=\"hmac-sha256\", headers=\"host date request-line\", signature=\"%s\"",
             g_api_key, signature_sha);
    free(signature_sha);

    char *authorization = base64_encode((const uint8_t *)authorization_origin, strlen(authorization_origin));
    if (!authorization) {
        ESP_LOGE(TAG, "Failed to base64 encode authorization");
        return NULL;
    }

    char *auth_url = (char *)malloc(2048);
    if (!auth_url) {
        free(authorization);
        return NULL;
    }

    char *encoded_date = url_encode(date_str);
    char *encoded_auth = url_encode(authorization);

    snprintf(auth_url, 2048,
             "%s?authorization=%s&date=%s&host=%s",
             TTS_WS_URL, encoded_auth, encoded_date, host);

    free(authorization);
    free(encoded_date);
    free(encoded_auth);

    return auth_url;
}

// ========== WebSocket 事件处理 ==========

static void tts_ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    
    // Use global active session instead of handler_args
    xfyun_tts_session_t *session = g_active_session;
    if (!session && event_id != WEBSOCKET_EVENT_CONNECTED && event_id != WEBSOCKET_EVENT_DISCONNECTED) {
        return;  // No active session, ignore data events
    }

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Persistent WebSocket connected");
            if (session) {
                session->connected = true;
            }
            break;

        case WEBSOCKET_EVENT_DATA:
        {
            if (!session || !data->data_len || !data->data_ptr) break;
            
            // Accumulate data into buffer
            size_t needed = session->buffer_len + data->data_len + 1;
            if (needed > session->buffer_capacity) {
                size_t new_cap = needed + 4096;
                char *new_buf = realloc(session->buffer, new_cap);
                if (!new_buf) {
                    ESP_LOGE(TAG, "Failed to allocate buffer");
                    session->error = true;
                    break;
                }
                session->buffer = new_buf;
                session->buffer_capacity = new_cap;
            }

            memcpy(session->buffer + session->buffer_len, data->data_ptr, data->data_len);
            session->buffer_len += data->data_len;
            session->buffer[session->buffer_len] = '\0';

            // Try to parse accumulated JSON
            cJSON *root = cJSON_Parse(session->buffer);
            if (!root) {
                // Incomplete JSON, wait for more data
                if (session->buffer_len > 100000) {
                    ESP_LOGE(TAG, "Buffer overflow, clearing");
                    session->buffer_len = 0;
                }
                break;
            }

            // Successfully parsed JSON, clear buffer for next message
            session->buffer_len = 0;

            cJSON *code_item = cJSON_GetObjectItem(root, "code");
            if (!code_item || !cJSON_IsNumber(code_item)) {
                ESP_LOGW(TAG, "Invalid response: missing 'code' field");
                cJSON_Delete(root);
                break;
            }
            
            int code = code_item->valueint;
            if (code != 0) {
                cJSON *msg_item = cJSON_GetObjectItem(root, "message");
                const char *message = (msg_item && cJSON_IsString(msg_item)) ? msg_item->valuestring : "Unknown error";
                ESP_LOGE(TAG, "TTS error: code=%d, message=%s", code, message);
                session->error = true;
                cJSON_Delete(root);
                break;
            }

            cJSON *data_obj = cJSON_GetObjectItem(root, "data");
            if (data_obj) {
                cJSON *audio_item = cJSON_GetObjectItem(data_obj, "audio");
                cJSON *status_item = cJSON_GetObjectItem(data_obj, "status");

                if (audio_item && audio_item->valuestring) {
                    // 解码Base64音频数据
                    uint8_t *audio_data = NULL;
                    size_t audio_len = 0;
                    if (base64_decode(audio_item->valuestring, &audio_data, &audio_len) == 0) {
                        session->audio_count++;
                        session->total_audio_bytes += audio_len;
                        ESP_LOGI(TAG, "Received audio chunk #%d, size: %d bytes (total: %d)", 
                                 session->audio_count, audio_len, session->total_audio_bytes);
                        
                        // 调用回调函数（检查中断标志，避免 abort 后继续播放）
                        if (session->audio_cb && !g_tts_abort) {
                            session->audio_cb(audio_data, audio_len, session->user_ctx);
                        }
                        free(audio_data);
                    }
                }

                // status=2 表示最后一帧
                if (status_item && status_item->valueint == 2) {
                    ESP_LOGI(TAG, "TTS synthesis completed, received %d audio chunks", session->audio_count);
                    session->finished = true;
                    // 立即通知主线程完成
                    if (session->sem) {
                        xSemaphoreGive(session->sem);
                    }
                }
            }

            cJSON_Delete(root);
            break;
        }

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            if (session) {
                session->error = true;
                // 通知主线程发生错误
                if (session->sem) {
                    xSemaphoreGive(session->sem);
                }
            }
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Persistent WebSocket disconnected");
            if (session && session->sem) {
                session->error = true;
                xSemaphoreGive(session->sem);
            }
            break;

        default:
            break;
    }
}

// ========== 持久连接管理 ==========

/* 仅建立 WebSocket TLS 连接，不持锁，返回 client handle（失败返回 NULL） */
static esp_websocket_client_handle_t tts_ws_open_new(void)
{
    char *auth_url = build_auth_url_tts();
    if (!auth_url) return NULL;

    esp_websocket_client_config_t ws_cfg = {
        .uri = auth_url,
        .buffer_size = 17 * 1024,
        .task_stack = 6 * 1024,
        .disable_auto_reconnect = true,
        .reconnect_timeout_ms = 8000,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 8,   /* 讲飞 TTS 服务端 idle timeout ~10-15s，8s ping 保活预建连接 */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_websocket_client_handle_t client = esp_websocket_client_init(&ws_cfg);
    free(auth_url);
    if (!client) return NULL;

    /* 不注册 event handler — 预建连接暂时闲置，使用时再注册 */
    /* xTaskCreate 在任务数峰值期可能短暂失败，最多重试2次 */
    esp_err_t start_ret = ESP_FAIL;
    for (int att = 0; att < 3 && start_ret != ESP_OK; att++) {
        if (att > 0) vTaskDelay(pdMS_TO_TICKS(350));
        start_ret = esp_websocket_client_start(client);
    }
    if (start_ret != ESP_OK) {
        ESP_LOGW(TAG, "tts_ws_open_new: start failed after retries");
        esp_websocket_client_destroy(client);
        return NULL;
    }
    int retry = 0;
    while (!esp_websocket_client_is_connected(client) && retry < 80) {
        vTaskDelay(pdMS_TO_TICKS(100));
        retry++;
    }
    if (!esp_websocket_client_is_connected(client)) {
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        return NULL;
    }
    return client;
}

/* 后台异步关闭旧连接：等待 TCP 干净关闭不影响主线程 */
static void tts_close_old_task(void *arg)
{
    esp_websocket_client_handle_t old = (esp_websocket_client_handle_t)arg;
    esp_websocket_client_stop(old);
    esp_websocket_client_destroy(old);
    vTaskDelete(NULL);
}

/* 后台预建任务：在当前 batch 合成期间建好下一条连接存入 g_ws_next */
static void tts_preconn_bg_task(void *arg)
{
    /* 步骤5在合成结束后才调用此函数，AES 硬件此时已空闲，无需等待 */

    /* 若管线已提前将主连接保活存入 g_ws_next，无需再建，直接退出 */
    if (g_ws_next) {
        ESP_LOGI(TAG, "Pipeline: bg pre-conn skipped (ws_next already set)");
        vTaskDelete(NULL);
        return;
    }

    esp_websocket_client_handle_t c = tts_ws_open_new();
    if (!c) {
        /* 首次失败：等 1s（旧连接 TCP 四次挥手完成 + 任务回收）*/
        ESP_LOGW(TAG, "Pipeline: bg pre-conn first attempt failed, retrying after 1s");
        vTaskDelay(pdMS_TO_TICKS(1000));
        /* 等待期间管线可能已保活了 g_ws_next，不再重试，避免占用任务槽干扰下一轮 IAT */
        if (g_ws_next) {
            ESP_LOGI(TAG, "Pipeline: bg pre-conn retry skipped (ws_next set during wait)");
            vTaskDelete(NULL);
            return;
        }
        c = tts_ws_open_new();
    }
    if (c) {
        if (g_ws_next) {
            /* 竞争：tts_ws_open_new 期间管线保活了 g_ws_next，丢弃刚建的连接 */
            ESP_LOGW(TAG, "Pipeline: bg pre-conn discarding new conn (ws_next already set)");
            esp_websocket_client_stop(c);
            esp_websocket_client_destroy(c);
        } else {
            g_ws_next = c;
            ESP_LOGI(TAG, "Pipeline: bg pre-conn done (heap=%d)", (int)esp_get_free_heap_size());
        }
    } else {
        ESP_LOGW(TAG, "Pipeline: bg pre-conn failed");
    }
    vTaskDelete(NULL);
}

static esp_err_t ensure_ws_connected(void)
{
    if (g_ws_client && esp_websocket_client_is_connected(g_ws_client)) {
        return ESP_OK;  // Already connected
    }
    
    // Create or recreate WebSocket client
    if (g_ws_client) {
        esp_websocket_client_stop(g_ws_client);
        esp_websocket_client_destroy(g_ws_client);
        g_ws_client = NULL;
    }
    
    char *auth_url = build_auth_url_tts();
    if (!auth_url) {
        ESP_LOGE(TAG, "Failed to build auth URL");
        return ESP_FAIL;
    }
    
    esp_websocket_client_config_t ws_cfg = {
        .uri = auth_url,
        .buffer_size = 17 * 1024,
        .task_stack = 6 * 1024,
        .disable_auto_reconnect = true,   /* 合成连接不允许自动重连：
                                           * auto-reconnect 与 stop/destroy 并发会导致
                                           * destroy_and_free_resources 中 pxMutex=NULL 崩溃 */
        .reconnect_timeout_ms = 8000,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 8,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    
    g_ws_client = esp_websocket_client_init(&ws_cfg);
    free(auth_url);
    
    if (!g_ws_client) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        return ESP_FAIL;
    }
    
    esp_websocket_register_events(g_ws_client, WEBSOCKET_EVENT_ANY, tts_ws_event_handler, NULL);

    /* 任务峰值期可能出现 websocket task 创建失败，增加重试与退避 */
    esp_err_t start_ret = ESP_FAIL;
    for (int att = 0; att < 6 && start_ret != ESP_OK; att++) {
        if (att > 0) {
            ESP_LOGW(TAG, "ensure_ws_connected: start retry %d (task peak)", att);
            vTaskDelay(pdMS_TO_TICKS(200 + att * 120));
        }
        start_ret = esp_websocket_client_start(g_ws_client);
    }
    if (start_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client after retries");
        esp_websocket_client_destroy(g_ws_client);
        g_ws_client = NULL;
        return ESP_FAIL;
    }

    // Wait for connection
    int retry = 0;
    while (!esp_websocket_client_is_connected(g_ws_client) && retry < 50) {
        vTaskDelay(pdMS_TO_TICKS(100));
        retry++;
    }
    
    if (!esp_websocket_client_is_connected(g_ws_client)) {
        ESP_LOGE(TAG, "WebSocket connection timeout");
        esp_websocket_client_destroy(g_ws_client);
        g_ws_client = NULL;
        return ESP_ERR_TIMEOUT;
    }
    
    ESP_LOGI(TAG, "Persistent WebSocket ready");
    return ESP_OK;
}

// ========== 公共API ==========

esp_err_t xfyun_tts_init(const xfyun_tts_config_t *config)
{
    if (!config || !config->app_id || !config->api_key || !config->api_secret) {
        ESP_LOGE(TAG, "Invalid config");
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(g_app_id, config->app_id, sizeof(g_app_id) - 1);
    strncpy(g_api_key, config->api_key, sizeof(g_api_key) - 1);
    strncpy(g_api_secret, config->api_secret, sizeof(g_api_secret) - 1);
    
    // Create mutex for WebSocket access
    if (!g_ws_mutex) {
        g_ws_mutex = xSemaphoreCreateMutex();
        if (!g_ws_mutex) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    g_ready = true;
    ESP_LOGI(TAG, "Xunfei TTS initialized with APPID: %s", g_app_id);
    
    // Establish persistent WebSocket connection
    ESP_LOGI(TAG, "Establishing persistent WebSocket connection...");
    esp_err_t ret = ensure_ws_connected();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to establish initial connection, will retry on first synthesis");
    }
    
    return ESP_OK;
}

/* ===== 无锁版 synthesize：假设调用者已持有 g_ws_mutex 且连接已就绪 ===== */
static esp_err_t tts_synthesize_unlocked(const char *text,
                                          const xfyun_tts_params_t *params,
                                          xfyun_tts_audio_callback_t audio_callback,
                                          void *user_ctx,
                                          uint32_t timeout_ms,
                                          char **reuse_buf,
                                          size_t *reuse_cap)
{
    if (!g_ready || !text || !audio_callback) return ESP_ERR_INVALID_ARG;

    // 初始化会话（栈上）
    xfyun_tts_session_t session = {
        .audio_cb = audio_callback,
        .user_ctx = user_ctx,
        .finished = false,
        .connected = true,
        .error = false,
        .sem = xSemaphoreCreateBinary(),
        .audio_count = 0,
        .total_audio_bytes = 0,
        .buffer = *reuse_buf,
        .buffer_len = 0,
        .buffer_capacity = *reuse_cap
    };
    if (!session.sem) return ESP_ERR_NO_MEM;

    /* 首次调用时分配 buffer */
    if (!session.buffer) {
        session.buffer_capacity = 8192;
        session.buffer = malloc(session.buffer_capacity);
        if (!session.buffer) {
            vSemaphoreDelete(session.sem);
            return ESP_ERR_NO_MEM;
        }
    }

    g_active_session = &session;

    // 构建请求 JSON
    cJSON *root = cJSON_CreateObject();
    cJSON *common = cJSON_CreateObject();
    cJSON *business = cJSON_CreateObject();
    cJSON *data_obj = cJSON_CreateObject();

    cJSON_AddStringToObject(common, "app_id", g_app_id);
    cJSON_AddItemToObject(root, "common", common);

    cJSON_AddStringToObject(business, "aue", "raw");
    cJSON_AddStringToObject(business, "auf", "audio/L16;rate=16000");
    cJSON_AddStringToObject(business, "vcn", params && params->voice_name ? params->voice_name : "xiaoyan");
    cJSON_AddNumberToObject(business, "speed", params ? params->speed : 50);
    cJSON_AddNumberToObject(business, "volume", params ? params->volume : 50);
    cJSON_AddNumberToObject(business, "pitch", params ? params->pitch : 50);
    cJSON_AddStringToObject(business, "tte", "UTF8");
    cJSON_AddItemToObject(root, "business", business);

    char *text_b64 = base64_encode((const uint8_t *)text, strlen(text));
    cJSON_AddNumberToObject(data_obj, "status", 2);
    cJSON_AddStringToObject(data_obj, "text", text_b64);
    free(text_b64);
    cJSON_AddItemToObject(root, "data", data_obj);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        g_active_session = NULL;
        *reuse_buf = session.buffer;
        *reuse_cap = session.buffer_capacity;
        vSemaphoreDelete(session.sem);
        return ESP_FAIL;
    }

    int ret = esp_websocket_client_send_text(g_ws_client, json_str, strlen(json_str), pdMS_TO_TICKS(5000));
    free(json_str);

    if (ret < 0) {
        g_active_session = NULL;
        *reuse_buf = session.buffer;
        *reuse_cap = session.buffer_capacity;
        vSemaphoreDelete(session.sem);
        return ESP_FAIL;
    }

    // 等待完成
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_tick = pdMS_TO_TICKS(timeout_ms);
    TickType_t progress_tick = start_tick;
    int last_audio_count = 0;

    while (!session.finished && !session.error && !g_tts_abort) {
        TickType_t elapsed = xTaskGetTickCount() - start_tick;
        if (elapsed >= timeout_tick) break;
        if (session.audio_count != last_audio_count) {
            last_audio_count = session.audio_count;
            progress_tick = xTaskGetTickCount();
        }
        if (session.audio_count == 0 && elapsed >= pdMS_TO_TICKS(TTS_FIRST_AUDIO_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "TTS first audio timeout (%dms), forcing reconnect", TTS_FIRST_AUDIO_TIMEOUT_MS);
            break;
        }
        if (session.audio_count > 0 && (xTaskGetTickCount() - progress_tick) >= pdMS_TO_TICKS(TTS_PROGRESS_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "TTS progress stalled (%dms, chunks=%d), forcing reconnect",
                     TTS_PROGRESS_TIMEOUT_MS, session.audio_count);
            break;
        }
        if (xSemaphoreTake(session.sem, pdMS_TO_TICKS(100)) == pdTRUE) break;
    }

    bool was_aborted = g_tts_abort;

    ESP_LOGI(TAG, "TTS seg done: ok=%d err=%d chunks=%d bytes=%d",
             session.finished, session.error, session.audio_count, (int)session.total_audio_bytes);
    g_last_seg_audio_chunks = session.audio_count;
    g_last_seg_audio_bytes = (int)session.total_audio_bytes;

    g_active_session = NULL;
    /* 把 buffer 归还给调用者复用（不 free） */
    *reuse_buf = session.buffer;
    *reuse_cap = session.buffer_capacity;
    session.buffer = NULL;
    vSemaphoreDelete(session.sem);

    if (was_aborted) return ESP_ERR_NOT_FINISHED;
    if (session.error) return ESP_FAIL;
    if (!session.finished) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

/* 判断字节是否是 UTF-8 多字节字符的首字节（非后续字节）*/
static bool utf8_is_start_byte(unsigned char c)
{
    return (c & 0xC0) != 0x80;
}

/* 在 UTF-8 字符边界处截断，确保 out 以完整字符结尾 */
static size_t utf8_safe_truncate(const char *src, size_t max_bytes)
{
    if (strlen(src) <= max_bytes) return strlen(src);
    size_t pos = max_bytes;
    /* 向前退到最近的 UTF-8 起始字节 */
    while (pos > 0 && !utf8_is_start_byte((unsigned char)src[pos])) {
        pos--;
    }
    return pos;
}

/* 判断字节序列在 pos 处是否是句末标点（UTF-8）
 * 支持：。！？\n 及其全角/ASCII 变体 */
static bool is_sentence_end(const char *s, size_t pos)
{
    unsigned char c = (unsigned char)s[pos];
    /* ASCII 句末 */
    if (c == '\n' || c == '!' || c == '?') return true;
    /* 中文句末标点为 UTF-8 三字节：检查 pos 前有足够空间 */
    if (pos + 2 < strlen(s)) {
        unsigned char b0 = (unsigned char)s[pos];
        unsigned char b1 = (unsigned char)s[pos+1];
        unsigned char b2 = (unsigned char)s[pos+2];
        /* 。= E3 80 82  ！= EF BC 81  ？= EF BC 9F  ；= EF BC 9B  ，= EF BC 8C */
        if (b0==0xE3 && b1==0x80 && b2==0x82) return true; /* 。 */
        if (b0==0xEF && b1==0xBC && b2==0x81) return true; /* ！ */
        if (b0==0xEF && b1==0xBC && b2==0x9F) return true; /* ？ */
        if (b0==0xEF && b1==0xBC && b2==0x9B) return true; /* ；（次要断句点）*/
    }
    return false;
}

// Public API: Split long text at sentence boundaries and synthesize in segments
// 全程持有 g_ws_mutex，段间复用 buffer，减少 mutex/connect/malloc 开销
esp_err_t xfyun_tts_synthesize(const char *text,
                                const xfyun_tts_params_t *params,
                                xfyun_tts_audio_callback_t audio_callback,
                                void *user_ctx,
                                uint32_t timeout_ms)
{
    if (!text || strlen(text) == 0) {
        ESP_LOGE(TAG, "Empty text");
        return ESP_ERR_INVALID_ARG;
    }

    size_t total_len = strlen(text);
    ESP_LOGI(TAG, "TTS synthesis requested for %d bytes text", (int)total_len);

    /* 重置中断标志：上一轮 abort 后残留的 true 不应影响新的合成 */
    g_tts_abort = false;

    /* ---- 全程持有 mutex，仅连接一次 ---- */
    if (!g_ws_mutex) return ESP_FAIL;
    xSemaphoreTake(g_ws_mutex, portMAX_DELAY);

    esp_err_t conn_ret = ensure_ws_connected();
    if (conn_ret != ESP_OK) {
        ESP_LOGE(TAG, "TTS WS connect failed at entry");
        xSemaphoreGive(g_ws_mutex);
        return conn_ret;
    }

    /* 段间复用的音频接收 buffer */
    char  *shared_buf = NULL;
    size_t shared_cap = 0;

/* 单段安全长度：讯飞 TTS 服务端单次合成约 40-45s 上限。
 * 500 字节原文 ≈ 167 个汉字 ≈ 37s，留有足够余量避免服务端强制关闭。 */
#define TTS_SEGMENT_MAX 500

    if (total_len <= TTS_SEGMENT_MAX) {
        /* 足够短，直接合成（已持有 mutex + 连接） */
        esp_err_t ret = tts_synthesize_unlocked(text, params, audio_callback,
                                                 user_ctx, timeout_ms,
                                                 &shared_buf, &shared_cap);

        /* 讯飞 TTS 服务端会在 status=2 后自动关闭连接，必须销毁以免污染后续任务（如 Pipeline）*/
        if (g_ws_client) {
            esp_websocket_client_stop(g_ws_client);
            esp_websocket_client_destroy(g_ws_client);
            g_ws_client = NULL;
        }

        free(shared_buf);
        g_active_session = NULL;
        xSemaphoreGive(g_ws_mutex);
        return ret;
    }

    /* 长文本：按句末标点拆分，每段不超过 TTS_SEGMENT_MAX 字节 */
    ESP_LOGI(TAG, "Long text, splitting into segments (<= %d bytes each)", TTS_SEGMENT_MAX);

    char *seg = malloc(TTS_SEGMENT_MAX + 4); /* 留 3 字节给 UTF-8 溢出 + \0 */
    if (!seg) {
        xSemaphoreGive(g_ws_mutex);
        return ESP_ERR_NO_MEM;
    }

    size_t src_pos   = 0;    /* 当前在原文的位置 */
    size_t seg_start = 0;    /* 当前段的起始位置 */
    int    seg_idx   = 0;
    esp_err_t final_ret = ESP_OK;

    while (src_pos < total_len) {
        /* 计算剩余长度 */
        size_t remaining = total_len - seg_start;

        if (remaining <= TTS_SEGMENT_MAX) {
            /* 最后一段，直接取剩余全部 */
            memcpy(seg, text + seg_start, remaining);
            seg[remaining] = '\0';
            src_pos = total_len;
        } else {
            /* 在 [seg_start, seg_start + TTS_SEGMENT_MAX] 范围内寻找最近的句末标点 */
            size_t window = TTS_SEGMENT_MAX;
            /* 先对齐到 UTF-8 字符边界 */
            window = utf8_safe_truncate(text + seg_start, window);

            /* 从 window 向前扫描，找最后一个句末标点 */
            size_t cut = window;
            bool found = false;
            for (size_t i = window; i > 0; i--) {
                if (utf8_is_start_byte((unsigned char)text[seg_start + i - 1]) &&
                    is_sentence_end(text + seg_start, i - 1)) {
                    /* 包含标点本身：中文标点长度 3，ASCII 长度 1 */
                    unsigned char c = (unsigned char)text[seg_start + i - 1];
                    size_t punct_len = (c >= 0x80) ? 3 : 1;
                    cut = (i - 1) + punct_len;
                    found = true;
                    break;
                }
            }
            if (!found) {
                /* 没找到标点，硬截到 UTF-8 边界 */
                cut = utf8_safe_truncate(text + seg_start, TTS_SEGMENT_MAX);
            }

            memcpy(seg, text + seg_start, cut);
            seg[cut] = '\0';
            seg_start += cut;
            src_pos    = seg_start;
        }

        /* 跳过空段 */
        if (strlen(seg) == 0) continue;

        seg_idx++;
        ESP_LOGI(TAG, "TTS segment %d (%d bytes): %.40s...", seg_idx, (int)strlen(seg), seg);

        /* 检查中断标志 */
        if (g_tts_abort) {
            ESP_LOGW(TAG, "TTS aborted before segment %d", seg_idx);
            final_ret = ESP_ERR_NOT_FINISHED;
            break;
        }

        /* 合成本段（mutex 已持有，连接已就绪，buffer 复用） */
        esp_err_t ret = tts_synthesize_unlocked(seg, params, audio_callback,
                                                 user_ctx, timeout_ms,
                                                 &shared_buf, &shared_cap);
        if (ret == ESP_FAIL) {
            ESP_LOGW(TAG, "Segment %d failed, reconnecting and retrying...", seg_idx);
            /* 在 mutex 内重连 */
            if (g_ws_client) {
                esp_websocket_client_stop(g_ws_client);
                esp_websocket_client_destroy(g_ws_client);
                g_ws_client = NULL;
            }
            vTaskDelay(pdMS_TO_TICKS(300));
            if (ensure_ws_connected() == ESP_OK) {
                ret = tts_synthesize_unlocked(seg, params, audio_callback,
                                               user_ctx, timeout_ms,
                                               &shared_buf, &shared_cap);
            }
        }

        if (ret == ESP_ERR_NOT_FINISHED) {
            /* 用户主动中断 */
            final_ret = ret;
            break;
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "TTS segment %d failed: %s", seg_idx, esp_err_to_name(ret));
            final_ret = ret;
            /* 继续尝试下一段，不中断整个流程 */
        }

        /* 主动预重连：合成完一段后服务器通常会关闭 WebSocket，
         * 趁 ring buffer 中仍有音频在播放时预先建立下一段的连接，
         * 把 TLS 握手与音频播放重叠，消除段间静音 */
        if (src_pos < total_len && !g_tts_abort) {
            vTaskDelay(pdMS_TO_TICKS(50));  /* 等待服务端断连事件到达 */
            if (!g_ws_client || !esp_websocket_client_is_connected(g_ws_client)) {
                ESP_LOGI(TAG, "Pre-reconnect for segment %d (heap=%d)",
                         seg_idx + 1, (int)esp_get_free_heap_size());
                if (g_ws_client) {
                    esp_websocket_client_stop(g_ws_client);
                    esp_websocket_client_destroy(g_ws_client);
                    g_ws_client = NULL;
                }
                ensure_ws_connected();
            }
        } else {
            /* 全部合成结束，销毁不再可复用的旧连接 */
            if (g_ws_client) {
                esp_websocket_client_stop(g_ws_client);
                esp_websocket_client_destroy(g_ws_client);
                g_ws_client = NULL;
            }
        }
    }

    free(seg);
    free(shared_buf);
    g_active_session = NULL;
    xSemaphoreGive(g_ws_mutex);
    return final_ret;
}

// ========== TTS 流水线（Pipeline） ==========

#define TTS_PIPELINE_QUEUE_DEPTH 48  /* 足够容纳 Spark 最长回复的所有句子 */

typedef struct xfyun_tts_pipeline_s {
    QueueHandle_t              queue;
    SemaphoreHandle_t          done_sem;
    TaskHandle_t               task;
    xfyun_tts_params_t         params;
    char                       voice_buf[32];   /* 保存 voice_name 字符串的内存 */
    xfyun_tts_audio_callback_t audio_cb;
    void                      *user_ctx;
    volatile bool              abort;
} xfyun_tts_pipeline_t;

/* 重连失败时把当前批次放回队列头，避免内容丢失 */
static void pipeline_requeue_batch_front(xfyun_tts_pipeline_t *p, char *batch, bool put_sentinel)
{
    if (!p || !batch) return;

    if (put_sentinel) {
        char *s = NULL;
        xQueueSendToFront(p->queue, &s, 0);
    }

    if (xQueueSendToFront(p->queue, &batch, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "Pipeline: failed to requeue batch, dropping as last resort");
        free(batch);
    } else {
        ESP_LOGW(TAG, "Pipeline: requeued failed batch to front for retry");
    }
}

static void tts_pipeline_task(void *arg)
{
    xfyun_tts_pipeline_t *p = (xfyun_tts_pipeline_t *)arg;

    char  *shared_buf = NULL;
    size_t shared_cap = 0;

    /* 提前预连接：pipeline 启动时立即建立 TTS WS，与 Spark 请求/响应并行，缩短首句延迟 */
    g_tts_abort = false;
    xSemaphoreTake(g_ws_mutex, portMAX_DELAY);
    /* 优先复用上一轮管线结束时保留的预建连接（避免重新 TLS 握手）*/
    if (!g_ws_client && g_ws_next && esp_websocket_client_is_connected(g_ws_next)) {
        esp_websocket_register_events(g_ws_next, WEBSOCKET_EVENT_ANY, tts_ws_event_handler, NULL);
        g_ws_client = g_ws_next;
        g_ws_next   = NULL;
        ESP_LOGI(TAG, "Pipeline: reusing kept pre-conn from last round");
    }
    bool connected = (ensure_ws_connected() == ESP_OK);
    if (connected) {
        ESP_LOGI(TAG, "Pipeline: TTS pre-connected at start (heap=%d)", (int)esp_get_free_heap_size());
    } else {
        ESP_LOGW(TAG, "Pipeline: TTS pre-connect failed, will retry on first sentence");
    }

    /* TTS API 单次请求上限约 8000 字节；批量缓冲取 4000 字节，留有余量 */
#define TTS_BATCH_BUF_SIZE  500

    while (!p->abort) {
        /* --- 步骤 1：阻塞等待第一个句子到来（35s 超时） --- */
        char *sentence = NULL;
        if (xQueueReceive(p->queue, &sentence, pdMS_TO_TICKS(35000)) != pdTRUE) {
            ESP_LOGW(TAG, "Pipeline queue timeout");
            break;
        }
        if (!sentence) break;   /* NULL 哨兵 → 正常结束 */

        /* --- 步骤 2：积累 Spark token 直到内容足够或超时，再合并发送 ---
         * 策略：满足任意一个条件才发出，兼顾低延迟与稳定性：
         *  a) 遇到 NULL 哨兵（Spark 已结束）
         *  b) batch 内容已 >= 64 字节
         *  c) 等待超过 550ms */
        char   *batch     = (char *)malloc(TTS_BATCH_BUF_SIZE);
        bool    sentinel  = false;   /* 本批中遇到 NULL 哨兵则提前终止外层循环 */
        size_t  batch_len = 0;

        if (batch) {
            /* 先放入第一句 */
            size_t slen = strlen(sentence);
            if (slen < TTS_BATCH_BUF_SIZE - 1) {
                memcpy(batch, sentence, slen);
                batch_len = slen;
            }
            free(sentence);
            sentence = NULL;

            /* 监听队列，适当等待组合更多碎片，但不应等太久导致第一句首字延迟 */
            TickType_t batch_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(20); 
            while (!sentinel && batch_len < 10 && batch_len < TTS_BATCH_BUF_SIZE - 64) {
                TickType_t now    = xTaskGetTickCount();
                TickType_t remain = (now < batch_deadline) ? (batch_deadline - now) : 0;
                if (remain == 0) break;
                char *more = NULL;
                if (xQueueReceive(p->queue, &more, remain) != pdTRUE) break;
                if (!more) { sentinel = true; break; }
                size_t mlen = strlen(more);
                if (batch_len + mlen < TTS_BATCH_BUF_SIZE - 1) {
                    memcpy(batch + batch_len, more, mlen);
                    batch_len += mlen;
                }
                free(more);
            }

            /* 内容已 >= 60 字节但还没到哨兵，继续非阻塞排空已就绪的队列条目 */
            if (!sentinel) {
                char *more = NULL;
                while (batch_len < TTS_BATCH_BUF_SIZE - 64 &&
                       xQueueReceive(p->queue, &more, 0) == pdTRUE) {
                    if (!more) { sentinel = true; break; }
                    size_t mlen = strlen(more);
                    if (batch_len + mlen < TTS_BATCH_BUF_SIZE - 1) {
                        memcpy(batch + batch_len, more, mlen);
                        batch_len += mlen;
                    }
                    free(more);
                    more = NULL;
                }
            }
            batch[batch_len] = '\0';
        } else {
            /* 分配失败：降级为单句处理 */
            batch = sentence;
            sentence = NULL;
            batch_len = batch ? strlen(batch) : 0;
        }

        if (batch_len == 0) { free(batch); if (sentinel) break; continue; }

        ESP_LOGI(TAG, "Pipeline batch: %d bytes to TTS", (int)batch_len);

        /* --- 步骤 3：确保 TTS WS 连接就绪 ---
         * 优先使用 pipeline 启动时预建的连接，或上一轮 post-loop bg pre-conn 建的连接。
         * 批次间重连（多批次回复）走 ensure_ws_connected()，不等待 bg pre-conn
         *（bg pre-conn 只在 post-loop 启动，批次间不可能有进行中的 bg pre-conn）。 */
        if (!g_ws_client || !esp_websocket_client_is_connected(g_ws_client)) {
            if (g_ws_client) {
                esp_websocket_client_stop(g_ws_client);
                esp_websocket_client_destroy(g_ws_client);
                g_ws_client = NULL;
            }
            /* 立即检查 g_ws_next（无等待：批次间不会有新的 bg pre-conn 在建） */
            if (g_ws_next && esp_websocket_client_is_connected(g_ws_next)) {
                esp_websocket_register_events(g_ws_next, WEBSOCKET_EVENT_ANY,
                                              tts_ws_event_handler, NULL);
                g_ws_client = g_ws_next;
                g_ws_next   = NULL;
                connected   = true;
                ESP_LOGI(TAG, "Pipeline: using pre-built conn for batch (heap=%d)",
                         (int)esp_get_free_heap_size());
            } else {
                if (g_ws_next) {
                    esp_websocket_client_stop(g_ws_next);
                    esp_websocket_client_destroy(g_ws_next);
                    g_ws_next = NULL;
                }
                ESP_LOGI(TAG, "Pipeline: TTS WS reconnect before batch (heap=%d)",
                         (int)esp_get_free_heap_size());
                if (ensure_ws_connected() != ESP_OK) {
                    ESP_LOGE(TAG, "Pipeline: TTS WS reconnect failed, keep batch for retry");
                    pipeline_requeue_batch_front(p, batch, sentinel);
                    batch = NULL;
                    vTaskDelay(pdMS_TO_TICKS(250));
                    continue;
                }
                connected = true;
            }
        }

        /* --- 步骤 4：合成批量文本 ---
         * 注意：不再在合成期间并行建 TLS 连接，原因是并行握手会触发
         * "esp-aes: Failed to allocate memory"，导致第二批音频失败。
         * 新策略：合成结束/旧连接关闭后再启动 bg pre-conn，AES 硬件已空闲。 */
        esp_err_t ret = tts_synthesize_unlocked(batch, &p->params, p->audio_cb,
                                                 p->user_ctx, TTS_PIPE_BATCH_TIMEOUT_MS,
                                                 &shared_buf, &shared_cap);

        if (ret == ESP_ERR_TIMEOUT && g_last_seg_audio_chunks > 0) {
            ESP_LOGW(TAG, "Pipeline batch partial audio (%d chunks), skip retry to avoid long stall",
                     g_last_seg_audio_chunks);
            ret = ESP_OK;
        }

        /* 合成失败时重试一次：重建连接后再试，避免第二批静默丢失 */
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FINISHED && !p->abort) {
            ESP_LOGW(TAG, "Pipeline batch failed, retrying once with fresh conn...");
            if (g_ws_client) {
                esp_websocket_client_stop(g_ws_client);
                esp_websocket_client_destroy(g_ws_client);
                g_ws_client = NULL;
            }
            if (g_ws_next && esp_websocket_client_is_connected(g_ws_next)) {
                esp_websocket_register_events(g_ws_next, WEBSOCKET_EVENT_ANY,
                                              tts_ws_event_handler, NULL);
                g_ws_client = g_ws_next;
                g_ws_next   = NULL;
                ESP_LOGI(TAG, "Pipeline: retry using pre-conn");
            } else {
                ensure_ws_connected();
            }
            if (g_ws_client) {
                ret = tts_synthesize_unlocked(batch, &p->params, p->audio_cb,
                                              p->user_ctx, TTS_PIPE_BATCH_TIMEOUT_MS,
                                              &shared_buf, &shared_cap);
                if (ret == ESP_ERR_TIMEOUT && g_last_seg_audio_chunks > 0) {
                    ESP_LOGW(TAG, "Pipeline batch retry partial audio (%d chunks), continue",
                             g_last_seg_audio_chunks);
                    ret = ESP_OK;
                }
                if (ret == ESP_OK) ESP_LOGI(TAG, "Pipeline batch retry succeeded");
                else ESP_LOGE(TAG, "Pipeline batch retry also failed");
            }
        }
        free(batch);

        if (ret == ESP_ERR_NOT_FINISHED) {
            ESP_LOGW(TAG, "Pipeline TTS aborted mid-batch");
            /* abort 路径：异步关闭脏连接，防止后续复用引发 Buffer overflow */
            if (g_ws_client) {
                if (xTaskCreate(tts_close_old_task, "tts_old_cls", 3072,
                                g_ws_client, 2, NULL) != pdPASS) {
                    esp_websocket_client_stop(g_ws_client);
                    esp_websocket_client_destroy(g_ws_client);
                }
                g_ws_client = NULL;
            }
            break;
        }
        if (ret == ESP_FAIL || ret == ESP_ERR_TIMEOUT) {
            if (ret == ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "Pipeline batch timeout (status=2 not received), closing conn");
            } else {
                ESP_LOGW(TAG, "Pipeline batch failed, will reconnect before next batch");
            }
            /* 合成连接可能处于未定状态，异步关闭防止复用脏连接 */
            if (g_ws_client) {
                if (xTaskCreate(tts_close_old_task, "tts_old_cls", 3072,
                                g_ws_client, 2, NULL) != pdPASS) {
                    esp_websocket_client_stop(g_ws_client);
                    esp_websocket_client_destroy(g_ws_client);
                }
                g_ws_client = NULL;
            }
            if (sentinel) break;
            continue;
        }

        if (sentinel) break;  /* 哨兵已在本批次内被消费，退出循环 */

        /* --- 步骤 5：关闭旧连接 ---
         * 合成已结束，AES 硬件空闲。
         * bg pre-conn 在 while 循环结束后（post-loop）统一启动一次，
         * 避免 sentinel 在 batch 内被消费时 step5 + post-loop 双重 launch。 */
        if (!p->abort) {
            /* 后台异步关闭旧连接 */
            if (g_ws_client) {
                if (xTaskCreate(tts_close_old_task, "tts_old_cls", 3072,
                                g_ws_client, 2, NULL) != pdPASS) {
                    esp_websocket_client_stop(g_ws_client);
                    esp_websocket_client_destroy(g_ws_client);
                }
                g_ws_client = NULL;
            }
        }
    }

    /* 排空剩余队列 */
    char *leftover;
    while (xQueueReceive(p->queue, &leftover, 0) == pdTRUE) {
        if (leftover) free(leftover);
    }

    /* 清理后台预建的备用连接（若有残留）*/
    if (g_ws_next) {
        esp_websocket_client_stop(g_ws_next);
        esp_websocket_client_destroy(g_ws_next);
        g_ws_next = NULL;
    }

    /* 清理主连接：讯飞 TTS 服务端在每次合成结束（status=2）后即关闭连接，
     * 已使用的合成连接不可跨轮复用（复用会导致发送请求后服务端无回应、60s 超时、
     * 然后 stop/destroy 与 auto-reconnect 并发引发 pxMutex=NULL 崩溃）。
     * 总是异步关闭旧连接，并由预建任务为下一轮建立全新连接。 */
    if (g_ws_client) {
        if (xTaskCreate(tts_close_old_task, "tts_old_cls2", 3072,
                        g_ws_client, 2, NULL) != pdPASS) {
            esp_websocket_client_stop(g_ws_client);
            esp_websocket_client_destroy(g_ws_client);
        }
        g_ws_client = NULL;
    }

    /* 非 abort 退出：为下一轮对话预建全新 TTS 连接（AES 此时已空闲） */
    if (!p->abort && !g_ws_next) {
        xTaskCreate(tts_preconn_bg_task, "tts_pre_bg", 4096, NULL, 4, NULL);
    }

    free(shared_buf);
    g_active_session = NULL;
    xSemaphoreGive(g_ws_mutex);  /* 无论预连接是否成功，mutex 均在 pipeline 启动时取得 */
    xSemaphoreGive(p->done_sem);
    p->task = NULL;
    vTaskDelete(NULL);
}

esp_err_t xfyun_tts_pipeline_start(const xfyun_tts_params_t *params,
                                    xfyun_tts_audio_callback_t audio_cb,
                                    void *user_ctx,
                                    xfyun_tts_pipeline_handle_t *out_handle)
{
    if (!out_handle || !audio_cb) return ESP_ERR_INVALID_ARG;

    xfyun_tts_pipeline_t *p = (xfyun_tts_pipeline_t *)calloc(1, sizeof(*p));
    if (!p) return ESP_ERR_NO_MEM;

    p->queue    = xQueueCreate(TTS_PIPELINE_QUEUE_DEPTH, sizeof(char *));
    p->done_sem = xSemaphoreCreateBinary();
    if (!p->queue || !p->done_sem) {
        if (p->queue)    vQueueDelete(p->queue);
        if (p->done_sem) vSemaphoreDelete(p->done_sem);
        free(p);
        return ESP_ERR_NO_MEM;
    }

    if (params) {
        p->params = *params;
        if (params->voice_name) {
            strncpy(p->voice_buf, params->voice_name, sizeof(p->voice_buf) - 1);
            p->params.voice_name = p->voice_buf;
        }
    }
    p->audio_cb = audio_cb;
    p->user_ctx = user_ctx;
    p->abort    = false;

    /* 任务峰值期 xTaskCreate 可能失败：先尝试 pinned 6KB，再降级到普通任务 5KB */
    BaseType_t rc = pdFAIL;
    for (int att = 0; att < 3 && rc != pdPASS; att++) {
        if (att > 0) {
            ESP_LOGW(TAG, "tts_pipeline_start: task create retry %d", att);
            vTaskDelay(pdMS_TO_TICKS(350));
        }
        rc = xTaskCreatePinnedToCore(tts_pipeline_task, "tts_pipe",
                                     6 * 1024, p, 6, &p->task, 0);
    }
    for (int att = 0; att < 2 && rc != pdPASS; att++) {
        ESP_LOGW(TAG, "tts_pipeline_start: fallback create attempt %d", att + 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        rc = xTaskCreate(tts_pipeline_task, "tts_pipe",
                         6 * 1024, p, 6, &p->task);
    }
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "TTS pipeline task create failed after retries");
        vQueueDelete(p->queue);
        vSemaphoreDelete(p->done_sem);
        free(p);
        return ESP_FAIL;
    }

    *out_handle = (xfyun_tts_pipeline_handle_t)p;
    ESP_LOGI(TAG, "TTS pipeline started");
    return ESP_OK;
}

esp_err_t xfyun_tts_pipeline_feed(xfyun_tts_pipeline_handle_t handle, const char *sentence)
{
    xfyun_tts_pipeline_t *p = (xfyun_tts_pipeline_t *)handle;
    if (!p || !sentence || strlen(sentence) == 0) return ESP_ERR_INVALID_ARG;

    char *copy = strdup(sentence);
    if (!copy) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Pipeline feed: %d bytes: %.40s...", (int)strlen(sentence), sentence);

    if (xQueueSend(p->queue, &copy, pdMS_TO_TICKS(5000)) != pdTRUE) {
        free(copy);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t xfyun_tts_pipeline_finish(xfyun_tts_pipeline_handle_t handle, uint32_t wait_ms, volatile bool *abort_flag)
{
    xfyun_tts_pipeline_t *p = (xfyun_tts_pipeline_t *)handle;
    if (!p) return ESP_ERR_INVALID_ARG;

    /* 发送 NULL 哨兵告知流水线结束 */
    char *sentinel = NULL;
    xQueueSend(p->queue, &sentinel, pdMS_TO_TICKS(2000));

    /* 周期性等待任务退出，以便响应中断指针 */
    esp_err_t ret = ESP_ERR_TIMEOUT;
    uint32_t step = 50; 
    uint32_t waited = 0;
    bool aborted_by_flag = false;

    if (!p->task) {
        ret = ESP_OK;
    } else {
        while (wait_ms == portMAX_DELAY || waited < wait_ms) {
            if (!aborted_by_flag && abort_flag && *abort_flag) {
                ESP_LOGW(TAG, "Pipeline finish interrupted by abort_flag");
                xfyun_tts_pipeline_abort(handle);
                aborted_by_flag = true;
                // 继续循环，等待后段任务确认退出并给出 done_sem
            }
            if (xSemaphoreTake(p->done_sem, pdMS_TO_TICKS(step)) == pdTRUE) {
                ret = ESP_OK;
                break;
            }
            if (wait_ms != portMAX_DELAY) {
                waited += step;
            }
        }
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Pipeline finish timeout, dirty abort");
        xfyun_tts_pipeline_abort(handle);
        return ret; // 脏退出，不可释放 p 否则可能导致后台任务崩溃
    }

    vQueueDelete(p->queue);
    vSemaphoreDelete(p->done_sem);
    free(p);
    ESP_LOGI(TAG, "TTS pipeline finished %s safely", aborted_by_flag ? "via abort" : "normally");
    return aborted_by_flag ? ESP_ERR_TIMEOUT : ESP_OK;
}

void xfyun_tts_pipeline_abort(xfyun_tts_pipeline_handle_t handle)
{
    xfyun_tts_pipeline_t *p = (xfyun_tts_pipeline_t *)handle;
    if (!p) return;
    p->abort = true;
    /* 阶段3（合成中）需要中断当前 TTS 段 */
    xfyun_tts_abort();
    /* 阶段1（积累中）需要唤醒队列等待 → 发哨兵 */
    char *sentinel = NULL;
    xQueueSend(p->queue, &sentinel, 0);
    ESP_LOGW(TAG, "TTS pipeline abort requested");
}

// ========== 连接管理 ==========

static void tts_async_close_task(void *arg) {
    esp_websocket_client_handle_t client = (esp_websocket_client_handle_t)arg;
    if (client) {
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
    }
    vTaskDelete(NULL);
}

void xfyun_tts_disconnect(void)
{
    if (!g_ws_mutex) return;

    xSemaphoreTake(g_ws_mutex, portMAX_DELAY);

    if (g_ws_client) {
        ESP_LOGI(TAG, "Disconnecting persistent TTS WebSocket to free memory... (async)");
        xTaskCreate(tts_async_close_task, "tts_ws_close", 4096, g_ws_client, 5, NULL);
        g_ws_client = NULL;
    }

    if (g_ws_next) {
        ESP_LOGI(TAG, "Disconnecting queued TTS WebSocket to free memory... (async)");
        xTaskCreate(tts_async_close_task, "tts_ws_close_n", 4096, g_ws_next, 5, NULL);
        g_ws_next = NULL;
    }

    xSemaphoreGive(g_ws_mutex);
}

esp_err_t xfyun_tts_reconnect(void)
{
    if (!g_ws_mutex) return ESP_FAIL;
    
    xSemaphoreTake(g_ws_mutex, portMAX_DELAY);
    
    esp_err_t ret = ESP_OK;
    if (!g_ws_client) {
        ESP_LOGI(TAG, "Reconnecting persistent TTS WebSocket...");
        ret = ensure_ws_connected();
    }
    
    xSemaphoreGive(g_ws_mutex);
    return ret;
}

void xfyun_tts_abort(void)
{
    ESP_LOGW(TAG, "TTS abort requested");
    g_tts_abort = true;
    // 唤醒等待中的信号量
    if (g_active_session && g_active_session->sem) {
        xSemaphoreGive(g_active_session->sem);
    }
}
