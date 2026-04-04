#include "ha_discovery.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "ha_discovery";

extern int mqtt_publish(const char *topic, const char *payload, int qos, int retain);

/* Common device block */
#define DEV \
    "\"availability_topic\":\"wrg2/availability\"," \
    "\"device\":{\"identifiers\":[\"wrg2mqtt\"],\"name\":\"Meltem M-WRG-II\"," \
    "\"manufacturer\":\"Meltem\",\"model\":\"M-WRG-II\"}"

/* entity_category shortcuts — controls where entity appears on the device page:
 *   (none)       → main card: Temperatures, Air Quality, Fan Speeds, Controls
 *   DIAG         → Diagnostic section: Status & Maintenance
 *   CFG          → Configuration section: Humidity Config, Ext Input Config
 */
#define DIAG "\"entity_category\":\"diagnostic\","
#define CFG  "\"entity_category\":\"config\","

static void pub(const char *topic, const char *payload)
{
    mqtt_publish(topic, payload, 1, 1);
}

/* Publish empty retained payload → HA removes the entity */
static void del(const char *topic)
{
    mqtt_publish(topic, "", 1, 1);
}

void ha_discovery_publish(void)
{
    char buf[700];

    /* ── Delete stale entities from previous firmware versions ──────────── */
    /* Phase 1 entities (wrong unique_id paths or replaced) */
    del("homeassistant/sensor/wrg2_supply_temp/config");
    del("homeassistant/sensor/wrg2_extract_temp/config");
    del("homeassistant/sensor/wrg2_fan_speed/config");
    del("homeassistant/binary_sensor/wrg2_bypass/config");
    del("homeassistant/fan/wrg2_fan/config");
    /* Phase 3 control entities replaced by buttons + separate numbers */
    del("homeassistant/select/wrg2_mode/config");
    del("homeassistant/number/wrg2_fan_level/config");
    del("homeassistant/number/wrg2_fan_exhaust_level/config");

    /* ════════════════════════════════════════════════════════════════════════
     * TEMPERATURES  (main card)
     * ════════════════════════════════════════════════════════════════════════ */

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

    /* ════════════════════════════════════════════════════════════════════════
     * AIR QUALITY  (main card)
     * ════════════════════════════════════════════════════════════════════════ */

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

    /* ════════════════════════════════════════════════════════════════════════
     * STATUS & MAINTENANCE  (Diagnostic section)
     * ════════════════════════════════════════════════════════════════════════ */

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Operating Mode\","
        "\"state_topic\":\"wrg2/status/operating_mode\","
        DIAG
        "\"unique_id\":\"wrg2_operating_mode\"," DEV "}");
    pub("homeassistant/sensor/wrg2_operating_mode/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Error\","
        "\"state_topic\":\"wrg2/status/error\","
        "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
        "\"device_class\":\"problem\","
        DIAG
        "\"unique_id\":\"wrg2_error\"," DEV "}");
    pub("homeassistant/binary_sensor/wrg2_error/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Filter Due\","
        "\"state_topic\":\"wrg2/status/filter_due\","
        "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
        "\"device_class\":\"problem\","
        DIAG
        "\"unique_id\":\"wrg2_filter_due\"," DEV "}");
    pub("homeassistant/binary_sensor/wrg2_filter_due/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Frost Protection\","
        "\"state_topic\":\"wrg2/status/frost_active\","
        "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
        "\"device_class\":\"cold\","
        DIAG
        "\"unique_id\":\"wrg2_frost\"," DEV "}");
    pub("homeassistant/binary_sensor/wrg2_frost/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Filter Days Remaining\","
        "\"state_topic\":\"wrg2/status/filter_days_left\","
        "\"unit_of_measurement\":\"d\","
        "\"state_class\":\"measurement\","
        DIAG
        "\"unique_id\":\"wrg2_filter_days\"," DEV "}");
    pub("homeassistant/sensor/wrg2_filter_days/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Device Operating Hours\","
        "\"state_topic\":\"wrg2/status/hours_device\","
        "\"unit_of_measurement\":\"h\","
        "\"state_class\":\"total_increasing\","
        DIAG
        "\"unique_id\":\"wrg2_hours_device\"," DEV "}");
    pub("homeassistant/sensor/wrg2_hours_device/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Motor Operating Hours\","
        "\"state_topic\":\"wrg2/status/hours_motors\","
        "\"unit_of_measurement\":\"h\","
        "\"state_class\":\"total_increasing\","
        DIAG
        "\"unique_id\":\"wrg2_hours_motors\"," DEV "}");
    pub("homeassistant/sensor/wrg2_hours_motors/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Reboot\","
        "\"command_topic\":\"wrg2/control/reboot\","
        "\"payload_press\":\"reboot\","
        "\"device_class\":\"restart\","
        DIAG
        "\"unique_id\":\"wrg2_btn_reboot\"," DEV "}");
    pub("homeassistant/button/wrg2_btn_reboot/config", buf);

    /* ════════════════════════════════════════════════════════════════════════
     * CONTROL  (main card)
     * ════════════════════════════════════════════════════════════════════════ */

    /* Off button (41120=1, 41132=0) */
    snprintf(buf, sizeof(buf),
        "{\"name\":\"Switch Off\","
        "\"command_topic\":\"wrg2/control/mode/set\","
        "\"payload_press\":\"off\","
        "\"unique_id\":\"wrg2_btn_off\"," DEV "}");
    pub("homeassistant/button/wrg2_btn_off/config", buf);

    /* Humidity button (41120=2, 41121=112, 41132=0) */
    snprintf(buf, sizeof(buf),
        "{\"name\":\"Humidity Control\","
        "\"command_topic\":\"wrg2/control/mode/set\","
        "\"payload_press\":\"humidity\","
        "\"unique_id\":\"wrg2_btn_humidity\"," DEV "}");
    pub("homeassistant/button/wrg2_btn_humidity/config", buf);

    /* Balanced fan number (41120=3, 41121=val*2, 41132=0) */
    snprintf(buf, sizeof(buf),
        "{\"name\":\"Manual Balanced Fan\","
        "\"state_topic\":\"wrg2/status/fan_supply_target\","
        "\"command_topic\":\"wrg2/control/fan_balanced/set\","
        "\"min\":0,\"max\":100,\"step\":5,"
        "\"unit_of_measurement\":\"m\\u00b3/h\","
        "\"unique_id\":\"wrg2_fan_balanced\"," DEV "}");
    pub("homeassistant/number/wrg2_fan_balanced/config", buf);

    /* Unbalanced supply (41120=4, 41121=val*2, 41132=0) */
    snprintf(buf, sizeof(buf),
        "{\"name\":\"Unbalanced Supply Fan\","
        "\"state_topic\":\"wrg2/status/fan_supply_target\","
        "\"command_topic\":\"wrg2/control/fan_unbal_supply/set\","
        "\"min\":0,\"max\":100,\"step\":5,"
        "\"unit_of_measurement\":\"m\\u00b3/h\","
        "\"unique_id\":\"wrg2_fan_unbal_supply\"," DEV "}");
    pub("homeassistant/number/wrg2_fan_unbal_supply/config", buf);

    /* Unbalanced exhaust (41120=4, 41122=val*2, 41132=0) */
    snprintf(buf, sizeof(buf),
        "{\"name\":\"Unbalanced Exhaust Fan\","
        "\"state_topic\":\"wrg2/status/fan_exhaust_target\","
        "\"command_topic\":\"wrg2/control/fan_unbal_exhaust/set\","
        "\"min\":0,\"max\":100,\"step\":5,"
        "\"unit_of_measurement\":\"m\\u00b3/h\","
        "\"unique_id\":\"wrg2_fan_unbal_exhaust\"," DEV "}");
    pub("homeassistant/number/wrg2_fan_unbal_exhaust/config", buf);

    /* ════════════════════════════════════════════════════════════════════════
     * HUMIDITY CONTROL CONFIG  (Configuration section)
     * ════════════════════════════════════════════════════════════════════════ */

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Humidity Start Setpoint\","
        "\"state_topic\":\"wrg2/config/hum_setpoint\","
        "\"command_topic\":\"wrg2/config/hum_setpoint/set\","
        "\"min\":40,\"max\":80,\"step\":1,"
        "\"unit_of_measurement\":\"%%\","
        CFG
        "\"unique_id\":\"wrg2_cfg_hum_sp\"," DEV "}");
    pub("homeassistant/number/wrg2_cfg_hum_sp/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Humidity Min Fan Level\","
        "\"state_topic\":\"wrg2/config/hum_fan_min\","
        "\"command_topic\":\"wrg2/config/hum_fan_min/set\","
        "\"min\":0,\"max\":100,\"step\":1,"
        "\"unit_of_measurement\":\"%%\","
        CFG
        "\"unique_id\":\"wrg2_cfg_hum_fan_min\"," DEV "}");
    pub("homeassistant/number/wrg2_cfg_hum_fan_min/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Humidity Max Fan Level\","
        "\"state_topic\":\"wrg2/config/hum_fan_max\","
        "\"command_topic\":\"wrg2/config/hum_fan_max/set\","
        "\"min\":0,\"max\":100,\"step\":1,"
        "\"unit_of_measurement\":\"%%\","
        CFG
        "\"unique_id\":\"wrg2_cfg_hum_fan_max\"," DEV "}");
    pub("homeassistant/number/wrg2_cfg_hum_fan_max/config", buf);

    /* ════════════════════════════════════════════════════════════════════════
     * EXTERNAL INPUT CONFIG  (Configuration section)
     * ════════════════════════════════════════════════════════════════════════ */

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Ext Input Fan Level\","
        "\"state_topic\":\"wrg2/config/ext_fan_level\","
        "\"command_topic\":\"wrg2/config/ext_fan_level/set\","
        "\"min\":0,\"max\":100,\"step\":1,"
        "\"unit_of_measurement\":\"%%\","
        CFG
        "\"unique_id\":\"wrg2_cfg_ext_fan\"," DEV "}");
    pub("homeassistant/number/wrg2_cfg_ext_fan/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Ext Input On Delay\","
        "\"state_topic\":\"wrg2/config/ext_on_delay\","
        "\"command_topic\":\"wrg2/config/ext_on_delay/set\","
        "\"min\":0,\"max\":60,\"step\":1,"
        "\"unit_of_measurement\":\"min\","
        CFG
        "\"unique_id\":\"wrg2_cfg_ext_on\"," DEV "}");
    pub("homeassistant/number/wrg2_cfg_ext_on/config", buf);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Ext Input Off Delay\","
        "\"state_topic\":\"wrg2/config/ext_off_delay\","
        "\"command_topic\":\"wrg2/config/ext_off_delay/set\","
        "\"min\":0,\"max\":120,\"step\":1,"
        "\"unit_of_measurement\":\"min\","
        CFG
        "\"unique_id\":\"wrg2_cfg_ext_off\"," DEV "}");
    pub("homeassistant/number/wrg2_cfg_ext_off/config", buf);

    ESP_LOGI(TAG, "discovery: 8 deleted, 26 published");
}
