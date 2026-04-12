#include "ai_chat.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "yuyin.h"
#include "xfyun_iat.h"
#include "xfyun_tts.h"
#include "xfyun_spark.h"
#include "xfyun_image.h"
#include "speaker.h"
#include "camera.h"
#include "esp_camera.h"
#include "img_converters.h"
#include <string.h>
#include <stdlib.h>

#define TAG "AI_CHAT"

#define CHAT_SAMPLE_RATE     16000
#define CHAT_TIMEOUT_MS      20000
#define CHAT_SILENCE_MS           300   // 进一步缩短为300ms，以更快结束录音
#define CHAT_NO_SPEECH_TIMEOUT_MS 5000  // 5s未检测到语音则退出对话模式
#define CHAT_SPEECH_START_THRESHOLD 70  // 说话起点阈值：兼顾安静说话与噪声抑制
#define CHAT_SILENCE_THRESHOLD 60       // 开口后静音阈值：轻声尾音不易被截断
#define CHAT_SPEECH_START_HITS 2        // 连续命中次数：降低偶发噪声误触发
#define CHAT_FORCE_SPEECH_ACTIVITY 350   // 活动度足够高时强制判定为开口（适配ICS-43434）
#define AUDIO_RINGBUF_SIZE   (48 * 1024)  // 48KB ≈ 1.5s，覆盖上一轮drain(~1.2s)+本轮IAT TLS(~0.7s)

// 全局变量
static ai_chat_state_t g_chat_state = AI_CHAT_IDLE;
static SemaphoreHandle_t g_chat_mutex = NULL;
static ai_chat_callback_t g_status_callback = NULL;
static TaskHandle_t g_chat_task_handle = NULL;
static bool g_chat_ready = false;
static volatile bool g_interrupt_flag = false;    // 按键打断标志
static volatile bool g_exit_flag = false;         // 退出对话模式标志
static volatile bool g_stop_recording_flag = false; // 按键主动结束录音标志

// 内部函数声明
static void set_chat_state(ai_chat_state_t state, const char *message);
static void chat_task(void *pvParameters);

/**
 * @brief 设置对话状态并触发回调
 */
static void set_chat_state(ai_chat_state_t state, const char *message)
{
    if (g_chat_mutex) {
        xSemaphoreTake(g_chat_mutex, portMAX_DELAY);
    }
    
    g_chat_state = state;
    
    if (g_chat_mutex) {
        xSemaphoreGive(g_chat_mutex);
    }
    
    if (g_status_callback) {
        g_status_callback(state, message);
    }
    
    ESP_LOGI(TAG, "State: %d, Message: %s", state, message ? message : "NULL");
}


// 后台任务：持续从AFE读取音频数据避免ringbuffer溢出
// 当 g_audio_ringbuf != NULL 时，将音频写入环形缓冲区供发送任务使用
// 当 g_audio_ringbuf == NULL 时，仅做排空处理
static RingbufHandle_t g_audio_ringbuf = NULL;

static void afe_reader_task(void *pvParameters)
{
    bool *keep_running = (bool *)pvParameters;
    int afe_chunk = yuyin_get_afe_chunksize();
    if (afe_chunk <= 0) afe_chunk = 512;
    
    int16_t *buf = (int16_t *)malloc(afe_chunk * sizeof(int16_t));
    if (!buf) {
        ESP_LOGE(TAG, "afe_reader: no memory");
        vTaskDelete(NULL);
        return;
    }
    
    while (*keep_running) {
        int bytes = yuyin_get_afe_audio(buf, afe_chunk, 100);
        if (bytes > 0 && g_audio_ringbuf != NULL) {
            // 写入环形缓冲区，如果满了就丢弃（不阻塞）
            if (xRingbufferSend(g_audio_ringbuf, buf, bytes, 0) != pdTRUE) {
                // 缓冲区满，丢弃最旧的数据腾出空间
                size_t item_size;
                void *old = xRingbufferReceive(g_audio_ringbuf, &item_size, 0);
                if (old) vRingbufferReturnItem(g_audio_ringbuf, old);
                xRingbufferSend(g_audio_ringbuf, buf, bytes, 0);
            }
        }
        // 无论是否有ringbuf，fetch都在yuyin_get_afe_audio中被调用了
        // 所以AFE的内部ringbuffer不会溢出
    }
    
    free(buf);
    vTaskDelete(NULL);
}

/* ===== Spark → TTS 流水线辅助 ===== */

/**
 * 去除常见 Markdown 格式符（* # _ ` ~ >）并裁剪首尾空白，就地修改。
 * UTF-8 安全：只操作 ASCII 单字节字符，不破坏多字节汉字序列。
 */
