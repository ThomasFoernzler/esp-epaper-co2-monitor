#ifndef APP_CONFIG_H
#define APP_CONFIG_H

namespace app_config {

constexpr char kDeviceName[] = "E-Paper CO2 Monitor";
constexpr char kDeviceId[] = "epaper_co2_monitor";
constexpr char kMqttBaseTopic[] = "homeassistant/epaper_co2_monitor";

constexpr bool kEnableWifi = false;
constexpr bool kEnableMqtt = false;

constexpr uint32_t kSensorPollMs = 5000;
constexpr uint32_t kUiRefreshMs = 5000;
constexpr uint32_t kMqttPublishMs = 30 * 1000;
constexpr uint32_t kWifiRetryMs = 10 * 1000;
constexpr uint32_t kMqttRetryMs = 5 * 1000;

}  // namespace app_config

#endif
