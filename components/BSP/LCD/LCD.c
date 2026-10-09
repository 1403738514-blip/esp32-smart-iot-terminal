#include "LCD.h"
#include "SPI.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_ili9341.h"

static const char *TAG = "LCD";

static esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_panel_io_handle_t io_handle = NULL;


/*
 * 使用我们刚才裸 SPI 已经验证成功的初始化命令
 */
static const ili9341_lcd_init_cmd_t lcd_init_cmds[] = {

    /* Software Reset */
    {0x01, (uint8_t[]){}, 0, 120},

    /* Sleep Out */
    {0x11, (uint8_t[]){}, 0, 120},

    /* Pixel Format = RGB565 */
    {0x3A, (uint8_t[]){0x55}, 1, 0},

    /* Memory Access Control */
    {0x36, (uint8_t[]){0x88}, 1, 0},

    /* Display ON */
    {0x29, (uint8_t[]){}, 0, 100},
};


void lcd_init(void)
{
    SPI2_Init();


    /* =========================
     * LCD SPI IO
     * ========================= */

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_CS_PIN,
        .dc_gpio_num = LCD_DC_PIN,

        .spi_mode = 0,

        .pclk_hz = 5 * 1000 * 1000,

        .trans_queue_depth = 10,

        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };


    ESP_ERROR_CHECK(
        esp_lcd_new_panel_io_spi(
            (esp_lcd_spi_bus_handle_t)SPI2_HOST,
            &io_config,
            &io_handle
        )
    );


    /* =========================
     * 自定义 ILI9341 初始化
     * ========================= */

    ili9341_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size =
            sizeof(lcd_init_cmds) /
            sizeof(lcd_init_cmds[0]),
    };


    /* =========================
     * LCD Panel
     * ========================= */

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST_PIN,
        .rgb_endian = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,

        .vendor_config = &vendor_config,
    };


    ESP_ERROR_CHECK(
        esp_lcd_new_panel_ili9341(
            io_handle,
            &panel_config,
            &panel_handle
        )
    );


    ESP_LOGI(TAG, "开始 LCD RESET");

    ESP_ERROR_CHECK(
        esp_lcd_panel_reset(panel_handle)
    );


    ESP_LOGI(TAG, "开始 LCD INIT");

    ESP_ERROR_CHECK(
        esp_lcd_panel_init(panel_handle)
    );


    ESP_LOGI(TAG, "开始 LCD ON");

    ESP_ERROR_CHECK(
        esp_lcd_panel_disp_on_off(
            panel_handle,
            true
        )
    );


    ESP_LOGI(TAG, "LCD 初始化完成");
}


void lcd_clear(uint16_t color)
{
    static uint8_t *buffer = NULL;

    if (buffer == NULL)
    {
        buffer = heap_caps_malloc(
            240 * 320 * 2,
            MALLOC_CAP_DMA
        );

        if (buffer == NULL)
        {
            ESP_LOGE(TAG, "DMA buffer 申请失败");
            return;
        }
    }

    // RGB565：ILI9341 要求高字节在前
    uint8_t color_high = (color >> 8) & 0xFF;
    uint8_t color_low  = color & 0xFF;

    for (int i = 0; i < 240 * 320 * 2; i += 2)
    {
        buffer[i]     = color_high;
        buffer[i + 1] = color_low;
    }

    ESP_ERROR_CHECK(
        esp_lcd_panel_draw_bitmap(
            panel_handle,
            0,
            0,
            240,
            320,
            buffer
        )
    );

    ESP_LOGI(
        TAG,
        "LCD 颜色发送完成: 0x%04X",
        color
    );
}

esp_lcd_panel_handle_t lcd_get_panel_handle(void)
{
    return panel_handle; // 把身份证交出去
}

esp_lcd_panel_io_handle_t lcd_get_io_handle(void)
{
    return io_handle;
}