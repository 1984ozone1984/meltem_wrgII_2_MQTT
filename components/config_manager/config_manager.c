#include "config_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "config_manager";

#define NVS_NAMESPACE "wrg2_cfg"

wrg2_config_t g_config = {0};

/**
 * Load a string key from NVS into dest (max_len includes null terminator).
 * Returns true if the key was found and loaded.
 */
static bool load_str(nvs_handle_t nvs, const char *key, char *dest, size_t max_len)
{
    size_t required = max_len;
    esp_err_t err = nvs_get_str(nvs, key, dest, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    ESP_ERROR_CHECK(err);
    return true;
}

void config_manager_init(void)
{
    bool gpio_tx_set = false, gpio_rx_set = false, gpio_rts_set = false;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "NVS namespace '%s' not found, using defaults", NVS_NAMESPACE);
        /* Apply defaults below without opening handle */
        goto apply_defaults;
    }
    ESP_ERROR_CHECK(err);

    /* --- String fields --- */
    if (load_str(nvs, "hostname", g_config.hostname, sizeof(g_config.hostname))) {
        ESP_LOGD(TAG, "hostname = %s", g_config.hostname);
    }

    if (load_str(nvs, "wifi_ssid", g_config.wifi_ssid, sizeof(g_config.wifi_ssid))) {
        ESP_LOGD(TAG, "wifi_ssid = %s", g_config.wifi_ssid);
    }

    if (load_str(nvs, "wifi_pass", g_config.wifi_pass, sizeof(g_config.wifi_pass))) {
        ESP_LOGD(TAG, "wifi_pass = ***");
    }

    if (load_str(nvs, "mqtt_url", g_config.mqtt_url, sizeof(g_config.mqtt_url))) {
        ESP_LOGD(TAG, "mqtt_url = %s", g_config.mqtt_url);
    }

    if (load_str(nvs, "mqtt_user", g_config.mqtt_user, sizeof(g_config.mqtt_user))) {
        ESP_LOGD(TAG, "mqtt_user = %s", g_config.mqtt_user);
    }

    if (load_str(nvs, "mqtt_pass", g_config.mqtt_pass, sizeof(g_config.mqtt_pass))) {
        ESP_LOGD(TAG, "mqtt_pass = ***");
    }

    /* --- Numeric fields --- */
    {
        uint8_t val;
        err = nvs_get_u8(nvs, "mb_slave_id", &val);
        if (err == ESP_OK) {
            g_config.mb_slave_id = val;
            ESP_LOGD(TAG, "mb_slave_id = %u", g_config.mb_slave_id);
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_ERROR_CHECK(err);
        }
    }

    {
        uint32_t val;
        err = nvs_get_u32(nvs, "mb_baud", &val);
        if (err == ESP_OK) {
            g_config.mb_baud = val;
            ESP_LOGD(TAG, "mb_baud = %lu", (unsigned long)g_config.mb_baud);
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_ERROR_CHECK(err);
        }
    }

    {
        uint32_t val;
        err = nvs_get_u32(nvs, "poll_ivl", &val);
        if (err == ESP_OK) {
            g_config.poll_interval = val;
            ESP_LOGD(TAG, "poll_interval = %lu", (unsigned long)g_config.poll_interval);
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_ERROR_CHECK(err);
        }
    }

    {
        uint32_t val;
        err = nvs_get_u32(nvs, "pub_ivl", &val);
        if (err == ESP_OK) {
            g_config.pub_interval = val;
            ESP_LOGD(TAG, "pub_interval = %lu", (unsigned long)g_config.pub_interval);
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_ERROR_CHECK(err);
        }
    }

    {
        uint8_t val;
        err = nvs_get_u8(nvs, "mb_gpio_tx", &val);
        if (err == ESP_OK) {
            g_config.mb_gpio_tx = val; gpio_tx_set = true;
            ESP_LOGD(TAG, "mb_gpio_tx = %u", g_config.mb_gpio_tx);
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_ERROR_CHECK(err);
        }
    }

    {
        uint8_t val;
        err = nvs_get_u8(nvs, "mb_gpio_rx", &val);
        if (err == ESP_OK) {
            g_config.mb_gpio_rx = val; gpio_rx_set = true;
            ESP_LOGD(TAG, "mb_gpio_rx = %u", g_config.mb_gpio_rx);
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_ERROR_CHECK(err);
        }
    }

    {
        uint8_t val;
        err = nvs_get_u8(nvs, "mb_gpio_rts", &val);
        if (err == ESP_OK) {
            g_config.mb_gpio_rts = val; gpio_rts_set = true;
            ESP_LOGD(TAG, "mb_gpio_rts = %u", g_config.mb_gpio_rts);
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_ERROR_CHECK(err);
        }
    }

    nvs_close(nvs);

