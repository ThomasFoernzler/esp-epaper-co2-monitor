#include <Arduino.h>
#include <PubSubClient.h>
#include <SensirionI2cScd4x.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_SHTC3.h>
#include <esp_timer.h>
#include <lvgl.h>
#include <driver/adc.h>
#include <esp_adc_cal.h>

#include "app_config.h"
#include "board_config.h"
#include "board_power_bsp.h"
#include "epaper_driver_bsp.h"
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

namespace {

struct SensorSnapshot {
    float boardTempC = 0.0f;
    float boardHumidity = 0.0f;
    float scdTempC = 0.0f;
    float scdHumidity = 0.0f;
    uint16_t scdCo2Ppm = 0;
    float batteryVoltage = 0.0f;
    uint8_t batteryPercent = 0;
    bool boardOk = false;
    bool scdOk = false;
    bool batteryOk = false;
};

enum class DashboardPage : uint8_t {
    Sensor = 0,
    Debug = 1,
};

struct SensorUi {
    lv_obj_t* co2Label = nullptr;
    lv_obj_t* co2Value = nullptr;
    lv_obj_t* co2Unit = nullptr;
    lv_obj_t* co2Badge = nullptr;
    lv_obj_t* humidityValue = nullptr;
    lv_obj_t* humidityLabel = nullptr;
    lv_obj_t* temperatureLabel = nullptr;
    lv_obj_t* temperatureValue = nullptr;
    lv_obj_t* co2ScaleLine = nullptr;
    lv_obj_t* co2ScaleTick400 = nullptr;
    lv_obj_t* co2ScaleTick800 = nullptr;
    lv_obj_t* co2ScaleTick1000 = nullptr;
    lv_obj_t* co2ScaleTick1400 = nullptr;
    lv_obj_t* co2ScaleMarker = nullptr;
    lv_obj_t* co2ScaleMin = nullptr;
    lv_obj_t* co2Scale800 = nullptr;
    lv_obj_t* co2Scale1000 = nullptr;
    lv_obj_t* co2Scale1400 = nullptr;
    lv_obj_t* co2Status = nullptr;
};

struct DebugUi {
    lv_obj_t* boardTitle = nullptr;
    lv_obj_t* boardValue = nullptr;
    lv_obj_t* batteryTitle = nullptr;
    lv_obj_t* batteryValue = nullptr;
    lv_obj_t* wifiTitle = nullptr;
    lv_obj_t* wifiValue = nullptr;
    lv_obj_t* mqttTitle = nullptr;
    lv_obj_t* mqttValue = nullptr;
    lv_obj_t* uptimeTitle = nullptr;
    lv_obj_t* uptimeValue = nullptr;
};

epaper_driver_display* g_display = nullptr;
board_power_bsp_t* g_power = nullptr;
SemaphoreHandle_t g_lvglMutex = nullptr;
SensorUi g_sensorUi;
DebugUi g_debugUi;
SensorSnapshot g_snapshot;
WiFiClient g_wifiClient;
PubSubClient g_mqttClient(g_wifiClient);
Adafruit_SHTC3 g_shtc3;
SensirionI2cScd4x g_scd40;
TwoWire g_boardWire(0);

char g_errorMessage[64];
int16_t g_scdError = 0;

uint32_t g_lastWifiAttemptMs = 0;
uint32_t g_lastMqttAttemptMs = 0;
uint32_t g_lastSensorPollMs = 0;
uint32_t g_lastUiRefreshMs = 0;
uint32_t g_lastMqttPublishMs = 0;

DashboardPage g_currentPage = DashboardPage::Sensor;
bool g_lastButtonPressed = false;
uint32_t g_lastButtonChangeMs = 0;
lv_style_t g_labelStyle;
lv_style_t g_valueStyle;
lv_style_t g_debugLabelStyle;
lv_style_t g_debugValueStyle;
lv_style_t g_scaleTextStyle;
lv_style_t g_headerValueStyle;
lv_style_t g_headerUnitStyle;
esp_adc_cal_characteristics_t g_adcCharacteristics = {};
bool g_adcOk = false;

constexpr int kCo2ScaleX = 12;
constexpr int kCo2ScaleY = 74;
constexpr int kCo2ScaleWidth = 176;
constexpr int kCo2ScaleHeight = 2;
constexpr int kCo2ScaleMinPpm = 400;
constexpr int kCo2ScaleMaxPpm = 1400;

int co2PpmToScaleX(uint16_t ppm) {
    const int clamped_ppm =
        constrain(static_cast<int>(ppm), kCo2ScaleMinPpm, kCo2ScaleMaxPpm);
    return kCo2ScaleX +
           ((clamped_ppm - kCo2ScaleMinPpm) * kCo2ScaleWidth) /
               (kCo2ScaleMaxPpm - kCo2ScaleMinPpm);
}

void setScaleObjGeometry(lv_obj_t* obj, int x, int y, int w, int h) {
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
}

bool lvglLock(int timeout_ms) {
    const TickType_t timeout_ticks =
        (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(g_lvglMutex, timeout_ticks) == pdTRUE;
}

void lvglUnlock() { xSemaphoreGive(g_lvglMutex); }

void redrawUiNow() {
    if (!lvglLock(100)) {
        return;
    }
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    lvglUnlock();
}

void setHidden(lv_obj_t* obj, bool hidden) {
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void applySensorLayout() {
    setScaleObjGeometry(g_sensorUi.co2ScaleLine, kCo2ScaleX, kCo2ScaleY,
                        kCo2ScaleWidth, kCo2ScaleHeight);
    setScaleObjGeometry(g_sensorUi.co2ScaleTick400, co2PpmToScaleX(400),
                        kCo2ScaleY - 4, 2, 10);
    setScaleObjGeometry(g_sensorUi.co2ScaleTick800, co2PpmToScaleX(800),
                        kCo2ScaleY - 4, 2, 10);
    setScaleObjGeometry(g_sensorUi.co2ScaleTick1000, co2PpmToScaleX(1000),
                        kCo2ScaleY - 4, 2, 10);
    setScaleObjGeometry(g_sensorUi.co2ScaleTick1400, co2PpmToScaleX(1400) - 2,
                        kCo2ScaleY - 4, 2, 10);

    lv_obj_align(g_sensorUi.co2Label, LV_ALIGN_TOP_LEFT, 10, 14);
    lv_obj_align(g_sensorUi.co2Value, LV_ALIGN_TOP_LEFT, 58, 8);
    lv_obj_align(g_sensorUi.co2Unit, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_align(g_sensorUi.co2Badge, LV_ALIGN_TOP_RIGHT, -10, 30);
    lv_obj_align(g_sensorUi.co2ScaleMin, LV_ALIGN_TOP_LEFT, 8, 80);
    lv_obj_align(g_sensorUi.co2Scale800, LV_ALIGN_TOP_LEFT, 71, 80);
    lv_obj_align(g_sensorUi.co2Scale1000, LV_ALIGN_TOP_LEFT, 102, 80);
    lv_obj_align(g_sensorUi.co2Scale1400, LV_ALIGN_TOP_LEFT, 156, 80);
    lv_obj_align(g_sensorUi.humidityValue, LV_ALIGN_TOP_LEFT, 14, 120);
    lv_obj_align(g_sensorUi.humidityLabel, LV_ALIGN_TOP_LEFT, 14, 158);
    lv_obj_align(g_sensorUi.temperatureLabel, LV_ALIGN_TOP_LEFT, 116, 120);
    lv_obj_align(g_sensorUi.temperatureValue, LV_ALIGN_TOP_LEFT, 116, 146);
}

void applyDebugLayout() {
    lv_obj_align(g_debugUi.boardTitle, LV_ALIGN_TOP_LEFT, 10, 12);
    lv_obj_align(g_debugUi.boardValue, LV_ALIGN_TOP_LEFT, 78, 10);
    lv_obj_align(g_debugUi.batteryTitle, LV_ALIGN_TOP_LEFT, 10, 46);
    lv_obj_align(g_debugUi.batteryValue, LV_ALIGN_TOP_LEFT, 78, 44);
    lv_obj_align(g_debugUi.wifiTitle, LV_ALIGN_TOP_LEFT, 10, 82);
    lv_obj_align(g_debugUi.wifiValue, LV_ALIGN_TOP_LEFT, 78, 80);
    lv_obj_align(g_debugUi.mqttTitle, LV_ALIGN_TOP_LEFT, 10, 118);
    lv_obj_align(g_debugUi.mqttValue, LV_ALIGN_TOP_LEFT, 78, 116);
    lv_obj_align(g_debugUi.uptimeTitle, LV_ALIGN_TOP_LEFT, 10, 154);
    lv_obj_align(g_debugUi.uptimeValue, LV_ALIGN_TOP_LEFT, 78, 152);
}

void setSensorPageHidden(bool hidden) {
    setHidden(g_sensorUi.co2Label, hidden);
    setHidden(g_sensorUi.co2Value, hidden);
    setHidden(g_sensorUi.co2Unit, hidden);
    setHidden(g_sensorUi.co2Badge, hidden);
    setHidden(g_sensorUi.humidityValue, hidden);
    setHidden(g_sensorUi.humidityLabel, hidden);
    setHidden(g_sensorUi.temperatureLabel, hidden);
    setHidden(g_sensorUi.temperatureValue, hidden);
    setHidden(g_sensorUi.co2ScaleLine, hidden);
    setHidden(g_sensorUi.co2ScaleTick400, hidden);
    setHidden(g_sensorUi.co2ScaleTick800, hidden);
    setHidden(g_sensorUi.co2ScaleTick1000, hidden);
    setHidden(g_sensorUi.co2ScaleTick1400, hidden);
    setHidden(g_sensorUi.co2ScaleMarker, hidden);
    setHidden(g_sensorUi.co2ScaleMin, hidden);
    setHidden(g_sensorUi.co2Scale800, hidden);
    setHidden(g_sensorUi.co2Scale1000, hidden);
    setHidden(g_sensorUi.co2Scale1400, hidden);
    setHidden(g_sensorUi.co2Status, true);
}

void setDebugPageHidden(bool hidden) {
    setHidden(g_debugUi.boardTitle, hidden);
    setHidden(g_debugUi.boardValue, hidden);
    setHidden(g_debugUi.batteryTitle, hidden);
    setHidden(g_debugUi.batteryValue, hidden);
    setHidden(g_debugUi.wifiTitle, hidden);
    setHidden(g_debugUi.wifiValue, hidden);
    setHidden(g_debugUi.mqttTitle, hidden);
    setHidden(g_debugUi.mqttValue, hidden);
    setHidden(g_debugUi.uptimeTitle, hidden);
    setHidden(g_debugUi.uptimeValue, hidden);
}

void updateUi() {
    if (!lvglLock(100)) {
        return;
    }

    static char boardValue[64];
    static char batteryValue[32];
    static char wifiValue[64];
    static char mqttValue[32];
    static char uptimeValue[32];
    static char temperatureValue[32];
    static char humidityValue[32];
    static char wifiState[64];
    static char mqttState[32];
    static char co2Value[16];
    static char co2Status[8];

    snprintf(wifiState, sizeof(wifiState), "%s",
             !app_config::kEnableWifi
                 ? "disabled"
                 : (WiFi.status() == WL_CONNECTED
                        ? WiFi.localIP().toString().c_str()
                        : "disconnected"));
    snprintf(mqttState, sizeof(mqttState), "%s",
             !app_config::kEnableMqtt
                 ? "disabled"
                 : (g_mqttClient.connected() ? "connected" : "disconnected"));

    if (g_currentPage == DashboardPage::Sensor) {
        applySensorLayout();
        setSensorPageHidden(false);
        setDebugPageHidden(true);

        if (g_snapshot.scdOk) {
            snprintf(co2Value, sizeof(co2Value), "%u", g_snapshot.scdCo2Ppm);
            snprintf(temperatureValue, sizeof(temperatureValue), "%.1f",
                     g_snapshot.scdTempC);
            snprintf(humidityValue, sizeof(humidityValue), "%.1f",
                     g_snapshot.scdHumidity);
            if (g_snapshot.scdCo2Ppm < 800) {
                snprintf(co2Status, sizeof(co2Status), "OK");
            } else if (g_snapshot.scdCo2Ppm < 1000) {
                snprintf(co2Status, sizeof(co2Status), "!");
            } else {
                snprintf(co2Status, sizeof(co2Status), "!!");
            }
            setScaleObjGeometry(g_sensorUi.co2ScaleMarker,
                                co2PpmToScaleX(g_snapshot.scdCo2Ppm) - 2,
                                kCo2ScaleY - 8, 4, 16);
        } else {
            snprintf(co2Value, sizeof(co2Value), "--");
            snprintf(temperatureValue, sizeof(temperatureValue), "--.-");
            snprintf(humidityValue, sizeof(humidityValue), "--.-");
            snprintf(co2Status, sizeof(co2Status), "--");
            setScaleObjGeometry(g_sensorUi.co2ScaleMarker,
                                co2PpmToScaleX(400) - 2, kCo2ScaleY - 8, 4, 16);
        }
        lv_label_set_text(g_sensorUi.co2Value, co2Value);
        lv_label_set_text(g_sensorUi.temperatureValue, temperatureValue);
        lv_label_set_text(g_sensorUi.humidityValue, humidityValue);
        lv_label_set_text(g_sensorUi.co2Badge, co2Status);
    } else {
        applyDebugLayout();
        setSensorPageHidden(true);
        setDebugPageHidden(false);

        if (g_snapshot.boardOk) {
            snprintf(boardValue, sizeof(boardValue), "%.1f C   %.1f %%RH",
                     g_snapshot.boardTempC, g_snapshot.boardHumidity);
        } else {
            snprintf(boardValue, sizeof(boardValue), "unavailable");
        }
        if (g_snapshot.batteryOk) {
            snprintf(batteryValue, sizeof(batteryValue), "%u%%  %.2fV",
                     g_snapshot.batteryPercent, g_snapshot.batteryVoltage);
        } else {
            snprintf(batteryValue, sizeof(batteryValue), "unavailable");
        }
        snprintf(wifiValue, sizeof(wifiValue), "%s", wifiState);
        snprintf(mqttValue, sizeof(mqttValue), "%s", mqttState);
        snprintf(uptimeValue, sizeof(uptimeValue), "%lus", millis() / 1000UL);

        lv_label_set_text(g_debugUi.boardValue, boardValue);
        lv_label_set_text(g_debugUi.batteryValue, batteryValue);
        lv_label_set_text(g_debugUi.wifiValue, wifiValue);
        lv_label_set_text(g_debugUi.mqttValue, mqttValue);
        lv_label_set_text(g_debugUi.uptimeValue, uptimeValue);
    }
    lvglUnlock();

    Serial.printf("[UI] page=%s row1='%s' row2='%s' row3='%s' row4='%s'\n",
                  g_currentPage == DashboardPage::Sensor ? "Sensor" : "Debug",
                  g_currentPage == DashboardPage::Sensor ? co2Value : boardValue,
                  g_currentPage == DashboardPage::Sensor ? temperatureValue
                                                         : batteryValue,
                  g_currentPage == DashboardPage::Sensor ? humidityValue
                                                         : wifiValue,
                  g_currentPage == DashboardPage::Sensor ? co2Status
                                                         : mqttValue);
}

void uiCreate() {
    lv_obj_t* screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    lv_style_init(&g_labelStyle);
    lv_style_set_text_font(&g_labelStyle, &lv_font_montserrat_20);

    lv_style_init(&g_valueStyle);
    lv_style_set_text_font(&g_valueStyle, &lv_font_montserrat_28);

    lv_style_init(&g_debugLabelStyle);
    lv_style_set_text_font(&g_debugLabelStyle, &lv_font_montserrat_16);

    lv_style_init(&g_debugValueStyle);
    lv_style_set_text_font(&g_debugValueStyle, &lv_font_montserrat_20);

    lv_style_init(&g_scaleTextStyle);
    lv_style_set_text_font(&g_scaleTextStyle, &lv_font_montserrat_14);

    lv_style_init(&g_headerValueStyle);
    lv_style_set_text_font(&g_headerValueStyle, &lv_font_montserrat_28);

    lv_style_init(&g_headerUnitStyle);
    lv_style_set_text_font(&g_headerUnitStyle, &lv_font_montserrat_14);

    g_sensorUi.co2Label = lv_label_create(screen);
    lv_obj_add_style(g_sensorUi.co2Label, &g_labelStyle, 0);
    lv_label_set_text(g_sensorUi.co2Label, "CO2");

    g_sensorUi.co2Value = lv_label_create(screen);
    lv_obj_add_style(g_sensorUi.co2Value, &g_headerValueStyle, 0);
    lv_label_set_text(g_sensorUi.co2Value, "--");

    g_sensorUi.co2Unit = lv_label_create(screen);
    lv_obj_add_style(g_sensorUi.co2Unit, &g_headerUnitStyle, 0);
    lv_label_set_text(g_sensorUi.co2Unit, "ppm");

    g_sensorUi.co2Badge = lv_label_create(screen);
    lv_obj_add_style(g_sensorUi.co2Badge, &g_labelStyle, 0);
    lv_label_set_text(g_sensorUi.co2Badge, "OK");

    g_sensorUi.humidityValue = lv_label_create(screen);
    lv_obj_add_style(g_sensorUi.humidityValue, &g_valueStyle, 0);
    lv_obj_set_width(g_sensorUi.humidityValue, 80);
    lv_label_set_long_mode(g_sensorUi.humidityValue, LV_LABEL_LONG_WRAP);
    lv_label_set_text(g_sensorUi.humidityValue, "--.-");

    g_sensorUi.humidityLabel = lv_label_create(screen);
    lv_obj_add_style(g_sensorUi.humidityLabel, &g_labelStyle, 0);
    lv_obj_set_width(g_sensorUi.humidityLabel, 80);
    lv_label_set_text(g_sensorUi.humidityLabel, "Rel. Hum.");

    g_sensorUi.temperatureLabel = lv_label_create(screen);
    lv_obj_add_style(g_sensorUi.temperatureLabel, &g_labelStyle, 0);
    lv_obj_set_width(g_sensorUi.temperatureLabel, 80);
    lv_label_set_text(g_sensorUi.temperatureLabel, "Tmp C");

    g_sensorUi.temperatureValue = lv_label_create(screen);
    lv_obj_add_style(g_sensorUi.temperatureValue, &g_valueStyle, 0);
    lv_obj_set_width(g_sensorUi.temperatureValue, 80);
    lv_label_set_long_mode(g_sensorUi.temperatureValue, LV_LABEL_LONG_WRAP);
    lv_label_set_text(g_sensorUi.temperatureValue, "--.-");

    g_debugUi.boardTitle = lv_label_create(screen);
    lv_obj_add_style(g_debugUi.boardTitle, &g_debugLabelStyle, 0);
    lv_label_set_text(g_debugUi.boardTitle, "Board");

    g_debugUi.boardValue = lv_label_create(screen);
    lv_obj_add_style(g_debugUi.boardValue, &g_debugValueStyle, 0);
    lv_obj_set_width(g_debugUi.boardValue, 110);
    lv_label_set_long_mode(g_debugUi.boardValue, LV_LABEL_LONG_WRAP);
    lv_label_set_text(g_debugUi.boardValue, "init...");

    g_debugUi.batteryTitle = lv_label_create(screen);
    lv_obj_add_style(g_debugUi.batteryTitle, &g_debugLabelStyle, 0);
    lv_label_set_text(g_debugUi.batteryTitle, "Battery");

    g_debugUi.batteryValue = lv_label_create(screen);
    lv_obj_add_style(g_debugUi.batteryValue, &g_debugValueStyle, 0);
    lv_obj_set_width(g_debugUi.batteryValue, 110);
    lv_label_set_long_mode(g_debugUi.batteryValue, LV_LABEL_LONG_WRAP);
    lv_label_set_text(g_debugUi.batteryValue, "init...");

    g_debugUi.wifiTitle = lv_label_create(screen);
    lv_obj_add_style(g_debugUi.wifiTitle, &g_debugLabelStyle, 0);
    lv_label_set_text(g_debugUi.wifiTitle, "WiFi");

    g_debugUi.wifiValue = lv_label_create(screen);
    lv_obj_add_style(g_debugUi.wifiValue, &g_debugValueStyle, 0);
    lv_obj_set_width(g_debugUi.wifiValue, 110);
    lv_label_set_long_mode(g_debugUi.wifiValue, LV_LABEL_LONG_WRAP);
    lv_label_set_text(g_debugUi.wifiValue, "init...");

    g_debugUi.mqttTitle = lv_label_create(screen);
    lv_obj_add_style(g_debugUi.mqttTitle, &g_debugLabelStyle, 0);
    lv_label_set_text(g_debugUi.mqttTitle, "MQTT");

    g_debugUi.mqttValue = lv_label_create(screen);
    lv_obj_add_style(g_debugUi.mqttValue, &g_debugValueStyle, 0);
    lv_obj_set_width(g_debugUi.mqttValue, 110);
    lv_label_set_long_mode(g_debugUi.mqttValue, LV_LABEL_LONG_WRAP);
    lv_label_set_text(g_debugUi.mqttValue, "init...");

    g_debugUi.uptimeTitle = lv_label_create(screen);
    lv_obj_add_style(g_debugUi.uptimeTitle, &g_debugLabelStyle, 0);
    lv_label_set_text(g_debugUi.uptimeTitle, "Uptime");

    g_debugUi.uptimeValue = lv_label_create(screen);
    lv_obj_add_style(g_debugUi.uptimeValue, &g_debugValueStyle, 0);
    lv_obj_set_width(g_debugUi.uptimeValue, 110);
    lv_label_set_long_mode(g_debugUi.uptimeValue, LV_LABEL_LONG_WRAP);
    lv_label_set_text(g_debugUi.uptimeValue, "0s");

    auto makeScaleObj = [&](int w, int h) {
        lv_obj_t* obj = lv_obj_create(screen);
        lv_obj_remove_style_all(obj);
        lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(obj, 0, 0);
        lv_obj_set_style_radius(obj, 0, 0);
        lv_obj_set_size(obj, w, h);
        return obj;
    };

    g_sensorUi.co2ScaleLine = makeScaleObj(kCo2ScaleWidth, kCo2ScaleHeight);
    g_sensorUi.co2ScaleTick400 = makeScaleObj(2, 10);
    g_sensorUi.co2ScaleTick800 = makeScaleObj(2, 10);
    g_sensorUi.co2ScaleTick1000 = makeScaleObj(2, 10);
    g_sensorUi.co2ScaleTick1400 = makeScaleObj(2, 10);
    g_sensorUi.co2ScaleMarker = makeScaleObj(4, 16);

    g_sensorUi.co2ScaleMin = lv_label_create(screen);
    g_sensorUi.co2Scale800 = lv_label_create(screen);
    g_sensorUi.co2Scale1000 = lv_label_create(screen);
    g_sensorUi.co2Scale1400 = lv_label_create(screen);
    g_sensorUi.co2Status = lv_label_create(screen);

    lv_obj_add_style(g_sensorUi.co2ScaleMin, &g_scaleTextStyle, 0);
    lv_obj_add_style(g_sensorUi.co2Scale800, &g_scaleTextStyle, 0);
    lv_obj_add_style(g_sensorUi.co2Scale1000, &g_scaleTextStyle, 0);
    lv_obj_add_style(g_sensorUi.co2Scale1400, &g_scaleTextStyle, 0);
    lv_obj_add_style(g_sensorUi.co2Status, &g_scaleTextStyle, 0);

    lv_label_set_text(g_sensorUi.co2ScaleMin, "400");
    lv_label_set_text(g_sensorUi.co2Scale800, "800");
    lv_label_set_text(g_sensorUi.co2Scale1000, "1000");
    lv_label_set_text(g_sensorUi.co2Scale1400, "1400");
    lv_label_set_text(g_sensorUi.co2Status, "OK");
    lv_obj_add_flag(g_sensorUi.co2Status, LV_OBJ_FLAG_HIDDEN);

    applySensorLayout();
    applyDebugLayout();
    setSensorPageHidden(false);
    setDebugPageHidden(true);
}

bool initBatteryAdc() {
    g_power->powerVbatOn();
    if (adc1_config_width(ADC_WIDTH_BIT_12) != ESP_OK) {
        Serial.println("Battery ADC width config failed");
        return false;
    }
    if (adc1_config_channel_atten(ADC1_CHANNEL_3, ADC_ATTEN_DB_12) != ESP_OK) {
        Serial.println("Battery ADC channel config failed");
        return false;
    }

    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12,
                             1100, &g_adcCharacteristics);
    g_adcOk = true;

    Serial.println("Battery ADC ready on ADC1_CH3");
    return true;
}

void pollBattery() {
    if (!g_adcOk) {
        g_snapshot.batteryOk = false;
        return;
    }

    const int raw = adc1_get_raw(ADC1_CHANNEL_3);
    const uint32_t millivolts =
        esp_adc_cal_raw_to_voltage(raw, &g_adcCharacteristics);

    g_snapshot.batteryVoltage =
        0.001f * static_cast<float>(millivolts) * 2.0f;
    constexpr float kVoltEmpty = 3.0f;
    constexpr float kVoltFull = 4.12f;
    if (g_snapshot.batteryVoltage <= kVoltEmpty) {
        g_snapshot.batteryPercent = 0;
    } else if (g_snapshot.batteryVoltage >= kVoltFull) {
        g_snapshot.batteryPercent = 100;
    } else {
        g_snapshot.batteryPercent = static_cast<uint8_t>(
            ((g_snapshot.batteryVoltage - kVoltEmpty) /
             (kVoltFull - kVoltEmpty)) *
            100.0f);
    }
    g_snapshot.batteryOk = true;
    Serial.printf("[BAT] %.2fV %u%%\n", g_snapshot.batteryVoltage,
                  g_snapshot.batteryPercent);
}

void lvglFlushCb(lv_disp_drv_t* disp_drv, const lv_area_t* area,
                 lv_color_t* color_map) {
    uint16_t* buffer = reinterpret_cast<uint16_t*>(color_map);
    g_display->EPD_Clear();

    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            const uint8_t color =
                (*buffer < 0x7fff) ? DRIVER_COLOR_BLACK : DRIVER_COLOR_WHITE;
            const uint16_t rotated_x = board_config::kDisplayWidth - 1 - y;
            const uint16_t rotated_y = x;
            g_display->EPD_DrawColorPixel(rotated_x, rotated_y, color);
            ++buffer;
        }
    }

    g_display->EPD_DisplayPart();
    lv_disp_flush_ready(disp_drv);
}

void lvglTick(void*) { lv_tick_inc(board_config::kLvglTickPeriodMs); }

void lvglTask(void*) {
    uint32_t delay_ms = board_config::kLvglTaskMaxDelayMs;
    for (;;) {
        if (lvglLock(-1)) {
            delay_ms = lv_timer_handler();
            lvglUnlock();
        }
        delay_ms = constrain(delay_ms, board_config::kLvglTaskMinDelayMs,
                             board_config::kLvglTaskMaxDelayMs);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

void initDisplay() {
    g_power->powerEpdOn();
    g_power->powerAudioOff();
    g_power->powerVbatOff();

    custom_lcd_spi_t driver_config = {};
    driver_config.cs = board_config::kEpdCsPin;
    driver_config.dc = board_config::kEpdDcPin;
    driver_config.rst = board_config::kEpdRstPin;
    driver_config.busy = board_config::kEpdBusyPin;
    driver_config.mosi = board_config::kEpdMosiPin;
    driver_config.scl = board_config::kEpdSckPin;
    driver_config.spi_host = board_config::kEpaperSpiHost;
    driver_config.buffer_len = board_config::kDisplayBufferLen;

    g_display = new epaper_driver_display(board_config::kDisplayWidth,
                                          board_config::kDisplayHeight,
                                          driver_config);
    g_display->EPD_Init();
    g_display->EPD_Clear();
    g_display->EPD_DisplayPartBaseImage();
    g_display->EPD_Init_Partial();

    lv_init();

    auto* draw_buffer = static_cast<lv_color_t*>(
        malloc(sizeof(lv_color_t) * board_config::kDisplayWidth *
               board_config::kDisplayHeight));
    assert(draw_buffer != nullptr);

    static lv_disp_draw_buf_t disp_buf;
    lv_disp_draw_buf_init(&disp_buf, draw_buffer, nullptr,
                          board_config::kDisplayWidth *
                              board_config::kDisplayHeight);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = board_config::kDisplayWidth;
    disp_drv.ver_res = board_config::kDisplayHeight;
    disp_drv.flush_cb = lvglFlushCb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.full_refresh = 1;
    lv_disp_drv_register(&disp_drv);

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = &lvglTick;
    timer_args.arg = nullptr;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "lvgl_tick";
    timer_args.skip_unhandled_events = false;
    esp_timer_handle_t timer_handle = nullptr;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle));
    ESP_ERROR_CHECK(esp_timer_start_periodic(
        timer_handle, board_config::kLvglTickPeriodMs * 1000));

    g_lvglMutex = xSemaphoreCreateMutex();
    assert(g_lvglMutex != nullptr);
    xTaskCreatePinnedToCore(lvglTask, "lvgl", 8192, nullptr, 4, nullptr, 1);

    if (lvglLock(-1)) {
        uiCreate();
        lvglUnlock();
    }
}

bool initBoardSensor() {
    if (!g_shtc3.begin(&g_boardWire)) {
        Serial.println("SHTC3 init failed");
        return false;
    }
    Serial.println("SHTC3 init ok on GPIO47/48");
    return true;
}

bool initScd40() {
    delay(30);
    g_scd40.begin(g_boardWire, board_config::kScd40Address);
    Serial.println("SCD40 begin on shared GPIO47/48 bus");

    g_scdError = g_scd40.wakeUp();
    if (g_scdError != 0) {
        errorToString(g_scdError, g_errorMessage, sizeof(g_errorMessage));
        Serial.printf("SCD40 wakeUp failed: %s\n", g_errorMessage);
    } else {
        Serial.println("SCD40 wakeUp ok");
    }

    g_scdError = g_scd40.stopPeriodicMeasurement();
    if (g_scdError != 0) {
        errorToString(g_scdError, g_errorMessage, sizeof(g_errorMessage));
        Serial.printf("SCD40 stopPeriodicMeasurement failed: %s\n",
                      g_errorMessage);
    }

    g_scdError = g_scd40.reinit();
    if (g_scdError != 0) {
        errorToString(g_scdError, g_errorMessage, sizeof(g_errorMessage));
        Serial.printf("SCD40 reinit failed: %s\n", g_errorMessage);
        return false;
    }

    g_scdError = g_scd40.startPeriodicMeasurement();
    if (g_scdError != 0) {
        errorToString(g_scdError, g_errorMessage, sizeof(g_errorMessage));
        Serial.printf("SCD40 startPeriodicMeasurement failed: %s\n",
                      g_errorMessage);
        return false;
    }

    Serial.println("SCD40 periodic measurement started");
    return true;
}

void initDashboardButton() {
    pinMode(board_config::kBootButtonPin, INPUT_PULLUP);
    g_lastButtonPressed = digitalRead(board_config::kBootButtonPin) == LOW;
    g_lastButtonChangeMs = millis();
    Serial.println("Dashboard button ready on GPIO0");
}

void handleDashboardButton() {
    const bool pressed = digitalRead(board_config::kBootButtonPin) == LOW;
    const uint32_t now = millis();

    if (pressed != g_lastButtonPressed &&
        now - g_lastButtonChangeMs >= 30) {
        g_lastButtonChangeMs = now;
        g_lastButtonPressed = pressed;

        if (pressed) {
            g_currentPage = (g_currentPage == DashboardPage::Sensor)
                                ? DashboardPage::Debug
                                : DashboardPage::Sensor;
            Serial.printf("[UI] switched to %s page\n",
                          g_currentPage == DashboardPage::Sensor ? "Sensor"
                                                                 : "Debug");
            updateUi();
            redrawUiNow();
        }
    }
}

void connectWifi() {
    if (!app_config::kEnableWifi) {
        return;
    }
    if (WiFi.status() == WL_CONNECTED) {
        return;
    }
    if (millis() - g_lastWifiAttemptMs < app_config::kWifiRetryMs) {
        return;
    }

    g_lastWifiAttemptMs = millis();
    Serial.printf("Connecting to WiFi SSID %s\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void publishDiscovery(const char* object_id, const char* name,
                      const char* unit, const char* device_class,
                      const char* state_class, const char* value_template) {
    char topic[192];
    char payload[768];

    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config",
             app_config::kDeviceId, object_id);

    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s/state\","
             "\"val_tpl\":\"%s\",\"avty_t\":\"%s/availability\",\"pl_avail\":"
             "\"online\",\"pl_not_avail\":\"offline\",\"unit_of_meas\":\"%s\","
             "\"dev_cla\":\"%s\",\"stat_cla\":\"%s\",\"dev\":{\"ids\":[\"%s\"],"
             "\"name\":\"%s\",\"mf\":\"Waveshare/Sensirion\",\"mdl\":"
             "\"ESP32-S3-ePaper-1.54 + SCD40\"}}",
             name, app_config::kDeviceId, object_id, app_config::kMqttBaseTopic,
             value_template, app_config::kMqttBaseTopic, unit, device_class,
             state_class, app_config::kDeviceId, app_config::kDeviceName);