static void strip_markdown_inplace(char *s)
{
    if (!s) return;
    /* pass 1: 删除 markdown 控制字符 */
    char *r = s, *w = s;
    while (*r) {
        if (*r == '*' || *r == '#' || *r == '_' || *r == '`' || *r == '~' || *r == '>') {
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
    /* pass 2: 裁剪首部空白/换行 */
    r = s;
    while (*r == ' ' || *r == '\n' || *r == '\r') r++;
    if (r != s) memmove(s, r, strlen(r) + 1);
    /* pass 3: 裁剪尾部空白/换行 */
    size_t l = strlen(s);
    while (l > 0 && (s[l-1] == ' ' || s[l-1] == '\n' || s[l-1] == '\r')) s[--l] = '\0';
}

/* 返回第一个句末标点之后的偏移（>start），若未找到则返回 0
 * strong=true  → 仅识别强标点 。！？；
 * strong=false → 也识别弱标点 ，（逗号，用于长句强制切割）*/
static size_t find_sentence_end(const char *text, size_t start, size_t len, bool strong_only)
{
    for (size_t i = start; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n' || c == '!' || c == '?') return i + 1;
        if (i + 2 < len) {
            unsigned char b1 = (unsigned char)text[i + 1];
            unsigned char b2 = (unsigned char)text[i + 2];
            if ((c == 0xE3 && b1 == 0x80 && b2 == 0x82) ||  /* 。 */
                (c == 0xEF && b1 == 0xBC && b2 == 0x81) ||  /* ！ */
                (c == 0xEF && b1 == 0xBC && b2 == 0x9F) ||  /* ？ */
                (c == 0xEF && b1 == 0xBC && b2 == 0x9B))    /* ；(次要) */
                return i + 3;
            if ((c == 0xEF && b1 == 0xBD && b2 == 0x9E))    /* ～ (波浪号) */
                return i + 3;
            if (!strong_only &&
                (c == 0xEF && b1 == 0xBC && b2 == 0x8C))    /* ，(弱断句) */
                return i + 3;
        }
    }
    return 0;  /* 暂无完整句子 */
}

typedef struct {
    xfyun_tts_pipeline_handle_t pipe;
    size_t                       last_fed;    /* 已送入流水线的字节数 */
    volatile bool               *p_interrupt; /* 指向 g_interrupt_flag */
} spark_pipe_ctx_t;

/* 超过此长度仍无强标点时，在逗号处断句，确保 TTS 及早开始。改小一点使得第一句可以早发 */
#define SPARK_FORCE_FEED_BYTES  9 
#define MIN_TTS_SEGMENT_BYTES   6   /* ~2 Chinese characters */

/* Spark 流式回调：检测句末，将完整句子送入 TTS 流水线 */
static void spark_to_tts_chunk_cb(const char *full_text, bool is_final, void *user_ctx)
{
    spark_pipe_ctx_t *ctx = (spark_pipe_ctx_t *)user_ctx;
    if (!full_text || !ctx || !ctx->pipe || *ctx->p_interrupt) return;

    size_t full_len = strlen(full_text);
    size_t pos      = ctx->last_fed;

    /* 提取所有已有完整句末标点的片段 */
    while (pos < full_len) {
        size_t pending = full_len - ctx->last_fed;

        /* 优先用强标点切句；若积累过长则允许逗号切割 */
        bool strong_only = (pending <= SPARK_FORCE_FEED_BYTES);
        size_t end = find_sentence_end(full_text, pos, full_len, strong_only);
        if (end == 0) break;  /* 还没有句末 */

        size_t seg_len = end - ctx->last_fed;

        /* 逗号切割产生的短碎片：继续向后搜索，不立即发送
         * 强标点（。！？）结尾的短句直接发送（如"好的。"）*/
        if (!strong_only && seg_len < MIN_TTS_SEGMENT_BYTES) {
            pos = end;   /* 仅移动搜索位置，last_fed 不变 */
            continue;
        }

        if (seg_len >= 3) {   /* 至少一个汉字宽度才送 */
            char *seg = (char *)malloc(seg_len + 1);
            if (seg) {
                memcpy(seg, full_text + ctx->last_fed, seg_len);
                seg[seg_len] = '\0';
                strip_markdown_inplace(seg);
                if (seg[0] != '\0')   /* strip 后可能为空（纯 markdown 行）*/
                    xfyun_tts_pipeline_feed(ctx->pipe, seg);
                free(seg);
            }
        }
        ctx->last_fed = end;
        pos = end;
    }

    /* 最后一块：强制送出剩余文本（即使没有句末标点）*/
    if (is_final && ctx->last_fed < full_len) {
        char *seg = strdup(full_text + ctx->last_fed);
        if (seg) {
            strip_markdown_inplace(seg);
            if (seg[0] != '\0')
                xfyun_tts_pipeline_feed(ctx->pipe, seg);
            free(seg);
        }
        ctx->last_fed = full_len;
    }
}

typedef struct { xfyun_spark_conn_t *out; volatile bool done; } PreConnArg;
static PreConnArg g_preconn_arg;
static xfyun_spark_conn_t g_spark_preconn = NULL;


typedef struct { esp_websocket_client_handle_t client; volatile bool done; } ImgPreConnArg;
static ImgPreConnArg g_img_preconn_arg;

static void img_preconn_task(void *arg) {
    ImgPreConnArg *a = (ImgPreConnArg *)arg;
    a->client = xfyun_image_preconnect();
    a->done = true;
    vTaskDelete(NULL);
}

static void spark_preconn_task(void *arg) {
    PreConnArg *a = (PreConnArg *)arg;
    xfyun_spark_preconnect(a->out);
    a->done = true;
    vTaskDelete(NULL);
}

static void tts_audio_callback(const uint8_t *data, size_t len, void *user_ctx) {
    if (data && len > 0) speaker_play_pcm((const int16_t *)data, len);
}

/* 去直流后的活动度估计：
 * 1) 使用 MAD(相对均值的平均绝对偏差)抑制麦克风直流偏置；
 * 2) 叠加少量峰峰值，提升对爆破音/辅音的敏感度。 */
static int32_t estimate_pcm_activity(const int16_t *pcm, size_t samples)
{
    if (!pcm || samples == 0) return 0;

    int64_t sum = 0;
    int16_t vmin = 32767;
    int16_t vmax = -32768;
    for (size_t i = 0; i < samples; ++i) {
        int16_t v = pcm[i];
        sum += v;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }

    int32_t mean = (int32_t)(sum / (int64_t)samples);
    int64_t mad_sum = 0;
    for (size_t i = 0; i < samples; ++i) {
        int32_t d = (int32_t)pcm[i] - mean;
        mad_sum += (d >= 0) ? d : -d;
    }

    int32_t mad = (int32_t)(mad_sum / (int64_t)samples);
    int32_t p2p = (int32_t)vmax - (int32_t)vmin;
    return mad + (p2p / 8);
}

/* 基于噪声底和当前语音能量，给出自适应起声/静音阈值。 */
static int32_t calc_dynamic_start_threshold(int32_t noise_floor)
{
    int32_t th = noise_floor + 180;
    if (th < CHAT_SPEECH_START_THRESHOLD) th = CHAT_SPEECH_START_THRESHOLD;
    return th;
}

static int32_t calc_dynamic_silence_threshold(int32_t noise_floor, int32_t speech_level)
{
    int32_t th = noise_floor + ((speech_level - noise_floor) / 3);
    if (th < CHAT_SILENCE_THRESHOLD) th = CHAT_SILENCE_THRESHOLD;
    if (speech_level > 200 && th > speech_level - 120) th = speech_level - 120;
    if (th < CHAT_SILENCE_THRESHOLD) th = CHAT_SILENCE_THRESHOLD;
    return th;
}

/**
 * @brief 执行一轮对话（录音→IAT→Spark→TTS）
 * @param reader_running  指向reader task控制标志（由chat_task管理生命周期）
 * @return true 正常完成, false 被中断
 */
static bool do_one_conversation(bool *reader_running)
{
    g_interrupt_flag = false;
    g_stop_recording_flag = false;
    speaker_unmute();  // 恢复音频输出（上一轮可能被mute打断了）

    set_chat_state(AI_CHAT_RECORDING, "Recording voice...");

    /* TTS 的持久 WS 连接保持不动（PSRAM 支持多路 TLS 并存），
     * 不再 disconnect + 200ms 等待，直接连接 IAT */
    ESP_LOGI(TAG, "Free heap before IAT: %d bytes", esp_get_free_heap_size());

    /* 复用上一轮播放时预建的 ringbuf（用户可在播放期间开口，数据已缓冲）;
     * 若不存在（首轮或中断后）则新建。
     * 48KB ≈ 1.5s，覆盖上一轮 drain + 本轮 IAT TLS 握手全程。 */
    if (!g_audio_ringbuf) {
        g_audio_ringbuf = xRingbufferCreate(AUDIO_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
        if (!g_audio_ringbuf) {
            ESP_LOGE(TAG, "Failed to create audio ringbuffer");
            set_chat_state(AI_CHAT_ERROR, "No memory");
            return true;
        }
        ESP_LOGI(TAG, "Ringbuf created fresh");
    } else {
        ESP_LOGI(TAG, "Reusing pre-created ringbuf (%d bytes already buffered)",
                 (int)xRingbufferGetCurFreeSize(g_audio_ringbuf));
    }

    // 连接IAT WebSocket（ringbuf 已就绪，IAT 握手期间的音频会被缓冲保留）
    xfyun_iat_session_handle_t iat_session = NULL;
    esp_err_t iat_ret = xfyun_iat_streaming_start(&iat_session, NULL, NULL);
    if (iat_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start streaming IAT (heap=%d)", esp_get_free_heap_size());
        vRingbufferDelete(g_audio_ringbuf);
        g_audio_ringbuf = NULL;
        set_chat_state(AI_CHAT_ERROR, "Speech recognition failed");
        return true;
    }

    ESP_LOGI(TAG, "IAT connected, ringbuf buffered (heap=%d)", esp_get_free_heap_size());

    g_spark_preconn = NULL;
    g_preconn_arg.out  = &g_spark_preconn;
    g_preconn_arg.done = false;

    int afe_chunk = yuyin_get_afe_chunksize();
    if (afe_chunk <= 0) afe_chunk = 512;
    const size_t chunk_ms = (afe_chunk * 1000) / CHAT_SAMPLE_RATE;
    const size_t max_duration_s = CONFIG_AI_CHAT_RECORD_TIME;
    const size_t max_chunks = (max_duration_s * 1000) / chunk_ms;

    ESP_LOGI(TAG, "AFE chunk=%d samples (%d ms), max_chunks=%d", afe_chunk, (int)chunk_ms, (int)max_chunks);

    int silent_frames = 0;
    const int silence_frames_limit = CHAT_SILENCE_MS / chunk_ms;
    const int no_speech_frames_limit = CHAT_NO_SPEECH_TIMEOUT_MS / chunk_ms;
    int no_speech_frames = 0;
    bool has_speech = false;
    int speech_start_hits = 0;
    int32_t noise_floor = 0;
    int32_t speech_level = 0;
    bool no_speech_exit = false;
    bool iat_feed_unstable = false;

    for (size_t i = 0; i < max_chunks && !g_interrupt_flag; ++i) {
        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(g_audio_ringbuf, &item_size, pdMS_TO_TICKS(250), afe_chunk * sizeof(int16_t));

        if (item && item_size > 0) {
            size_t samples = item_size / sizeof(int16_t);
            int16_t *pcm = (int16_t *)item;

            if (i < 5 || i % 25 == 0) {
                ESP_LOGI(TAG, "chunk %d/%d (%d samples)", (int)(i+1), (int)max_chunks, (int)samples);
            }

            int32_t activity = estimate_pcm_activity(pcm, samples);
            if (noise_floor == 0) {
                noise_floor = activity;
            } else if (!has_speech) {
                noise_floor = (noise_floor * 7 + activity) / 8;
            }
            if (has_speech) {
                if (speech_level == 0) speech_level = activity;
                else speech_level = (speech_level * 7 + activity) / 8;
            }

            int32_t start_th = calc_dynamic_start_threshold(noise_floor);
            int32_t silence_th = calc_dynamic_silence_threshold(noise_floor, speech_level > 0 ? speech_level : activity);

            if (i < 5) {
                ESP_LOGI(TAG, "  activity=%d (noise=%d, speech=%d, start_th=%d, silence_th=%d)",
                         activity, noise_floor, speech_level, start_th, silence_th);
            }

            int32_t threshold = has_speech ? silence_th : start_th;
            if (activity < threshold) {
                silent_frames++;
                if (!has_speech) speech_start_hits = 0;
            } else {
                if (!has_speech) {
                    speech_start_hits++;
                    if (speech_start_hits >= CHAT_SPEECH_START_HITS || activity >= CHAT_FORCE_SPEECH_ACTIVITY) {
                        has_speech = true;
                        speech_level = activity;
                        silent_frames = 0;
                    }
                } else {
                    silent_frames = 0;
                }
            }
            if (!has_speech) {
                no_speech_frames++;
            }

            // 发送给 IAT — 可能阻塞，但不影响 AFE 读取（reader task 独立运行）
            esp_err_t feed_ret = xfyun_iat_streaming_feed(iat_session, pcm, samples);
            vRingbufferReturnItem(g_audio_ringbuf, item);

            if (feed_ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to feed audio chunk %d/%d", (int)(i+1), (int)max_chunks);
                iat_feed_unstable = true;
                break;
            }

            if (g_stop_recording_flag) {
                ESP_LOGI(TAG, "Button pressed: stopping recording early");
                /* 继续排空 ringbuf 中尚未发送的尾部音频（约 150ms），
                 * 避免最后音节（如"故事"）因按键检测延迟而被截断 */
                TickType_t tail_end = xTaskGetTickCount() + pdMS_TO_TICKS(150);
                while (xTaskGetTickCount() < tail_end && !g_interrupt_flag) {
                    size_t tail_size = 0;
                    void *tail_item = xRingbufferReceive(g_audio_ringbuf, &tail_size, pdMS_TO_TICKS(30));
                    if (tail_item && tail_size > 0) {
                        xfyun_iat_streaming_feed(iat_session, (int16_t *)tail_item, tail_size / sizeof(int16_t));
                        vRingbufferReturnItem(g_audio_ringbuf, tail_item);
                    } else {
                        if (tail_item) vRingbufferReturnItem(g_audio_ringbuf, tail_item);
                        break; /* ringbuf 已空 */
                    }
                }
                break;
            }

            if (has_speech && silent_frames >= silence_frames_limit) {
                ESP_LOGI(TAG, "Silence detected for %d ms after speech, stopping early", CHAT_SILENCE_MS);
                break;
            }
            // 超过无语音超时时间，退出对话模式（用户已离开）
            if (!has_speech && no_speech_frames >= no_speech_frames_limit) {
                ESP_LOGI(TAG, "No speech for %d ms (based on frames), returning to wake-word mode", CHAT_NO_SPEECH_TIMEOUT_MS);
                no_speech_exit = true;
                break;
            }
        } else {
            if (!has_speech) {
                const int idle_frames = (int)((250 + chunk_ms - 1) / chunk_ms);
                no_speech_frames += (idle_frames > 0) ? idle_frames : 1;
                if (no_speech_frames >= no_speech_frames_limit) {
                    ESP_LOGI(TAG, "No speech timeout reached while waiting audio chunks");
                    no_speech_exit = true;
                    if (item) vRingbufferReturnItem(g_audio_ringbuf, item);
                    break;
                }
            }
            if (item) vRingbufferReturnItem(g_audio_ringbuf, item);
        }
    }

    ESP_LOGI(TAG, "Finished sending audio chunks");

    // 停止向 ringbuf 写入（reader task 继续运行但 g_audio_ringbuf=NULL 时仅排空）
    RingbufHandle_t old_rb = g_audio_ringbuf;
    g_audio_ringbuf = NULL;
    vRingbufferDelete(old_rb);

    char *transcript = NULL;  /* 提前声明，使 goto have_transcript 路径也能访问 */

    // 如果录音阶段被中断
    if (g_interrupt_flag) {
        ESP_LOGW(TAG, "Recording interrupted, discarding");
        xfyun_iat_streaming_finish(iat_session, NULL, 1000);
        xfyun_iat_streaming_close(iat_session);
        return false;
    }

    // 无语音超时：尝试获取 IAT 结果，若服务端已识别到内容则继续对话（振幅判断误识了语音）
    if (no_speech_exit) {
        char *ns_transcript = NULL;
        esp_err_t ns_ret = xfyun_iat_streaming_finish(iat_session, &ns_transcript, 1500);
        xfyun_iat_streaming_close(iat_session);
        iat_session = NULL;
        if (ns_ret == ESP_OK && ns_transcript && strlen(ns_transcript) > 0) {
            /* IAT 实际识别到了内容（振幅阈值判断失误），继续正常对话流程 */
            ESP_LOGI(TAG, "No-speech heuristic wrong, IAT got: %s -- continuing", ns_transcript);
            /* 启动 Spark 预连接，跑正常流程 */
            BaseType_t preconn_rc2 = xTaskCreatePinnedToCore(spark_preconn_task, "spark_pre", 6 * 1024,
                                                              &g_preconn_arg, 4, NULL, 0);
            if (preconn_rc2 != pdPASS) {
                g_preconn_arg.done = true;
                ESP_LOGW(TAG, "Spark pre-connect task create failed in no-speech continuation");
            }
            while (!g_preconn_arg.done && !g_interrupt_flag) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            /* 跳过剩余 no_speech_exit 退出逻辑，直接进入 transcript 处理 */
            iat_ret = ESP_OK;
            transcript = ns_transcript;  /* 赋值给函数级变量，goto 后仍可访问 */
            goto have_transcript;
        }
        free(ns_transcript);
        ESP_LOGI(TAG, "No speech timeout confirmed, returning to wake-word mode");
        set_chat_state(AI_CHAT_IDLE, "No speech detected");
        g_exit_flag = true;
        return true;
    }

    /* ---- 录音已结束，IAT 不再发送数据，AES/DMA 空闲 ----
     * 现在安全启动 Spark 预连接：与 IAT finish 等待（服务器处理 ~1-2s）并行执行，
     * 完全避免了录音期间的三路 TLS 竞争问题。 */
    if (!iat_feed_unstable) {
        BaseType_t preconn_rc = xTaskCreatePinnedToCore(spark_preconn_task, "spark_pre", 6 * 1024,
                                                         &g_preconn_arg, 4, NULL, 0);
        if (preconn_rc == pdPASS) {
            ESP_LOGI(TAG, "Spark pre-connect started (recording done, AES free)");
        } else {
            g_preconn_arg.done = true;
            ESP_LOGW(TAG, "Spark pre-connect task create failed, will use normal connect path");
        }
    } else {
        g_preconn_arg.done = true;
        ESP_LOGW(TAG, "Skip Spark pre-connect: IAT feed was unstable in this round");
    }

    // 获取最终识别结果（与 Spark 预连接并行等待）
    iat_ret = xfyun_iat_streaming_finish(iat_session, &transcript, 10000);

    /* IAT 同步关闭，确保它的 TLS/TCP 完成销毁，防止资源争抢 */
    xfyun_iat_streaming_close(iat_session);
    iat_session = NULL;

    /* 确保预连接任务已完成（大概率在 IAT finish 等待期间已完成）
     * 必须等待其完成，否则后续立刻启动 TTS 流水线会导致多个 TLS 握手并发，极易触发 OOM */
    while (!g_preconn_arg.done && !g_interrupt_flag) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "Spark preconn done=%d, handle=%p", (int)g_preconn_arg.done, g_spark_preconn);

    if (iat_ret != ESP_OK || !transcript || strlen(transcript) == 0) {
        ESP_LOGE(TAG, "Streaming IAT failed or empty result (attempt 1)");
        if (transcript) { free(transcript); transcript = NULL; }
        /* g_spark_preconn 保留，重试成功后直接复用（with_conn 会自动处理断线重连）*/

        /* === 录音/IAT 失败：自动重试一次 === */
        if (!g_interrupt_flag && !g_exit_flag) {
            /* 提前建好 ringbuf，冷却期间的音频也能缓冲进去，避免用户开口即丢 */
            if (!g_audio_ringbuf) {
                g_audio_ringbuf = xRingbufferCreate(AUDIO_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
            }
            ESP_LOGW(TAG, "Retrying IAT after 500ms cooldown (socket cleanup)...");
            set_chat_state(AI_CHAT_RECORDING, "Retrying, please speak again...");
            vTaskDelay(pdMS_TO_TICKS(500));  // 等待 IAT socket TCP 四次挥手完成

            iat_session = NULL;
            iat_ret = xfyun_iat_streaming_start(&iat_session, NULL, NULL);
            if (iat_ret == ESP_OK) {
                /* ringbuf 若在等待期间被删除则重建 */
                if (!g_audio_ringbuf) {
                    g_audio_ringbuf = xRingbufferCreate(AUDIO_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
                }
                if (!g_audio_ringbuf) {
                    xfyun_iat_streaming_finish(iat_session, NULL, 1000);
                    xfyun_iat_streaming_close(iat_session);
                } else {
                    set_chat_state(AI_CHAT_RECORDING, "Recording voice (retry)...");
                    silent_frames = 0;
                    has_speech = false;
                    speech_start_hits = 0;
                    noise_floor = 0;
                    speech_level = 0;
                    g_stop_recording_flag = false;  /* 重试时重置 */
                    /* 重试轮次独立的无语音超时 */
                    int no_speech_frames2 = 0;
                    bool no_speech_exit2 = false;

                    for (size_t i = 0; i < max_chunks && !g_interrupt_flag; ++i) {
                        size_t item_size2 = 0;
                        void *item2 = xRingbufferReceiveUpTo(g_audio_ringbuf, &item_size2, pdMS_TO_TICKS(250), afe_chunk * sizeof(int16_t));
                        if (item2 && item_size2 > 0) {
                            size_t samples2 = item_size2 / sizeof(int16_t);
                            if (i < 5 || i % 25 == 0) {
                                ESP_LOGI(TAG, "chunk %d/%d (retry, %d samples)", (int)(i+1), (int)max_chunks, (int)samples2);
                            }
                            int16_t *pcm2 = (int16_t *)item2;

                            int32_t activity2 = estimate_pcm_activity(pcm2, samples2);
                            if (noise_floor == 0) {
                                noise_floor = activity2;
                            } else if (!has_speech) {
                                noise_floor = (noise_floor * 7 + activity2) / 8;
                            }
                            if (has_speech) {
                                if (speech_level == 0) speech_level = activity2;
                                else speech_level = (speech_level * 7 + activity2) / 8;
                            }

                            int32_t start_th2 = calc_dynamic_start_threshold(noise_floor);
                            int32_t silence_th2 = calc_dynamic_silence_threshold(noise_floor, speech_level > 0 ? speech_level : activity2);
                            int32_t threshold2 = has_speech ? silence_th2 : start_th2;
                            if (activity2 < threshold2) {
                                silent_frames++;
                                if (!has_speech) speech_start_hits = 0;
                            } else {
                                if (!has_speech) {
                                    speech_start_hits++;
                                    if (speech_start_hits >= CHAT_SPEECH_START_HITS || activity2 >= CHAT_FORCE_SPEECH_ACTIVITY) {
                                        has_speech = true;
                                        speech_level = activity2;
                                        silent_frames = 0;
                                    }
                                } else {
                                    silent_frames = 0;
                                }
                            }
                            
                            if (!has_speech) no_speech_frames2++;

                            esp_err_t fr = xfyun_iat_streaming_feed(iat_session, pcm2, samples2);
                            vRingbufferReturnItem(g_audio_ringbuf, item2);
                            if (fr != ESP_OK) break;
                            if (g_stop_recording_flag) {
                                ESP_LOGI(TAG, "Button pressed: stopping recording early (retry)");
                                TickType_t tail_end2 = xTaskGetTickCount() + pdMS_TO_TICKS(150);
                                while (xTaskGetTickCount() < tail_end2 && !g_interrupt_flag) {
                                    size_t tail_sz2 = 0;
                                    void *tail_it2 = xRingbufferReceive(g_audio_ringbuf, &tail_sz2, pdMS_TO_TICKS(30));
                                    if (tail_it2 && tail_sz2 > 0) {
                                        xfyun_iat_streaming_feed(iat_session, (int16_t *)tail_it2, tail_sz2 / sizeof(int16_t));
                                        vRingbufferReturnItem(g_audio_ringbuf, tail_it2);
                                    } else {
                                        if (tail_it2) vRingbufferReturnItem(g_audio_ringbuf, tail_it2);
                                        break;
                                    }
                                }
                                break;
                            }
                            if (has_speech && silent_frames >= silence_frames_limit) break;
                            /* 超过无语音超时，退出重试 */
                            if (!has_speech && no_speech_frames2 >= no_speech_frames_limit) {
                                no_speech_exit2 = true;
                                break;
                            }
                        } else {
                            if (!has_speech) {
                                const int idle_frames2 = (int)((250 + chunk_ms - 1) / chunk_ms);
                                no_speech_frames2 += (idle_frames2 > 0) ? idle_frames2 : 1;
                                if (no_speech_frames2 >= no_speech_frames_limit) {
                                    no_speech_exit2 = true;
                                    if (item2) vRingbufferReturnItem(g_audio_ringbuf, item2);
                                    break;
                                }
                            }
                            if (item2) vRingbufferReturnItem(g_audio_ringbuf, item2);
                        }
                    }

                    old_rb = g_audio_ringbuf; g_audio_ringbuf = NULL; vRingbufferDelete(old_rb);

                    if (no_speech_exit2) {
                        xfyun_iat_streaming_finish(iat_session, NULL, 500);
                        xfyun_iat_streaming_close(iat_session);
                        ESP_LOGI(TAG, "No speech in retry, returning to wake-word mode");
                        if (g_spark_preconn) { xfyun_spark_conn_close(g_spark_preconn); g_spark_preconn = NULL; }
                        set_chat_state(AI_CHAT_IDLE, "No speech detected");
                        g_exit_flag = true;
                        return true;
                    }

                    if (!g_interrupt_flag) {
                        iat_ret = xfyun_iat_streaming_finish(iat_session, &transcript, 10000);
                    } else {
                        xfyun_iat_streaming_finish(iat_session, NULL, 1000);
                    }
                    xfyun_iat_streaming_close(iat_session);
                }
            }

            if (iat_ret != ESP_OK || !transcript || strlen(transcript) == 0) {
                ESP_LOGE(TAG, "Streaming IAT retry also failed");
                if (g_spark_preconn) { xfyun_spark_conn_close(g_spark_preconn); g_spark_preconn = NULL; }
                set_chat_state(AI_CHAT_ERROR, "Speech recognition failed");
                if (transcript) free(transcript);
                return true;
            }
            ESP_LOGI(TAG, "IAT retry succeeded: %s", transcript);
        } else {
            if (g_spark_preconn) { xfyun_spark_conn_close(g_spark_preconn); g_spark_preconn = NULL; }
            set_chat_state(AI_CHAT_ERROR, "Speech recognition failed");
            return g_interrupt_flag ? false : true;
        }
    }

    ESP_LOGI(TAG, "Transcript: %s", transcript);

    /* 给 Spark 的指令前缀：要求口语化、禁 Markdown。
     * 口语化指令能让模型直接开口说话，跳过"标题：《...》"等结构，
     * 大幅减少 TTFT（首 token 输出时间）。
     * 字数限制在50字以内，确保TTS快速播放完毕、用户无需久等。 */
have_transcript:;   /* no_speech_exit 但 IAT 有结果时跳入此处 */

    // ========== 意图判断：是否调用图片理解 ==========
    bool is_image_intent = false;
    if (transcript) {
        if (strstr(transcript, "拍照") || strstr(transcript, "这是什么") || 
            strstr(transcript, "图片") || strstr(transcript, "看下这张图") || 
            strstr(transcript, "看这张图") || strstr(transcript, "这是啥") ||
            strstr(transcript, "看看") || strstr(transcript, "帮我看") ||
            strstr(transcript, "看到了什么") || strstr(transcript, "周围")) {
            is_image_intent = true;
        }
    }

    if (is_image_intent) {
        ESP_LOGI(TAG, "Image intent detected, tearing down SPARK/TTS and preconnecting Image WS");
        if (g_spark_preconn) { xfyun_spark_conn_close(g_spark_preconn); g_spark_preconn = NULL; }
        xfyun_tts_disconnect();

        g_img_preconn_arg.client = NULL;
        g_img_preconn_arg.done = false;
        BaseType_t img_preconn_rc = xTaskCreatePinnedToCore(img_preconn_task, "img_pre", 8 * 1024, &g_img_preconn_arg, 4, NULL, 0);
        if (img_preconn_rc != pdPASS) {
            g_img_preconn_arg.done = true;
            ESP_LOGW(TAG, "Image pre-connect task create failed");
        } else {
            ESP_LOGI(TAG, "Image pre-connect started concurrently with camera capture");
        }

        set_chat_state(AI_CHAT_PROCESSING, "Taking picture...");
        
        speaker_unmute();
        speaker_play_prompt();
        vTaskDelay(pdMS_TO_TICKS(100)); // 让提示音响完
        
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "获取摄像头画面失败");
            char *err_reply = strdup("抱歉，摄像头获取画面失败");
            if (transcript) free(transcript);
            transcript = err_reply;
            goto fallback_to_chat;
        }

        uint8_t *jpg_buf = NULL;
        size_t jpg_len = 0;
        bool converted = frame2jpg(fb, 30, &jpg_buf, &jpg_len); // Lower quality to 30 to speed up base64/upload
        esp_camera_fb_return(fb);
        
        if (!converted || !jpg_buf) {
            ESP_LOGE(TAG, "图片转码失败");
            char *err_reply = strdup("图片处理失败");
            if (transcript) free(transcript);
            transcript = err_reply;
            goto fallback_to_chat;
        }
        
        ESP_LOGI(TAG, "拍照成功，JPEG大小: %d 字节", (int)jpg_len);
        if (g_spark_preconn) { xfyun_spark_conn_close(g_spark_preconn); g_spark_preconn = NULL; }
        xfyun_tts_disconnect(); // 释放TLS内存以便图生文请求
        
        set_chat_state(AI_CHAT_PROCESSING, "Uploading image to Spark...");
        char *image_reply = NULL;
        char *img_prompt = NULL;
        if (transcript) {
            asprintf(&img_prompt, "你是一个视觉助手，请用自然口语详细描述这张图片的内容。用户原话是：%s", transcript);
        } else {
            asprintf(&img_prompt, "你是一个视觉助手，请用自然口语详细描述这张图片的内容。");
        }
        
        // Wait for pre-connect to finish if checking
        while (!g_img_preconn_arg.done && !g_interrupt_flag) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        set_chat_state(AI_CHAT_PLAYING, "Connecting TTS for Image...");
        static const xfyun_tts_params_t img_tts_params = { .voice_name = "x4_yezi", .speed = 65, .volume = 10, .pitch = 50 };
        xfyun_tts_pipeline_handle_t img_tts_pipe = NULL;
        xfyun_tts_pipeline_start(&img_tts_params, tts_audio_callback, NULL, &img_tts_pipe);
        if (!img_tts_pipe) {
            vTaskDelay(pdMS_TO_TICKS(400));
            xfyun_tts_pipeline_start(&img_tts_params, tts_audio_callback, NULL, &img_tts_pipe);
        }
        if (img_tts_pipe) {
            speaker_start_stream();
        }
        
        spark_pipe_ctx_t img_ctx = { .pipe = img_tts_pipe, .last_fed = 0, .p_interrupt = &g_interrupt_flag };

        esp_err_t img_ret;
        if (g_img_preconn_arg.client != NULL) {
            img_ret = xfyun_image_understand_with_conn(g_img_preconn_arg.client, jpg_buf, jpg_len, img_prompt, spark_to_tts_chunk_cb, &img_ctx, &image_reply, 30000);
        } else {
            img_ret = xfyun_image_understand(jpg_buf, jpg_len, img_prompt, spark_to_tts_chunk_cb, &img_ctx, &image_reply, 30000);
        }
        if (img_prompt) free(img_prompt);
        free(jpg_buf);
        if (img_ret == ESP_OK && image_reply) {
            ESP_LOGI(TAG, "图片理解成功: %s", image_reply);
            
            // Flush any remaining text in the pipeline
            spark_to_tts_chunk_cb(image_reply, true, &img_ctx);

            if (transcript) free(transcript);
            transcript = NULL;

            if (img_tts_pipe) {
                // 等待流水线合成播放完毕，而不是通过 xfyun_tts_synthesize 重复合成
                xfyun_tts_pipeline_finish(img_tts_pipe, portMAX_DELAY, &g_interrupt_flag);
            }

            speaker_drain(6000);
            speaker_stop_stream();
            speaker_mute();

            free(image_reply);
            return !g_interrupt_flag;
        } else {
            ESP_LOGE(TAG, "图片理解API请求失败");
            if (img_tts_pipe) {
                xfyun_tts_pipeline_abort(img_tts_pipe);
                xfyun_tts_pipeline_finish(img_tts_pipe, 3000, NULL);
            }
            if (image_reply) free(image_reply);
            char *err_reply = strdup("图片理解失败，可能是网络问题");
            if (transcript) free(transcript);
            transcript = err_reply;
            goto fallback_to_chat;
        }
    }

fallback_to_chat:

    static const char SPARK_PREFIX[] =
        "用简单的口语直接回答（禁Markdown）。问题：";
    size_t prefix_len  = sizeof(SPARK_PREFIX) - 1;
    size_t trans_len   = strlen(transcript);
    char *spark_input  = (char *)malloc(prefix_len + trans_len + 1);
    if (spark_input) {
        memcpy(spark_input, SPARK_PREFIX, prefix_len);
        memcpy(spark_input + prefix_len, transcript, trans_len);
        spark_input[prefix_len + trans_len] = '\0';
    } else {
        spark_input = transcript;  /* 内存不足时降级，使用原始文本 */
    }

    // ===== 流水线模式：Spark 流式输出并发送入 TTS 合成 =====
    set_chat_state(AI_CHAT_PLAYING, "Connecting TTS...");

    // 1. 先尝试启动 TTS 流水线任务（避免先开 speaker task 抬高任务峰值）
    static const xfyun_tts_params_t tts_params = {
        .voice_name = "x4_yezi", .speed = 65, .volume = 10, .pitch = 50
    };
    xfyun_tts_pipeline_handle_t tts_pipe = NULL;
    xfyun_tts_pipeline_start(&tts_params, tts_audio_callback, NULL, &tts_pipe);

    // tts_pipe 为 NULL 说明任务创建失败（任务峰值），稍等后重试一次
    if (!tts_pipe) {
        ESP_LOGW(TAG, "TTS pipeline create failed, waiting 400ms and retrying once...");
        vTaskDelay(pdMS_TO_TICKS(400));
        xfyun_tts_pipeline_start(&tts_params, tts_audio_callback, NULL, &tts_pipe);
    }
    // 2. 流水线创建成功后再启动 speaker 流
    if (tts_pipe) {
        speaker_start_stream();
    }
    // 两次都失败：降级为先用 Spark 获取回复再同步 TTS 播放
    if (!tts_pipe) {
        ESP_LOGW(TAG, "TTS pipeline create failed twice, falling back to Spark+sync TTS");
        speaker_start_stream();
        set_chat_state(AI_CHAT_PROCESSING, "Chatting with AI (fallback)...");
        char *ai_fb_reply = NULL;
        esp_err_t spark_fb_ret;
        if (g_spark_preconn) {
            ESP_LOGI(TAG, "Fallback: using pre-connected Spark WS");
            spark_fb_ret = xfyun_spark_chat_stream_with_conn(
                g_spark_preconn, spark_input, NULL, NULL, &ai_fb_reply, 30000);
            g_spark_preconn = NULL;  /* 句柄已在函数内释放 */
        } else {
            spark_fb_ret = xfyun_spark_chat_stream(spark_input, NULL, NULL, &ai_fb_reply, 30000);
        }
        if (spark_input != transcript) free(spark_input);
        free(transcript);
        if (spark_fb_ret == ESP_OK && ai_fb_reply && !g_interrupt_flag) {
            set_chat_state(AI_CHAT_PLAYING, "Playing AI response (sync)...");
            xfyun_tts_params_t tts_p = { .voice_name = "x4_yezi", .speed = 50, .volume = 10, .pitch = 50 };
            xfyun_tts_synthesize(ai_fb_reply, &tts_p, tts_audio_callback, NULL, 60000);
            free(ai_fb_reply);
            speaker_drain(6000);
        } else {
            if (g_spark_preconn) { xfyun_spark_conn_close(g_spark_preconn); g_spark_preconn = NULL; }
            if (ai_fb_reply) free(ai_fb_reply);
            set_chat_state(AI_CHAT_ERROR, "Failed to get chat response");
            speaker_stop_stream();
            return true;
        }
        return !g_interrupt_flag;
    }

    // 3. Spark 流式请求
    //    优先使用预连接句柄（省去 TLS 握手）；若预连接失败则回退到普通方式
    set_chat_state(AI_CHAT_PROCESSING, "Chatting with AI...");
    spark_pipe_ctx_t spark_ctx = {
        .pipe        = tts_pipe,
        .last_fed    = 0,
        .p_interrupt = &g_interrupt_flag
    };
    char *ai_reply = NULL;
    esp_err_t spark_ret;
    if (g_spark_preconn) {
        ESP_LOGI(TAG, "Using pre-connected Spark WS (no TLS handshake needed)");
        spark_ret = xfyun_spark_chat_stream_with_conn(
            g_spark_preconn, spark_input,
            spark_to_tts_chunk_cb, &spark_ctx, &ai_reply, 30000);
        g_spark_preconn = NULL;  /* 句柄已在函数内释放 */
    } else {
        ESP_LOGW(TAG, "Spark pre-connect not ready, falling back to normal connect");
        spark_ret = xfyun_spark_chat_stream(
            spark_input, spark_to_tts_chunk_cb, &spark_ctx, &ai_reply, 30000);
    }
    // 注意：spark_input / transcript 在本节后才释放，error=-6 重试仍需使用 spark_input

    // 4a. 被中断：中止流水线并丢弃结果
    if (g_interrupt_flag) {
        ESP_LOGW(TAG, "Spark/TTS interrupted");
        if (tts_pipe) { xfyun_tts_pipeline_abort(tts_pipe); xfyun_tts_pipeline_finish(tts_pipe, 3000, NULL); }
        if (ai_reply) free(ai_reply);
        if (spark_input != transcript) free(spark_input);
        free(transcript);
        speaker_mute();
        speaker_stop_stream();
        return false;
    }

    // 4b. Spark 失败：中止流水线，按错误类型处理
    if (spark_ret != ESP_OK || !ai_reply) {
        int spark_err = xfyun_spark_get_last_error();
        ESP_LOGE(TAG, "Spark API 调用失败 (error=%d)", spark_err);
        if (tts_pipe) { xfyun_tts_pipeline_abort(tts_pipe); xfyun_tts_pipeline_finish(tts_pipe, 3000, NULL); }
        tts_pipe = NULL;

        /* error=-6：服务端在响应前静默关闭了连接（多路 TLS 竞争 / 空闲超时）
         * → 等 300ms 后用普通路径重试一次，不直接报错退出 */
        if (spark_err == -6 && !g_interrupt_flag) {
            ESP_LOGW(TAG, "Spark disconnected before response, retrying once...");
            vTaskDelay(pdMS_TO_TICKS(300));
            if (ai_reply) { free(ai_reply); ai_reply = NULL; }
            speaker_stop_stream();
            speaker_start_stream();
            xfyun_tts_pipeline_start(&tts_params, tts_audio_callback, NULL, &tts_pipe);
            if (tts_pipe) {
                spark_pipe_ctx_t retry_ctx = { .pipe = tts_pipe, .last_fed = 0,
                                               .p_interrupt = &g_interrupt_flag };
                spark_ret = xfyun_spark_chat_stream(
                    spark_input, spark_to_tts_chunk_cb, &retry_ctx, &ai_reply, 30000);
                if (spark_ret == ESP_OK && ai_reply) {
                    ESP_LOGI(TAG, "Spark retry OK: %s", ai_reply);
                    goto spark_ok;
                }
                xfyun_tts_pipeline_abort(tts_pipe);
                xfyun_tts_pipeline_finish(tts_pipe, 3000, NULL);
                tts_pipe = NULL;
                spark_err = xfyun_spark_get_last_error();
                ESP_LOGE(TAG, "Spark retry also failed (error=%d)", spark_err);
            }
        }

        /* 内容审核相关错误：10013/10014/10019/10021 → 用TTS提示用户 */
        if (spark_err == 10013 || spark_err == 10014 ||
            spark_err == 10019 || spark_err == 10021) {
            ESP_LOGW(TAG, "Content moderation triggered, notifying user via TTS");
            set_chat_state(AI_CHAT_PLAYING, "Content moderation...");
            xfyun_tts_params_t tts_p = { .voice_name = "x4_yezi", .speed = 50, .volume = 10, .pitch = 50 };
            xfyun_tts_synthesize("抱歉，该问题涉及敏感内容，请换个话题。",
                                 &tts_p, tts_audio_callback, NULL, 30000);
            if (spark_input != transcript) free(spark_input);
            free(transcript);
            speaker_drain(3000);
            return true;
        }

        if (spark_input != transcript) free(spark_input);
        free(transcript);
        set_chat_state(AI_CHAT_ERROR, "Failed to get chat response");
        speaker_stop_stream();
        return true;
    }

spark_ok:
    if (spark_input != transcript) free(spark_input);
    free(transcript);
    ESP_LOGI(TAG, "AI replied (%d bytes): %.80s...", (int)strlen(ai_reply), ai_reply);
    set_chat_state(AI_CHAT_PLAYING, "Playing AI response...");

    // 6. 等待流水线完成所有句子合成（Spark 已结束，队列会被哨兵关闭）
    if (tts_pipe) {
        xfyun_tts_pipeline_finish(tts_pipe, portMAX_DELAY, &g_interrupt_flag);
        tts_pipe = NULL;
    }
    free(ai_reply);

    if (g_interrupt_flag) {
        // 中断：清理可能残留的 ringbuf
        if (g_audio_ringbuf) {
            vRingbufferDelete(g_audio_ringbuf);
            g_audio_ringbuf = NULL;
        }
        speaker_mute();
        speaker_stop_stream();
        return false;
    }

    // 7. 排空 DMA 缓冲区，确保最后的音频播放完整
    // 注意：这里决不能在排空前预建下一轮的 ringbuf，否则麦克风会把当前正在播放的最后几句 TTS 音频全部录进去，
    // 导致严重的“自问自答”（录入扬声器声音并触发下一轮 IAT 识别）。
    speaker_drain(6000);
    
    // 播放彻底结束后，再去停止扬声器等外设流，静音
    speaker_stop_stream();
    speaker_mute();

    return true;
}

/**
 * @brief AI对话主任务 — 支持连续对话
 */
static void chat_task(void *pvParameters)
{
    ESP_LOGI(TAG, "AI Chat task started (continuous mode)");

    // 暂停命令词识别
    yuyin_pause();

    // 对话期间关闭 WiFi 省电模式（modem-sleep 会导致 ~307ms 网络停顿，
    // 使讯飞 IAT 音频上传出现明显卡顿，引发服务端 error 10165）
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "WiFi power save disabled for voice session");

    // 启动 AFE reader task — 整个对话期间持续运行，保持 AFE ringbuffer 不溢出
    bool reader_running = true;
    TaskHandle_t reader_task = NULL;
    xTaskCreatePinnedToCore(afe_reader_task, "afe_reader", 4 * 1024, &reader_running, 5, &reader_task, 1);

    bool keep_going = true;
    while (keep_going) {
        g_interrupt_flag = false;

        // 检查退出标志
        if (g_exit_flag) {
            ESP_LOGW(TAG, "Exit flag set, leaving conversation mode");
            break;
        }

        bool completed = do_one_conversation(&reader_running);

        // 对话过程中收到退出请求
        if (g_exit_flag) {
            ESP_LOGW(TAG, "Exit requested during conversation");
            break;
        }

        if (completed) {
            if (g_chat_state == AI_CHAT_ERROR) {
                // IAT 连续识别失败（通常是网络问题）→ 退出对话，回到唤醒词模式
                ESP_LOGI(TAG, "Recognition failed, returning to wake-word mode");
                set_chat_state(AI_CHAT_IDLE, "Recognition failed");
                break;
            }
            // 正常完成一轮 → 立刻开始下一轮监听（连续对话模式）
            ESP_LOGI(TAG, "Conversation round done, listening again...");
        } else {
            // 被中断 → 立刻开始下一轮（连续对话模式）
            ESP_LOGI(TAG, "Interrupted, starting new round immediately");
        }
    }

    set_chat_state(AI_CHAT_IDLE, "Conversation ended");

    // 清理可能残留的预建 ringbuf（g_exit_flag 退出时 do_one_conversation 未消耗）
    if (g_audio_ringbuf) {
        RingbufHandle_t rb = g_audio_ringbuf;
        g_audio_ringbuf = NULL;   // 先置 NULL，reader task 停止写入
        vRingbufferDelete(rb);
    }

    // 确保 speaker stream 已停止（正常成功轮次不调用 speaker_stop_stream，这里兜底清理）
    speaker_stop_stream();

    // 停止 reader task
    reader_running = false;
    vTaskDelay(pdMS_TO_TICKS(200));  // 等 reader 退出

    // 恢复 WiFi 省电模式
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    ESP_LOGI(TAG, "WiFi power save restored");

    yuyin_resume();
    g_chat_task_handle = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief 初始化AI对话模块
 */
esp_err_t ai_chat_init(void)
{
    ESP_LOGI(TAG, "Initializing AI Chat module (Xunfei Spark)...");

    // 初始化讯飞 IAT 客户端
    xfyun_iat_config_t iat_cfg = {
        .app_id = CONFIG_XFYUN_APP_ID,
        .api_key = CONFIG_XFYUN_API_KEY,
        .api_secret = CONFIG_XFYUN_API_SECRET,
    };

    esp_err_t iat_ret = xfyun_iat_init(&iat_cfg);
    if (iat_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init Xunfei IAT client: %s", esp_err_to_name(iat_ret));
        return iat_ret;
    }
    
    // 初始化讯飞 TTS 客户端
    xfyun_tts_config_t tts_cfg = {
        .app_id = CONFIG_XFYUN_APP_ID,
        .api_key = CONFIG_XFYUN_API_KEY,
        .api_secret = CONFIG_XFYUN_API_SECRET,
    };
    
    esp_err_t tts_ret = xfyun_tts_init(&tts_cfg);
    if (tts_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init Xunfei TTS client: %s", esp_err_to_name(tts_ret));
        return tts_ret;
    }

    xfyun_spark_config_t spark_cfg = {
        .app_id = CONFIG_XFYUN_APP_ID,
        .api_key = CONFIG_XFYUN_API_KEY,
        .api_secret = CONFIG_XFYUN_API_SECRET,
        .domain = "generalv3", // 降级为 v3.0 (Spark Pro)，参数量更小，输出字数的速度会比 v3.5 (Max) 快一点，且同样支持搜索
    };

    esp_err_t spark_ret = xfyun_spark_init(&spark_cfg);
    if (spark_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init Xunfei Spark client: %s", esp_err_to_name(spark_ret));
        return spark_ret;
    }
    
    // 创建互斥锁
    g_chat_mutex = xSemaphoreCreateMutex();
    if (!g_chat_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    
    g_chat_state = AI_CHAT_IDLE;
    g_chat_ready = true;
    
    ESP_LOGI(TAG, "AI Chat module initialized successfully");
    
    return ESP_OK;
}

/**
 * @brief 开始一次AI对话（连续模式）
 */
esp_err_t ai_chat_start_conversation(ai_chat_callback_t callback)
{
    if (!g_chat_ready) {
        ESP_LOGE(TAG, "AI Chat not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 如果已经在对话中，当作打断处理
    if (g_chat_task_handle != NULL) {
        ESP_LOGI(TAG, "Chat already in progress, treating as interrupt");
        ai_chat_interrupt();
        return ESP_OK;
    }
    
    g_status_callback = callback;
    g_interrupt_flag = false;
    g_exit_flag = false;
    
    // 创建对话任务
    BaseType_t ret = xTaskCreatePinnedToCore(
        chat_task,
        "ai_chat_task",
        16 * 1024,
        NULL,
        5,
        &g_chat_task_handle,
        1
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create chat task");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

/**
 * @brief 主动结束录音阶段（等同于按下"说完了"），继续后续 IAT→Spark→TTS 流程
 */
void ai_chat_stop_recording(void)
{
    ESP_LOGI(TAG, ">>> STOP RECORDING requested <<<");
    g_stop_recording_flag = true;
}

/**
 * @brief 打断当前对话，立即开始新一轮录音
 */
void ai_chat_interrupt(void)
{
    ESP_LOGW(TAG, ">>> INTERRUPT requested <<<");
    g_interrupt_flag = true;

    // 中断 Spark 流式请求（如果在等待中）
    xfyun_spark_abort();
    
    // 中断图片理解
    xfyun_image_abort();

    // 如果正在TTS播放，中止它
    xfyun_tts_abort();
    
    // 立即静音并清空I2S DMA缓冲区，确保声音立刻停止
    speaker_mute();
}

/**
 * @brief 退出对话模式（优雅地结束整个对话循环）
 */
void ai_chat_exit(void)
{
    ESP_LOGW(TAG, ">>> EXIT requested <<<");
    g_exit_flag = true;
    g_interrupt_flag = true;   // 同时中断当前操作

    // 中断 Spark 流式请求（如果在等待中）
    xfyun_spark_abort();
    
    // 中断图片理解
    xfyun_image_abort();

    // 如果正在TTS播放，中止它
    xfyun_tts_abort();

    // 立即静音
    speaker_mute();
}

bool ai_chat_is_exiting(void)
{
    return g_exit_flag;
}

/**
 * @brief 停止当前对话
 */
esp_err_t ai_chat_stop_conversation(void)
{
    if (g_chat_task_handle) {
        vTaskDelete(g_chat_task_handle);
        g_chat_task_handle = NULL;
        set_chat_state(AI_CHAT_IDLE, "Conversation stopped");
    }
    
    return ESP_OK;
}

/**
 * @brief 清空对话历史
 */
esp_err_t ai_chat_clear_history(void)
{
    // TODO: 如果需要持久化对话历史，可在此清空
    ESP_LOGI(TAG, "Chat history cleared");
    return ESP_OK;
}

/**
 * @brief 获取当前对话状态
 */
ai_chat_state_t ai_chat_get_state(void)
{
    ai_chat_state_t state;
    
    if (g_chat_mutex) {
        xSemaphoreTake(g_chat_mutex, portMAX_DELAY);
    }
    
    state = g_chat_state;
    
    if (g_chat_mutex) {
        xSemaphoreGive(g_chat_mutex);
    }
    
    return state;
}
