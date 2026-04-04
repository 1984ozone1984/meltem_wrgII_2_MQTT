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

/* Control topics */
#define TOPIC_FAN_SET         "wrg2/control/fan_level/set"
#define TOPIC_FAN_EXHAUST_SET "wrg2/control/fan_exhaust/set"
#define TOPIC_MODE_SET        "wrg2/control/mode/set"
#define TOPIC_OTA             "wrg2/ota/trigger"

/* Implemented in app_main.c — post commands to the control queue */
extern void wrg2_enqueue_mode(const char *payload);
extern void wrg2_enqueue_fan(const char *payload);
extern void wrg2_enqueue_fan_exhaust(const char *payload);

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
        esp_mqtt_client_subscribe(s_client, TOPIC_FAN_SET,         1);
        esp_mqtt_client_subscribe(s_client, TOPIC_FAN_EXHAUST_SET, 1);
        esp_mqtt_client_subscribe(s_client, TOPIC_MODE_SET,        1);
        esp_mqtt_client_subscribe(s_client, TOPIC_OTA,             1);

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
            ESP_LOGI(TAG, "OTA trigger received, URL: %s", payload);
            ota_manager_handle_trigger(payload);
        } else if (strcmp(topic, TOPIC_MODE_SET) == 0) {
            ESP_LOGI(TAG, "mode set: %s", payload);
            wrg2_enqueue_mode(payload);
        } else if (strcmp(topic, TOPIC_FAN_SET) == 0) {
            ESP_LOGI(TAG, "fan supply set: %s m³/h", payload);
            wrg2_enqueue_fan(payload);
        } else if (strcmp(topic, TOPIC_FAN_EXHAUST_SET) == 0) {
            ESP_LOGI(TAG, "fan exhaust set: %s m³/h", payload);
            wrg2_enqueue_fan_exhaust(payload);
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
