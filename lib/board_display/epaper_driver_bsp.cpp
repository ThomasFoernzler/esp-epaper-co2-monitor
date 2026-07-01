#include "epaper_driver_bsp.h"

#include <Arduino.h>
#include <esp_log.h>
#include <string.h>

static const char* TAG = "epaper";

static const uint8_t WF_Full_1IN54[159] =
{
    0x80,	0x48,	0x40,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x40,	0x48,	0x80,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x80,	0x48,	0x40,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x40,	0x48,	0x80,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0xA,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x8,	0x1,	0x0,	0x8,	0x1,	0x0,	0x2,
    0xA,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x22,	0x22,	0x22,	0x22,	0x22,	0x22,	0x0,	0x0,	0x0,
    0x22,	0x17,	0x41,	0x0,	0x32,	0x20
};

static uint8_t WF_PARTIAL_1IN54_0[159] =
{
    0x0,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x80,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x40,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0xF,0x0,0x0,0x0,0x0,0x0,0x0,
    0x1,0x1,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x22,0x22,0x22,0x22,0x22,0x22,0x0,0x0,0x0,
    0x02,0x17,0x41,0xB0,0x32,0x28,
};

epaper_driver_display::epaper_driver_display(int width, int height,
                                             custom_lcd_spi_t lcd_spi_data)
    : lcd_spi_data_(lcd_spi_data),
      width_(width),
      height_(height),
      spi_(nullptr),
      buffer_(nullptr) {
    spi_port_init();
    spi_gpio_init();
    buffer_ = static_cast<uint8_t*>(malloc(lcd_spi_data_.buffer_len));
    assert(buffer_);
    EPD_Clear();
}

epaper_driver_display::~epaper_driver_display() {
    if (buffer_ != nullptr) {
        free(buffer_);
    }
}

void epaper_driver_display::spi_gpio_init() {
    gpio_config_t config = {};
    config.intr_type = GPIO_INTR_DISABLE;
    config.mode = GPIO_MODE_OUTPUT;
    config.pin_bit_mask =
        (1ULL << lcd_spi_data_.rst) | (1ULL << lcd_spi_data_.dc) |
        (1ULL << lcd_spi_data_.cs);
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&config));

    config.mode = GPIO_MODE_INPUT;
    config.pin_bit_mask = (1ULL << lcd_spi_data_.busy);
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&config));

    set_rst_1();
}

void epaper_driver_display::spi_port_init() {
    spi_bus_config_t buscfg = {};
    buscfg.miso_io_num = -1;
    buscfg.mosi_io_num = lcd_spi_data_.mosi;
    buscfg.sclk_io_num = lcd_spi_data_.scl;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = width_ * height_;

    spi_device_interface_config_t devcfg = {};
    devcfg.spics_io_num = -1;
    devcfg.clock_speed_hz = 40 * 1000 * 1000;
    devcfg.mode = 0;
    devcfg.queue_size = 7;

    ESP_ERROR_CHECK(spi_bus_initialize(
        static_cast<spi_host_device_t>(lcd_spi_data_.spi_host), &buscfg,
        SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(
        static_cast<spi_host_device_t>(lcd_spi_data_.spi_host), &devcfg, &spi_));
}

void epaper_driver_display::read_busy() {
    while (gpio_get_level((gpio_num_t)lcd_spi_data_.busy) == 1) {
        delay(5);
    }
}

void epaper_driver_display::SPI_SendByte(uint8_t data) {
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_buffer = &data;
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi_, &t));
}

void epaper_driver_display::EPD_SendData(uint8_t data) {
    set_dc_1();
    set_cs_0();
    SPI_SendByte(data);
    set_cs_1();
}

void epaper_driver_display::EPD_SendCommand(uint8_t command) {
    set_dc_0();
    set_cs_0();
    SPI_SendByte(command);
    set_cs_1();
}

void epaper_driver_display::writeBytes(const uint8_t* buffer, int len) {
    set_dc_1();
    set_cs_0();
    spi_transaction_t t = {};
    t.length = 8 * len;
    t.tx_buffer = buffer;
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi_, &t));
    set_cs_1();
}

void epaper_driver_display::EPD_SetWindows(uint16_t x_start, uint16_t y_start,
                                           uint16_t x_end, uint16_t y_end) {
    EPD_SendCommand(0x44);
    EPD_SendData((x_start >> 3) & 0xFF);
    EPD_SendData((x_end >> 3) & 0xFF);

    EPD_SendCommand(0x45);
    EPD_SendData(y_start & 0xFF);
    EPD_SendData((y_start >> 8) & 0xFF);
    EPD_SendData(y_end & 0xFF);
    EPD_SendData((y_end >> 8) & 0xFF);
}

