#ifndef __AI_CHAT_H__
#define __AI_CHAT_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief AI对话状态枚举
 */
typedef enum {
    AI_CHAT_IDLE,           // 空闲状态
    AI_CHAT_RECORDING,      // 正在录音
    AI_CHAT_PROCESSING,     // 正在处理（上传/对话）
    AI_CHAT_PLAYING,        // 正在播放回复
    AI_CHAT_ERROR           // 错误状态
} ai_chat_state_t;

/**
 * @brief AI对话回调函数类型
 * 
 * @param state 当前对话状态
 * @param message 状态描述信息（可选）
 */
typedef void (*ai_chat_callback_t)(ai_chat_state_t state, const char *message);

/**
 * @brief 初始化AI对话模块
 * 
 * @return esp_err_t ESP_OK表示成功
 */
esp_err_t ai_chat_init(void);

/**
 * @brief 开始一次AI对话
 * 
 * 该函数会：
 * 1. 录制用户语音（固定时长或VAD检测）
 * 2. 使用讯飞IAT转换为文字
 * 3. 发送到讯飞星火获取回复
 * 4. 使用讯飞TTS转换为语音
 * 5. 播放语音回复
 * 
 * @param callback 状态回调函数（可选）
 * @return esp_err_t ESP_OK表示启动成功
 */
esp_err_t ai_chat_start_conversation(ai_chat_callback_t callback);

/**
 * @brief 停止当前对话
 * 
 * @return esp_err_t ESP_OK表示成功
 */
esp_err_t ai_chat_stop_conversation(void);

/**
 * @brief 主动结束录音阶段（"说完了"按键），继续后续 IAT→Spark→TTS 流程
 *
 * 仅在 AI_CHAT_RECORDING 阶段有效，立即停止录音并提交识别，不中断对话。
 */
void ai_chat_stop_recording(void);

/**
 * 
 * 可以在TTS播放、AI处理、甚至录音阶段调用。
 * 会中断当前操作并重新开始录音。
 */
void ai_chat_interrupt(void);

/**
 * @brief 退出对话模式（优雅地结束整个对话循环）
 *
 * 与 ai_chat_interrupt 不同，interrupt 只是中断当前轮次并开始下一轮，
 * 而 exit 会终止整个对话模式，回到空闲状态。
 */
void ai_chat_exit(void);

/**
 * @brief 清空对话历史
 * 
 * @return esp_err_t ESP_OK表示成功
 */
esp_err_t ai_chat_clear_history(void);

/**
 * @brief 获取当前对话状态
 * 
 * @return ai_chat_state_t 当前状态
 */
ai_chat_state_t ai_chat_get_state(void);

/**
 * @brief 返回对话是否正处于退出中（ai_chat_exit 已被调用但任务尚未结束）
 *
 * @return true  已请求退出
 * @return false 对话周期正常
 */
bool ai_chat_is_exiting(void);

#ifdef __cplusplus
}
#endif

#endif /* __AI_CHAT_H__ */
