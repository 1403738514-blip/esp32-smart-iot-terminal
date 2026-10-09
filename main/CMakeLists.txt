idf_component_register(SRCS "main.c"
                            "my_font_chinese_16.c"
                            "icon_sunny.c"
                            "icon_cloudy.c"
                            "icon_overcast.c"
                            "icon_rainy.c"
                            "icon_thunderstorm.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_lcd esp_lcd_ili9341 esp_lcd_touch esp_wifi nvs_flash esp_netif BSP esp_http_client json mbedtls mqtt esp_https_ota app_update)