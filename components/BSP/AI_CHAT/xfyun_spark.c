#include "xfyun_spark.h"

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

#define TAG "XFYUN_SPARK"

#define SPARK_HOST "spark-api.xf-yun.com"
#define SPARK_PATH "/v3.1/chat"
#define SPARK_WS_URL "wss://spark-api.xf-yun.com/v3.1/chat"

#define SPARK_DEFAULT_DOMAIN "generalv3"
#define SPARK_DEFAULT_TEMPERATURE 0.5
#define SPARK_DEFAULT_MAX_TOKENS 1024
#define SPARK_WS_CONNECT_WAIT_MS 2000

static char g_app_id[64];
static char g_api_key[96];
static char g_api_secret[128];
static char g_domain[16];
static bool g_ready = false;
static int  g_last_error = 0;

typedef struct {
    char *text;
    size_t len;
    size_t cap;
    int error_code;
    bool finished;
    bool notified;
    SemaphoreHandle_t sem;
    /* 流式回调（可为 NULL）*/
    spark_stream_cb_t on_chunk;
    void *on_chunk_ctx;
} spark_session_t;

/* 当前正在等待 Spark 流式响应的 session（NULL 表示当前无活动请求）
 * 由 xfyun_spark_abort() 使用，外部打断时踢出阻塞的 xSemaphoreTake。 */
static spark_session_t *g_active_spark_session = NULL;

