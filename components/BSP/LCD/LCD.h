#ifndef __LCD_H__
#define __LCD_H__

#include <stdint.h>
#include "esp_lcd_panel_ops.h"

void lcd_init(void);
void lcd_clear(uint16_t color);
esp_lcd_panel_handle_t lcd_get_panel_handle(void); // 屏幕句柄
esp_lcd_panel_io_handle_t lcd_get_io_handle(void); // 通信句柄 (新增)

#endif