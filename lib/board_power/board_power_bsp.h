#ifndef BOARD_POWER_BSP_H
#define BOARD_POWER_BSP_H

#include <stdint.h>

class board_power_bsp_t {
public:
    board_power_bsp_t(uint8_t epd_power_pin, uint8_t audio_power_pin,
                      uint8_t vbat_power_pin);

    void powerEpdOn();
    void powerEpdOff();
    void powerAudioOn();
    void powerAudioOff();
    void powerVbatOn();
    void powerVbatOff();

private:
    uint8_t epd_power_pin_;
    uint8_t audio_power_pin_;
    uint8_t vbat_power_pin_;
};

#endif

