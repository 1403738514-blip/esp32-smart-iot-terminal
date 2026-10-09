#include "SPI.h"

void SPI2_Init(void)
{
    spi_bus_config_t buscfg = {
        .miso_io_num = LCD_MISO_PIN,
        .mosi_io_num = LCD_MOSI_PIN,
        .sclk_io_num = LCD_SCK_PIN,
        .quadhd_io_num = -1,
        .quadwp_io_num = -1,
        .max_transfer_sz = 4096,
    };

    ESP_ERROR_CHECK(
        spi_bus_initialize(
            SPI2_HOST,
            &buscfg,
            SPI_DMA_CH_AUTO
        )
    );
}