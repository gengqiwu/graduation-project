#include "xfyun_iat.h"

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
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "cJSON.h"

#define TAG "XFYUN_IAT"
#define IAT_FRAME_INTERVAL_MS 40
#define IAT_MAX_SN_TRACK 256

static char g_app_id[64];
static char g_api_key[96];
static char g_api_secret[128];
static bool g_ready = false;

typedef struct {
    char *text;
    size_t len;
    size_t cap;
    char *sn_text[IAT_MAX_SN_TRACK + 1];
    int max_sn;
    int err;
    bool finished;
    bool notified;
    SemaphoreHandle_t sem;
} xfyun_session_t;

typedef struct {
    esp_websocket_client_handle_t ws_client;
    xfyun_session_t session;
    xfyun_iat_partial_callback_t partial_cb;
    void *user_ctx;
    int frame_count;
    bool is_first_frame;
    TickType_t next_send_tick;
} xfyun_streaming_session_t;

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
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            *pout++ = c;
        } else {
            *pout++ = '%';
            *pout++ = hex[c >> 4];
            *pout++ = hex[c & 0x0F];
        }
    }
    *pout = '\0';
    return out;
}

static esp_err_t build_signed_url(char **out_url)
{
    if (!out_url) return ESP_ERR_INVALID_ARG;

    time_t now = 0;
    time(&now);
    if (now < 1700000000) {
        ESP_LOGW(TAG, "System time may be unsynchronized; signature could fail. Please ensure SNTP is running.");
    }

    struct tm gmt = {0};
    gmtime_r(&now, &gmt);

    char date_buf[64];
    strftime(date_buf, sizeof(date_buf), "%a, %d %b %Y %H:%M:%S GMT", &gmt);

    char *signature_origin = NULL;
    asprintf(&signature_origin,
             "host: iat-api.xfyun.cn\n"
             "date: %s\n"
             "GET /v2/iat HTTP/1.1",
             date_buf);
    if (!signature_origin) return ESP_ERR_NO_MEM;

    char *signature_b64 = NULL;
    esp_err_t ret = hmac_sha256_base64(g_api_secret, signature_origin, &signature_b64);
    free(signature_origin);
    if (ret != ESP_OK) return ret;

    char *authorization_origin = NULL;
    asprintf(&authorization_origin,
             "api_key=\"%s\", algorithm=\"hmac-sha256\", headers=\"host date request-line\", signature=\"%s\"",
             g_api_key, signature_b64);
    free(signature_b64);
    if (!authorization_origin) return ESP_ERR_NO_MEM;

    char *authorization_b64 = base64_encode((const uint8_t *)authorization_origin, strlen(authorization_origin));
    free(authorization_origin);
    if (!authorization_b64) return ESP_ERR_NO_MEM;

    char *date_enc = url_encode(date_buf);
    char *auth_enc = url_encode(authorization_b64);
    
    if (!date_enc || !auth_enc) {
        free(authorization_b64);
        free(date_enc);
        free(auth_enc);
        return ESP_ERR_NO_MEM;
    }

    asprintf(out_url,
             "wss://iat-api.xfyun.cn/v2/iat?authorization=%s&date=%s&host=iat-api.xfyun.cn",
             auth_enc,
             date_enc);

    free(authorization_b64);
    free(date_enc);
    free(auth_enc);

    return *out_url ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool append_text(xfyun_session_t *ctx, const char *fragment)
{
    if (!fragment || fragment[0] == '\0') return true;
    size_t need = strlen(fragment);
    if (ctx->len + need + 1 > ctx->cap) {
        size_t new_cap = (ctx->cap == 0 ? 256 : ctx->cap * 2);
        while (new_cap < ctx->len + need + 1) {
            new_cap *= 2;
        }
        char *new_buf = (char *)realloc(ctx->text, new_cap);
        if (!new_buf) return false;
        ctx->text = new_buf;
        ctx->cap = new_cap;
    }
    memcpy(ctx->text + ctx->len, fragment, need);
    ctx->len += need;
    ctx->text[ctx->len] = '\0';
    return true;
}

static void clear_sn_range(xfyun_session_t *ctx, int begin_sn, int end_sn)
{
    if (!ctx) return;
    if (begin_sn < 1) begin_sn = 1;
    if (end_sn > IAT_MAX_SN_TRACK) end_sn = IAT_MAX_SN_TRACK;
    if (begin_sn > end_sn) return;
    for (int sn = begin_sn; sn <= end_sn; ++sn) {
        if (ctx->sn_text[sn]) {
            free(ctx->sn_text[sn]);
            ctx->sn_text[sn] = NULL;
        }
    }
}

static bool rebuild_text_from_sn(xfyun_session_t *ctx)
{
    if (!ctx) return false;

    size_t total = 0;
    int upper = (ctx->max_sn > IAT_MAX_SN_TRACK) ? IAT_MAX_SN_TRACK : ctx->max_sn;
    for (int sn = 1; sn <= upper; ++sn) {
        if (ctx->sn_text[sn]) total += strlen(ctx->sn_text[sn]);
    }

    if (total + 1 > ctx->cap) {
        size_t new_cap = (ctx->cap == 0) ? 256 : ctx->cap;
        while (new_cap < total + 1) new_cap *= 2;
        char *new_buf = (char *)realloc(ctx->text, new_cap);
        if (!new_buf) return false;
        ctx->text = new_buf;
        ctx->cap = new_cap;
    }

    size_t pos = 0;
    for (int sn = 1; sn <= upper; ++sn) {
        if (ctx->sn_text[sn]) {
            size_t n = strlen(ctx->sn_text[sn]);
            memcpy(ctx->text + pos, ctx->sn_text[sn], n);
            pos += n;
        }
    }
    if (!ctx->text) return false;
    ctx->text[pos] = '\0';
    ctx->len = pos;
    return true;
}

static char *extract_result_text(cJSON *result)
{
    if (!result) return NULL;

    cJSON *ws = cJSON_GetObjectItem(result, "ws");
    if (!ws || !cJSON_IsArray(ws)) {
        return strdup("");
    }

    size_t cap = 128;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    buf[0] = '\0';

    int arr_size = cJSON_GetArraySize(ws);
    for (int i = 0; i < arr_size; ++i) {
        cJSON *ws_item = cJSON_GetArrayItem(ws, i);
        cJSON *cw = ws_item ? cJSON_GetObjectItem(ws_item, "cw") : NULL;
        if (cw && cJSON_IsArray(cw) && cJSON_GetArraySize(cw) > 0) {
            cJSON *w_item = cJSON_GetArrayItem(cw, 0);
            cJSON *w = w_item ? cJSON_GetObjectItem(w_item, "w") : NULL;
            if (w && cJSON_IsString(w) && w->valuestring) {
                size_t need = strlen(w->valuestring);
                if (len + need + 1 > cap) {
                    size_t new_cap = cap;
                    while (new_cap < len + need + 1) new_cap *= 2;
                    char *new_buf = (char *)realloc(buf, new_cap);
                    if (!new_buf) {
                        free(buf);
                        return NULL;
                    }
                    buf = new_buf;
                    cap = new_cap;
                }
                memcpy(buf + len, w->valuestring, need);
                len += need;
                buf[len] = '\0';
            }
        }
    }

    return buf;
}

static void update_text_from_result(xfyun_session_t *ctx, cJSON *result)
{
    if (!ctx || !result) return;

    cJSON *sn_item = cJSON_GetObjectItem(result, "sn");
    int sn = (sn_item && cJSON_IsNumber(sn_item)) ? sn_item->valueint : -1;

    char *frag = extract_result_text(result);
    if (!frag) return;

    cJSON *pgs_item = cJSON_GetObjectItem(result, "pgs");
    bool has_pgs = (pgs_item && cJSON_IsString(pgs_item) && pgs_item->valuestring);
    bool is_rpl = (has_pgs && strcmp(pgs_item->valuestring, "rpl") == 0);

    if (has_pgs && sn >= 1 && sn <= IAT_MAX_SN_TRACK) {
        if (is_rpl) {
            cJSON *rg_item = cJSON_GetObjectItem(result, "rg");
            if (rg_item && cJSON_IsArray(rg_item) && cJSON_GetArraySize(rg_item) >= 2) {
                cJSON *b = cJSON_GetArrayItem(rg_item, 0);
                cJSON *e = cJSON_GetArrayItem(rg_item, 1);
                if (b && e && cJSON_IsNumber(b) && cJSON_IsNumber(e)) {
                    clear_sn_range(ctx, b->valueint, e->valueint);
                }
            }
        }

        if (ctx->sn_text[sn]) {
            free(ctx->sn_text[sn]);
            ctx->sn_text[sn] = NULL;
        }
        ctx->sn_text[sn] = frag;
        if (sn > ctx->max_sn) ctx->max_sn = sn;
        if (!rebuild_text_from_sn(ctx)) {
            /* 回退为追加，避免内存紧张时丢词 */
            append_text(ctx, frag);
        }
        return;
    }

    /* 未开启动态修正或无 sn：按旧协议追加 */
    append_text(ctx, frag);
    free(frag);
}

static void free_session_sn_text(xfyun_session_t *ctx)
{
    if (!ctx) return;
    for (int sn = 1; sn <= IAT_MAX_SN_TRACK; ++sn) {
        if (ctx->sn_text[sn]) {
            free(ctx->sn_text[sn]);
            ctx->sn_text[sn] = NULL;
        }
    }
    ctx->max_sn = 0;
}

static void signal_done(xfyun_session_t *ctx)
{
    if (!ctx->finished) {
        ctx->finished = true;
    }
    if (ctx->sem && !ctx->notified) {
        ctx->notified = true;
        xSemaphoreGive(ctx->sem);
    }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    xfyun_session_t *ctx = (xfyun_session_t *)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected");
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket disconnected");
            signal_done(ctx);
            break;
        case WEBSOCKET_EVENT_DATA: {
            if (!data || data->data_len == 0) break;

            char *json = (char *)malloc(data->data_len + 1);
            if (!json) break;
            memcpy(json, data->data_ptr, data->data_len);
            json[data->data_len] = '\0';

            cJSON *root = cJSON_Parse(json);
            free(json);
            if (!root) {
                ESP_LOGW(TAG, "Failed to parse JSON");
                break;
            }

            cJSON *code = cJSON_GetObjectItem(root, "code");
            if (code && cJSON_IsNumber(code) && code->valueint != 0) {
                ctx->err = code->valueint;
                signal_done(ctx);
                cJSON_Delete(root);
                break;
            }

            cJSON *data_obj = cJSON_GetObjectItem(root, "data");
            if (data_obj) {
                cJSON *result = cJSON_GetObjectItem(data_obj, "result");
                if (result) {
                    update_text_from_result(ctx, result);
                }

                cJSON *status = cJSON_GetObjectItem(data_obj, "status");
                if (status && cJSON_IsNumber(status) && status->valueint == 2) {
                    signal_done(ctx);
                }
            }

            cJSON_Delete(root);
            break;
        }
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            ctx->err = -1;
            signal_done(ctx);
            break;
        default:
            break;
    }
}

