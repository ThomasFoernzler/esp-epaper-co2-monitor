#ifndef EPAPER_DRIVER_BSP_H
#define EPAPER_DRIVER_BSP_H

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <stdint.h>

enum ColorImage : uint8_t {
    DRIVER_COLOR_WHITE = 0xff,
    DRIVER_COLOR_BLACK = 0x00,
    FONT_BACKGROUND = DRIVER_COLOR_WHITE,
};

struct custom_lcd_spi_t {
    uint8_t cs;
    uint8_t dc;
    uint8_t rst;
    uint8_t busy;
    uint8_t mosi;
    uint8_t scl;
    int spi_host;
    int buffer_len;
};

class epaper_driver_display {
public:
    epaper_driver_display(int width, int height, custom_lcd_spi_t lcd_spi_data);
    ~epaper_driver_display();

    void EPD_Init();
    void EPD_Clear();
    void EPD_Display();
    void EPD_DisplayPartBaseImage();
    void EPD_Init_Partial();
    void EPD_DisplayPart();
    void EPD_DrawColorPixel(uint16_t x, uint16_t y, uint8_t color);

private:
    custom_lcd_spi_t lcd_spi_data_;
    int width_;
    int height_;
    spi_device_handle_t spi_;
    uint8_t* buffer_;

    void spi_gpio_init();
    void spi_port_init();
    void read_busy();
    void SPI_SendByte(uint8_t data);
    void EPD_SendData(uint8_t data);
    void EPD_SendCommand(uint8_t command);
    void writeBytes(const uint8_t* buffer, int len);
    void EPD_SetWindows(uint16_t x_start, uint16_t y_start, uint16_t x_end,
                        uint16_t y_end);
    void EPD_SetCursor(uint16_t x_start, uint16_t y_start);
    void EPD_SetLut(const uint8_t* lut);
    void EPD_TurnOnDisplay();
    void EPD_TurnOnDisplayPart();

    void set_cs_1() { gpio_set_level((gpio_num_t)lcd_spi_data_.cs, 1); }
    void set_cs_0() { gpio_set_level((gpio_num_t)lcd_spi_data_.cs, 0); }
    void set_dc_1() { gpio_set_level((gpio_num_t)lcd_spi_data_.dc, 1); }
    void set_dc_0() { gpio_set_level((gpio_num_t)lcd_spi_data_.dc, 0); }
    void set_rst_1() { gpio_set_level((gpio_num_t)lcd_spi_data_.rst, 1); }
    void set_rst_0() { gpio_set_level((gpio_num_t)lcd_spi_data_.rst, 0); }
};

#endif