    g_mqttClient.publish(topic, payload, true);
}

void publishHomeAssistantDiscovery() {
    publishDiscovery("board_temperature", "Board Temperature", "C",
                     "temperature", "measurement", "{{ value_json.board_temp }}");
    publishDiscovery("board_humidity", "Board Humidity", "%", "humidity",
                     "measurement", "{{ value_json.board_humidity }}");
    publishDiscovery("scd40_co2", "SCD40 CO2", "ppm", "carbon_dioxide",
                     "measurement", "{{ value_json.scd40_co2 }}");
    publishDiscovery("scd40_temperature", "SCD40 Temperature", "C",
                     "temperature", "measurement",
                     "{{ value_json.scd40_temp }}");
    publishDiscovery("scd40_humidity", "SCD40 Humidity", "%", "humidity",
                     "measurement", "{{ value_json.scd40_humidity }}");
    g_mqttClient.publish((String(app_config::kMqttBaseTopic) + "/availability")
                             .c_str(),
                         "online", true);
}

void connectMqtt() {
    if (!app_config::kEnableMqtt || !app_config::kEnableWifi) {
        return;
    }
    if (WiFi.status() != WL_CONNECTED || g_mqttClient.connected()) {
        return;
    }
    if (millis() - g_lastMqttAttemptMs < app_config::kMqttRetryMs) {
        return;
    }

    g_lastMqttAttemptMs = millis();
    g_mqttClient.setServer(MQTT_HOST, MQTT_PORT);
    g_mqttClient.setBufferSize(1536);

    if (g_mqttClient.connect(app_config::kDeviceId, MQTT_USERNAME,
                             MQTT_PASSWORD,
                             (String(app_config::kMqttBaseTopic) + "/availability")
                                 .c_str(),
                             1, true, "offline")) {
        Serial.println("MQTT connected");
        publishHomeAssistantDiscovery();
    } else {
        Serial.printf("MQTT connect failed, rc=%d\n", g_mqttClient.state());
    }
}