static void websocket_streaming_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    xfyun_streaming_session_t *stream_ctx = (xfyun_streaming_session_t *)handler_args;
    xfyun_session_t *ctx = &stream_ctx->session;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Streaming WebSocket connected");
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Streaming WebSocket disconnected");
            /* 尝试打印服务端在断开前发送的数据（通常包含错误信息） */
            if (data && data->data_ptr && data->data_len > 0) {
                ESP_LOGW(TAG, "Server msg on disconnect (%d bytes): %.*s",
                         data->data_len, data->data_len, data->data_ptr);
            }
            signal_done(ctx);
            break;
        case WEBSOCKET_EVENT_DATA: {
            if (!data || data->data_len == 0) break;

            char *json = (char *)malloc(data->data_len + 1);
            if (!json) break;
            memcpy(json, data->data_ptr, data->data_len);
            json[data->data_len] = '\0';

            cJSON *root = cJSON_Parse(json);
            free(json);
            if (!root) {
                ESP_LOGW(TAG, "Failed to parse JSON");
                break;
            }

            cJSON *code = cJSON_GetObjectItem(root, "code");
            if (code && cJSON_IsNumber(code) && code->valueint != 0) {
                ctx->err = code->valueint;
                signal_done(ctx);
                cJSON_Delete(root);
                break;
            }

            cJSON *data_obj = cJSON_GetObjectItem(root, "data");
            if (data_obj) {
                cJSON *result = cJSON_GetObjectItem(data_obj, "result");
                if (result) {
                    update_text_from_result(ctx, result);
                    
                    // 触发部分结果回调
                    if (stream_ctx->partial_cb && ctx->text) {
                        stream_ctx->partial_cb(ctx->text, stream_ctx->user_ctx);
                    }
                }

                cJSON *status = cJSON_GetObjectItem(data_obj, "status");
                if (status && cJSON_IsNumber(status) && status->valueint == 2) {
                    signal_done(ctx);
                }
            }

            cJSON_Delete(root);
            break;
        }
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "Streaming WebSocket error");
            /* 打印服务端返回的错误数据（如果有） */
            if (data && data->data_ptr && data->data_len > 0) {
                ESP_LOGE(TAG, "Error payload (%d bytes): %.*s",
                         data->data_len, data->data_len, data->data_ptr);
            }
            ctx->err = -1;
            signal_done(ctx);
            break;
        default:
            break;
    }
}

