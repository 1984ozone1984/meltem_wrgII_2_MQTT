#pragma once
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char     hostname[32];
    char     wifi_ssid[64];
    char     wifi_pass[64];
    char     mqtt_url[128];
    char     mqtt_user[32];
    char     mqtt_pass[32];
    uint8_t  mb_slave_id;
    uint32_t mb_baud;
    uint32_t poll_interval;  /* Modbus read interval (seconds) */
    uint32_t pub_interval;   /* MQTT publish interval (seconds) */
    uint8_t  mb_gpio_tx;     /* UART TX GPIO (default 43) */
    uint8_t  mb_gpio_rx;     /* UART RX GPIO (default 44) */
    uint8_t  mb_gpio_rts;    /* RS-485 DE/RE GPIO (default 2, unused if auto-dir module) */
} wrg2_config_t;

extern wrg2_config_t g_config;

void      config_manager_init(void);
esp_err_t config_manager_save_hostname(const char *hostname);
esp_err_t config_manager_save_wifi(const char *ssid, const char *pass);
esp_err_t config_manager_save_mqtt(const char *url, const char *user, const char *pass);
esp_err_t config_manager_save_modbus(uint8_t slave_id, uint32_t baud,
                                     uint32_t poll_interval, uint32_t pub_interval,
                                     uint8_t gpio_tx, uint8_t gpio_rx, uint8_t gpio_rts);