/* Start websocket with short retries and bounded wait to avoid lingering task pressure. */
static esp_err_t spark_ws_start_and_wait(esp_websocket_client_handle_t client, const char *scene)
{
    esp_err_t start_ret = ESP_FAIL;
    for (int att = 0; att < 3 && start_ret != ESP_OK; att++) {
        if (att > 0) {
            ESP_LOGW(TAG, "%s: ws start retry %d", scene, att);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        start_ret = esp_websocket_client_start(client);
    }
    if (start_ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: ws start failed (%s)", scene, esp_err_to_name(start_ret));
        return start_ret;
    }

    const int max_poll = SPARK_WS_CONNECT_WAIT_MS / 100;
    for (int i = 0; i < max_poll && !esp_websocket_client_is_connected(client); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!esp_websocket_client_is_connected(client)) {
        ESP_LOGE(TAG, "%s: ws connect timeout (%dms)", scene, SPARK_WS_CONNECT_WAIT_MS);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

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

    char signature_origin[256];
    snprintf(signature_origin, sizeof(signature_origin),
             "host: %s\n"
             "date: %s\n"
             "GET %s HTTP/1.1",
             SPARK_HOST, date_buf, SPARK_PATH);

    char *signature_b64 = NULL;
    esp_err_t ret = hmac_sha256_base64(g_api_secret, signature_origin, &signature_b64);
    if (ret != ESP_OK) return ret;

    char authorization_origin[512];
    snprintf(authorization_origin, sizeof(authorization_origin),
             "api_key=\"%s\", algorithm=\"hmac-sha256\", headers=\"host date request-line\", signature=\"%s\"",
             g_api_key, signature_b64);
    free(signature_b64);

    char *authorization_b64 = base64_encode((const uint8_t *)authorization_origin, strlen(authorization_origin));
    if (!authorization_b64) return ESP_ERR_NO_MEM;

    char *date_enc = url_encode(date_buf);
    char *auth_enc = url_encode(authorization_b64);
    free(authorization_b64);

    if (!date_enc || !auth_enc) {
        free(date_enc);
        free(auth_enc);
        return ESP_ERR_NO_MEM;
    }

    asprintf(out_url,
             "%s?authorization=%s&date=%s&host=%s",
             SPARK_WS_URL, auth_enc, date_enc, SPARK_HOST);

    free(date_enc);
    free(auth_enc);

    return *out_url ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool append_text(spark_session_t *ctx, const char *fragment)
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

static void signal_done(spark_session_t *ctx)
{
    if (!ctx->finished) {
        ctx->finished = true;
    }
    if (ctx->sem && !ctx->notified) {
        ctx->notified = true;
        xSemaphoreGive(ctx->sem);
    }
}

/**
 * @brief 外部中断：如果有活跌的 Spark 请求，立即踢出其阻塞等待。
 * 可在任意任务中安全调用。
 */
void xfyun_spark_abort(void)
{
    spark_session_t *s = g_active_spark_session;
    if (s) {
        s->error_code = -5;  /* 表示被 abort */
        signal_done(s);       /* 踢出 xSemaphoreTake */
    }
}

static void spark_ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    if (!g_active_spark_session || handler_args != g_active_spark_session) return;
    spark_session_t *ctx = (spark_session_t *)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected");
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket disconnected");
            /* 若尚未收到任何文字，说明服务端在响应前就关闭了连接（竞争/空闲超时），
             * 用 -6 区分"有文字且 status=2 触发的正常关闭"与"无数据的意外关闭"。 */
            if (!ctx->text || ctx->len == 0) {
                ctx->error_code = -6;
            }
            signal_done(ctx);
            break;
        case WEBSOCKET_EVENT_DATA: {
            if (!data || !data->data_ptr || data->data_len <= 0) {
                break;
            }
            char *json_str = (char *)malloc(data->data_len + 1);
            if (!json_str) {
                ctx->error_code = -1;
                signal_done(ctx);
                break;
            }
            memcpy(json_str, data->data_ptr, data->data_len);
            json_str[data->data_len] = '\0';

            cJSON *root = cJSON_Parse(json_str);
            free(json_str);
            if (!root) {
                break;
            }

            cJSON *header = cJSON_GetObjectItem(root, "header");
            cJSON *code = header ? cJSON_GetObjectItem(header, "code") : NULL;
            cJSON *header_status = header ? cJSON_GetObjectItem(header, "status") : NULL;
            if (code && cJSON_IsNumber(code) && code->valueint != 0) {
                ctx->error_code = code->valueint;
                ESP_LOGE(TAG, "Spark error code: %d", ctx->error_code);
                cJSON_Delete(root);
                signal_done(ctx);
                break;
            }

            cJSON *payload = cJSON_GetObjectItem(root, "payload");
            cJSON *choices = payload ? cJSON_GetObjectItem(payload, "choices") : NULL;
            cJSON *choices_status = choices ? cJSON_GetObjectItem(choices, "status") : NULL;
            cJSON *text_arr = choices ? cJSON_GetObjectItem(choices, "text") : NULL;
            if (text_arr && cJSON_IsArray(text_arr)) {
                int count = cJSON_GetArraySize(text_arr);
                for (int i = 0; i < count; ++i) {
                    cJSON *item = cJSON_GetArrayItem(text_arr, i);
                    cJSON *content = item ? cJSON_GetObjectItem(item, "content") : NULL;
                    if (content && cJSON_IsString(content)) {
                        if (!append_text(ctx, content->valuestring)) {
                            ctx->error_code = -2;
                            ESP_LOGE(TAG, "Failed to expand response buffer");
                            cJSON_Delete(root);
                            signal_done(ctx);
                            return;
                        }
                    }
                }
            }

            bool is_end =
                (choices_status && cJSON_IsNumber(choices_status) && choices_status->valueint == 2) ||
                (header_status  && cJSON_IsNumber(header_status)  && header_status->valueint  == 2);

            /* 流式回调：每次有新文字就通知一次 */
            if (ctx->on_chunk && ctx->text && ctx->len > 0) {
                ctx->on_chunk(ctx->text, is_end, ctx->on_chunk_ctx);
            }

            if (is_end) {
                cJSON_Delete(root);
                signal_done(ctx);
                break;
            }

            cJSON_Delete(root);
            break;
        }
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            ctx->error_code = -3;
            signal_done(ctx);
            break;
        default:
            break;
    }
}

static char *build_request_json(const char *user_text)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *header = cJSON_AddObjectToObject(root, "header");
    cJSON_AddStringToObject(header, "app_id", g_app_id);
    cJSON_AddStringToObject(header, "uid", "esp32s3");

    cJSON *parameter = cJSON_AddObjectToObject(root, "parameter");
    cJSON *chat = cJSON_AddObjectToObject(parameter, "chat");
    cJSON_AddStringToObject(chat, "domain", g_domain[0] ? g_domain : SPARK_DEFAULT_DOMAIN);
    cJSON_AddNumberToObject(chat, "temperature", SPARK_DEFAULT_TEMPERATURE);
    cJSON_AddNumberToObject(chat, "max_tokens", SPARK_DEFAULT_MAX_TOKENS);

    // 启用星火大模型内置的 Web 搜索能力 (支持回答"抖音今天的热搜"等实时问题)
    cJSON *tools = cJSON_AddArrayToObject(chat, "tools");
    cJSON *tool_item = cJSON_CreateObject();
    cJSON_AddStringToObject(tool_item, "type", "web_search");
    cJSON *web_search = cJSON_AddObjectToObject(tool_item, "web_search");
    cJSON_AddBoolToObject(web_search, "enable", true);
    cJSON_AddBoolToObject(web_search, "show_ref_label", false); // 设为false不输出复杂的引用URL
    cJSON_AddItemToArray(tools, tool_item);

    cJSON *payload = cJSON_AddObjectToObject(root, "payload");
    cJSON *message = cJSON_AddObjectToObject(payload, "message");
    cJSON *text = cJSON_AddArrayToObject(message, "text");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "role", "user");
    cJSON_AddStringToObject(item, "content", user_text ? user_text : "");
    cJSON_AddItemToArray(text, item);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

