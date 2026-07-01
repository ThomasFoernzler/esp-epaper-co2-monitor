#include <Arduino.h>
#include <PubSubClient.h>
#include <SensirionI2cScd4x.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_SHTC3.h>
#include <esp_timer.h>
#include <lvgl.h>

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
    bool boardOk = false;
    bool scdOk = false;
};

enum class DashboardPage : uint8_t {
    Sensor = 0,
    Debug = 1,
};

struct UiLabels {
    lv_obj_t* title = nullptr;
    lv_obj_t* boardTitle = nullptr;
    lv_obj_t* boardValue = nullptr;
    lv_obj_t* scdTitle = nullptr;
    lv_obj_t* scdValue = nullptr;
    lv_obj_t* wifiTitle = nullptr;
    lv_obj_t* wifiValue = nullptr;
    lv_obj_t* mqttTitle = nullptr;
    lv_obj_t* mqttValue = nullptr;
    lv_obj_t* updatedTitle = nullptr;
    lv_obj_t* updatedValue = nullptr;
};

epaper_driver_display* g_display = nullptr;
board_power_bsp_t* g_power = nullptr;
SemaphoreHandle_t g_lvglMutex = nullptr;
UiLabels g_ui;
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

bool lvglLock(int timeout_ms) {
    const TickType_t timeout_ticks =
        (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(g_lvglMutex, timeout_ticks) == pdTRUE;
}

void lvglUnlock() { xSemaphoreGive(g_lvglMutex); }

const char* currentPageTitle() {
    return g_currentPage == DashboardPage::Sensor ? "Sensor" : "Debug";
}

void updateUi() {
    if (!lvglLock(100)) {
        return;
    }

    static char row1Value[64];
    static char row2Value[64];
    static char row3Value[64];
    static char row4Value[64];
    static char row5Value[48];
    static char wifiState[64];
    static char mqttState[32];

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
        lv_label_set_text(g_ui.title, "CO2 Monitor / Sensor");
        lv_label_set_text(g_ui.boardTitle, "CO2");
        lv_label_set_text(g_ui.scdTitle, "Temp");
        lv_label_set_text(g_ui.wifiTitle, "Humidity");
        lv_label_set_text(g_ui.mqttTitle, "Status");
        lv_label_set_text(g_ui.updatedTitle, "Page");

        if (g_snapshot.scdOk) {
            snprintf(row1Value, sizeof(row1Value), "%u ppm",
                     g_snapshot.scdCo2Ppm);
            snprintf(row2Value, sizeof(row2Value), "%.1f C",
                     g_snapshot.scdTempC);
            snprintf(row3Value, sizeof(row3Value), "%.1f %%RH",
                     g_snapshot.scdHumidity);
            snprintf(row4Value, sizeof(row4Value), "measuring");
        } else {
            snprintf(row1Value, sizeof(row1Value), "unavailable");
            snprintf(row2Value, sizeof(row2Value), "--.- C");
            snprintf(row3Value, sizeof(row3Value), "--.- %%RH");
            snprintf(row4Value, sizeof(row4Value), "waiting");
        }
        snprintf(row5Value, sizeof(row5Value), "%s", currentPageTitle());
    } else {
        lv_label_set_text(g_ui.title, "CO2 Monitor / Debug");
        lv_label_set_text(g_ui.boardTitle, "Board");
        lv_label_set_text(g_ui.scdTitle, "WiFi");
        lv_label_set_text(g_ui.wifiTitle, "MQTT");
        lv_label_set_text(g_ui.mqttTitle, "SCD40");
        lv_label_set_text(g_ui.updatedTitle, "Uptime");

        if (g_snapshot.boardOk) {
            snprintf(row1Value, sizeof(row1Value), "%.1f C   %.1f %%RH",
                     g_snapshot.boardTempC, g_snapshot.boardHumidity);
        } else {
            snprintf(row1Value, sizeof(row1Value), "unavailable");
        }
        snprintf(row2Value, sizeof(row2Value), "%s", wifiState);
        snprintf(row3Value, sizeof(row3Value), "%s", mqttState);
        snprintf(row4Value, sizeof(row4Value), "%s",
                 g_snapshot.scdOk ? "online" : "unavailable");
        snprintf(row5Value, sizeof(row5Value), "%lus", millis() / 1000UL);
    }

    lv_label_set_text(g_ui.boardValue, row1Value);
    lv_label_set_text(g_ui.scdValue, row2Value);
    lv_label_set_text(g_ui.wifiValue, row3Value);
    lv_label_set_text(g_ui.mqttValue, row4Value);
    lv_label_set_text(g_ui.updatedValue, row5Value);
    lvglUnlock();

    Serial.printf("[UI] page=%s row1='%s' row2='%s' row3='%s' row4='%s' row5='%s'\n",
                  currentPageTitle(), row1Value, row2Value, row3Value,
                  row4Value, row5Value);
}

