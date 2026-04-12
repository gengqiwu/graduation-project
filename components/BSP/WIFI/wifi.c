#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h" // 引入 MAC 地址相关宏
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "wifi.h"
#include "led.h"

static const char *TAG = "WIFI_MGR";
static int s_retry_num = 0;
// 标志位：是否正在扫描 (用于阻止扫描期间的自动重连)
static bool s_is_scanning = false;
static httpd_handle_t server = NULL;

/* --- Web 配网页面 HTML --- */
static const char* index_html = 
    "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>ESP32 Config</title>"
    "<style>body{font-family:Arial,sans-serif;background:#f0f2f5;padding:20px;text-align:center;max-width:500px;margin:0 auto}"
    ".card{background:#fff;padding:20px;border-radius:12px;box-shadow:0 4px 12px rgba(0,0,0,0.1)}"
    "h2{color:#333;margin-bottom:20px}input{width:100%;padding:12px;margin:10px 0;border:1px solid #ddd;border-radius:8px;box-sizing:border-box}"
    "button{width:100%;padding:12px;border:none;border-radius:8px;font-size:16px;cursor:pointer;margin-top:10px;transition:0.3s}"
    ".btn-primary{background:#007bff;color:#fff}.btn-primary:active{background:#0056b3}"
    ".btn-scan{background:#28a745;color:#fff;margin-bottom:15px}.btn-scan:active{background:#1e7e34}"
    "#wifi-list{text-align:left;max-height:300px;overflow-y:auto;border:1px solid #eee;border-radius:8px;display:none;margin-bottom:15px}"
    ".wifi-item{padding:12px;border-bottom:1px solid #eee;cursor:pointer;display:flex;justify-content:space-between;align-items:center}"
    ".wifi-item:last-child{border-bottom:none}.wifi-item:hover{background:#f8f9fa}"
    ".rssi{font-size:12px;color:#666;background:#e9ecef;padding:2px 6px;border-radius:4px}"
    ".loading{display:none;color:#666;margin:10px 0}</style>"
    "<script>"
    "function sel(ssid){document.getElementById('ssid').value=ssid;document.getElementById('pass').focus();}"
    "function scan(){"
    "  var list=document.getElementById('wifi-list');var load=document.getElementById('loading');"
    "  list.style.display='none';load.style.display='block';list.innerHTML='';"
    "  fetch('/scan').then(r=>r.json()).then(d=>{"
    "    load.style.display='none';list.style.display='block';"
    "    d.forEach(w=>{"
    "      var div=document.createElement('div');div.className='wifi-item';"
    "      div.onclick=function(){sel(w.s)};"
    "      div.innerHTML='<span>'+w.s+'</span><span class=\"rssi\">'+w.r+'dBm</span>';"
    "      list.appendChild(div);"
    "    });"
    "  }).catch(e=>{load.textContent='扫描失败';});"
    "}"
    "</script></head><body>"
    "<div class='card'><h2>WiFi 配网</h2>"
    "<button type='button' class='btn-scan' onclick='scan()'>🔍 扫描附近 WiFi</button>"
    "<div id='loading' class='loading'>正在扫描中...</div>"
    "<div id='wifi-list'></div>"
    "<form action='/settings' method='post'>"
    "<input type='text' id='ssid' name='ssid' placeholder='WiFi 名称 (SSID)' required>"
    "<input type='password' id='pass' name='password' placeholder='WiFi 密码'>"
    "<button type='submit' class='btn-primary'>保存并连接</button>"
    "</form></div></body></html>";

/* URL 解码辅助函数 */
static void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a'-'A';
            if (a >= 'A') a -= ('A' - 10); else a -= '0';
            if (b >= 'a') b -= 'a'-'A';
            if (b >= 'A') b -= ('A' - 10); else b -= '0';
            *dst++ = 16*a+b;
            src+=3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* 查找参数值 (简单实现) */
static void get_param(const char* data, const char* key, char* out_val, int out_len) {
    char* start = strstr(data, key);
    if (!start) return;
    start += strlen(key) + 1; // skip key and '='
    char* end = strchr(start, '&');
    int len = end ? (end - start) : strlen(start);
    if (len >= out_len) len = out_len - 1;
    
    char temp[100] = {0};
    strncpy(temp, start, len);
    url_decode(out_val, temp);
}

