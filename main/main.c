#include <stdio.h>
#include "LCD.h"      // 屏幕底层驱动
#include "freertos/FreeRTOS.h"     // 实时操作系统
#include "freertos/task.h"       // 任务调度
#include "freertos/queue.h"         // 任务间通信队列
#include "esp_lvgl_port.h"         // 乐鑫官方 LVGL 移植层
#include "lvgl.h"               // LVGL 图形库
#include "DHT11.h"              // 温湿度传感器
#include <string.h>         
#include "esp_wifi.h"             // WiFi 驱动
#include "esp_event.h"             // 事件循环（WiFi、MQTT底层都靠它）
#include "esp_netif.h"              // 网络接口层
#include "nvs_flash.h"              // 非易失存储（保存WiFi密码等）
#include "esp_sntp.h"  // NTP 时间同步专用
#include <time.h>
#include <sys/time.h>
#include "esp_http_client.h"            // HTTPS 请求天气 API
#include "cJSON.h"                  // JSON 解析
#include "esp_crt_bundle.h"            // HTTPS 证书包          
#include "mqtt_client.h"      
#include "driver/ledc.h"       

// 触摸屏头文件
#include "esp_lcd_touch.h"                    // 触摸屏驱动
#include "esp_lcd_touch_xpt2046.h"               // XPT2046 触摸芯片驱动

#include "esp_https_ota.h"
#include "esp_ota_ops.h"



#define ENCODER_A_PIN 34
#define ENCODER_B_PIN 35
static int encoder_pwm_val = 1023; // 当前亮度值 (0-1023)

LV_FONT_DECLARE(my_font_chinese_16);    // 声明 16px 中文字体
LV_IMG_DECLARE(icon_sunny);
LV_IMG_DECLARE(icon_cloudy);
LV_IMG_DECLARE(icon_overcast);
LV_IMG_DECLARE(icon_rainy);
LV_IMG_DECLARE(icon_thunderstorm);

char outdoor_weather[32] = "未知";
char outdoor_temp[16] = "0";

static lv_obj_t *mqtt_mbox = NULL; // 全局弹窗句柄   全局弹窗句柄，防野指针

// 定义接收室外天气数据的结构体
typedef struct {
    char weather[32];
    char temp[16];
} outdoor_data_t;



// 定义 MQTT 消息队列
QueueHandle_t mqtt_queue;

// 定义 MQTT 消息结构体手机发来的内容
typedef struct {
    char message[128];
} mqtt_msg_t;    

// 定义天气队列
QueueHandle_t outdoor_queue; 


// ==========================================
// 全局变量与函数声明
// ==========================================
QueueHandle_t temp_queue;                 // 温度队列

QueueHandle_t humi_queue;

static esp_lcd_touch_handle_t tp = NULL;          // 触摸屏句柄
esp_mqtt_client_handle_t mqtt_client = NULL;      //客户端句柄

void Task_UI(void *pvParameters);

void Task_Sensor(void *pvParameters);

// ==========================================
// 编码器任务：旋钮调光
// ==========================================
void Task_Encoder(void *pvParameters)
{
    int last_a = 1;
    printf("【编码器】任务启动...\n");

    while (1) {
        int current_a = gpio_get_level(ENCODER_A_PIN);
        
        if (current_a == 0 && last_a == 1) {
    
            int b = gpio_get_level(ENCODER_B_PIN);
            
            // 根据方向加减
            if (b == 1) { 
                encoder_pwm_val += 50; 
            } else {
                encoder_pwm_val -= 50; 
            }

            if (encoder_pwm_val > 1023) encoder_pwm_val = 1023;
            if (encoder_pwm_val < 0) encoder_pwm_val = 0;

       
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, encoder_pwm_val);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            
            // 只把当前值发给 UI，让 UI 自己去显示，不阻塞串口
        }
        last_a = current_a;
        vTaskDelay(pdMS_TO_TICKS(5)); // 5ms 轮询一次，极快响应
    }
}

