#include "mqtt_manager.h"
#include "ha_discovery.h"
#include "ota_manager.h"
#include "config_manager.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include <string.h>
#include <stdbool.h>

static const char *TAG = "mqtt_manager";

static esp_mqtt_client_handle_t s_client    = NULL;
static volatile bool            s_connected = false;

/* Control topics — one per action, matching the web control page */
#define TOPIC_MODE_SET            "wrg2/control/mode/set"          /* "off"|"humidity" */
#define TOPIC_FAN_BALANCED_SET    "wrg2/control/fan_balanced/set"  /* 0-100 m³/h → mode=3 */
#define TOPIC_FAN_UNBAL_SUPPLY    "wrg2/control/fan_unbal_supply/set"  /* 0-100 → mode=4 */
#define TOPIC_FAN_UNBAL_EXHAUST   "wrg2/control/fan_unbal_exhaust/set" /* 0-100 → mode=4 */
#define TOPIC_OTA                 "wrg2/ota/trigger"

/* Config write topics */
#define TOPIC_CFG_HUM_SETPOINT    "wrg2/config/hum_setpoint/set"   /* 42000 */
#define TOPIC_CFG_HUM_FAN_MIN     "wrg2/config/hum_fan_min/set"    /* 42001 */
#define TOPIC_CFG_HUM_FAN_MAX     "wrg2/config/hum_fan_max/set"    /* 42002 */
#define TOPIC_CFG_EXT_FAN_LEVEL   "wrg2/config/ext_fan_level/set"  /* 42007 */
#define TOPIC_CFG_EXT_ON_DELAY    "wrg2/config/ext_on_delay/set"   /* 42008 */
#define TOPIC_CFG_EXT_OFF_DELAY   "wrg2/config/ext_off_delay/set"  /* 42009 */

/* Implemented in app_main.c — post commands to the control queue */
extern void wrg2_enqueue_mode(const char *payload);
extern void wrg2_enqueue_fan_balanced(const char *payload);
extern void wrg2_enqueue_fan_unbal_supply(const char *payload);
extern void wrg2_enqueue_fan_unbal_exhaust(const char *payload);
extern void wrg2_enqueue_write_reg(uint16_t addr, const char *payload);