esp_err_t xfyun_iat_init(const xfyun_iat_config_t *config)
{
    if (!config || !config->app_id || !config->api_key || !config->api_secret) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(g_app_id, config->app_id, sizeof(g_app_id) - 1);
    g_app_id[sizeof(g_app_id) - 1] = '\0';

    strncpy(g_api_key, config->api_key, sizeof(g_api_key) - 1);
    g_api_key[sizeof(g_api_key) - 1] = '\0';

    strncpy(g_api_secret, config->api_secret, sizeof(g_api_secret) - 1);
    g_api_secret[sizeof(g_api_secret) - 1] = '\0';

    g_ready = true;
    ESP_LOGI(TAG, "Xunfei IAT client configured (app_id=%s)", g_app_id);
    return ESP_OK;
}

esp_err_t xfyun_iat_transcribe(const int16_t *pcm_samples, size_t sample_count, char **out_text, uint32_t timeout_ms)
{
    if (!g_ready) return ESP_ERR_INVALID_STATE;
    if (!pcm_samples || sample_count == 0 || !out_text) return ESP_ERR_INVALID_ARG;

    *out_text = NULL;

    char *url = NULL;
    esp_err_t ret = build_signed_url(&url);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to build signed URL");
        return ret;
    }
    
    ESP_LOGI(TAG, "WebSocket URL: %s", url);

    xfyun_session_t session = {
        .text = NULL,
        .len = 0,
        .cap = 0,
        .err = 0,
        .finished = false,
        .notified = false,
        .sem = xSemaphoreCreateBinary(),
    };

    esp_websocket_client_config_t cfg = {
        .uri = url,
        .disable_auto_reconnect = true,
        .network_timeout_ms = 30000,   // Increase to 30s for slow networks
        .crt_bundle_attach = esp_crt_bundle_attach,
        .subprotocol = NULL,
        .user_agent = "esp32-xfyun-iat",
        .buffer_size = 17 * 1024,           // Increase send buffer
        .task_stack = 6 * 1024,
        .ping_interval_sec = 30,       // Keep connection alive
    };

    esp_websocket_client_handle_t client = esp_websocket_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init websocket client");
        free(url);
        if (session.sem) vSemaphoreDelete(session.sem);
        return ESP_FAIL;
    }

    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, &session);

    ret = esp_websocket_client_start(client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket start failed: %s", esp_err_to_name(ret));
        esp_websocket_client_destroy(client);
        free(url);
        if (session.sem) vSemaphoreDelete(session.sem);
        return ret;
    }

    // Wait until the connection is established
    for (int i = 0; i < 50 && !esp_websocket_client_is_connected(client); ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!esp_websocket_client_is_connected(client)) {
        ESP_LOGE(TAG, "WebSocket not connected");
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        free(url);
        if (session.sem) vSemaphoreDelete(session.sem);
        return ESP_FAIL;
    }

    size_t total_bytes = sample_count * sizeof(int16_t);
    const uint8_t *audio = (const uint8_t *)pcm_samples;
    const size_t chunk_bytes = 1280; // 40ms at 16kHz mono PCM16 (doc-recommended frame)

    size_t offset = 0;
    int frame_idx = 0;
    while (offset < total_bytes) {
        size_t send_len = (total_bytes - offset > chunk_bytes) ? chunk_bytes : (total_bytes - offset);
        int status = 1;
        if (frame_idx == 0) {
            status = 0;
        }
        if (offset + send_len >= total_bytes) {
            status = 2;
        }

        char *audio_b64 = base64_encode(audio + offset, send_len);
        if (!audio_b64) {
            ret = ESP_ERR_NO_MEM;
            break;
        }

        char *frame = NULL;
        if (status == 0) {
            asprintf(&frame,
                "{\"common\":{\"app_id\":\"%s\"},"
                "\"business\":{\"language\":\"zh_cn\",\"domain\":\"iat\",\"accent\":\"mandarin\",\"vad_eos\":300,\"ptt\":1,\"nunum\":1,\"rlang\":\"zh-cn\"},"
                "\"data\":{\"status\":0,\"format\":\"audio/L16;rate=16000\",\"encoding\":\"raw\",\"audio\":\"%s\"}}",
                g_app_id,
                audio_b64);
        } else {
            asprintf(&frame,
                "{\"data\":{\"status\":%d,\"format\":\"audio/L16;rate=16000\",\"encoding\":\"raw\",\"audio\":\"%s\"}}",
                status,
                audio_b64);
        }

        free(audio_b64);

        if (!frame) {
            ret = ESP_ERR_NO_MEM;
            break;
        }

        int sent = esp_websocket_client_send_text(client, frame, strlen(frame), portMAX_DELAY);
        free(frame);
        if (sent < 0) {
            ESP_LOGE(TAG, "Failed to send frame");
            ret = ESP_FAIL;
            break;
        }

        offset += send_len;
        frame_idx++;
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    // Send a final empty frame if we bailed early
    if (ret != ESP_OK && esp_websocket_client_is_connected(client)) {
        const char *final_frame = "{\"data\":{\"status\":2,\"format\":\"audio/L16;rate=16000\",\"encoding\":\"raw\",\"audio\":\"\"}}";
        esp_websocket_client_send_text(client, final_frame, strlen(final_frame), pdMS_TO_TICKS(5000));
    }

    if (ret == ESP_OK && session.sem) {
        if (xSemaphoreTake(session.sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
            ESP_LOGE(TAG, "Recognition timeout");
            ret = ESP_ERR_TIMEOUT;
        }
    }

    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
    free(url);
    if (session.sem) vSemaphoreDelete(session.sem);

    if (ret == ESP_OK && session.err != 0) {
        ESP_LOGE(TAG, "IAT returned error code %d", session.err);
        ret = ESP_FAIL;
    }

    if (ret == ESP_OK && session.text) {
        *out_text = session.text;
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "IAT Recognition Result: %s", session.text);
        ESP_LOGI(TAG, "========================================");
    } else {
        free(session.text);
        if (ret == ESP_OK) {
            ESP_LOGW(TAG, "IAT completed but no text recognized");
        }
    }

    free_session_sn_text(&session);

    return ret;
}

