#include "xfyun_image.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "cJSON.h"

#define TAG "XFYUN_IMAGE"

static void image_ws_async_destroy_task(void *arg) {
    esp_websocket_client_handle_t client = (esp_websocket_client_handle_t)arg;
    if (client) {
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
    }
    vTaskDelete(NULL);
}

static void async_destroy_image_client(esp_websocket_client_handle_t client) {
    if (client) {
        xTaskCreate(image_ws_async_destroy_task, "img_ws_close", 4096, client, 5, NULL);
    }
}


/* 图片理解服务端点 */
#define IMAGE_HOST   "spark-api.cn-huabei-1.xf-yun.com"
#define IMAGE_PATH   "/v2.1/image"
#define IMAGE_WS_URL "wss://spark-api.cn-huabei-1.xf-yun.com/v2.1/image"

static char g_app_id[64];
static char g_api_key[96];
static char g_api_secret[128];
static bool g_ready = false;

/* ===== abort 支持 ===== */
static volatile bool g_image_abort = false;

typedef struct image_session_s image_session_t;  /* 前置声明 */
static image_session_t *g_active_image_sess = NULL;

/* ===== 内部会话结构 ===== */
typedef struct image_session_s {
    xfyun_image_chunk_cb_t chunk_cb;
    void  *user_ctx;
    char  *text;          /* 累积的回复文本 */
    size_t len;
    size_t cap;
    char  *recv_buf;      /* WebSocket 分片帧累积缓冲 */
    size_t recv_len;
    size_t recv_cap;
    int    error_code;
    bool   finished;
    bool   notified;
    SemaphoreHandle_t sem;
} image_session_t;  /* 注：typedef 与前置声明一致 */

/* ===== 工具函数（与 xfyun_spark.c 相同签名逻辑）===== */

static char *base64_encode_data(const uint8_t *data, size_t len)
{
    size_t out_len = 0;
    mbedtls_base64_encode(NULL, 0, &out_len, data, len);
    char *out = (char *)malloc(out_len + 1);
    if (!out) return NULL;
    if (mbedtls_base64_encode((unsigned char *)out, out_len, &out_len, data, len) != 0) {
        free(out);
        return NULL;
    }
    out[out_len] = '\0';
    return out;
}

static esp_err_t hmac_sha256_base64(const char *key, const char *input, char **out_b64)
{
    unsigned char digest[32];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return ESP_FAIL;

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    esp_err_t ret = ESP_FAIL;
    do {
        if (mbedtls_md_setup(&ctx, info, 1) != 0) break;
        if (mbedtls_md_hmac_starts(&ctx, (const unsigned char *)key, strlen(key)) != 0) break;
        if (mbedtls_md_hmac_update(&ctx, (const unsigned char *)input, strlen(input)) != 0) break;
        if (mbedtls_md_hmac_finish(&ctx, digest) != 0) break;
        char *encoded = base64_encode_data(digest, sizeof(digest));
        if (!encoded) break;
        *out_b64 = encoded;
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
    char *p = out;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            *p++ = c;
        } else {
            *p++ = '%'; *p++ = hex[c >> 4]; *p++ = hex[c & 0x0F];
        }
    }
    *p = '\0';
    return out;
}

static esp_err_t build_signed_url(char **out_url)
{
    time_t now = 0;
    time(&now);
    struct tm gmt = {0};
    gmtime_r(&now, &gmt);
    char date_buf[64];
    strftime(date_buf, sizeof(date_buf), "%a, %d %b %Y %H:%M:%S GMT", &gmt);

    char sig_origin[256];
    snprintf(sig_origin, sizeof(sig_origin),
             "host: %s\ndate: %s\nGET %s HTTP/1.1",
             IMAGE_HOST, date_buf, IMAGE_PATH);

    char *sig_b64 = NULL;
    if (hmac_sha256_base64(g_api_secret, sig_origin, &sig_b64) != ESP_OK) return ESP_FAIL;

    char auth_origin[512];
    snprintf(auth_origin, sizeof(auth_origin),
             "api_key=\"%s\", algorithm=\"hmac-sha256\", headers=\"host date request-line\", signature=\"%s\"",
             g_api_key, sig_b64);
    free(sig_b64);

    char *auth_b64 = base64_encode_data((const uint8_t *)auth_origin, strlen(auth_origin));
    if (!auth_b64) return ESP_ERR_NO_MEM;

    char *date_enc = url_encode(date_buf);
    char *auth_enc = url_encode(auth_b64);
    free(auth_b64);

    if (!date_enc || !auth_enc) { free(date_enc); free(auth_enc); return ESP_ERR_NO_MEM; }

    asprintf(out_url, "%s?authorization=%s&date=%s&host=%s",
             IMAGE_WS_URL, auth_enc, date_enc, IMAGE_HOST);
    free(date_enc);
    free(auth_enc);
    return *out_url ? ESP_OK : ESP_ERR_NO_MEM;
}