void publishState() {
    if (!app_config::kEnableMqtt || !g_mqttClient.connected()) {
        return;
    }

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"board_temp\":%.2f,\"board_humidity\":%.2f,"
             "\"scd40_co2\":%u,\"scd40_temp\":%.2f,\"scd40_humidity\":%.2f}",
             g_snapshot.boardTempC, g_snapshot.boardHumidity,
             g_snapshot.scdCo2Ppm, g_snapshot.scdTempC, g_snapshot.scdHumidity);
    g_mqttClient.publish((String(app_config::kMqttBaseTopic) + "/state").c_str(),
                         payload, true);
}

void pollSensors() {
    pollBattery();

    sensors_event_t humidity;
    sensors_event_t temperature;

    if (g_shtc3.getEvent(&humidity, &temperature)) {
        g_snapshot.boardTempC = temperature.temperature;
        g_snapshot.boardHumidity = humidity.relative_humidity;
        g_snapshot.boardOk = true;
        Serial.printf("[SHTC3] temp=%.2fC humidity=%.2f%%\n",
                      g_snapshot.boardTempC, g_snapshot.boardHumidity);
    } else {
        g_snapshot.boardOk = false;
        Serial.println("[SHTC3] read failed");
    }

    bool data_ready = false;
    g_scdError = g_scd40.getDataReadyStatus(data_ready);
    if (g_scdError != 0) {
        errorToString(g_scdError, g_errorMessage, sizeof(g_errorMessage));
        Serial.printf("[SCD40] getDataReadyStatus failed: %s\n", g_errorMessage);
    }
    if (g_scdError == 0 && data_ready) {
        uint16_t co2 = 0;
        float temperature_c = 0.0f;
        float humidity_rh = 0.0f;
        g_scdError =
            g_scd40.readMeasurement(co2, temperature_c, humidity_rh);
        if (g_scdError == 0) {
            g_snapshot.scdCo2Ppm = co2;
            g_snapshot.scdTempC = temperature_c;
            g_snapshot.scdHumidity = humidity_rh;
            g_snapshot.scdOk = true;
            Serial.printf("[SCD40] co2=%uppm temp=%.2fC humidity=%.2f%%\n",
                          g_snapshot.scdCo2Ppm, g_snapshot.scdTempC,
                          g_snapshot.scdHumidity);
        } else {
            errorToString(g_scdError, g_errorMessage, sizeof(g_errorMessage));
            Serial.printf("[SCD40] readMeasurement failed: %s\n", g_errorMessage);
        }
    } else if (g_scdError == 0) {
        Serial.println("[SCD40] data not ready yet");
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(250);
    Serial.println();
    Serial.println("Booting e-paper CO2 monitor");
    Serial.println("Shared sensor bus: SDA=GPIO47 SCL=GPIO48");
    Serial.println("GPIO19/20 must not be used for I2C while USB debugging is active");

    g_power = new board_power_bsp_t(board_config::kEpdPowerPin,
                                    board_config::kAudioPowerPin,
                                    board_config::kVbatPowerPin);
    g_boardWire.begin(board_config::kBoardI2cSdaPin,
                      board_config::kBoardI2cSclPin);
    g_boardWire.setClock(100000);

    initDashboardButton();
    initDisplay();
    initBatteryAdc();
    initBoardSensor();
    initScd40();
    pollSensors();
    updateUi();
}

void loop() {
    handleDashboardButton();
    connectWifi();
    connectMqtt();
    if (app_config::kEnableMqtt) {
        g_mqttClient.loop();
    }

    const uint32_t now = millis();

    if (now - g_lastSensorPollMs >= app_config::kSensorPollMs) {
        g_lastSensorPollMs = now;
        pollSensors();
    }

    if (now - g_lastUiRefreshMs >= app_config::kUiRefreshMs) {
        g_lastUiRefreshMs = now;
        updateUi();
    }

    if (app_config::kEnableMqtt &&
        now - g_lastMqttPublishMs >= app_config::kMqttPublishMs) {
        g_lastMqttPublishMs = now;
        publishState();
    }

    delay(50);
}