// ==================== 流式API实现 ====================

esp_err_t xfyun_iat_streaming_start(xfyun_iat_session_handle_t *out_session, 
                                     xfyun_iat_partial_callback_t partial_cb, 
                                     void *user_ctx)
{
    if (!g_ready || !out_session) return ESP_ERR_INVALID_ARG;

    xfyun_streaming_session_t *stream = calloc(1, sizeof(xfyun_streaming_session_t));
    if (!stream) return ESP_ERR_NO_MEM;

    stream->partial_cb = partial_cb;
    stream->user_ctx = user_ctx;
    stream->frame_count = 0;
    stream->is_first_frame = true;
    stream->next_send_tick = xTaskGetTickCount();
    stream->session.sem = xSemaphoreCreateBinary();

    char *url = NULL;
    esp_err_t ret = build_signed_url(&url);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to build signed URL");
        if (stream->session.sem) vSemaphoreDelete(stream->session.sem);
        free(stream);
        return ret;
    }

    esp_websocket_client_config_t cfg = {
        .uri = url,
        .disable_auto_reconnect = true,
        .network_timeout_ms = 30000,   // Increase to 30s for slow networks
        .crt_bundle_attach = esp_crt_bundle_attach,
        .subprotocol = NULL,
        .user_agent = "esp32-xfyun-iat-stream",
        .buffer_size = 17 * 1024,           // Increase send buffer
        .task_stack = 6 * 1024,
        .ping_interval_sec = 30,       // Keep connection alive
    };

    stream->ws_client = esp_websocket_client_init(&cfg);
    free(url);

    if (!stream->ws_client) {
        ESP_LOGE(TAG, "Failed to init websocket client");
        if (stream->session.sem) vSemaphoreDelete(stream->session.sem);
        free(stream);
        return ESP_FAIL;
    }

    esp_websocket_register_events(stream->ws_client, WEBSOCKET_EVENT_ANY, 
                                   websocket_streaming_event_handler, stream);

    ret = esp_websocket_client_start(stream->ws_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket start failed");
        esp_websocket_client_destroy(stream->ws_client);
        if (stream->session.sem) vSemaphoreDelete(stream->session.sem);
        free(stream);
        return ret;
    }

    // 等待连接建立 (最多15秒，适应慢速网络)
    ESP_LOGI(TAG, "Waiting for WebSocket connection...");
    for (int i = 0; i < 150 && !esp_websocket_client_is_connected(stream->ws_client); ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!esp_websocket_client_is_connected(stream->ws_client)) {
        ESP_LOGE(TAG, "Streaming WebSocket connection timeout after 15s");
        esp_websocket_client_stop(stream->ws_client);
        esp_websocket_client_destroy(stream->ws_client);
        if (stream->session.sem) vSemaphoreDelete(stream->session.sem);
        free(stream);
        return ESP_FAIL;
    }

    *out_session = stream;
    ESP_LOGI(TAG, "Streaming session started");
    return ESP_OK;
}

