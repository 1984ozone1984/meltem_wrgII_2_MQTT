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
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "app_main";

/* ── Control command queue ─────────────────────────────────────────────────
 * MQTT event handler (mqtt_manager) posts commands here.
 * control_task() processes them from the Modbus task context.
 * ----------------------------------------------------------------------- */
typedef enum {
    CMD_MODE,             /* "off" | "humidity"                  */
    CMD_FAN_BALANCED,     /* 0-100 m³/h → mode=3, 41121=val*2   */
    CMD_FAN_UNBAL_SUPPLY, /* 0-100 m³/h → mode=4, 41121=val*2   */
    CMD_FAN_UNBAL_EXHAUST,/* 0-100 m³/h → mode=4, 41122=val*2   */
    CMD_WRITE_REG         /* generic single-register write (cfg) */
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    char       payload[32];
    uint16_t   reg_addr;  /* used by CMD_WRITE_REG */
} wrg2_cmd_t;

static QueueHandle_t s_cmd_queue;

/* Called by mqtt_manager and config_server via extern declarations */
void wrg2_enqueue_mode(const char *payload)
{
    wrg2_cmd_t cmd = { .type = CMD_MODE };
    strncpy(cmd.payload, payload, sizeof(cmd.payload) - 1);
    xQueueSend(s_cmd_queue, &cmd, 0);
}

void wrg2_enqueue_fan_balanced(const char *payload)
{
    wrg2_cmd_t cmd = { .type = CMD_FAN_BALANCED };
    strncpy(cmd.payload, payload, sizeof(cmd.payload) - 1);
    xQueueSend(s_cmd_queue, &cmd, 0);
}

void wrg2_enqueue_fan_unbal_supply(const char *payload)
{
    wrg2_cmd_t cmd = { .type = CMD_FAN_UNBAL_SUPPLY };
    strncpy(cmd.payload, payload, sizeof(cmd.payload) - 1);
    xQueueSend(s_cmd_queue, &cmd, 0);
}

void wrg2_enqueue_fan_unbal_exhaust(const char *payload)
{
    wrg2_cmd_t cmd = { .type = CMD_FAN_UNBAL_EXHAUST };
    strncpy(cmd.payload, payload, sizeof(cmd.payload) - 1);
    xQueueSend(s_cmd_queue, &cmd, 0);
}

void wrg2_enqueue_write_reg(uint16_t addr, const char *payload)
{
    wrg2_cmd_t cmd = { .type = CMD_WRITE_REG, .reg_addr = addr };
    strncpy(cmd.payload, payload, sizeof(cmd.payload) - 1);
    xQueueSend(s_cmd_queue, &cmd, 0);
}

/* ── Mode helpers ──────────────────────────────────────────────────────────
 * Strings must match the HA select options in ha_discovery.
 * ----------------------------------------------------------------------- */
static const char *mode_to_str(uint8_t mode, uint8_t fan_target)
{
    if (mode == 1) return "off";
    if (mode == 3) return "manual";
    if (mode == 4) return "manual_unbal";
    /* mode=2 (regulated) and mode=0 (default/unset) → humidity for -F variant */
    (void)fan_target;
    return "humidity";
}

/* ── MQTT status publishing ────────────────────────────────────────────────
 * Publishes all sensor values. Called from polling_task after a successful read.
 * ----------------------------------------------------------------------- */
