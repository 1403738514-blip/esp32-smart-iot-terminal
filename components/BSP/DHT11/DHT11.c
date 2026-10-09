#include "DHT11.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 初始化 DHT11 数据引脚
void dht11_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DHT11_GPIO_PIN),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD, // 开漏输入输出模式
        .pull_up_en = GPIO_PULLUP_ENABLE,  // 打开内部上拉电阻
    };
    gpio_config(&io_conf);
    gpio_set_level(DHT11_GPIO_PIN, 1);
}

// 读取温湿度
esp_err_t dht11_read(float *temperature, float *humidity) {
    uint8_t data[5] = {0};
    uint8_t i, j;

    // 1. 主机发送开始信号（拉低至少 18ms，再拉高 20-40us）
    gpio_set_direction(DHT11_GPIO_PIN, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(DHT11_GPIO_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(DHT11_GPIO_PIN, 1);
    esp_rom_delay_us(30);

    // 2. 切换为输入模式，准备接收响应
    gpio_set_direction(DHT11_GPIO_PIN, GPIO_MODE_INPUT);
    
    // 【极其重要】关中断！DHT11 对时序要求极高，微秒级误差就会失败，绝不能被系统打断
    portDISABLE_INTERRUPTS();

        int64_t timeout;
        // 等待 DHT11 响应（拉低 80us 再拉高 80us）
        timeout = esp_timer_get_time();
        while (gpio_get_level(DHT11_GPIO_PIN) == 1) { if (esp_timer_get_time() - timeout > 100) { portENABLE_INTERRUPTS(); return ESP_FAIL; } }
        timeout = esp_timer_get_time();
        while (gpio_get_level(DHT11_GPIO_PIN) == 0) { if (esp_timer_get_time() - timeout > 100) { portENABLE_INTERRUPTS(); return ESP_FAIL; } }
        timeout = esp_timer_get_time();
        while (gpio_get_level(DHT11_GPIO_PIN) == 1) { if (esp_timer_get_time() - timeout > 100) { portENABLE_INTERRUPTS(); return ESP_FAIL; } }

    // 3. 读取 40 位数据（5 个字节）
    for (i = 0; i < 5; i++) {
        for (j = 0; j < 8; j++) {
            // 等待 50us 低电平结束
            timeout = esp_timer_get_time();
            while (gpio_get_level(DHT11_GPIO_PIN) == 0) { 
                if (esp_timer_get_time() - timeout > 100) { portENABLE_INTERRUPTS(); return ESP_FAIL; } 
            }
            
            // 延时 40us 后再读电平：如果是 0，此时高电平已结束；如果是 1，此时还是高电平
            esp_rom_delay_us(40); 
            data[i] <<= 1;
            if (gpio_get_level(DHT11_GPIO_PIN) == 1) { 
                data[i] |= 1; 
            }
            
            // 等待高电平结束
            timeout = esp_timer_get_time();
            while (gpio_get_level(DHT11_GPIO_PIN) == 1) { 
                if (esp_timer_get_time() - timeout > 100) { portENABLE_INTERRUPTS(); return ESP_FAIL; } 
            }
        }
    }
    portENABLE_INTERRUPTS(); // 恢复中断

    // 4. 校验数据（前 4 个字节之和 == 第 5 个字节）
    if (data[0] + data[1] + data[2] + data[3] != data[4]) {
        return ESP_ERR_INVALID_CRC;
    }

    // 5. 解析温湿度（DHT11 小数部分通常为 0，这里保留常规计算公式）
    *humidity = data[0] + data[1] * 0.1f;
    *temperature = data[2] + data[3] * 0.1f;
    return ESP_OK;
}