static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t settings_handler(httpd_req_t *req) {
    char buf[200];
    int ret, remaining = req->content_len;
    if (remaining >= sizeof(buf)) remaining = sizeof(buf) - 1;

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    char ssid[32] = {0};
    char password[64] = {0};
    
    get_param(buf, "ssid", ssid, sizeof(ssid));
    get_param(buf, "password", password, sizeof(password));

    ESP_LOGI(TAG, "Received SSID: %s, PASS: %s", ssid, password);

    if (strlen(ssid) > 0) {
        wifi_config_t wifi_config = {0};
        memcpy(wifi_config.sta.ssid, ssid, strlen(ssid));
        memcpy(wifi_config.sta.password, password, strlen(password));
        
        esp_wifi_set_storage(WIFI_STORAGE_FLASH); // 确保写入 Flash
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        
        httpd_resp_send(req, "保存成功！设备正在重启连接新网络...", HTTPD_RESP_USE_STRLEN);
        
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart(); // 重启生效
    } else {
        httpd_resp_send(req, "SSID 不能为空", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

/* 扫描 WiFi 处理函数 */
static esp_err_t scan_handler(httpd_req_t *req) {
    // 0. 设置扫描标志位，阻止自动连接
    s_is_scanning = true;

    // 1. 系统正在通过 STA 连接时不允许扫描，先断开连接
    esp_wifi_disconnect();
    
    // 2. 确保在 APSTA 模式
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        vTaskDelay(pdMS_TO_TICKS(100)); // 让模式切换事件先处理
    }
    
    // 3. 设置扫描配置
    wifi_scan_config_t scan_config = {
        .ssid = 0,
        .bssid = 0,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100, // 减少扫描时间，避免 AP 断连
        .scan_time.active.max = 300
    };
    
    // 4. 开始扫描 (阻塞模式)
    ESP_LOGI(TAG, "Start scanning...");
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    
    // 恢复扫描标志位
    s_is_scanning = false;

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(ret));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // 3. 获取扫描结果
    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(ap_num * sizeof(wifi_ap_record_t));
    if (!ap_list) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    esp_wifi_scan_get_ap_records(&ap_num, ap_list);

    // 4. 构建 JSON 响应
    char *json_response = (char *)malloc(ap_num * 100 + 32); // 估算缓冲区大小
    if (!json_response) {
        free(ap_list);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    strcpy(json_response, "[");
    for (int i = 0; i < ap_num; i++) {
        char item[128];
        // 过滤空 SSID
        if (strlen((char *)ap_list[i].ssid) > 0) {
            snprintf(item, sizeof(item), "{\"s\":\"%s\",\"r\":%d}%s", 
                ap_list[i].ssid, ap_list[i].rssi, (i < ap_num - 1) ? "," : "");
            strcat(json_response, item);
        }
    }
    // 处理最后一个逗号
    if (json_response[strlen(json_response)-1] == ',') {
        json_response[strlen(json_response)-1] = '\0';
    }
    strcat(json_response, "]");

    // 5. 发送响应
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_response, strlen(json_response));

    // 6. 清理内存
    free(ap_list);
    free(json_response);
    return ESP_OK;
}

static void start_web_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 3; 
    config.stack_size = 8192; // 增加 WebServer 堆栈以处理 JSON

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/", .method = HTTP_GET, .handler = index_handler
        });
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/settings", .method = HTTP_POST, .handler = settings_handler
        });
        // [新增] 注册扫描接口
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/scan", .method = HTTP_GET, .handler = scan_handler
        });
        ESP_LOGI(TAG, "Web Server Started");
    }
}

static void start_ap_mode(void) {
    ESP_LOGI(TAG, "Entering AP Mode for Configuration...");
    
    // 安全地停止 WiFi，忽略状态错误
    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "WiFi stop failed: %s, continue anyway", esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(100)); // 短暂延时
    }
    
    esp_wifi_set_mode(WIFI_MODE_AP);
    
    wifi_config_t ap_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN
        },
    };
    if (strlen(AP_PASS) > 0) {
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        strncpy((char*)ap_config.ap.password, AP_PASS, 64);
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "AP Mode Started. SSID: %s, IP: 192.168.4.1", AP_SSID);
    
    // 启动 Web Server
    start_web_server();
    
    // 亮紫色灯提示 AP 模式
    led_set_color(150, 0, 150);
}

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!s_is_scanning) {
            esp_wifi_connect();
        }
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_is_scanning) {
            if (s_retry_num < MAX_RETRY) {
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGI(TAG, "Retry connect %d...", s_retry_num);
                led_set_color(50, 50, 0); // 黄灯重连中
            } else {
                ESP_LOGW(TAG, "Connect failed. Switching to AP mode.");
                start_ap_mode(); // 超过重试次数，切 AP
            }
        }
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        led_set_color(0, 50, 0); // 绿灯常亮
    }
    // [新增] 监听 AP 相关事件，打印调试信息
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station "MACSTR" joined, AID=%d", MAC2STR(event->mac), event->aid);
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Station "MACSTR" left, AID=%d", MAC2STR(event->mac), event->aid);
    }
}


void wifi_init_sta(void) {
    s_retry_num = 0;
    
    // 如果已经初始化过，不要重复 init
    esp_netif_init();
    esp_event_loop_create_default();
    
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap(); 

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL);

    // 检查是否有保存的配置
    wifi_config_t wifi_cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK) {
        if (strlen((char*)wifi_cfg.sta.ssid) > 0) {
            ESP_LOGI(TAG, "Found Config. SSID: %s", wifi_cfg.sta.ssid);
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            ESP_ERROR_CHECK(esp_wifi_start());
            return;
        }
    }

    ESP_LOGI(TAG, "No Config Found. Starting AP.");
    start_ap_mode();
}
