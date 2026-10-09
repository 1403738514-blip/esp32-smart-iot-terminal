# 自动扫描 LCD 和 SPI 文件夹里的 .c 文件
file(GLOB_RECURSE src_dirs
    LCD/*.c
    SPI/*.c
    DHT11/*.c
)

set(include_dirs
    LCD
    SPI
    DHT11)

# 这里加上了 esp_lcd 和 esp_lcd_ili9341
set(requires
    driver
    esp_timer
    esp_lcd
    esp_lcd_ili9341)

idf_component_register(SRCS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)