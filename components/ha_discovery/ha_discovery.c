#include "ha_discovery.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "ha_discovery";

extern int mqtt_publish(const char *topic, const char *payload, int qos, int retain);

/* Common device block — inlined into every discovery payload */
#define DEV \
    "\"availability_topic\":\"wrg2/availability\"," \
    "\"device\":{\"identifiers\":[\"wrg2mqtt\"],\"name\":\"Meltem M-WRG-II\"," \
    "\"manufacturer\":\"Meltem\",\"model\":\"M-WRG-II\"}"

/* Helper: build and publish one discovery payload */
static void pub(const char *disc_topic, const char *payload)
{
    mqtt_publish(disc_topic, payload, 1, 1);
}

void ha_discovery_publish(void)
{
    char buf[600];

    /* ── Temperature sensors ─────────────────────────────────────────────── */

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Supply Air Temperature\","
        "\"state_topic\":\"wrg2/status/temperature_supply\","
        "\"unit_of_measurement\":\"\xc2\xb0\x43\",\"device_class\":\"temperature\","
        "\"state_class\":\"measurement\","
        "\"unique_id\":\"wrg2_temp_supply\"," DEV "}");
    pub("homeassistant/sensor/wrg2_temp_supply/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Extract Air Temperature\","
        "\"state_topic\":\"wrg2/status/temperature_extract\","
        "\"unit_of_measurement\":\"\xc2\xb0\x43\",\"device_class\":\"temperature\","
        "\"state_class\":\"measurement\","
        "\"unique_id\":\"wrg2_temp_extract\"," DEV "}");
    pub("homeassistant/sensor/wrg2_temp_extract/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Exhaust Air Temperature\","
        "\"state_topic\":\"wrg2/status/temperature_exhaust\","
        "\"unit_of_measurement\":\"\xc2\xb0\x43\",\"device_class\":\"temperature\","
        "\"state_class\":\"measurement\","
        "\"unique_id\":\"wrg2_temp_exhaust\"," DEV "}");
    pub("homeassistant/sensor/wrg2_temp_exhaust/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Outdoor Air Temperature\","
        "\"state_topic\":\"wrg2/status/temperature_outdoor\","
        "\"unit_of_measurement\":\"\xc2\xb0\x43\",\"device_class\":\"temperature\","
        "\"state_class\":\"measurement\","
        "\"unique_id\":\"wrg2_temp_outdoor\"," DEV "}");
    pub("homeassistant/sensor/wrg2_temp_outdoor/config", buf);

    /* ── Humidity sensors ────────────────────────────────────────────────── */

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Extract Air Humidity\","
        "\"state_topic\":\"wrg2/status/humidity_extract\","
        "\"unit_of_measurement\":\"%%\",\"device_class\":\"humidity\","
        "\"state_class\":\"measurement\","
        "\"unique_id\":\"wrg2_hum_extract\"," DEV "}");
    pub("homeassistant/sensor/wrg2_hum_extract/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Supply Air Humidity\","
        "\"state_topic\":\"wrg2/status/humidity_supply\","
        "\"unit_of_measurement\":\"%%\",\"device_class\":\"humidity\","
        "\"state_class\":\"measurement\","
        "\"unique_id\":\"wrg2_hum_supply\"," DEV "}");
    pub("homeassistant/sensor/wrg2_hum_supply/config", buf);

    /* ── Fan speed sensors (actual m³/h) ─────────────────────────────────── */

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Supply Fan Speed\","
        "\"state_topic\":\"wrg2/status/fan_supply_m3h\","
        "\"unit_of_measurement\":\"m\\u00b3/h\","
        "\"state_class\":\"measurement\","
        "\"unique_id\":\"wrg2_fan_supply\"," DEV "}");
    pub("homeassistant/sensor/wrg2_fan_supply/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Exhaust Fan Speed\","
        "\"state_topic\":\"wrg2/status/fan_exhaust_m3h\","
        "\"unit_of_measurement\":\"m\\u00b3/h\","
        "\"state_class\":\"measurement\","
        "\"unique_id\":\"wrg2_fan_exhaust\"," DEV "}");
    pub("homeassistant/sensor/wrg2_fan_exhaust/config", buf);

    /* ── Status binary sensors ───────────────────────────────────────────── */

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Error\","
        "\"state_topic\":\"wrg2/status/error\","
        "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
        "\"device_class\":\"problem\","
        "\"unique_id\":\"wrg2_error\"," DEV "}");
    pub("homeassistant/binary_sensor/wrg2_error/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Filter Due\","
        "\"state_topic\":\"wrg2/status/filter_due\","
        "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
        "\"device_class\":\"problem\","
        "\"unique_id\":\"wrg2_filter_due\"," DEV "}");
    pub("homeassistant/binary_sensor/wrg2_filter_due/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Frost Protection\","
        "\"state_topic\":\"wrg2/status/frost_active\","
        "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
        "\"device_class\":\"cold\","
        "\"unique_id\":\"wrg2_frost\"," DEV "}");
    pub("homeassistant/binary_sensor/wrg2_frost/config", buf);

    /* ── Operating mode select ───────────────────────────────────────────── */
    /* Options match mode_to_str() in app_main.c and control_task mapping.   */

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Operating Mode\","
        "\"state_topic\":\"wrg2/status/operating_mode\","
        "\"command_topic\":\"wrg2/control/mode/set\","
        "\"options\":[\"off\",\"humidity\",\"manual\",\"manual_unbal\"],"
        "\"unique_id\":\"wrg2_mode\"," DEV "}");
    pub("homeassistant/select/wrg2_mode/config", buf);

    /* ── Fan level numbers ───────────────────────────────────────────────── */

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Fan Level Supply\","
        "\"state_topic\":\"wrg2/status/fan_level\","
        "\"command_topic\":\"wrg2/control/fan_level/set\","
        "\"min\":0,\"max\":100,\"step\":5,"
        "\"unit_of_measurement\":\"m\\u00b3/h\","
        "\"unique_id\":\"wrg2_fan_level\"," DEV "}");
    pub("homeassistant/number/wrg2_fan_level/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Fan Level Exhaust\","
        "\"state_topic\":\"wrg2/status/fan_exhaust_level\","
        "\"command_topic\":\"wrg2/control/fan_exhaust/set\","
        "\"min\":0,\"max\":100,\"step\":5,"
        "\"unit_of_measurement\":\"m\\u00b3/h\","
        "\"unique_id\":\"wrg2_fan_exhaust_level\"," DEV "}");
    pub("homeassistant/number/wrg2_fan_exhaust_level/config", buf);

    ESP_LOGI(TAG, "15 discovery entities published");
}
