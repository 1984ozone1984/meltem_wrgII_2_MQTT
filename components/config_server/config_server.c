#include "config_server.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "config_server";

/* ── HTML helpers ─────────────────────────────────────────────────────────── */

static const char CSS[] =
    "body{font-family:sans-serif;margin:0;padding:16px;background:#f4f4f4;}"
    ".box{background:#fff;border-radius:8px;padding:20px;margin:14px 0;"
         "box-shadow:0 2px 6px rgba(0,0,0,.12);}"
    "h1{margin:0 0 4px;font-size:1.4em;color:#222;}"
    "h2{font-size:1.1em;color:#444;margin:0 0 12px;}"
    "label{display:block;font-weight:bold;margin:10px 0 3px;color:#333;}"
    "input{width:100%;padding:8px;border:1px solid #ccc;border-radius:4px;"
          "box-sizing:border-box;font-size:1em;}"
    "button{background:#1a73e8;color:#fff;padding:10px 20px;border:none;"
            "border-radius:4px;cursor:pointer;font-size:1em;margin-top:14px;}"
    "button:hover{background:#1558b0;}"
    "button.r{background:#d93025;} button.r:hover{background:#b52a1e;}"
    "nav a{display:inline-block;margin:0 8px 12px 0;padding:8px 14px;"
          "background:#1a73e8;color:#fff;text-decoration:none;border-radius:4px;}"
    "nav a:hover{background:#1558b0;}"
    ".ok{color:#188038;} .err{color:#d93025;}"
    "table{width:100%;border-collapse:collapse;}"
    "td,th{padding:6px 8px;text-align:left;border-bottom:1px solid #eee;}"
    "th{background:#f8f8f8;}";

/* ── URL-decode ───────────────────────────────────────────────────────────── */

