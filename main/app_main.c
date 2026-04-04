#include "system_core.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "config_server.h"
#include "mqtt_manager.h"
#include "ha_discovery.h"
#include "ota_manager.h"
#include "wrg2_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include <stdio.h>

static const char *TAG = "app_main";

static const char *mode_to_str(uint8_t mode)
{
    switch (mode) {
        case 1: return "off";
        case 2: return "auto";
        case 3: /* balanced manual */
        case 4: /* unbalanced manual */
            return "manual";
        default: return "auto";
    }
}

static void polling_task(void *arg)
{
    wrg2_data_t data;
    char buf[32];

    esp_task_wdt_add(NULL);
    while (1) {
        esp_task_wdt_reset();

        if (wrg2_read_all(&data) == ESP_OK) {
            ESP_LOGI(TAG, "supply=%.1f°C extract=%.1f°C outdoor=%.1f°C exhaust=%.1f°C "
                     "fan_s=%um³/h fan_e=%um³/h hum_ext=%u%% mode=%s%s%s",
                     data.temp_supply, data.temp_extract,
                     data.temp_outdoor, data.temp_exhaust,
                     data.fan_supply_m3h, data.fan_exhaust_m3h,
                     data.humidity_extract,
                     mode_to_str(data.mode),
                     data.error_flag  ? " ERROR"      : "",
                     data.filter_due  ? " FILTER_DUE" : "");

            if (mqtt_manager_is_connected()) {
                snprintf(buf, sizeof(buf), "%.1f", data.temp_supply);
                mqtt_publish("wrg2/status/temperature_supply",  buf,  1, 1);

                snprintf(buf, sizeof(buf), "%.1f", data.temp_extract);
                mqtt_publish("wrg2/status/temperature_extract", buf,  1, 1);

                snprintf(buf, sizeof(buf), "%u", data.fan_supply_m3h);
                mqtt_publish("wrg2/status/fan_speed",           buf,  1, 1);

                /* bypass_state: no direct Modbus register on M-WRG-II.
                 * The bypass damper is controlled automatically by the unit.
                 * Report frost_active as a proxy: frost = bypass closed. */
                mqtt_publish("wrg2/status/bypass_state",
                             data.frost_active ? "OFF" : "ON", 1, 1);

                mqtt_publish("wrg2/status/operating_mode",
                             mode_to_str(data.mode), 1, 1);

                if (data.error_flag) {
                    ESP_LOGW(TAG, "Device reports error — check unit");
                }
                if (data.filter_due) {
                    ESP_LOGW(TAG, "Filter change due (%u days overdue)",
                             data.filter_days_left);
                }
            }
        } else {
            ESP_LOGW(TAG, "Modbus read failed — will retry in %lus",
                     (unsigned long)g_config.poll_interval);
        }

        vTaskDelay(pdMS_TO_TICKS(g_config.poll_interval * 1000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== WRG2MQTT starting ===");

    /* 1. NVS flash init + watchdog */
    system_core_init();

    /* 2. Load config from NVS (hostname, WiFi/MQTT credentials, Modbus defaults) */
    config_manager_init();

    /* 3. WiFi: STA with AP fallback + mDNS hostname.local */
    wifi_manager_init();
    wifi_manager_start();

    /* 4. HTTP config portal — always available in both STA and AP mode */
    config_server_start();

    /* 5. Modbus RTU driver — independent of network, init early */
    if (wrg2_driver_init(g_config.mb_slave_id, g_config.mb_baud) != ESP_OK) {
        ESP_LOGE(TAG, "Modbus RTU init failed");
        /* Non-fatal: polling task will report errors */
    }

    /* Polling task runs regardless of MQTT — logs data and publishes when connected */
    xTaskCreate(polling_task, "poll", 4096, NULL, 5, NULL);

    if (wifi_manager_is_connected()) {
        if (g_config.mqtt_url[0] != '\0') {
            mqtt_manager_init();
            ha_discovery_publish();
            ota_manager_init();
        } else {
            ESP_LOGW(TAG, "No MQTT broker configured — open http://%s.local/config", g_config.hostname);
        }
    } else {
        ESP_LOGW(TAG, "AP provisioning mode — open http://192.168.4.1 to configure WiFi");
    }
}