/* ===== 会话辅助 ===== */

static bool append_text(image_session_t *s, const char *frag)
{
    if (!frag || frag[0] == '\0') return true;
    
    size_t need = strlen(frag);
    if (s->len + need + 1 > s->cap) {
        size_t new_cap = (s->cap == 0 ? 512 : s->cap * 2);
        while (new_cap < s->len + need + 1) new_cap *= 2;
        char *nb = (char *)realloc(s->text, new_cap);
        if (!nb) return false;
        s->text = nb; s->cap = new_cap;
    }
    memcpy(s->text + s->len, frag, need);
    s->len += need;
    s->text[s->len] = '\0';

    if (s->chunk_cb) {
        s->chunk_cb(s->text, false, s->user_ctx);
    }

    return true;
}

static void signal_done(image_session_t *s)
{
    if (!s->finished) s->finished = true;
    if (s->sem && !s->notified) {
        s->notified = true;
        xSemaphoreGive(s->sem);
    }
}

/* ===== WebSocket 事件处理 ===== */

static void image_ws_event_handler(void *args, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    if (!g_active_image_sess || args != g_active_image_sess) return;
    image_session_t *s = (image_session_t *)args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected");
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket disconnected");
            signal_done(s);
            break;
        case WEBSOCKET_EVENT_DATA: {
            if (!data || !data->data_len || !data->data_ptr) break;

            /* ---- 分片帧累积 ----
             * data->payload_offset : 本块在完整消息中的起始偏移
             * data->payload_len    : 完整消息总长度
             * data->data_len       : 本块长度
             */
            if (data->payload_offset == 0) {
                /* 新消息开始，重置累积缓冲 */
                s->recv_len = 0;
            }
            /* 扩容 */
            size_t need = s->recv_len + data->data_len + 1;
            if (need > s->recv_cap) {
                size_t nc = s->recv_cap ? s->recv_cap * 2 : 4096;
                while (nc < need) nc *= 2;
                char *nb = (char *)realloc(s->recv_buf, nc);
                if (!nb) { s->error_code = -1; signal_done(s); break; }
                s->recv_buf = nb; s->recv_cap = nc;
            }
            memcpy(s->recv_buf + s->recv_len, data->data_ptr, data->data_len);
            s->recv_len += data->data_len;

            /* 仅当完整消息接收完毕再解析 */
            if ((int)(data->payload_offset + data->data_len) < (int)data->payload_len) {
                break;  /* 还有更多分片，继续等待 */
            }
            s->recv_buf[s->recv_len] = '\0';

            char *json_str = s->recv_buf;  /* 直接使用累积缓冲，不再额外 malloc */

            cJSON *root = cJSON_Parse(json_str);
            /* 注：json_str 指向 s->recv_buf，生命期由 sess 管理，无需 free */
            if (!root) break;

            /* 检查错误码 */
            cJSON *header = cJSON_GetObjectItem(root, "header");
            cJSON *code   = header ? cJSON_GetObjectItem(header, "code") : NULL;
            if (code && cJSON_IsNumber(code) && code->valueint != 0) {
                s->error_code = code->valueint;
                ESP_LOGE(TAG, "Image API error code: %d", s->error_code);
                cJSON_Delete(root);
                signal_done(s);
                break;
            }

            /* 提取回复文本 */
            cJSON *payload  = cJSON_GetObjectItem(root, "payload");
            cJSON *choices  = payload ? cJSON_GetObjectItem(payload, "choices") : NULL;
            cJSON *ch_status = choices ? cJSON_GetObjectItem(choices, "status") : NULL;
            cJSON *text_arr = choices ? cJSON_GetObjectItem(choices, "text")   : NULL;
            if (text_arr && cJSON_IsArray(text_arr)) {
                int cnt = cJSON_GetArraySize(text_arr);
                for (int i = 0; i < cnt; ++i) {
                    cJSON *item    = cJSON_GetArrayItem(text_arr, i);
                    cJSON *content = item ? cJSON_GetObjectItem(item, "content") : NULL;
                    if (content && cJSON_IsString(content)) {
                        if (!append_text(s, content->valuestring)) {
                            s->error_code = -2;
                            cJSON_Delete(root);
                            signal_done(s);
                            return;
                        }
                    }
                }
            }

            /* status==2 表示最后一帧 */
            if (ch_status && cJSON_IsNumber(ch_status) && ch_status->valueint == 2) {
                cJSON_Delete(root);
                signal_done(s);
                break;
            }

            cJSON_Delete(root);
            break;
        }
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            s->error_code = -3;
            signal_done(s);
            break;
        default:
            break;
    }
}

