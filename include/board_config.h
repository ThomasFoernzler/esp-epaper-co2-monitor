#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include <driver/gpio.h>
#include <driver/spi_master.h>

namespace board_config {

constexpr spi_host_device_t kEpaperSpiHost = SPI2_HOST;

constexpr int kDisplayWidth = 200;
constexpr int kDisplayHeight = 200;
constexpr int kDisplayBufferLen = (kDisplayWidth * kDisplayHeight) / 8;

constexpr gpio_num_t kEpdDcPin = GPIO_NUM_10;
constexpr gpio_num_t kEpdCsPin = GPIO_NUM_11;
constexpr gpio_num_t kEpdSckPin = GPIO_NUM_12;
constexpr gpio_num_t kEpdMosiPin = GPIO_NUM_13;
constexpr gpio_num_t kEpdRstPin = GPIO_NUM_9;
constexpr gpio_num_t kEpdBusyPin = GPIO_NUM_8;

constexpr gpio_num_t kEpdPowerPin = GPIO_NUM_6;
constexpr gpio_num_t kAudioPowerPin = GPIO_NUM_42;
constexpr gpio_num_t kVbatPowerPin = GPIO_NUM_17;
constexpr gpio_num_t kBootButtonPin = GPIO_NUM_0;

constexpr gpio_num_t kBoardI2cSdaPin = GPIO_NUM_47;
constexpr gpio_num_t kBoardI2cSclPin = GPIO_NUM_48;

constexpr gpio_num_t kExternalI2cSdaPin = GPIO_NUM_47;
constexpr gpio_num_t kExternalI2cSclPin = GPIO_NUM_48;

constexpr uint8_t kShtc3Address = 0x70;
constexpr uint8_t kScd40Address = 0x62;

constexpr uint32_t kLvglTickPeriodMs = 5;
constexpr uint32_t kLvglTaskMaxDelayMs = 500;
constexpr uint32_t kLvglTaskMinDelayMs = 100;

}  // namespace board_config

#endif
