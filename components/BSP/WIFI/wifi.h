#ifndef __WIFI_H__
#define __WIFI_H__

#include "esp_err.h"

// AP 模式下的配置
#define AP_SSID      "ESP32-Config"
#define AP_PASS      "" // 开放热点，或设置密码
#define MAX_RETRY    3  // 尝试连接保存网络的次数

/**
 * @brief 智能 WiFi 初始化
 * - 自动尝试连接保存的网络
 * - 失败则开启 AP+Web 配网模式
 */
void wifi_init_sta(void);

#endif