/* Availability topic */
#define TOPIC_AVAIL      "wrg2/availability"

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to broker");
        s_connected = true;

        /* Publish availability = online */
        esp_mqtt_client_publish(s_client, TOPIC_AVAIL, "online", 6, 1, 1);

        /* Subscribe to control topics */
        esp_mqtt_client_subscribe(s_client, TOPIC_MODE_SET,          1);
        esp_mqtt_client_subscribe(s_client, TOPIC_FAN_BALANCED_SET,  1);
        esp_mqtt_client_subscribe(s_client, TOPIC_FAN_UNBAL_SUPPLY,  1);
        esp_mqtt_client_subscribe(s_client, TOPIC_FAN_UNBAL_EXHAUST, 1);
        esp_mqtt_client_subscribe(s_client, TOPIC_OTA,               1);
        esp_mqtt_client_subscribe(s_client, TOPIC_CFG_HUM_SETPOINT,  1);
        esp_mqtt_client_subscribe(s_client, TOPIC_CFG_HUM_FAN_MIN,   1);
        esp_mqtt_client_subscribe(s_client, TOPIC_CFG_HUM_FAN_MAX,   1);
        esp_mqtt_client_subscribe(s_client, TOPIC_CFG_EXT_FAN_LEVEL, 1);
        esp_mqtt_client_subscribe(s_client, TOPIC_CFG_EXT_ON_DELAY,  1);
        esp_mqtt_client_subscribe(s_client, TOPIC_CFG_EXT_OFF_DELAY, 1);

        /* Re-publish HA discovery on every (re)connect */
        ha_discovery_publish();
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected from broker");
        s_connected = false;
        break;

    case MQTT_EVENT_DATA: {
        /* Make null-terminated copies for safe logging and comparison */
        char topic[128]   = {0};
        char payload[256] = {0};

        int topic_len   = event->topic_len   < (int)(sizeof(topic)   - 1)
                          ? event->topic_len   : (int)(sizeof(topic)   - 1);
        int payload_len = event->data_len    < (int)(sizeof(payload) - 1)
                          ? event->data_len    : (int)(sizeof(payload) - 1);

        memcpy(topic,   event->topic, topic_len);
        memcpy(payload, event->data,  payload_len);

        ESP_LOGD(TAG, "DATA topic=%s payload=%s", topic, payload);

        if (strcmp(topic, TOPIC_OTA) == 0) {
            ESP_LOGI(TAG, "OTA trigger: %s", payload);
            ota_manager_handle_trigger(payload);
        } else if (strcmp(topic, TOPIC_MODE_SET) == 0) {
            ESP_LOGI(TAG, "mode: %s", payload);
            wrg2_enqueue_mode(payload);
        } else if (strcmp(topic, TOPIC_FAN_BALANCED_SET) == 0) {
            ESP_LOGI(TAG, "fan balanced: %s m³/h", payload);
            wrg2_enqueue_fan_balanced(payload);
        } else if (strcmp(topic, TOPIC_FAN_UNBAL_SUPPLY) == 0) {
            ESP_LOGI(TAG, "fan unbal supply: %s m³/h", payload);
            wrg2_enqueue_fan_unbal_supply(payload);
        } else if (strcmp(topic, TOPIC_FAN_UNBAL_EXHAUST) == 0) {
            ESP_LOGI(TAG, "fan unbal exhaust: %s m³/h", payload);
            wrg2_enqueue_fan_unbal_exhaust(payload);
        } else if (strcmp(topic, TOPIC_CFG_HUM_SETPOINT) == 0) {
            ESP_LOGI(TAG, "cfg hum setpoint: %s%%", payload);
            wrg2_enqueue_write_reg(42000, payload);
        } else if (strcmp(topic, TOPIC_CFG_HUM_FAN_MIN) == 0) {
            ESP_LOGI(TAG, "cfg hum fan min: %s%%", payload);
            wrg2_enqueue_write_reg(42001, payload);
        } else if (strcmp(topic, TOPIC_CFG_HUM_FAN_MAX) == 0) {
            ESP_LOGI(TAG, "cfg hum fan max: %s%%", payload);
            wrg2_enqueue_write_reg(42002, payload);
        } else if (strcmp(topic, TOPIC_CFG_EXT_FAN_LEVEL) == 0) {
            ESP_LOGI(TAG, "cfg ext fan level: %s%%", payload);
            wrg2_enqueue_write_reg(42007, payload);
        } else if (strcmp(topic, TOPIC_CFG_EXT_ON_DELAY) == 0) {
            ESP_LOGI(TAG, "cfg ext on delay: %s min", payload);
            wrg2_enqueue_write_reg(42008, payload);
        } else if (strcmp(topic, TOPIC_CFG_EXT_OFF_DELAY) == 0) {
            ESP_LOGI(TAG, "cfg ext off delay: %s min", payload);
            wrg2_enqueue_write_reg(42009, payload);
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "  reported from esp-tls: 0x%x",
                     event->error_handle->esp_tls_last_esp_err);
            ESP_LOGE(TAG, "  reported from tls stack: 0x%x",
                     event->error_handle->esp_tls_stack_err);
            ESP_LOGE(TAG, "  captured as transport's socket errno: %d",
                     event->error_handle->esp_transport_sock_errno);
        }
        break;

    default:
        ESP_LOGD(TAG, "unhandled event id=%d", (int)event_id);
        break;
    }
}

void mqtt_manager_init(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri                      = g_config.mqtt_url,
        .credentials.username                    = g_config.mqtt_user,
        .credentials.authentication.password     = g_config.mqtt_pass,
        .session.last_will.topic                 = TOPIC_AVAIL,
        .session.last_will.msg                   = "offline",
        .session.last_will.qos                   = 1,
        .session.last_will.retain                = 1,
        .network.reconnect_timeout_ms            = 5000,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));

    ESP_LOGI(TAG, "init done, connecting to %s", g_config.mqtt_url);
}

int mqtt_publish(const char *topic, const char *payload, int qos, int retain)
{
    if (!s_client) {
        ESP_LOGW(TAG, "publish called before client init");
        return -1;
    }
    return esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain);
}

bool mqtt_manager_is_connected(void)
{
    return s_connected;
}