apply_defaults:
    /* Apply defaults for any fields that were not found in NVS */
    if (g_config.hostname[0] == '\0') {
        strncpy(g_config.hostname, "wrg2mqtt", sizeof(g_config.hostname) - 1);
    }
    if (g_config.mb_slave_id == 0) {
        g_config.mb_slave_id = 1;
        ESP_LOGD(TAG, "mb_slave_id defaulting to %u", g_config.mb_slave_id);
    }
    if (g_config.mb_baud == 0) {
        g_config.mb_baud = 19200;  /* M-WRG-II factory default (datasheet §16.1) */
        ESP_LOGD(TAG, "mb_baud defaulting to %lu", (unsigned long)g_config.mb_baud);
    }
    if (g_config.poll_interval == 0) {
        g_config.poll_interval = 10;
        ESP_LOGD(TAG, "poll_interval defaulting to %lu", (unsigned long)g_config.poll_interval);
    }
    if (g_config.pub_interval == 0) {
        g_config.pub_interval = 30;
        ESP_LOGD(TAG, "pub_interval defaulting to %lu", (unsigned long)g_config.pub_interval);
    }
    if (!gpio_tx_set) {
        g_config.mb_gpio_tx = 43;
        ESP_LOGD(TAG, "mb_gpio_tx defaulting to %u", g_config.mb_gpio_tx);
    }
    if (!gpio_rx_set) {
        g_config.mb_gpio_rx = 44;
        ESP_LOGD(TAG, "mb_gpio_rx defaulting to %u", g_config.mb_gpio_rx);
    }
    if (!gpio_rts_set) {
        g_config.mb_gpio_rts = 2;
        ESP_LOGD(TAG, "mb_gpio_rts defaulting to %u", g_config.mb_gpio_rts);
    }

    ESP_LOGI(TAG, "init done");
}

esp_err_t config_manager_save_hostname(const char *hostname)
{
    if (!hostname || hostname[0] == '\0') return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    nvs_set_str(nvs, "hostname", hostname);
    err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err == ESP_OK) {
        strncpy(g_config.hostname, hostname, sizeof(g_config.hostname) - 1);
        ESP_LOGI(TAG, "hostname saved: %s", g_config.hostname);
    }
    return err;
}

esp_err_t config_manager_save_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    nvs_set_str(nvs, "wifi_ssid", ssid ? ssid : "");
    nvs_set_str(nvs, "wifi_pass", pass ? pass : "");
    err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err == ESP_OK) {
        strncpy(g_config.wifi_ssid, ssid ? ssid : "", sizeof(g_config.wifi_ssid) - 1);
        strncpy(g_config.wifi_pass, pass ? pass : "", sizeof(g_config.wifi_pass) - 1);
        ESP_LOGI(TAG, "WiFi credentials saved");
    }
    return err;
}

esp_err_t config_manager_save_modbus(uint8_t slave_id, uint32_t baud,
                                     uint32_t poll_interval, uint32_t pub_interval,
                                     uint8_t gpio_tx, uint8_t gpio_rx, uint8_t gpio_rts)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    nvs_set_u8 (nvs, "mb_slave_id",  slave_id);
    nvs_set_u32(nvs, "mb_baud",      baud);
    nvs_set_u32(nvs, "poll_ivl",     poll_interval);
    nvs_set_u32(nvs, "pub_ivl",      pub_interval);
    nvs_set_u8 (nvs, "mb_gpio_tx",   gpio_tx);
    nvs_set_u8 (nvs, "mb_gpio_rx",   gpio_rx);
    nvs_set_u8 (nvs, "mb_gpio_rts",  gpio_rts);
    err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err == ESP_OK) {
        g_config.mb_slave_id   = slave_id;
        g_config.mb_baud       = baud;
        g_config.poll_interval = poll_interval;
        g_config.pub_interval  = pub_interval;
        g_config.mb_gpio_tx    = gpio_tx;
        g_config.mb_gpio_rx    = gpio_rx;
        g_config.mb_gpio_rts   = gpio_rts;
        ESP_LOGI(TAG, "modbus config saved: slave=%u baud=%lu poll=%lus pub=%lus tx=%u rx=%u rts=%u",
                 slave_id, (unsigned long)baud,
                 (unsigned long)poll_interval, (unsigned long)pub_interval,
                 gpio_tx, gpio_rx, gpio_rts);
    }
    return err;
}

esp_err_t config_manager_save_mqtt(const char *url, const char *user, const char *pass)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    nvs_set_str(nvs, "mqtt_url",  url  ? url  : "");
    nvs_set_str(nvs, "mqtt_user", user ? user : "");
    nvs_set_str(nvs, "mqtt_pass", pass ? pass : "");
    err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err == ESP_OK) {
        strncpy(g_config.mqtt_url,  url  ? url  : "", sizeof(g_config.mqtt_url)  - 1);
        strncpy(g_config.mqtt_user, user ? user : "", sizeof(g_config.mqtt_user) - 1);
        strncpy(g_config.mqtt_pass, pass ? pass : "", sizeof(g_config.mqtt_pass) - 1);
        ESP_LOGI(TAG, "MQTT credentials saved");
    }
    return err;
}