esp_err_t xfyun_spark_init(const xfyun_spark_config_t *config)
{
    if (!config || !config->app_id || !config->api_key || !config->api_secret) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(g_app_id, sizeof(g_app_id), "%s", config->app_id);
    snprintf(g_api_key, sizeof(g_api_key), "%s", config->api_key);
    snprintf(g_api_secret, sizeof(g_api_secret), "%s", config->api_secret);

    if (config->domain && config->domain[0] != '\0') {
        snprintf(g_domain, sizeof(g_domain), "%s", config->domain);
    } else {
        snprintf(g_domain, sizeof(g_domain), "%s", SPARK_DEFAULT_DOMAIN);
    }

    g_ready = true;
    ESP_LOGI(TAG, "Spark client initialized (domain=%s)", g_domain);
    return ESP_OK;
}

esp_err_t xfyun_spark_chat_stream(const char *user_text,
                                   spark_stream_cb_t on_chunk,
                                   void *on_chunk_ctx,
                                   char **out_reply,
                                   uint32_t timeout_ms)
{
    if (!g_ready || !out_reply) return ESP_ERR_INVALID_STATE;

    *out_reply = NULL;

    char *signed_url = NULL;
    esp_err_t ret = build_signed_url(&signed_url);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to build signed URL");
        return ret;
    }

    spark_session_t session = {
        .text = NULL,
        .len = 0,
        .cap = 0,
        .error_code = 0,
        .finished = false,
        .notified = false,
        .sem = xSemaphoreCreateBinary(),
        .on_chunk = on_chunk,
        .on_chunk_ctx = on_chunk_ctx,
    };
    if (!session.sem) {
        free(signed_url);
        return ESP_ERR_NO_MEM;
    }

    esp_websocket_client_config_t ws_cfg = {
        .uri = signed_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 17 * 1024,
        .task_stack = 6 * 1024,
        .disable_auto_reconnect = true,
        .reconnect_timeout_ms = 10000,
        .network_timeout_ms   = 10000,
    };

    esp_websocket_client_handle_t client = esp_websocket_client_init(&ws_cfg);
    if (!client) {
        free(signed_url);
        vSemaphoreDelete(session.sem);
        return ESP_ERR_NO_MEM;
    }

    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, spark_ws_event_handler, &session);
    if (spark_ws_start_and_wait(client, "spark_chat") != ESP_OK) {
        esp_websocket_client_destroy(client);
        free(signed_url);
        vSemaphoreDelete(session.sem);
        return ESP_FAIL;
    }

    char *request_json = build_request_json(user_text);
    if (!request_json) {
        esp_websocket_client_destroy(client);
        free(signed_url);
        vSemaphoreDelete(session.sem);
        return ESP_ERR_NO_MEM;
    }

    g_last_error = 0;
    int sent = esp_websocket_client_send_text(client, request_json, strlen(request_json), pdMS_TO_TICKS(5000));
    free(request_json);
    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send Spark request");
        esp_websocket_client_destroy(client);
        free(signed_url);
        vSemaphoreDelete(session.sem);
        return ESP_FAIL;
    }

    g_active_spark_session = &session;
    if (xSemaphoreTake(session.sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "Spark response timeout");
        session.error_code = -4;
    }
    g_active_spark_session = NULL;

    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
    free(signed_url);

    if (session.error_code != 0 || !session.text) {
        g_last_error = session.error_code;
        free(session.text);
        vSemaphoreDelete(session.sem);
        return ESP_FAIL;
    }

    *out_reply = session.text;
    vSemaphoreDelete(session.sem);
    return ESP_OK;
}