void epaper_driver_display::EPD_SetCursor(uint16_t x_start, uint16_t y_start) {
    EPD_SendCommand(0x4E);
    EPD_SendData(x_start & 0xFF);

    EPD_SendCommand(0x4F);
    EPD_SendData(y_start & 0xFF);
    EPD_SendData((y_start >> 8) & 0xFF);
}

void epaper_driver_display::EPD_SetLut(const uint8_t* lut) {
    EPD_SendCommand(0x32);
    writeBytes(lut, 153);
    read_busy();

    EPD_SendCommand(0x3f);
    EPD_SendData(lut[153]);
    EPD_SendCommand(0x03);
    EPD_SendData(lut[154]);
    EPD_SendCommand(0x04);
    EPD_SendData(lut[155]);
    EPD_SendData(lut[156]);
    EPD_SendData(lut[157]);
    EPD_SendCommand(0x2c);
    EPD_SendData(lut[158]);
}

void epaper_driver_display::EPD_TurnOnDisplay() {
    EPD_SendCommand(0x22);
    EPD_SendData(0xc7);
    EPD_SendCommand(0x20);
    read_busy();
}

void epaper_driver_display::EPD_TurnOnDisplayPart() {
    EPD_SendCommand(0x22);
    EPD_SendData(0xcf);
    EPD_SendCommand(0x20);
    read_busy();
}

void epaper_driver_display::EPD_Init() {
    set_rst_1();
    delay(50);
    set_rst_0();
    delay(20);
    set_rst_1();
    delay(50);

    read_busy();
    EPD_SendCommand(0x12);
    read_busy();

    EPD_SendCommand(0x01);
    EPD_SendData(0xC7);
    EPD_SendData(0x00);
    EPD_SendData(0x01);

    EPD_SendCommand(0x11);
    EPD_SendData(0x01);

    EPD_SetWindows(0, width_ - 1, height_ - 1, 0);

    EPD_SendCommand(0x3C);
    EPD_SendData(0x01);

    EPD_SendCommand(0x18);
    EPD_SendData(0x80);

    EPD_SendCommand(0x22);
    EPD_SendData(0xB1);
    EPD_SendCommand(0x20);

    EPD_SetCursor(0, height_ - 1);
    read_busy();
    EPD_SetLut(WF_Full_1IN54);
}

void epaper_driver_display::EPD_Clear() {
    memset(buffer_, 0xff, lcd_spi_data_.buffer_len);
}

void epaper_driver_display::EPD_Display() {
    EPD_SendCommand(0x24);
    writeBytes(buffer_, lcd_spi_data_.buffer_len);
    EPD_TurnOnDisplay();
}

void epaper_driver_display::EPD_DisplayPartBaseImage() {
    EPD_SendCommand(0x24);
    writeBytes(buffer_, lcd_spi_data_.buffer_len);
    EPD_SendCommand(0x26);
    writeBytes(buffer_, lcd_spi_data_.buffer_len);
    EPD_TurnOnDisplay();
}

void epaper_driver_display::EPD_Init_Partial() {
    set_rst_1();
    delay(50);
    set_rst_0();
    delay(20);
    set_rst_1();
    delay(50);

    read_busy();
    EPD_SetLut(WF_PARTIAL_1IN54_0);

    EPD_SendCommand(0x37);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x40);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00);

    EPD_SendCommand(0x3C);
    EPD_SendData(0x80);

    EPD_SendCommand(0x22);
    EPD_SendData(0xc0);
    EPD_SendCommand(0x20);
    read_busy();
}

void epaper_driver_display::EPD_DisplayPart() {
    EPD_SendCommand(0x24);
    writeBytes(buffer_, lcd_spi_data_.buffer_len);
    EPD_TurnOnDisplayPart();
}

void epaper_driver_display::EPD_DrawColorPixel(uint16_t x, uint16_t y,
                                               uint8_t color) {
    if (x >= width_ || y >= height_) {
        ESP_LOGE(TAG, "Out of bounds pixel: (%u,%u)", x, y);
        return;
    }

    const uint16_t index = y * (width_ / 8) + (x >> 3);
    const uint8_t bit = 7 - (x & 0x07);

    if (color == DRIVER_COLOR_WHITE) {
        buffer_[index] |= (0x01 << bit);
    } else {
        buffer_[index] &= ~(0x01 << bit);
    }
}
