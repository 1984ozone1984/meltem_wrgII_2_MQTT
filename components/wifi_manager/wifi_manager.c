/**
 * WRG2MQTT - WiFi Manager
 *
 * STA mode with automatic AP fallback:
 *  - If no credentials are stored → AP mode immediately
 *  - If credentials stored but connection fails after timeout → AP mode
 *  - AP SSID: "WRG2-Setup"  IP: 192.168.4.1  (open network)
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "wifi_manager.h"
#include "config_manager.h"
#include "mdns.h"

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

#define STA_TIMEOUT_S       30
#define STA_MAX_RETRIES     10
#define AP_SSID             "WRG2-Setup"
#define AP_CHANNEL          6
#define AP_MAX_CONN         4
#define AP_IP               "192.168.4.1"

static EventGroupHandle_t s_evt_group  = NULL;
static esp_netif_t       *s_sta_netif  = NULL;
static esp_netif_t       *s_ap_netif   = NULL;
static bool               s_connected  = false;
static bool               s_ap_mode    = false;
static int                s_retry      = 0;
static char               s_ip[16]     = {0};

/* ── Event handler ──────────────────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA started, connecting...");
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                s_connected = false;
                s_ip[0] = '\0';
                if (s_retry < STA_MAX_RETRIES) {
                    s_retry++;
                    ESP_LOGI(TAG, "Retry %d/%d...", s_retry, STA_MAX_RETRIES);
                    esp_wifi_connect();
                } else {
                    ESP_LOGW(TAG, "Connection failed after %d retries", STA_MAX_RETRIES);
                    xEventGroupSetBits(s_evt_group, WIFI_FAIL_BIT);
                }
                break;

            case WIFI_EVENT_AP_START:
                s_ap_mode = true;
                snprintf(s_ip, sizeof(s_ip), AP_IP);
                ESP_LOGW(TAG, "AP mode started: SSID=%s  IP=%s", AP_SSID, AP_IP);
                ESP_LOGW(TAG, "Connect to '%s' and open http://%s to configure",
                         AP_SSID, AP_IP);
                break;

            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *ev = event_data;
                ESP_LOGI(TAG, "Client connected to AP, AID=%d", ev->aid);
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *ev = event_data;
                ESP_LOGI(TAG, "Client disconnected from AP, AID=%d", ev->aid);
                break;
            }

            default:
                break;
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = event_data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip);
        s_retry     = 0;
        s_connected = true;
        s_ap_mode   = false;
        xEventGroupSetBits(s_evt_group, WIFI_CONNECTED_BIT);
    }
}

/* ── Internal helpers ───────────────────────────────────────────────────────── */

static void start_mdns(void)
{
    mdns_free();   /* no-op if not running; safe to call before init */
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(g_config.hostname);
    mdns_instance_name_set("WRG2MQTT Gateway");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS started: %s.local", g_config.hostname);
}

static esp_err_t start_ap(void)
{
    wifi_config_t cfg = {
        .ap = {
            .ssid           = AP_SSID,
            .ssid_len       = sizeof(AP_SSID) - 1,
            .channel        = AP_CHANNEL,
            .authmode       = WIFI_AUTH_OPEN,
            .max_connection = AP_MAX_CONN,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Set full TX power — default "world safe" mode runs at reduced power,
     * making the AP invisible to nearby devices. 78 = 19.5 dBm (maximum). */
    esp_wifi_set_max_tx_power(78);
    start_mdns();

    return ESP_OK;
}

static esp_err_t start_sta(void)
{
    char ssid[64] = {0};
    char pass[64] = {0};

    strncpy(ssid, g_config.wifi_ssid, sizeof(ssid) - 1);
    strncpy(pass, g_config.wifi_pass, sizeof(pass) - 1);

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid,     ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    /* clear sensitive data from stack */
    memset(ssid, 0, sizeof(ssid));
    memset(pass, 0, sizeof(pass));

    esp_netif_set_hostname(s_sta_netif, g_config.hostname);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    memset(cfg.sta.password, 0, sizeof(cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to: %s", g_config.wifi_ssid);
    return ESP_OK;
}

/* ── Public API ─────────────────────────────────────────────────────────────── */

void wifi_manager_init(void)
{
    s_evt_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    /* Set country BEFORE start — false = don't wait for beacon to learn country,
     * apply AT power/channel rules immediately → full TX power on channels 1-13 */
    ESP_ERROR_CHECK(esp_wifi_set_country_code("AT", false));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    ESP_LOGI(TAG, "init done");
}

void wifi_manager_start(void)
{
    /* No SSID configured → go directly to AP mode */
    if (g_config.wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "No SSID configured, starting AP mode");
        start_ap();
        return;
    }

    /* Try STA mode with timeout */
    s_retry = 0;
    xEventGroupClearBits(s_evt_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    if (start_sta() != ESP_OK) {
        ESP_LOGW(TAG, "STA start failed, falling back to AP mode");
        start_ap();
        return;
    }

    ESP_LOGI(TAG, "Waiting for connection (timeout: %ds)...", STA_TIMEOUT_S);
    EventBits_t bits = xEventGroupWaitBits(
        s_evt_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(STA_TIMEOUT_S * 1000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected in STA mode, IP=%s", s_ip);
        start_mdns();
        return;
    }

    /* Connection timed out or failed → switch to AP mode */
    ESP_LOGW(TAG, "STA connection failed, switching to AP mode");
    ESP_ERROR_CHECK(esp_wifi_stop());
    vTaskDelay(pdMS_TO_TICKS(500));
    start_ap();
}

bool wifi_manager_is_connected(void) { return s_connected; }
bool wifi_manager_is_ap_mode(void)   { return s_ap_mode;   }

void wifi_manager_get_ip(char *buf, size_t len)
{
    if (buf && len > 0) {
        strncpy(buf, s_ip, len - 1);
        buf[len - 1] = '\0';
    }
}

void wifi_manager_get_ap_ssid(char *buf, size_t len)
{
    if (buf && len > 0) {
        strncpy(buf, AP_SSID, len - 1);
        buf[len - 1] = '\0';
    }
}