static void url_decode(char *dst, const char *src, size_t len)
{
    size_t i = 0;
    while (*src && i < len - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char a = src[1], b = src[2];
            a = (a >= 'A') ? ((a & 0xDF) - 'A' + 10) : (a - '0');
            b = (b >= 'A') ? ((b & 0xDF) - 'A' + 10) : (b - '0');
            dst[i++] = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' '; src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static void get_field(const char *body, const char *key, char *out, size_t out_len)
{
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(body, search);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(search);
    const char *end = strchr(p, '&');
    size_t vlen = end ? (size_t)(end - p) : strlen(p);
    if (vlen >= out_len) vlen = out_len - 1;
    char tmp[256] = {0};
    if (vlen < sizeof(tmp)) { memcpy(tmp, p, vlen); tmp[vlen] = '\0'; }
    url_decode(out, tmp, out_len);
}

/* Read full POST body (up to max_len) */
static int read_body(httpd_req_t *req, char *buf, size_t max_len)
{
    int total = 0, remaining = (int)req->content_len;
    if (remaining <= 0 || (size_t)remaining >= max_len) remaining = (int)max_len - 1;
    while (total < remaining) {
        int r = httpd_req_recv(req, buf + total, remaining - total);
        if (r <= 0) break;
        total += r;
    }
    buf[total] = '\0';
    return total;
}

/* ── Reboot task ──────────────────────────────────────────────────────────── */

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

/* ── GET / ────────────────────────────────────────────────────────────────── */

static esp_err_t root_get(httpd_req_t *req)
{
    char ip[16];
    wifi_manager_get_ip(ip, sizeof(ip));
    bool ap = wifi_manager_is_ap_mode();

    char *buf = malloc(4096);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    snprintf(buf, 4096,
        "<!DOCTYPE html><html><head>"
        "<meta charset=UTF-8><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>WRG2MQTT</title><style>%s</style></head><body>"
        "<h1>WRG2MQTT</h1>"
        "<nav><a href='/'>Status</a><a href='/config'>Settings</a></nav>"
        "<div class=box><h2>Device Status</h2>"
        "<table>"
        "<tr><th>Mode</th><td>%s</td></tr>"
        "<tr><th>IP</th><td>%s</td></tr>"
        "<tr><th>WiFi SSID</th><td>%s</td></tr>"
        "<tr><th>MQTT Broker</th><td>%s</td></tr>"
        "</table></div>"
        "%s"
        "</body></html>",
        CSS,
        ap ? "Access Point (provisioning)" : "Station (connected)",
        ip,
        g_config.wifi_ssid[0] ? g_config.wifi_ssid : "(not set)",
        g_config.mqtt_url[0]  ? g_config.mqtt_url  : "(not set)",
        ap ? "<div class=box style='border:2px solid #e8a000;'>"
             "<h2>&#9888; Not connected to WiFi</h2>"
             "<p>Connect to <strong>WRG2-Setup</strong> (open network) "
             "then open <a href='http://192.168.4.1/config'>http://192.168.4.1/config</a> "
             "to configure WiFi and MQTT credentials.</p></div>"
           : "");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

/* ── GET /config ──────────────────────────────────────────────────────────── */

static esp_err_t config_get(httpd_req_t *req)
{
    char *buf = malloc(4096);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    snprintf(buf, 4096,
        "<!DOCTYPE html><html><head>"
        "<meta charset=UTF-8><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>WRG2MQTT Settings</title><style>%s</style></head><body>"
        "<h1>WRG2MQTT Settings</h1>"
        "<nav><a href='/'>Status</a><a href='/config'>Settings</a></nav>"

        "<div class=box><h2>Device</h2>"
        "<form method=POST action=/config/hostname>"
        "<label>Hostname</label>"
        "<input type=text name=hostname value='%s' maxlength=31 placeholder='wrg2mqtt'>"
        "<p style='color:#888;font-size:.9em'>Accessible at hostname.local via mDNS. "
        "Takes effect after reboot.</p>"
        "<button type=submit>Save Hostname</button>"
        "</form></div>"

        "<div class=box><h2>WiFi</h2>"
        "<form method=POST action=/config/wifi>"
        "<label>SSID</label>"
        "<input type=text name=ssid value='%s' maxlength=32 placeholder='Network name'>"
        "<label>Password</label>"
        "<input type=password name=pass maxlength=63 placeholder='Leave empty to keep current'>"
        "<button type=submit>Save WiFi &amp; Restart</button>"
        "</form></div>"

        "<div class=box><h2>MQTT</h2>"
        "<form method=POST action=/config/mqtt>"
        "<label>Broker URL</label>"
        "<input type=text name=url value='%s' maxlength=127 placeholder='mqtt://192.168.1.x:1883'>"
        "<label>Username (optional)</label>"
        "<input type=text name=user value='%s' maxlength=31>"
        "<label>Password (optional)</label>"
        "<input type=password name=pass maxlength=31 placeholder='Leave empty to keep current'>"
        "<button type=submit>Save MQTT</button>"
        "</form></div>"

        "<div class=box><h2>Device</h2>"
        "<form method=POST action=/reboot>"
        "<button type=submit class=r>Reboot</button>"
        "</form></div>"

        "</body></html>",
        CSS,
        g_config.hostname,
        g_config.wifi_ssid,
        g_config.mqtt_url,
        g_config.mqtt_user);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

/* ── POST /config/hostname ────────────────────────────────────────────────── */

static esp_err_t config_hostname_post(httpd_req_t *req)
{
    char body[128];
    read_body(req, body, sizeof(body));

    char hostname[32] = {0};
    get_field(body, "hostname", hostname, sizeof(hostname));

    if (hostname[0] != '\0') {
        config_manager_save_hostname(hostname);
    }

    const char *resp =
        "<!DOCTYPE html><html><head><meta charset=UTF-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Saved</title></head><body style='font-family:sans-serif;padding:20px'>"
        "<h2>&#10003; Hostname saved</h2>"
        "<p>Rebooting now. Device will be reachable at <strong>hostname.local</strong>.</p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    xTaskCreate(reboot_task, "reboot", 1024, NULL, 5, NULL);
    return ESP_OK;
}

/* ── POST /config/wifi ────────────────────────────────────────────────────── */

static esp_err_t config_wifi_post(httpd_req_t *req)
{
    char body[512];
    read_body(req, body, sizeof(body));

    char ssid[64] = {0}, pass[64] = {0};
    get_field(body, "ssid", ssid, sizeof(ssid));
    get_field(body, "pass", pass, sizeof(pass));

    /* Keep existing password if none supplied */
    if (pass[0] == '\0') strncpy(pass, g_config.wifi_pass, sizeof(pass) - 1);

    config_manager_save_wifi(ssid, pass);

    const char *resp =
        "<!DOCTYPE html><html><head><meta charset=UTF-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Saved</title></head><body style='font-family:sans-serif;padding:20px'>"
        "<h2>&#10003; WiFi credentials saved</h2>"
        "<p>Rebooting now. Connect back to your network and find the device at its new IP.</p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    xTaskCreate(reboot_task, "reboot", 1024, NULL, 5, NULL);
    return ESP_OK;
}

/* ── POST /config/mqtt ────────────────────────────────────────────────────── */

static esp_err_t config_mqtt_post(httpd_req_t *req)
{
    char body[512];
    read_body(req, body, sizeof(body));

    char url[128] = {0}, user[32] = {0}, pass[32] = {0};
    get_field(body, "url",  url,  sizeof(url));
    get_field(body, "user", user, sizeof(user));
    get_field(body, "pass", pass, sizeof(pass));

    if (pass[0] == '\0') strncpy(pass, g_config.mqtt_pass, sizeof(pass) - 1);

    config_manager_save_mqtt(url, user, pass);

    const char *resp =
        "<!DOCTYPE html><html><head><meta charset=UTF-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Saved</title></head><body style='font-family:sans-serif;padding:20px'>"
        "<h2>&#10003; MQTT settings saved</h2>"
        "<p><a href='/config'>Back to settings</a></p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── POST /reboot ─────────────────────────────────────────────────────────── */

static esp_err_t reboot_post(httpd_req_t *req)
{
    const char *resp =
        "<!DOCTYPE html><html><head><meta charset=UTF-8></head>"
        "<body style='font-family:sans-serif;padding:20px'>"
        "<h2>Rebooting...</h2></body></html>";
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    xTaskCreate(reboot_task, "reboot", 1024, NULL, 5, NULL);
    return ESP_OK;
}

/* ── Start server ─────────────────────────────────────────────────────────── */

esp_err_t config_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",                 .method = HTTP_GET,  .handler = root_get             },
        { .uri = "/config",           .method = HTTP_GET,  .handler = config_get           },
        { .uri = "/config/hostname",  .method = HTTP_POST, .handler = config_hostname_post },
        { .uri = "/config/wifi",      .method = HTTP_POST, .handler = config_wifi_post     },
        { .uri = "/config/mqtt",      .method = HTTP_POST, .handler = config_mqtt_post     },
        { .uri = "/reboot",           .method = HTTP_POST, .handler = reboot_post          },
    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    ESP_LOGI(TAG, "started on port 80");
    return ESP_OK;
}
