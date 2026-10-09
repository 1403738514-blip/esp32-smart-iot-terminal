#ifndef __SPI_H__
#define __SPI_H__

#include <string.h>
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

/* 定义引脚 */
#define LCD_CS_PIN    GPIO_NUM_15
#define LCD_DC_PIN    GPIO_NUM_2
#define LCD_RST_PIN   GPIO_NUM_4
#define LCD_MOSI_PIN  GPIO_NUM_23
#define LCD_SCK_PIN   GPIO_NUM_18
#define LCD_MISO_PIN  GPIO_NUM_19

void SPI2_Init(void);

#endif