void parse_weather_json(char *json_str) {
    // 1. 把字符串解析成 cJSON 树
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        printf("JSON 解析失败！\n");
        return;
    }

    // 2. 取出 results 数组
    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (results && cJSON_IsArray(results)) {
        cJSON *first_item = cJSON_GetArrayItem(results, 0); // 取第一个元素
        if (first_item) {
            // 3. 取出 "now" 对象
            cJSON *now = cJSON_GetObjectItem(first_item, "now");
            if (now) {
                // 4. 取出 "text" (天气现象) 和 "temperature" (温度)
                cJSON *text = cJSON_GetObjectItem(now, "text");
                cJSON *temp = cJSON_GetObjectItem(now, "temperature");

                if (text && temp) {
                    // 把数据拷贝到全局变量里
                    strncpy(outdoor_weather, text->valuestring, sizeof(outdoor_weather) - 1);
                    strncpy(outdoor_temp, temp->valuestring, sizeof(outdoor_temp) - 1);
                    printf("【解析成功】室外天气: %s, 室外温度: %s°C\n", outdoor_weather, outdoor_temp);
                }
            }
        }
    }
    // 释放内存，防止内存泄漏
    cJSON_Delete(root);
}


char response_buffer[1024] = {0}; 
int response_len = 0;             // 记录当前长度



static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        // 安全拷贝：确保不会撑爆缓冲区
        if (evt->data_len > 0 && response_len + evt->data_len < sizeof(response_buffer) - 1) {
            memcpy(response_buffer + response_len, evt->data, evt->data_len);
            response_len += evt->data_len;
            response_buffer[response_len] = '\0';
        }
    }
    return ESP_OK;
}

// ==========================================
// 按钮点击事件回调函数
// ==========================================
static void btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_PRESSED) {
        printf("【成功】按钮被按下！\n");
    }
}

// ==========================================
// 【新增】WiFi 事件回调函数
// ==========================================
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        printf("【WiFi】正在连接热点...\n");
        esp_wifi_connect(); 
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        printf("【WiFi】连接失败，正在重试...\n");
        esp_wifi_connect(); 
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        printf("【WiFi】连接成功！获取到 IP: " IPSTR "\n", IP2STR(&event->ip_info.ip));
    }
}

    // ==========================================
// 【新增】MQTT 事件回调函数
// ==========================================
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    
    if (event->event_id == MQTT_EVENT_CONNECTED) {
        printf("【MQTT】连接成功！\n");
        // 订阅主题 "esp32/notify"
        esp_mqtt_client_subscribe(event->client, "esp32/notify", 0);
        printf("【MQTT】已订阅主题: esp32/notify\n");
    } 
    else if (event->event_id == MQTT_EVENT_DATA) {
        printf("【MQTT】收到消息: %.*s\n", event->data_len, event->data);

        // 把收到的消息，打包扔进队列
        mqtt_msg_t msg_to_send;
        int copy_len = event->data_len < sizeof(msg_to_send.message) - 1 ? event->data_len : sizeof(msg_to_send.message) - 1;
        strncpy(msg_to_send.message, event->data, copy_len);
        msg_to_send.message[copy_len] = '\0';
        xQueueSend(mqtt_queue, &msg_to_send, 0); 
    } 
    else if (event->event_id == MQTT_EVENT_DISCONNECTED) {
        printf("【MQTT】断开连接，正在重连...\n");
    }
}