static void publish_status(const wrg2_data_t *d)
{
    char buf[32];

#define PUB(topic, fmt, ...) \
    do { snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__); \
         mqtt_publish(topic, buf, 1, 1); } while (0)

    PUB("wrg2/status/temperature_supply",  "%.1f", d->temp_supply);
    PUB("wrg2/status/temperature_extract", "%.1f", d->temp_extract);
    PUB("wrg2/status/temperature_exhaust", "%.1f", d->temp_exhaust);
    PUB("wrg2/status/temperature_outdoor", "%.1f", d->temp_outdoor);
    PUB("wrg2/status/humidity_extract",    "%u",   d->humidity_extract);
    PUB("wrg2/status/humidity_supply",     "%u",   d->humidity_supply);
    PUB("wrg2/status/fan_supply_m3h",      "%u",   d->fan_supply_m3h);
    PUB("wrg2/status/fan_exhaust_m3h",     "%u",   d->fan_exhaust_m3h);
    PUB("wrg2/status/fan_supply_target",   "%u",   d->fan_target_supply  / 2);
    PUB("wrg2/status/fan_exhaust_target",  "%u",   d->fan_target_exhaust / 2);
    PUB("wrg2/status/frost_active",        "%s",   d->frost_active ? "ON" : "OFF");
    PUB("wrg2/status/error",               "%s",   d->error_flag   ? "ON" : "OFF");
    PUB("wrg2/status/filter_due",          "%s",   d->filter_due   ? "ON" : "OFF");
    PUB("wrg2/status/filter_days_left",    "%u",   d->filter_days_left);
    PUB("wrg2/status/hours_device",        "%lu",  (unsigned long)d->hours_device);
    PUB("wrg2/status/hours_motors",        "%lu",  (unsigned long)d->hours_motors);

    /* CO2: skip publishing if sensor not installed (0x7FFF = no sensor) */
    if (d->co2_extract != 0x7FFF) {
        PUB("wrg2/status/co2_extract", "%u", d->co2_extract);
    }

    mqtt_publish("wrg2/status/operating_mode",
                 mode_to_str(d->mode, d->fan_target_supply), 1, 1);

    /* Config registers — published so HA number entities show current values */
    PUB("wrg2/config/hum_setpoint",  "%u", d->cfg_hum_setpoint);
    PUB("wrg2/config/hum_fan_min",   "%u", d->cfg_hum_fan_min);
    PUB("wrg2/config/hum_fan_max",   "%u", d->cfg_hum_fan_max);
    PUB("wrg2/config/ext_fan_level", "%u", d->cfg_ext_fan_level);
    PUB("wrg2/config/ext_on_delay",  "%u", d->cfg_ext_on_delay);
    PUB("wrg2/config/ext_off_delay", "%u", d->cfg_ext_off_delay);

#undef PUB
}

/* ── Polling task ──────────────────────────────────────────────────────── */

static void polling_task(void *arg)
{
    wrg2_data_t data;

    esp_task_wdt_add(NULL);
    while (1) {
        esp_task_wdt_reset();

        if (wrg2_read_all(&data) == ESP_OK) {
            ESP_LOGI(TAG, "[TEMP]   supply=%.1fC  extract=%.1fC  exhaust=%.1fC  outdoor=%.1fC",
                     data.temp_supply, data.temp_extract,
                     data.temp_exhaust, data.temp_outdoor);
            ESP_LOGI(TAG, "[FAN]    supply=%um3h  exhaust=%um3h  target=%u/%u(0-200)  mode=%s(%u)",
                     data.fan_supply_m3h, data.fan_exhaust_m3h,
                     data.fan_target_supply, data.fan_target_exhaust,
                     mode_to_str(data.mode, data.fan_target_supply), data.mode);
            if (data.co2_extract == 0x7FFF) {
                ESP_LOGI(TAG, "[AIR]    hum_extract=%u%%  hum_supply=%u%%  co2=N/A (no sensor)",
                         data.humidity_extract, data.humidity_supply);
            } else {
                ESP_LOGI(TAG, "[AIR]    hum_extract=%u%%  hum_supply=%u%%  co2=%uppm",
                         data.humidity_extract, data.humidity_supply, data.co2_extract);
            }
            ESP_LOGI(TAG, "[STATUS] error=%u  filter_due=%u  frost=%u  filter_days=%u  dev_h=%lu  mot_h=%lu",
                     data.error_flag, data.filter_due, data.frost_active,
                     data.filter_days_left,
                     (unsigned long)data.hours_device, (unsigned long)data.hours_motors);
            ESP_LOGI(TAG, "[CFG]    hum_sp=%u%%  fan=%u-%u%%  co2_sp=%uppm  fan=%u-%u%%  ext=%u%%  on=%umin  off=%umin",
                     data.cfg_hum_setpoint, data.cfg_hum_fan_min, data.cfg_hum_fan_max,
                     data.cfg_co2_setpoint, data.cfg_co2_fan_min, data.cfg_co2_fan_max,
                     data.cfg_ext_fan_level, data.cfg_ext_on_delay, data.cfg_ext_off_delay);

            if (mqtt_manager_is_connected()) {
                publish_status(&data);
                if (data.error_flag) ESP_LOGW(TAG, "Device reports error — check unit");
                if (data.filter_due) ESP_LOGW(TAG, "Filter change due");
            }
        } else {
            ESP_LOGW(TAG, "Modbus read failed — will retry in %lus",
                     (unsigned long)g_config.poll_interval);
        }

        vTaskDelay(pdMS_TO_TICKS(g_config.poll_interval * 1000));
    }
}