/* ===== 构造请求 JSON ===== */

static char *build_request_json(const char *image_b64, const char *question)
{
    cJSON *root = cJSON_CreateObject();

    /* header */
    cJSON *header = cJSON_AddObjectToObject(root, "header");
    cJSON_AddStringToObject(header, "app_id", g_app_id);
    cJSON_AddStringToObject(header, "uid", "esp32s3");

    /* parameter */
    cJSON *parameter = cJSON_AddObjectToObject(root, "parameter");
    cJSON *chat = cJSON_AddObjectToObject(parameter, "chat");
    cJSON_AddStringToObject(chat, "domain", "imagev3");
    cJSON_AddNumberToObject(chat, "temperature", 0.5);
    cJSON_AddNumberToObject(chat, "top_k", 4);
    cJSON_AddNumberToObject(chat, "max_tokens", 512);

    /* payload */
    cJSON *payload = cJSON_AddObjectToObject(root, "payload");
    cJSON *message = cJSON_AddObjectToObject(payload, "message");
    cJSON *text    = cJSON_AddArrayToObject(message, "text");

    /* 图片条目（array 第一个必须是图片）*/
    cJSON *img_item = cJSON_CreateObject();
    cJSON_AddStringToObject(img_item, "role", "user");
    cJSON_AddStringToObject(img_item, "content", image_b64);
    cJSON_AddStringToObject(img_item, "content_type", "image");
    cJSON_AddItemToArray(text, img_item);

    /* 问题条目 */
    cJSON *q_item = cJSON_CreateObject();
    cJSON_AddStringToObject(q_item, "role", "user");
    cJSON_AddStringToObject(q_item, "content", question ? question : "请描述这张图片的内容");
    cJSON_AddItemToArray(text, q_item);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

/* ===== 公开 API ===== */

esp_err_t xfyun_image_init(const xfyun_image_config_t *config)
{
    if (!config || !config->app_id || !config->api_key || !config->api_secret) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(g_app_id,    sizeof(g_app_id),    "%s", config->app_id);
    snprintf(g_api_key,   sizeof(g_api_key),   "%s", config->api_key);
    snprintf(g_api_secret,sizeof(g_api_secret),"%s", config->api_secret);
    g_ready = true;
    ESP_LOGI(TAG, "Image Understanding client initialized");
    return ESP_OK;
}


esp_websocket_client_handle_t xfyun_image_preconnect(void)
{
    if (!g_ready) return NULL;
    char *signed_url = NULL;
    if (build_signed_url(&signed_url) != ESP_OK) return NULL;

    esp_websocket_client_config_t ws_cfg = {
        .uri               = signed_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 17 * 1024,
        .task_stack = 6 * 1024,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms   = 10000,
    };
    esp_websocket_client_handle_t client = esp_websocket_client_init(&ws_cfg);
    free(signed_url);
    if (!client) return NULL;

    esp_websocket_client_start(client);
    ESP_LOGI(TAG, "Image pre-connect started");
    return client;
}

esp_err_t xfyun_image_understand_with_conn(esp_websocket_client_handle_t client,
                                   const uint8_t *image_data, size_t image_len,
                                   const char *question,
                                   xfyun_image_chunk_cb_t chunk_cb, void *user_ctx,
                                   char **out_reply, uint32_t timeout_ms)
{
    if (!client || !g_ready || !out_reply || !image_data || image_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_reply = NULL;

    ESP_LOGI(TAG, "Encoding image (%d bytes) to base64...", (int)image_len);
    char *img_b64 = base64_encode_data(image_data, image_len);
    if (!img_b64) {
        ESP_LOGE(TAG, "base64 encode failed (OOM)");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Base64 size: %d bytes", (int)strlen(img_b64));

    image_session_t sess = {
        .chunk_cb   = chunk_cb,
        .user_ctx   = user_ctx,
        .text       = NULL,
        .len        = 0,
        .cap        = 0,
        .recv_buf   = NULL,
        .recv_len   = 0,
        .recv_cap   = 0,
        .error_code = 0,
        .finished   = false,
        .notified   = false,
        .sem        = xSemaphoreCreateBinary(),
    };
    if (!sess.sem) { free(img_b64); return ESP_ERR_NO_MEM; }

    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, image_ws_event_handler, &sess);

    g_active_image_sess = &sess;
    g_image_abort = false;

    // wait for connection
    bool connected = false;
    for (int i = 0; i < 50; i++) {
        if (esp_websocket_client_is_connected(client)) {
            connected = true;
            break;
        }
        if (g_image_abort) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!connected || g_image_abort) {
        ESP_LOGE(TAG, "Image WS connect failed or aborted");
        async_destroy_image_client(client);
        free(img_b64);
        vSemaphoreDelete(sess.sem);
        g_active_image_sess = NULL;
        return ESP_FAIL;
    }

    char *req_json = build_request_json(img_b64, question);
    free(img_b64);
    if (!req_json) {
        async_destroy_image_client(client);
        vSemaphoreDelete(sess.sem);
        g_active_image_sess = NULL;
        return ESP_ERR_NO_MEM;
    }

    int sent = esp_websocket_client_send_text(client, req_json, strlen(req_json), portMAX_DELAY);
    free(req_json);

    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send image JSON");
        async_destroy_image_client(client);
        vSemaphoreDelete(sess.sem);
        g_active_image_sess = NULL;
        return ESP_FAIL;
    }

    xSemaphoreTake(sess.sem, pdMS_TO_TICKS(timeout_ms));
    g_active_image_sess = NULL;

    if (!sess.notified) {
        ESP_LOGW(TAG, "Image receive timeout");
    }

    async_destroy_image_client(client);

    esp_err_t final_ret = ESP_OK;
    if (sess.error_code != 0) {
        final_ret = ESP_FAIL;
    } else if (sess.text) {
        *out_reply = sess.text;
    } else {
        final_ret = ESP_FAIL;
    }

    if (sess.recv_buf) free(sess.recv_buf);
    vSemaphoreDelete(sess.sem);
    g_active_image_sess = NULL;
    return final_ret;
}

esp_err_t xfyun_image_understand(const uint8_t *image_data, size_t image_len,
                                   const char *question,
                                   xfyun_image_chunk_cb_t chunk_cb, void *user_ctx,
                                   char **out_reply, uint32_t timeout_ms)
{
    if (!g_ready || !out_reply || !image_data || image_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_reply = NULL;

    ESP_LOGI(TAG, "Encoding image (%d bytes) to base64...", (int)image_len);
    char *img_b64 = base64_encode_data(image_data, image_len);
    if (!img_b64) {
        ESP_LOGE(TAG, "base64 encode failed (OOM)");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Base64 size: %d bytes", (int)strlen(img_b64));

    char *signed_url = NULL;
    esp_err_t ret = build_signed_url(&signed_url);
    if (ret != ESP_OK) { free(img_b64); return ret; }

    image_session_t sess = {
        .chunk_cb   = chunk_cb,
        .user_ctx   = user_ctx,
        .text       = NULL,
        .len        = 0,
        .cap        = 0,
        .recv_buf   = NULL,
        .recv_len   = 0,
        .recv_cap   = 0,
        .error_code = 0,
        .finished   = false,
        .notified   = false,
        .sem        = xSemaphoreCreateBinary(),
    };
    if (!sess.sem) { free(img_b64); free(signed_url); return ESP_ERR_NO_MEM; }

    esp_websocket_client_config_t ws_cfg = {
        .uri               = signed_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 17 * 1024,          /* 足够容纳较大JSON响应帧 */
        .task_stack = 6 * 1024,          /* 事件处理任务栈 */
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms   = 10000,
    };
    esp_websocket_client_handle_t client = esp_websocket_client_init(&ws_cfg);
    if (!client) {
        free(img_b64); free(signed_url); vSemaphoreDelete(sess.sem);
        return ESP_ERR_NO_MEM;
    }

    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, image_ws_event_handler, &sess);
    esp_websocket_client_start(client);

    /* 等待连接，同时响应 abort */
    g_image_abort = false;
    g_active_image_sess = &sess;
    int retry = 0;
    while (!esp_websocket_client_is_connected(client) && retry < 60 && !g_image_abort) {
        vTaskDelay(pdMS_TO_TICKS(100));
        retry++;
    }
    if (g_image_abort || !esp_websocket_client_is_connected(client)) {
        const char *reason = g_image_abort ? "aborted" : "timeout";
        ESP_LOGW(TAG, "WebSocket connection %s", reason);
        g_active_image_sess = NULL;
        async_destroy_image_client(client);
        free(img_b64); free(signed_url); vSemaphoreDelete(sess.sem);
        return g_image_abort ? ESP_ERR_NOT_FINISHED : ESP_FAIL;
    }

    char *req_json = build_request_json(img_b64, question);
    free(img_b64);
    if (!req_json) {
        async_destroy_image_client(client);
        free(signed_url); vSemaphoreDelete(sess.sem);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Sending image understanding request (%d bytes JSON)...", (int)strlen(req_json));
    int sent = esp_websocket_client_send_text(client, req_json, strlen(req_json), pdMS_TO_TICKS(10000));
    free(req_json);
    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send request");
        async_destroy_image_client(client);
        free(signed_url); vSemaphoreDelete(sess.sem);
        return ESP_FAIL;
    }

    /* 等待回复，每 300ms 检查一次 abort */
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t limit_tick = pdMS_TO_TICKS(timeout_ms);
    while (!sess.finished && !sess.error_code && !g_image_abort) {
        if ((xTaskGetTickCount() - start_tick) >= limit_tick) {
            ESP_LOGE(TAG, "Image API response timeout");
            sess.error_code = -4;
            break;
        }
        xSemaphoreTake(sess.sem, pdMS_TO_TICKS(300));
    }
    bool was_aborted = g_image_abort;
    g_active_image_sess = NULL;

    esp_websocket_client_stop(client);
    async_destroy_image_client(client);
    free(signed_url);
    free(sess.recv_buf);   /* 释放分片帧累积缓冲 */
    vSemaphoreDelete(sess.sem);

    if (was_aborted) {
        ESP_LOGW(TAG, "Image API aborted by user");
        free(sess.text);
        return ESP_ERR_NOT_FINISHED;
    }
    if (sess.error_code != 0 || !sess.text) {
        ESP_LOGE(TAG, "Image API failed (code=%d)", sess.error_code);
        free(sess.text);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Image understanding result: %s", sess.text);
    *out_reply = sess.text;
    return ESP_OK;
}

void xfyun_image_abort(void)
{
    ESP_LOGW(TAG, "Image API abort requested");
    g_image_abort = true;
    /* 唤醒正在 xSemaphoreTake 等待的任务 */
    if (g_active_image_sess && g_active_image_sess->sem) {
        xSemaphoreGive(g_active_image_sess->sem);
    }
}
