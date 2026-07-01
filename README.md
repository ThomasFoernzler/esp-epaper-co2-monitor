# ESP32-S3 ePaper CO2 Monitor

PlatformIO project for the Waveshare `ESP32-S3-ePaper-1.54` board with:

- on-board SHTC3 temperature and humidity
- external SCD40 CO2 / temperature / humidity on the same I2C bus
- simple LVGL status UI on the e-paper display
- Wi-Fi with optional MQTT publishing for Home Assistant

## Board setup

The vendor Arduino examples use these GPIOs:

- e-paper SPI: `DC=10`, `CS=11`, `SCK=12`, `MOSI=13`, `RST=9`, `BUSY=8`
- power control: `EPD=6`, `AUDIO=42`, `VBAT=17`
- shared sensor I2C: `SDA=47`, `SCL=48`

This project keeps those pins and uses the generic PlatformIO board `esp32-s3-devkitm-1`.

## SCD40 wiring

Connect the SCD40 breakout to the board I2C bus:

- `3V3` -> `VIN` or `3V3` on the SCD40 breakout
- `GND` -> `GND`
- `GPIO47` -> `SDA`
- `GPIO48` -> `SCL`

Notes:

- The onboard SHTC3 and the external SCD40 are expected on the same `GPIO47/48` I2C bus.
- `GPIO19/20` must not be used for I2C while debugging over native USB, because they are the ESP32-S3 USB data pins.
- The SCD40 uses address `0x62`, so there is no address conflict.
- If you use a raw SCD40 module instead of a breakout, you need proper pull-ups on SDA/SCL.

## Configure

1. Copy `include/secrets.example.h` to `include/secrets.h`.
2. Fill in Wi-Fi credentials in `include/secrets.h`.
3. Leave MQTT values as placeholders if `kEnableMqtt` stays `false`.
4. Adjust `include/app_config.h` if you want different topic names or want to enable MQTT later.

## Build

```bash
pio run
pio run -t upload
pio device monitor -b 115200
```

## Home Assistant / MQTT

MQTT is disabled by default in `include/app_config.h`.

When enabled, the firmware publishes:

- discovery topics under `homeassistant/sensor/epaper_co2_monitor/...`
- retained state at `homeassistant/epaper_co2_monitor/state`
- retained availability at `homeassistant/epaper_co2_monitor/availability`

## Current scope

The UI is intentionally simple for the first version:

- board temperature / humidity
- SCD40 temperature / humidity / CO2
- Wi-Fi status
- MQTT status or `disabled`

No touch input or deep sleep is wired yet.
