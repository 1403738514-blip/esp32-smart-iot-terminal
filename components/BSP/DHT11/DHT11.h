#ifndef __DHT11_H__
#define __DHT11_H__

#include "driver/gpio.h"
#include "esp_err.h"

#define DHT11_GPIO_PIN GPIO_NUM_5 // 数据引脚接在 GPIO 5

void dht11_init(void);
esp_err_t dht11_read(float *temperature, float *humidity);

#endif