#include "ha_discovery.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "ha_discovery";

/* Forward-declare mqtt_publish to avoid a CMake circular dependency.
   The symbol is resolved at link time from the mqtt_manager component. */
extern int mqtt_publish(const char *topic, const char *payload, int qos, int retain);

/* Common device block (inlined into each payload) */
#define DEVICE_BLOCK \
    "\"availability_topic\":\"wrg2/availability\"," \
    "\"device\":{\"identifiers\":[\"wrg2mqtt\"],\"name\":\"Meltem WRG-II\"," \
    "\"manufacturer\":\"Meltem\",\"model\":\"WRG-II\"}"

void ha_discovery_publish(void)
{
    char buf[512];
    int  rc;

    /* 1 — Supply air temperature sensor */
    snprintf(buf, sizeof(buf),
        "{"
        "\"name\":\"Supply Air Temp\","
        "\"state_topic\":\"wrg2/status/temperature_supply\","
        "\"unit_of_measurement\":\"\xc2\xb0\x43\","   /* °C in UTF-8 */
        "\"device_class\":\"temperature\","
        "\"unique_id\":\"wrg2_supply_temp\","
        DEVICE_BLOCK
        "}");
    rc = mqtt_publish("homeassistant/sensor/wrg2_supply_temp/config", buf, 1, 1);
    ESP_LOGD(TAG, "published supply_temp discovery (rc=%d)", rc);

    /* 2 — Extract air temperature sensor */
    snprintf(buf, sizeof(buf),
        "{"
        "\"name\":\"Extract Air Temp\","
        "\"state_topic\":\"wrg2/status/temperature_extract\","
        "\"unit_of_measurement\":\"\xc2\xb0\x43\","
        "\"device_class\":\"temperature\","
        "\"unique_id\":\"wrg2_extract_temp\","
        DEVICE_BLOCK
        "}");
    rc = mqtt_publish("homeassistant/sensor/wrg2_extract_temp/config", buf, 1, 1);
    ESP_LOGD(TAG, "published extract_temp discovery (rc=%d)", rc);

    /* 3 — Fan speed sensor */
    snprintf(buf, sizeof(buf),
        "{"
        "\"name\":\"Fan Speed\","
        "\"state_topic\":\"wrg2/status/fan_speed\","
        "\"unique_id\":\"wrg2_fan_speed\","
        DEVICE_BLOCK
        "}");
    rc = mqtt_publish("homeassistant/sensor/wrg2_fan_speed/config", buf, 1, 1);
    ESP_LOGD(TAG, "published fan_speed discovery (rc=%d)", rc);

    /* 4 — Bypass binary sensor */
    snprintf(buf, sizeof(buf),
        "{"
        "\"name\":\"Bypass\","
        "\"state_topic\":\"wrg2/status/bypass_state\","
        "\"payload_on\":\"ON\","
        "\"payload_off\":\"OFF\","
        "\"unique_id\":\"wrg2_bypass\","
        DEVICE_BLOCK
        "}");
    rc = mqtt_publish("homeassistant/binary_sensor/wrg2_bypass/config", buf, 1, 1);
    ESP_LOGD(TAG, "published bypass discovery (rc=%d)", rc);

    /* 5 — Fan entity */
    snprintf(buf, sizeof(buf),
        "{"
        "\"name\":\"Fan\","
        "\"state_topic\":\"wrg2/status/fan_speed\","
        "\"command_topic\":\"wrg2/control/fan_level/set\","
        "\"unique_id\":\"wrg2_fan\","
        DEVICE_BLOCK
        "}");
    rc = mqtt_publish("homeassistant/fan/wrg2_fan/config", buf, 1, 1);
    ESP_LOGD(TAG, "published fan discovery (rc=%d)", rc);

    /* 6 — Operating mode select entity */
    snprintf(buf, sizeof(buf),
        "{"
        "\"name\":\"Operating Mode\","
        "\"state_topic\":\"wrg2/status/operating_mode\","
        "\"command_topic\":\"wrg2/control/mode/set\","
        "\"options\":[\"auto\",\"manual\",\"boost\",\"away\"],"
        "\"unique_id\":\"wrg2_mode\","
        DEVICE_BLOCK
        "}");
    rc = mqtt_publish("homeassistant/select/wrg2_mode/config", buf, 1, 1);
    ESP_LOGD(TAG, "published mode discovery (rc=%d)", rc);

    ESP_LOGI(TAG, "all discovery payloads published");
}
