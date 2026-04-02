#include "system_core.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "config_server.h"
#include "mqtt_manager.h"
#include "ha_discovery.h"
#include "ota_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"

static const char *TAG = "app_main";

static void polling_task(void *arg)
{
    esp_task_wdt_add(NULL);
    while (1) {
        esp_task_wdt_reset();
        if (mqtt_manager_is_connected()) {
            mqtt_publish("wrg2/status/temperature_supply",  "21.5", 1, 1);
            mqtt_publish("wrg2/status/temperature_extract", "18.0", 1, 1);
            mqtt_publish("wrg2/status/fan_speed",           "2",    1, 1);
            mqtt_publish("wrg2/status/bypass_state",        "OFF",  1, 1);
            mqtt_publish("wrg2/status/operating_mode",      "auto", 1, 1);
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

    if (wifi_manager_is_connected()) {
        /* Only start MQTT stack when we have a network */
        if (g_config.mqtt_url[0] != '\0') {
            mqtt_manager_init();
            ha_discovery_publish();
            ota_manager_init();
            xTaskCreate(polling_task, "poll", 4096, NULL, 5, NULL);
        } else {
            ESP_LOGW(TAG, "No MQTT broker configured — skipping MQTT init");
            ESP_LOGW(TAG, "Open http://%s.local/config to set broker URL", g_config.hostname);
        }
    } else {
        ESP_LOGW(TAG, "AP provisioning mode — open http://192.168.4.1 to configure WiFi");
    }
}