/* ── Control task ──────────────────────────────────────────────────────── */

static void control_task(void *arg)
{
    wrg2_cmd_t cmd;
    while (1) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;

        esp_err_t err = ESP_FAIL;

        if (cmd.type == CMD_MODE) {
            ESP_LOGI(TAG, "control: mode → %s", cmd.payload);
            if (strcmp(cmd.payload, "off") == 0) {
                err = wrg2_set_mode(1, 0);
            } else if (strcmp(cmd.payload, "humidity") == 0) {
                err = wrg2_set_mode(2, 112);
            } else {
                ESP_LOGW(TAG, "control: unknown mode '%s'", cmd.payload);
                continue;
            }
        } else if (cmd.type == CMD_FAN_BALANCED) {
            int m3h = atoi(cmd.payload);
            if (m3h < 0) m3h = 0;
            if (m3h > 100) m3h = 100;
            ESP_LOGI(TAG, "control: balanced fan → %d m³/h (mode=3, 41121=%d)", m3h, m3h*2);
            err = wrg2_set_fan_level((uint8_t)m3h);
        } else if (cmd.type == CMD_FAN_UNBAL_SUPPLY) {
            int m3h = atoi(cmd.payload);
            if (m3h < 0) m3h = 0;
            if (m3h > 100) m3h = 100;
            wrg2_data_t cur;
            uint8_t exh = (wrg2_get_last_data(&cur) && cur.fan_target_exhaust)
                          ? cur.fan_target_exhaust / 2 : 50;
            ESP_LOGI(TAG, "control: unbal supply → %d m³/h, exhaust stays %d m³/h", m3h, exh);
            err = wrg2_set_mode_unbalanced((uint8_t)m3h, exh);
        } else if (cmd.type == CMD_FAN_UNBAL_EXHAUST) {
            int m3h = atoi(cmd.payload);
            if (m3h < 0) m3h = 0;
            if (m3h > 100) m3h = 100;
            wrg2_data_t cur;
            uint8_t sup = (wrg2_get_last_data(&cur) && cur.fan_target_supply)
                          ? cur.fan_target_supply / 2 : 50;
            ESP_LOGI(TAG, "control: unbal exhaust → %d m³/h, supply stays %d m³/h", m3h, sup);
            err = wrg2_set_mode_unbalanced(sup, (uint8_t)m3h);
        } else if (cmd.type == CMD_WRITE_REG) {
            int val = atoi(cmd.payload);
            if (val < 0) val = 0;
            if (val > 65535) val = 65535;
            ESP_LOGI(TAG, "control: write reg %u = %d", cmd.reg_addr, val);
            err = wrg2_write_config(cmd.reg_addr, (uint16_t)val);
        }

        if (err == ESP_OK) {
            /* Short delay then read back to confirm and publish updated state */
            vTaskDelay(pdMS_TO_TICKS(500));
            wrg2_data_t data;
            if (wrg2_read_all(&data) == ESP_OK && mqtt_manager_is_connected()) {
                publish_status(&data);
            }
        } else {
            ESP_LOGE(TAG, "control: command failed");
        }
    }
}

/* ── app_main ───────────────────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "=== WRG2MQTT starting ===");

    system_core_init();
    config_manager_init();
    wifi_manager_init();
    wifi_manager_start();
    config_server_start();

    if (wrg2_driver_init(g_config.mb_slave_id, g_config.mb_baud) != ESP_OK) {
        ESP_LOGE(TAG, "Modbus RTU init failed");
    }

    s_cmd_queue = xQueueCreate(8, sizeof(wrg2_cmd_t));

    xTaskCreate(polling_task, "poll",    4096, NULL, 5, NULL);
    xTaskCreate(control_task, "control", 3072, NULL, 6, NULL);

    if (wifi_manager_is_connected()) {
        if (g_config.mqtt_url[0] != '\0') {
            mqtt_manager_init();
            ha_discovery_publish();
            ota_manager_init();
        } else {
            ESP_LOGW(TAG, "No MQTT broker configured — open http://%s.local/config",
                     g_config.hostname);
        }
    } else {
        ESP_LOGW(TAG, "AP provisioning mode — open http://192.168.4.1 to configure WiFi");
    }
}