void uiCreate() {
    lv_obj_t* screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);

    g_ui.title = lv_label_create(screen);
    lv_label_set_text(g_ui.title, "CO2 Monitor");
    lv_obj_align(g_ui.title, LV_ALIGN_TOP_LEFT, 10, 10);

    g_ui.boardTitle = lv_label_create(screen);
    lv_label_set_text(g_ui.boardTitle, "Board");
    lv_obj_align(g_ui.boardTitle, LV_ALIGN_TOP_LEFT, 10, 52);

    g_ui.boardValue = lv_label_create(screen);
    lv_obj_set_width(g_ui.boardValue, 110);
    lv_label_set_long_mode(g_ui.boardValue, LV_LABEL_LONG_WRAP);
    lv_label_set_text(g_ui.boardValue, "init...");
    lv_obj_align(g_ui.boardValue, LV_ALIGN_TOP_LEFT, 82, 52);

    g_ui.scdTitle = lv_label_create(screen);
    lv_label_set_text(g_ui.scdTitle, "SCD40");
    lv_obj_align(g_ui.scdTitle, LV_ALIGN_TOP_LEFT, 10, 88);

    g_ui.scdValue = lv_label_create(screen);
    lv_obj_set_width(g_ui.scdValue, 110);
    lv_label_set_long_mode(g_ui.scdValue, LV_LABEL_LONG_WRAP);
    lv_label_set_text(g_ui.scdValue, "init...");
    lv_obj_align(g_ui.scdValue, LV_ALIGN_TOP_LEFT, 82, 88);

    g_ui.wifiTitle = lv_label_create(screen);
    lv_label_set_text(g_ui.wifiTitle, "WiFi");
    lv_obj_align(g_ui.wifiTitle, LV_ALIGN_TOP_LEFT, 10, 132);

    g_ui.wifiValue = lv_label_create(screen);
    lv_obj_set_width(g_ui.wifiValue, 110);
    lv_label_set_long_mode(g_ui.wifiValue, LV_LABEL_LONG_WRAP);
    lv_label_set_text(g_ui.wifiValue, "init...");
    lv_obj_align(g_ui.wifiValue, LV_ALIGN_TOP_LEFT, 82, 132);

    g_ui.mqttTitle = lv_label_create(screen);
    lv_label_set_text(g_ui.mqttTitle, "MQTT");
    lv_obj_align(g_ui.mqttTitle, LV_ALIGN_TOP_LEFT, 10, 156);

    g_ui.mqttValue = lv_label_create(screen);
    lv_obj_set_width(g_ui.mqttValue, 110);
    lv_label_set_long_mode(g_ui.mqttValue, LV_LABEL_LONG_WRAP);
    lv_label_set_text(g_ui.mqttValue, "init...");
    lv_obj_align(g_ui.mqttValue, LV_ALIGN_TOP_LEFT, 82, 156);

    g_ui.updatedTitle = lv_label_create(screen);
    lv_label_set_text(g_ui.updatedTitle, "Uptime");
    lv_obj_align(g_ui.updatedTitle, LV_ALIGN_TOP_LEFT, 10, 180);

    g_ui.updatedValue = lv_label_create(screen);
    lv_obj_set_width(g_ui.updatedValue, 110);
    lv_label_set_text(g_ui.updatedValue, "0s");
    lv_obj_align(g_ui.updatedValue, LV_ALIGN_TOP_LEFT, 82, 180);
}

void lvglFlushCb(lv_disp_drv_t* disp_drv, const lv_area_t* area,
                 lv_color_t* color_map) {
    uint16_t* buffer = reinterpret_cast<uint16_t*>(color_map);
    g_display->EPD_Clear();

    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            const uint8_t color =
                (*buffer < 0x7fff) ? DRIVER_COLOR_BLACK : DRIVER_COLOR_WHITE;
            g_display->EPD_DrawColorPixel(x, y, color);
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
            Serial.printf("[UI] switched to %s page\n", currentPageTitle());
            updateUi();
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
