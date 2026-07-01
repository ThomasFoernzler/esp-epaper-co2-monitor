#include "board_power_bsp.h"

#include <driver/gpio.h>

board_power_bsp_t::board_power_bsp_t(uint8_t epd_power_pin,
                                     uint8_t audio_power_pin,
                                     uint8_t vbat_power_pin)
    : epd_power_pin_(epd_power_pin),
      audio_power_pin_(audio_power_pin),
      vbat_power_pin_(vbat_power_pin) {
    gpio_config_t config = {};
    config.intr_type = GPIO_INTR_DISABLE;
    config.mode = GPIO_MODE_OUTPUT;
    config.pin_bit_mask = (1ULL << epd_power_pin_) | (1ULL << audio_power_pin_) |
                          (1ULL << vbat_power_pin_);
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&config));
}

void board_power_bsp_t::powerEpdOn() { gpio_set_level((gpio_num_t)epd_power_pin_, 0); }

void board_power_bsp_t::powerEpdOff() { gpio_set_level((gpio_num_t)epd_power_pin_, 1); }

void board_power_bsp_t::powerAudioOn() {
    gpio_set_level((gpio_num_t)audio_power_pin_, 0);
}

void board_power_bsp_t::powerAudioOff() {
    gpio_set_level((gpio_num_t)audio_power_pin_, 1);
}

void board_power_bsp_t::powerVbatOn() { gpio_set_level((gpio_num_t)vbat_power_pin_, 1); }

void board_power_bsp_t::powerVbatOff() { gpio_set_level((gpio_num_t)vbat_power_pin_, 0); }