/* ===== 预连接实现 ===== */

typedef struct {
    esp_websocket_client_handle_t client;
    char *signed_url;   /* 保存以便释放 */
} xfyun_spark_preconn_impl_t;

/* 仅监听 CONNECTED 事件以记录日志，其余忽略 */
static void spark_preconn_event_handler(void *args, esp_event_base_t base,
                                         int32_t event_id, void *event_data)
{
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "Spark pre-connect: WebSocket TLS handshake done");
    }
}

esp_err_t xfyun_spark_preconnect(xfyun_spark_conn_t *out_conn)
{
    if (!g_ready || !out_conn) return ESP_ERR_INVALID_ARG;
    *out_conn = NULL;

    xfyun_spark_preconn_impl_t *pc = calloc(1, sizeof(*pc));
    if (!pc) return ESP_ERR_NO_MEM;

    esp_err_t ret = build_signed_url(&pc->signed_url);
    if (ret != ESP_OK) { free(pc); return ret; }

    esp_websocket_client_config_t ws_cfg = {
        .uri                    = pc->signed_url,
        .crt_bundle_attach      = esp_crt_bundle_attach,
        .buffer_size = 17 * 1024,
        .task_stack = 6 * 1024,
        .disable_auto_reconnect = true,
        .network_timeout_ms     = 10000,
    };

    pc->client = esp_websocket_client_init(&ws_cfg);
    if (!pc->client) {
        free(pc->signed_url); free(pc);
        return ESP_ERR_NO_MEM;
    }

    esp_websocket_register_events(pc->client, WEBSOCKET_EVENT_ANY,
                                   spark_preconn_event_handler, NULL);
    if (spark_ws_start_and_wait(pc->client, "spark_preconnect") != ESP_OK) {
        esp_websocket_client_stop(pc->client);
        esp_websocket_client_destroy(pc->client);
        free(pc->signed_url); free(pc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Spark pre-connect ready (heap=%d)", (int)esp_get_free_heap_size());
    *out_conn = (xfyun_spark_conn_t)pc;
    return ESP_OK;
}

esp_err_t xfyun_spark_chat_stream_with_conn(xfyun_spark_conn_t conn,
                                             const char *user_text,
                                             spark_stream_cb_t on_chunk,
                                             void *on_chunk_ctx,
                                             char **out_reply,
                                             uint32_t timeout_ms)
{
    xfyun_spark_preconn_impl_t *pc = (xfyun_spark_preconn_impl_t *)conn;
    if (!pc || !pc->client || !out_reply) return ESP_ERR_INVALID_ARG;
    *out_reply = NULL;

    /* ---- 检测预连接是否仍然存活 ---- */
    if (!esp_websocket_client_is_connected(pc->client)) {
        ESP_LOGW(TAG, "Spark pre-conn dropped by server, reconnecting...");
        esp_websocket_client_stop(pc->client);
        esp_websocket_client_destroy(pc->client);
        pc->client = NULL;
        free(pc->signed_url);
        pc->signed_url = NULL;

        /* 用当前时刻重新生成签名 URL */
        if (build_signed_url(&pc->signed_url) != ESP_OK) {
            free(pc);
            return ESP_FAIL;
        }
        esp_websocket_client_config_t ws_cfg = {
            .uri                    = pc->signed_url,
            .crt_bundle_attach      = esp_crt_bundle_attach,
            .buffer_size = 17 * 1024,
            .task_stack = 6 * 1024,
            .disable_auto_reconnect = true,
            .network_timeout_ms     = 10000,
        };
        pc->client = esp_websocket_client_init(&ws_cfg);
        if (!pc->client) { free(pc->signed_url); free(pc); return ESP_ERR_NO_MEM; }

        esp_websocket_register_events(pc->client, WEBSOCKET_EVENT_ANY,
                                       spark_preconn_event_handler, NULL);
        if (spark_ws_start_and_wait(pc->client, "spark_with_conn_reconnect") != ESP_OK) {
            ESP_LOGE(TAG, "Spark with_conn: reconnect failed");
            esp_websocket_client_stop(pc->client);
            esp_websocket_client_destroy(pc->client);
            free(pc->signed_url); free(pc);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Spark with_conn: reconnected ok");
    }

    spark_session_t session = {
        .text        = NULL,
        .len         = 0,
        .cap         = 0,
        .error_code  = 0,
        .finished    = false,
        .notified    = false,
        .sem         = xSemaphoreCreateBinary(),
        .on_chunk    = on_chunk,
        .on_chunk_ctx = on_chunk_ctx,
    };
    if (!session.sem) {
        esp_websocket_client_stop(pc->client);
        esp_websocket_client_destroy(pc->client);
        free(pc->signed_url); free(pc);
        return ESP_ERR_NO_MEM;
    }

    /* 重新注册携带 session 上下文的事件处理器 */
    esp_websocket_register_events(pc->client, WEBSOCKET_EVENT_ANY,
                                   spark_ws_event_handler, &session);

    char *request_json = build_request_json(user_text);
    if (!request_json) {
        vSemaphoreDelete(session.sem);
        esp_websocket_client_stop(pc->client);
        esp_websocket_client_destroy(pc->client);
        free(pc->signed_url); free(pc);
        return ESP_ERR_NO_MEM;
    }

    g_last_error = 0;
    int sent = esp_websocket_client_send_text(pc->client, request_json,
                                               strlen(request_json), pdMS_TO_TICKS(5000));
    free(request_json);
    if (sent < 0) {
        ESP_LOGE(TAG, "Spark with_conn: send failed");
        vSemaphoreDelete(session.sem);
        esp_websocket_client_stop(pc->client);
        esp_websocket_client_destroy(pc->client);
        free(pc->signed_url); free(pc);
        return ESP_FAIL;
    }

    /* 等待流式响应完成 */
    g_active_spark_session = &session;
    if (xSemaphoreTake(session.sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "Spark with_conn: response timeout");
        session.error_code = -4;
    }
    g_active_spark_session = NULL;

    esp_websocket_client_stop(pc->client);
    esp_websocket_client_destroy(pc->client);
    free(pc->signed_url);
    free(pc);
    vSemaphoreDelete(session.sem);

    if (session.error_code != 0 || !session.text) {
        g_last_error = session.error_code;
        free(session.text);
        return ESP_FAIL;
    }

    *out_reply = session.text;
    return ESP_OK;
}

void xfyun_spark_conn_close(xfyun_spark_conn_t conn)
{
    xfyun_spark_preconn_impl_t *pc = (xfyun_spark_preconn_impl_t *)conn;
    if (!pc) return;
    if (pc->client) {
        esp_websocket_client_stop(pc->client);
        esp_websocket_client_destroy(pc->client);
    }
    free(pc->signed_url);
    free(pc);
    ESP_LOGI(TAG, "Spark pre-conn closed (unused)");
}

/* 封装层：无回调版本 */
esp_err_t xfyun_spark_chat(const char *user_text, char **out_reply, uint32_t timeout_ms)
{
    return xfyun_spark_chat_stream(user_text, NULL, NULL, out_reply, timeout_ms);
}

int xfyun_spark_get_last_error(void)
{
    return g_last_error;
}