// ==========================================
// 主函数
// ==========================================
void app_main(void)
{
    printf("开始初始化底层 LCD...\n");
    lcd_init();

        // ==========================================
    // 初始化 PWM 背光 (BLK 接在 GPIO 27)
    // ==========================================
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT, // 0-1023
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000, 
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .gpio_num = 27, // 背光引脚
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 1023, // 开机先给最大亮度
        .hpoint = 0,
    };
    ledc_channel_config(&ledc_channel);

    temp_queue = xQueueCreate(5, sizeof(float)); 
    humi_queue = xQueueCreate(5, sizeof(float));


    outdoor_queue = xQueueCreate(5, sizeof(outdoor_data_t));

        
    mqtt_queue = xQueueCreate(5, sizeof(mqtt_msg_t)); // 创建 MQTT 消息队列



    // 初始化触摸屏（XPT2046）
    printf("开始初始化触摸屏...\n");
    esp_lcd_panel_io_spi_config_t tp_io_config = {
        .cs_gpio_num = 21,    
        .dc_gpio_num = -1,        
        .spi_mode = 0,            
        .pclk_hz = 1 * 1000 * 1000,           
        .trans_queue_depth = 3,               
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &tp_io_config, &tp_io_handle));

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = 600,
        .y_max = 600,
        .rst_gpio_num = -1,   
        .int_gpio_num = -1,   
        .levels = { .reset = 0, .interrupt = 0 },         
        .flags = {
            .swap_xy = 0,         
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tp_io_handle, &tp_cfg, &tp));
    printf("触摸屏初始化成功！\n");

    // 初始化 LVGL 并挂载屏幕和触摸
    printf("开始初始化 LVGL 核心...\n");
    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = lcd_get_panel_handle(),  
        .io_handle = lcd_get_io_handle(),    
        .buffer_size = 240 * 40,            
        .double_buffer = false,         
        .hres = 240,
        .vres = 320,
        .monochrome = false,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = true }, 
        .flags = { .buff_dma = true, .swap_bytes = true }     
    };
    lvgl_port_add_disp(&disp_cfg);

    lvgl_port_touch_cfg_t touch_cfg = {
        .disp = lv_disp_get_default(),   
        .handle = tp,         
    };
    lvgl_port_add_touch(&touch_cfg);



    // ==========================================
    //初始化 WiFi 并连接你的热点
    // ==========================================
    printf("开始初始化 WiFi...\n");
    
    // 初始化 NVS（WiFi 和 NTP 都需要用它存配置）
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());    //初始化网络底层协议栈
    ESP_ERROR_CHECK(esp_event_loop_create_default());   //创建一个“默认事件循环” 
    esp_netif_create_default_wifi_sta();       //创建默认的 WiFi STA（客户端）网卡

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 注册事件回调函数
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // 你的热点账号密码
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "12345678",     // 你的热点名字
            .password = "12345678", // 你的热点密码
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));  // 1. 身份设定：我是客户端（STA），我要连别人
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));  // 2. 掏出账号密码，塞进 WiFi 配置里
    ESP_ERROR_CHECK(esp_wifi_start());         // 3. 按下拨号键，开始连！

    // 等待 5 秒，让它连接并拿到 IP
    vTaskDelay(pdMS_TO_TICKS(5000)); 

    // ==========================================
    // 4. 配置 NTP 获取网络时间
    // ==========================================
    printf("开始同步网络时间...\n");
    // 设置东八区北京时间，不然时间会差 8 小时
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);   //设置运行模式为主动去询问
    esp_sntp_setservername(0, "ntp.aliyun.com"); // 使用阿里云的 NTP 服务器，国内更稳
    esp_sntp_init();

    // 等待时间同步成功（通常几秒钟）
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    while (timeinfo.tm_year < (2020 - 1900) && retry < 10) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        time(&now);
        localtime_r(&now, &timeinfo);
        retry++;
    }
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
    printf("【时间同步结果】: %s\n", time_str);

        // ==========================================
    // 【新增】初始化 MQTT 并连接
    // ==========================================
    printf("开始连接 MQTT 服务器...\n");
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://broker.emqx.io:1883", // 免费公共服务器
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg); // 只是赋值，不再声明类型
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);


     printf("开始测试天气 API 请求...\n");
    
    // 换上你的私钥
    char *url = "https://it=c";

    esp_http_client_config_t config = {
        .url = url,         // 地址
        .method = HTTP_METHOD_GET,             // 用 GET 方式去拿
        .timeout_ms = 10000,                  // 超过 10 秒拿不到就放弃
        .crt_bundle_attach = esp_crt_bundle_attach, // 挂载官方根证书包
        .skip_cert_common_name_check = true,        // 跳过域名验证，防止因为 IP 或域名格式报错
        .event_handler = http_event_handler,    // 挂载抓包器
    };
    
    response_len = 0;        // 长度清零 
    memset(response_buffer, 0, sizeof(response_buffer));     // 把之前可能残余的垃圾数据擦干净
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

        if (err == ESP_OK) {
        printf("HTTP 请求成功！状态码: %d\n", esp_http_client_get_status_code(client));
        printf("【服务器返回的JSON】:\n%s\n", response_buffer); // 直接打印抓到的 JSON
        parse_weather_json(response_buffer);
         outdoor_data_t data_to_send;
    strncpy(data_to_send.weather, outdoor_weather, sizeof(data_to_send.weather) - 1);
    strncpy(data_to_send.temp, outdoor_temp, sizeof(data_to_send.temp) - 1);
    xQueueSend(outdoor_queue, &data_to_send, 0);
    } else {
        printf("HTTP 请求失败: %s\n", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    printf("天气 API 测试结束。\n");

    // ==========================================
    // 初始化 DHT11 和队列，创建所有任务
    // ==========================================
    dht11_init();

        // 初始化编码器 GPIO
    gpio_set_direction(ENCODER_A_PIN, GPIO_MODE_INPUT);
    gpio_set_direction(ENCODER_B_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(ENCODER_A_PIN, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(ENCODER_B_PIN, GPIO_PULLUP_ONLY);
    
    // 创建编码器任务
    xTaskCreate(Task_Encoder, "Task_Encoder", 4096, NULL, 3, NULL);
    

    xTaskCreate(Task_Sensor, "Task_Sensor", 4096, NULL, 4, NULL);
     
    xTaskCreate(Task_UI, "Task_UI", 16384, NULL, 5, NULL);
    

    printf("全部初始化完成，系统开始运行！\n");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ==========================================
// 传感器任务：专门读取真实的 DHT11
// ==========================================
void Task_Sensor(void *pvParameters)
{
    float temp = 0, humi = 0;
    int error_cnt = 0;      
    
    while (1) {
        int retry = 0;
        bool success = false; 
        
        while (retry < 5) {
            gpio_set_level(DHT11_GPIO_PIN, 1);
            esp_rom_delay_us(100);
            
            if (dht11_read(&temp, &humi) == ESP_OK) {
                success = true;
                break;
            }
            retry++;
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        if (success) {
            error_cnt = 0; 
            printf("【传感器】真实温度: %.1f°C, 湿度: %.1f%%\n", temp, humi);
            xQueueSend(temp_queue, &temp, 0); 
            xQueueSend(humi_queue, &humi, 0);


            char mqtt_payload[64];
            snprintf(mqtt_payload, sizeof(mqtt_payload), "温度: %.1f°C, 湿度: %.1f%%", temp, humi);
            esp_mqtt_client_publish(mqtt_client, "esp32/dht11", mqtt_payload, 0, 1, 0);
        } else {
            error_cnt++;
            if (error_cnt >= 5) {
                printf("【传感器】连续读取失败，检查硬件...\n");
                error_cnt = 0; 
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000)); 
    }
}

// ==========================================
// UI 任务：专门刷新屏幕
// ==========================================
void Task_UI(void *pvParameters)
{
    printf("准备创建 LVGL 界面...\n");
    lvgl_port_lock(0);

        lv_obj_t *scr = lv_screen_active();             
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xE0F2FE), 0); // 浅蓝灰背景，看着清爽
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);            

    // ==========================================
    // 顶部天气卡片 (渐变背景 + 太阳 + 大温度)
    // ==========================================
    lv_obj_t *card_top = lv_obj_create(scr);
    lv_obj_set_size(card_top, 220, 130);
    lv_obj_align(card_top, LV_ALIGN_TOP_MID, 0, 10); // 靠上居中
    lv_obj_set_style_radius(card_top, 20, 0);        // 大圆角
    lv_obj_set_style_border_width(card_top, 0, 0);   // 无边框
    lv_obj_set_style_shadow_width(card_top, 15, 0);  // 阴影
    lv_obj_set_style_shadow_color(card_top, lv_color_hex(0x888888), 0);
    lv_obj_set_style_shadow_opa(card_top, LV_OPA_30, 0);
    
    // 设置卡片背景渐变色
    lv_obj_set_style_bg_color(card_top, lv_color_hex(0x3B82F6), 0); //设置卡片的基础背景色
    lv_obj_set_style_bg_grad_color(card_top, lv_color_hex(0x2DD4BF), 0);  //设置卡片的渐变色终点。
    lv_obj_set_style_bg_grad_dir(card_top, LV_GRAD_DIR_HOR, 0); // 水平渐变  //渐变要按什么方向走。

    // 创建天气图标
    lv_obj_t *weather_icon = lv_img_create(card_top);
    lv_img_set_src(weather_icon, &icon_sunny); // 默认显示晴天
    lv_obj_align(weather_icon, LV_ALIGN_TOP_LEFT, 15, 15); // 靠左上角

    // 大温度文字
    lv_obj_t *temp_label = lv_label_create(card_top);
    lv_label_set_text(temp_label, "27°C"); 
    lv_obj_set_style_text_font(temp_label, &lv_font_montserrat_32, 0); // 大字号
    lv_obj_set_style_text_color(temp_label, lv_color_white(), 0); 
    lv_obj_align(temp_label, LV_ALIGN_RIGHT_MID, -15, 0); // 靠右侧居中

    // 天气文字和日期
    lv_obj_t *weather_label = lv_label_create(card_top);
    lv_label_set_text(weather_label, "多云转晴");
    lv_obj_set_style_text_font(weather_label, &my_font_chinese_16, 0); 
    lv_obj_set_style_text_color(weather_label, lv_color_white(), 0); 
    lv_obj_align(weather_label, LV_ALIGN_LEFT_MID, 15, 15); // 在太阳下方

        // 时间标签（左上角，在太阳旁边）
    lv_obj_t *time_label = lv_label_create(card_top);
    lv_label_set_text(time_label, "12:00"); 
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_24, 0); 
    lv_obj_set_style_text_color(time_label, lv_color_white(), 0); 
    lv_obj_align(time_label, LV_ALIGN_TOP_LEFT, 65, 15); // 太阳右边

    // ==========================================
    // 【卡片2】底部信息卡片 (白底 + 灰字 + 整齐排版)
    // ==========================================
    lv_obj_t *card_bottom = lv_obj_create(scr);
    lv_obj_set_size(card_bottom, 220, 140);
    lv_obj_align(card_bottom, LV_ALIGN_BOTTOM_MID, 0, -10); 
    lv_obj_set_style_bg_color(card_bottom, lv_color_white(), 0); // 纯白卡片
    lv_obj_set_style_radius(card_bottom, 20, 0);
    lv_obj_set_style_border_width(card_bottom, 0, 0);
    lv_obj_set_style_shadow_width(card_bottom, 15, 0);
    lv_obj_set_style_shadow_color(card_bottom, lv_color_hex(0x888888), 0);
    lv_obj_set_style_shadow_opa(card_bottom, LV_OPA_30, 0);
    
    // 使用 Flex 布局让内容整齐排列
    lv_obj_set_flex_flow(card_bottom, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card_bottom, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // 去掉卡片内部的滚动条
    lv_obj_remove_flag(card_bottom, LV_OBJ_FLAG_SCROLLABLE);

    // 室内温度文字
    lv_obj_t *in_temp_label = lv_label_create(card_bottom);
    lv_label_set_text(in_temp_label, "室内温度: 25.0°C"); 
    lv_obj_set_style_text_font(in_temp_label, &my_font_chinese_16, 0); 
    lv_obj_set_style_text_color(in_temp_label, lv_color_hex(0x334155), 0); 

    // 室内湿度文字（用之前说的 humi_queue 接收更新）
    lv_obj_t *humi_label = lv_label_create(card_bottom);
    lv_label_set_text(humi_label, "室内湿度: 50.0%"); 
    lv_obj_set_style_text_font(humi_label, &my_font_chinese_16, 0); 
    lv_obj_set_style_text_color(humi_label, lv_color_hex(0x334155), 0); 

    // “点我”按钮
    lv_obj_t *btn = lv_button_create(card_bottom);
    lv_obj_set_size(btn, 100, 35);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3B82F6), 0); // 蓝色按钮
    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "点我");
    lv_obj_set_style_text_font(btn_label, &my_font_chinese_16, 0);
    lv_obj_center(btn_label);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_PRESSED, NULL);


    // 显示当前亮度的标签（替代滑动条）
    lv_obj_t *encoder_label = lv_label_create(card_bottom);
    lv_label_set_text(encoder_label, "亮度: 100%"); 
    lv_obj_set_style_text_font(encoder_label, &my_font_chinese_16, 0); 
    lv_obj_set_style_text_color(encoder_label, lv_color_hex(0x334155), 0); 


    lvgl_port_unlock(); 

    // ==========================================
    // 动态刷新与主循环
    // ==========================================
        float received_temp = 0;
    int temp_int_x10 = 0;
    char buf[64];
    float received_humi = 0;     
    char humi_buf[64];           
    static uint32_t last_time_tick = 0;

        static uint32_t mbox_timer = 0; // 用于MQTT弹窗倒计时

            static char current_mbox_msg[128] = ""; // 保存当前弹窗的消息内容，防止倒计时刷新时丢失
    static int last_remaining_sec = -1;     // 上次显示的剩余秒数，用于判断是否需要刷新
        static lv_obj_t *mbox_text_label = NULL; // 保存弹窗里文字标签的句柄
            static bool ota_triggered = false; // 防止OTA重复触发

    while (1) {
        // 1. 刷新室内温度 -> 更新 in_temp_label
        if (xQueueReceive(temp_queue, &received_temp, pdMS_TO_TICKS(0)) == pdTRUE) {
            temp_int_x10 = (int)(received_temp * 10); // 去掉前面的 int
            snprintf(buf, sizeof(buf), "室内温度: %d.%d°C", temp_int_x10 / 10, temp_int_x10 % 10); // 去掉前面的 char
            lvgl_port_lock(portMAX_DELAY); 
            lv_label_set_text(in_temp_label, buf); 
            lvgl_port_unlock(); 
        }

        // 2. 刷新室内湿度 -> 更新 humi_label
        if (xQueueReceive(humi_queue, &received_humi, pdMS_TO_TICKS(0)) == pdTRUE) {
            int humi_int_x10 = (int)(received_humi * 10); 
            snprintf(humi_buf, sizeof(humi_buf), "室内湿度: %d.%d%%", humi_int_x10 / 10, humi_int_x10 % 10);
            lvgl_port_lock(portMAX_DELAY); 
            lv_label_set_text(humi_label, humi_buf);
            lvgl_port_unlock(); 
        }

        // 3. 每秒刷新一次时间 -> 更新 time_label
        if (xTaskGetTickCount() - last_time_tick > pdMS_TO_TICKS(1000)) { // 去掉循环里的 static 定义
            last_time_tick = xTaskGetTickCount();
            time_t now;
            struct tm timeinfo;
            time(&now);
            localtime_r(&now, &timeinfo);
            char time_buf[16];
            snprintf(time_buf, sizeof(time_buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
            lvgl_port_lock(portMAX_DELAY); 
            lv_label_set_text(time_label, time_buf); 
            lvgl_port_unlock(); 
        }

        // 4. 刷新室外天气 -> 更新 temp_label 和 weather_label，并切换图片
        outdoor_data_t recv_data;
        if (xQueueReceive(outdoor_queue, &recv_data, pdMS_TO_TICKS(0)) == pdTRUE) {
            char temp_buf[32];
            snprintf(temp_buf, sizeof(temp_buf), "%s°C", recv_data.temp);
            
            lvgl_port_lock(portMAX_DELAY); 
            lv_label_set_text(temp_label, temp_buf); 
            lv_label_set_text(weather_label, recv_data.weather); 

            // =========================================================
            // 根据天气字符串，动态切换图片
            // =========================================================
            if (strstr(recv_data.weather, "雷") != NULL) {
                // 优先判断雷阵雨（因为它同时包含雨字）
                lv_img_set_src(weather_icon, &icon_thunderstorm);
            } 
            else if (strstr(recv_data.weather, "雨") != NULL || 
                     strstr(recv_data.weather, "雪") != NULL) {
                lv_img_set_src(weather_icon, &icon_rainy);
            } 
            else if (strstr(recv_data.weather, "阴") != NULL) {
                lv_img_set_src(weather_icon, &icon_overcast);
            } 
            else if (strstr(recv_data.weather, "云") != NULL) {
                lv_img_set_src(weather_icon, &icon_cloudy);
            } 
            else {
                // 默认晴天
                lv_img_set_src(weather_icon, &icon_sunny);
            }
            // =========================================================

            lvgl_port_unlock(); 
        }

        // 5. 刷新 MQTT 弹窗 (倒计时自动关闭)
        mqtt_msg_t recv_msg;
        if (xQueueReceive(mqtt_queue, &recv_msg, 0) == pdTRUE) {
            lvgl_port_lock(portMAX_DELAY);      
            if (mqtt_mbox != NULL) {
                lv_msgbox_close(mqtt_mbox);
                mqtt_mbox = NULL;
            }

            mqtt_mbox = lv_msgbox_create(NULL);
            ota_triggered = false; // 新弹窗出现时，重置标志
            lv_msgbox_add_title(mqtt_mbox, "远程通知");
            lv_obj_set_style_text_font(mqtt_mbox, &my_font_chinese_16, 0);

            // 把消息存到静态变量里，供后面倒计时刷新使用
            snprintf(current_mbox_msg, sizeof(current_mbox_msg), "%s", recv_msg.message);

            char mbox_text[256];
            snprintf(mbox_text, sizeof(mbox_text), "%s\n\n(20秒后自动关闭)", current_mbox_msg);

            mbox_text_label = lv_msgbox_add_text(mqtt_mbox, mbox_text);

            lv_obj_set_scrollable(mqtt_mbox, false); 
            lv_obj_center(mqtt_mbox); 

            mbox_timer = xTaskGetTickCount(); 
            last_remaining_sec = -1; // 重置倒计时记录
            lvgl_port_unlock(); 
        }

                // 6. 刷新编码器亮度显示
        static int last_pwm_val = -1; // 静态变量，记录上次的值，避免重复刷新
        if (encoder_pwm_val != last_pwm_val) {
            last_pwm_val = encoder_pwm_val;
            int percent = (encoder_pwm_val * 100) / 1023;
            char pwm_buf[32];
            snprintf(pwm_buf, sizeof(pwm_buf), "亮度: %d%%", percent);
            lvgl_port_lock(portMAX_DELAY); 
            lv_label_set_text(encoder_label, pwm_buf); 
            lvgl_port_unlock(); 
        }

                // 弹窗倒计时检测，20秒后自动关闭
        //弹窗倒计时动态刷新，每秒跳动一次
        if (mqtt_mbox != NULL) {
            int elapsed_ms = xTaskGetTickCount() - mbox_timer;
            int remaining_sec = 20 - (elapsed_ms / 1000); // 计算剩余秒数

            if (remaining_sec <= 0 && !ota_triggered) {

                ota_triggered = true;
                // 时间到，先把屏幕字改了，给个提示
                lvgl_port_lock(portMAX_DELAY);
                lv_label_set_text(mbox_text_label, "正在升级，请勿断电...");
                lvgl_port_unlock();

                printf("【OTA】收到MQTT更新指令，开始下载新固件...\n");

                // 【注意】这里的IP必须是你电脑的IP：192.168.186.178
                esp_http_client_config_t ota_http_config = {
                    .url = "http://192.168.186.178:8070/update.bin", 
                    .timeout_ms = 15000,
                    .buffer_size = 1024, // 保持在1KB，保护内存
                    .crt_bundle_attach = esp_crt_bundle_attach, // 防止底层报错
                    .skip_cert_common_name_check = true,        // 防止底层报错
                };
                esp_https_ota_config_t ota_config = {
                    .http_config = &ota_http_config,
                };
                
                printf("【OTA】开始连接电脑下载...\n");
                esp_err_t ota_ret = esp_https_ota(&ota_config);
                if (ota_ret == ESP_OK) {
                    printf("【OTA】下载成功！3秒后重启设备...\n");
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    esp_restart(); // 重启进入新固件
                } else {
                    printf("【OTA】下载失败，错误码: %s\n", esp_err_to_name(ota_ret));
                    lvgl_port_lock(portMAX_DELAY);
                    lv_label_set_text(mbox_text_label, "升级失败，请重试");
                    lvgl_port_unlock();
                }
                last_remaining_sec = -1;
            }
            else if (remaining_sec != last_remaining_sec) {
                // 秒数发生变化，刷新屏幕上的文字（变成 19, 18, 17...）
                last_remaining_sec = remaining_sec;
                char mbox_text[256];
                snprintf(mbox_text, sizeof(mbox_text), "%s\n\n(%d秒后自动关闭)", current_mbox_msg, remaining_sec);
                
            lvgl_port_lock(portMAX_DELAY);
            // 直接更新之前保存的标签，不用 lv_msgbox_get_text 了
            if (mbox_text_label != NULL) {
                lv_label_set_text(mbox_text_label, mbox_text);
            }
            lvgl_port_unlock();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}