esp_err_t xfyun_iat_streaming_feed(xfyun_iat_session_handle_t session,
                                    const int16_t *pcm_chunk,
                                    size_t sample_count)
{
    if (!session || !pcm_chunk || sample_count == 0) return ESP_ERR_INVALID_ARG;

    xfyun_streaming_session_t *stream = (xfyun_streaming_session_t *)session;
    if (stream->session.err != 0) {
        ESP_LOGE(TAG, "IAT session already closed (err=%d)", stream->session.err);
        return ESP_ERR_INVALID_STATE;
    }
    if (stream->session.finished) {
        /* 服务端已给出最终结果，后续音频无需继续发送。 */
        return ESP_OK;
    }
    if (!esp_websocket_client_is_connected(stream->ws_client)) {
        if (stream->session.finished) {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "WebSocket not connected in feed");
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t *pcm_bytes = (const uint8_t *)pcm_chunk;
    const size_t total_bytes = sample_count * sizeof(int16_t);
    const size_t frame_bytes = 1280;  /* 40ms @16kHz mono PCM16 */
    size_t off = 0;

    if (stream->frame_count == 0) {
        ESP_LOGI(TAG, "Encoding first audio frame (%d bytes total, split=%d)", (int)total_bytes, (int)frame_bytes);
    }

    while (off < total_bytes) {
        size_t cur = total_bytes - off;
        if (cur > frame_bytes) cur = frame_bytes;

        char *audio_b64 = base64_encode(pcm_bytes + off, cur);
        if (!audio_b64) return ESP_ERR_NO_MEM;

        char *frame = NULL;
        if (stream->is_first_frame) {
            asprintf(&frame,
                "{\"common\":{\"app_id\":\"%s\"},"
                "\"business\":{\"language\":\"zh_cn\",\"domain\":\"iat\",\"accent\":\"mandarin\",\"vad_eos\":300,\"ptt\":1,\"nunum\":1,\"rlang\":\"zh-cn\"},"
                "\"data\":{\"status\":0,\"format\":\"audio/L16;rate=16000\",\"encoding\":\"raw\",\"audio\":\"%s\"}}",
                g_app_id, audio_b64);
        } else {
            asprintf(&frame,
                "{\"data\":{\"status\":1,\"format\":\"audio/L16;rate=16000\",\"encoding\":\"raw\",\"audio\":\"%s\"}}",
                audio_b64);
        }
        free(audio_b64);
        if (!frame) return ESP_ERR_NO_MEM;

        if (stream->frame_count == 0) {
            ESP_LOGI(TAG, "Sending first frame (%d bytes)...", (int)strlen(frame));
        }

        // 移除人为延迟限制，跟紧麦克风的实际流速
        // TickType_t now_tick = xTaskGetTickCount();
        // if (now_tick < stream->next_send_tick) {
        //     vTaskDelay(stream->next_send_tick - now_tick);
        // }

        int sent = esp_websocket_client_send_text(stream->ws_client, frame, strlen(frame), pdMS_TO_TICKS(1200));
        if (stream->frame_count == 0) {
            ESP_LOGI(TAG, "First frame sent result: %d", sent);
        }
        free(frame);

        if (sent <= 0) {
            ESP_LOGE(TAG, "Failed to send audio frame (sent=%d, frame=%d)", sent, stream->frame_count);
            return ESP_FAIL;
        }

        stream->is_first_frame = false;
        stream->frame_count++;
        off += cur;
        
        // stream->next_send_tick = xTaskGetTickCount() + pdMS_TO_TICKS(IAT_FRAME_INTERVAL_MS);

        if (stream->session.err != 0 || stream->session.finished) {
            break;
        }
    }

    return ESP_OK;
}

esp_err_t xfyun_iat_streaming_finish(xfyun_iat_session_handle_t session,
                                      char **out_text,
                                      uint32_t timeout_ms)
{
    if (!session) return ESP_ERR_INVALID_ARG;
    xfyun_streaming_session_t *stream = (xfyun_streaming_session_t *)session;
    esp_err_t ret = ESP_OK;
    // 发送结束帧（status=2）
    if (esp_websocket_client_is_connected(stream->ws_client)) {
        const char *final_frame = 
            "{\"data\":{\"status\":2,\"format\":\"audio/L16;rate=16000\",\"encoding\":\"raw\",\"audio\":\"\"}}";
        esp_websocket_client_send_text(stream->ws_client, final_frame, strlen(final_frame), pdMS_TO_TICKS(5000));
    }

    // 等待识别完成
    if (stream->session.sem) {
        if (xSemaphoreTake(stream->session.sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
            ESP_LOGE(TAG, "Streaming recognition timeout");
            ret = ESP_ERR_TIMEOUT;
        }
    }

    // 检查错误
    if (ret == ESP_OK && stream->session.err != 0) {
        ESP_LOGE(TAG, "Streaming IAT error code %d", stream->session.err);
        ret = ESP_FAIL;
    }

    // 返回最终文本
    if (ret == ESP_OK && stream->session.text) {
        if (out_text) {
            *out_text = stream->session.text;
            stream->session.text = NULL;  // 转移所有权
            ESP_LOGI(TAG, "Streaming recognition completed: %s", *out_text);
        } else {
            ESP_LOGI(TAG, "Streaming recognition completed (ignored): %s", stream->session.text);
            free(stream->session.text);
            stream->session.text = NULL;
        }
    } else {
        if (out_text) *out_text = NULL;
        if (stream->session.text) {
            free(stream->session.text);
            stream->session.text = NULL;
        }
    }

    return ret;
}

static void async_close_task(void *arg) {
    esp_websocket_client_handle_t ws = (esp_websocket_client_handle_t)arg;
    if (ws) {
        esp_websocket_client_stop(ws);
        esp_websocket_client_destroy(ws);
    }
    vTaskDelete(NULL);
}

esp_err_t xfyun_iat_streaming_close(xfyun_iat_session_handle_t session)
{
    if (!session) return ESP_ERR_INVALID_ARG;

    xfyun_streaming_session_t *stream = (xfyun_streaming_session_t *)session;

    if (stream->ws_client) {
        if (xTaskCreate(async_close_task, "iat_cls", 3072, stream->ws_client, 2, NULL) != pdPASS) {
            esp_websocket_client_stop(stream->ws_client);
            esp_websocket_client_destroy(stream->ws_client);
        }
        stream->ws_client = NULL;
    }

    if (stream->session.sem) {
        vSemaphoreDelete(stream->session.sem);
        stream->session.sem = NULL;
    }

    if (stream->session.text) {
        free(stream->session.text);
        stream->session.text = NULL;
    }

    free_session_sn_text(&stream->session);

    free(stream);
    return ESP_OK;
}
