#include "config_server.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "wrg2_driver.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "config_server";

/* ── HTML helpers ─────────────────────────────────────────────────────────── */

static const char CSS[] =
    "body{font-family:sans-serif;margin:0;padding:16px;background:#f4f4f4;}"
    ".box{background:#fff;border-radius:8px;padding:20px;margin:14px 0;"
         "box-shadow:0 2px 6px rgba(0,0,0,.12);}"
    ".grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;}"
    "@media(max-width:640px){.grid{grid-template-columns:1fr}}"
    "h1{margin:0 0 4px;font-size:1.4em;color:#222;}"
    "h2{font-size:1em;color:#555;margin:0 0 10px;text-transform:uppercase;"
       "letter-spacing:.05em;border-bottom:2px solid #eee;padding-bottom:6px;}"
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
    ".ok{color:#188038;font-weight:bold;} .warn{color:#e8a000;font-weight:bold;}"
    ".err{color:#d93025;font-weight:bold;}"
    ".big{font-size:1.6em;font-weight:bold;color:#1a1a1a;}"
    ".unit{font-size:.8em;color:#888;margin-left:2px;}"
    "table{width:100%;border-collapse:collapse;}"
    "td,th{padding:5px 8px;text-align:left;border-bottom:1px solid #f0f0f0;font-size:.95em;}"
    "th{color:#888;font-weight:normal;width:60%;}"
    ".refresh{font-size:.8em;color:#aaa;margin-left:8px;}";

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

/* ── Mode description ─────────────────────────────────────────────────────── */

static const char *mode_str(uint8_t mode, uint8_t fan_target)
{
    switch (mode) {
        case 0: return "Default (unset)";  /* factory state, no explicit mode written */
        case 1: return "Off";
        case 2:
            if (fan_target == 16)  return "Automatic";
            if (fan_target == 112) return "Humidity controlled";
            if (fan_target == 144) return "CO2 controlled";
            return "Regulated";
        case 3: return "Manual (balanced)";
        case 4: return "Manual (unbalanced)";
        default: return "Unknown";
    }
}

/* ── GET / — sensor dashboard ─────────────────────────────────────────────── */

static esp_err_t root_get(httpd_req_t *req)
{
    char ip[16];
    wifi_manager_get_ip(ip, sizeof(ip));
    bool ap = wifi_manager_is_ap_mode();

    wrg2_data_t d = {0};
    bool have_data = wrg2_get_last_data(&d);

    char *buf = malloc(6144);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    int n = 0;
    n += snprintf(buf + n, 6144 - n,
        "<!DOCTYPE html><html><head>"
        "<meta charset=UTF-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<meta http-equiv=refresh content=10>"
        "<title>WRG2MQTT</title><style>%s</style></head><body>"
        "<h1>M-WRG-II <span class=refresh>(auto-refresh 10s)</span></h1>"
        "<nav><a href='/'>Status</a><a href='/config'>Settings</a></nav>",
        CSS);

    if (!have_data) {
        n += snprintf(buf + n, 6144 - n,
            "<div class=box><p>Waiting for first Modbus read...</p></div>");
    } else {
        /* ── Temperatures ── */
        n += snprintf(buf + n, 6144 - n,
            "<div class=grid>"
            "<div class=box><h2>Temperatures</h2><table>"
            "<tr><th>Supply (Zuluft)</th><td><span class=big>%.1f</span><span class=unit>°C</span></td></tr>"
            "<tr><th>Extract (Abluft)</th><td><span class=big>%.1f</span><span class=unit>°C</span></td></tr>"
            "<tr><th>Exhaust (Fortluft)</th><td><span class=big>%.1f</span><span class=unit>°C</span></td></tr>"
            "<tr><th>Outdoor (Außenluft)</th><td><span class=big>%.1f</span><span class=unit>°C</span></td></tr>"
            "</table></div>",
            d.temp_supply, d.temp_extract, d.temp_exhaust, d.temp_outdoor);

        /* ── Air quality ── */
        char co2_cell[64];
        if (d.co2_extract == 0x7FFF) {
            snprintf(co2_cell, sizeof(co2_cell), "<span style='color:#aaa'>N/A (no sensor)</span>");
        } else {
            snprintf(co2_cell, sizeof(co2_cell),
                     "<span class=big>%u</span><span class=unit>ppm</span>", d.co2_extract);
        }
        n += snprintf(buf + n, 6144 - n,
            "<div class=box><h2>Air Quality</h2><table>"
            "<tr><th>Humidity extract</th><td><span class=big>%u</span><span class=unit>%%</span></td></tr>"
            "<tr><th>Humidity supply</th><td><span class=big>%u</span><span class=unit>%%</span></td></tr>"
            "<tr><th>CO2 extract</th><td>%s</td></tr>"
            "</table></div>",
            d.humidity_extract, d.humidity_supply, co2_cell);

        /* ── Fans & Mode ── */
        n += snprintf(buf + n, 6144 - n,
            "<div class=box><h2>Fans &amp; Mode</h2><table>"
            "<tr><th>Mode</th><td><b>%s</b></td></tr>"
            "<tr><th>Supply fan (actual)</th><td><span class=big>%u</span><span class=unit>m&#179;/h</span></td></tr>"
            "<tr><th>Exhaust fan (actual)</th><td><span class=big>%u</span><span class=unit>m&#179;/h</span></td></tr>"
            "<tr><th>Target supply (0-200)</th><td>%u &rarr; %u m&#179;/h</td></tr>"
            "<tr><th>Target exhaust (0-200)</th><td>%u &rarr; %u m&#179;/h</td></tr>"
            "</table></div>",
            mode_str(d.mode, d.fan_target_supply),
            d.fan_supply_m3h, d.fan_exhaust_m3h,
            d.fan_target_supply,  d.fan_target_supply / 2,
            d.fan_target_exhaust, d.fan_target_exhaust / 2);

        /* ── Status & Maintenance ── */
        n += snprintf(buf + n, 6144 - n,
            "<div class=box><h2>Status &amp; Maintenance</h2><table>"
            "<tr><th>Device status</th><td>%s</td></tr>"
            "<tr><th>Frost protection</th><td>%s</td></tr>"
            "<tr><th>Filter</th><td>%s</td></tr>"
            "<tr><th>Days until filter change</th><td>%u</td></tr>"
            "<tr><th>Device operating hours</th><td>%lu h</td></tr>"
            "<tr><th>Motor operating hours</th><td>%lu h</td></tr>"
            "</table></div>",
            d.error_flag   ? "<span class=err>&#10007; ERROR</span>"
                           : "<span class=ok>&#10003; OK</span>",
            d.frost_active ? "<span class=warn>&#9744; Active</span>"
                           : "<span class=ok>Inactive</span>",
            d.filter_due   ? "<span class=warn>&#9888; Change needed</span>"
                           : "<span class=ok>&#10003; OK</span>",
            d.filter_days_left,
            (unsigned long)d.hours_device,
            (unsigned long)d.hours_motors);

        /* ── Configuration (42xxx) ── */
        n += snprintf(buf + n, 6144 - n,
            "<div class=box><h2>Humidity Control Config</h2><table>"
            "<tr><th>Start setpoint (42000)</th><td>%u%%</td></tr>"
            "<tr><th>Min fan level (42001)</th><td>%u%%</td></tr>"
            "<tr><th>Max fan level (42002)</th><td>%u%%</td></tr>"
            "</table></div>"
            "<div class=box><h2>CO2 Control Config</h2><table>"
            "<tr><th>Start setpoint (42003)</th><td>%u ppm</td></tr>"
            "<tr><th>Min fan level (42004)</th><td>%u%%</td></tr>"
            "<tr><th>Max fan level (42005)</th><td>%u%%</td></tr>"
            "</table></div>"
            "<div class=box><h2>External Input Config</h2><table>"
            "<tr><th>Fan level (42007)</th><td>%u%%</td></tr>"
            "<tr><th>On-delay (42008)</th><td>%u min</td></tr>"
            "<tr><th>Off-delay (42009)</th><td>%u min</td></tr>"
            "</table></div>",
            d.cfg_hum_setpoint, d.cfg_hum_fan_min, d.cfg_hum_fan_max,
            d.cfg_co2_setpoint, d.cfg_co2_fan_min, d.cfg_co2_fan_max,
            d.cfg_ext_fan_level, d.cfg_ext_on_delay, d.cfg_ext_off_delay);

        n += snprintf(buf + n, 6144 - n, "</div>"); /* close .grid */
    }

    /* ── Network info ── */
    n += snprintf(buf + n, 6144 - n,
        "<div class=box><h2>Network</h2><table>"
        "<tr><th>WiFi mode</th><td>%s</td></tr>"
        "<tr><th>IP address</th><td>%s</td></tr>"
        "<tr><th>MQTT broker</th><td>%s</td></tr>"
        "</table></div>",
        ap ? "Access Point (provisioning)" : "Station",
        ip,
        g_config.mqtt_url[0] ? g_config.mqtt_url : "<i>not configured</i>");

    if (ap) {
        n += snprintf(buf + n, 6144 - n,
            "<div class=box style='border-left:4px solid #e8a000'>"
            "<b>&#9888; Not connected to WiFi.</b> Open "
            "<a href='/config'>/config</a> to set credentials.</div>");
    }

    n += snprintf(buf + n, 6144 - n, "</body></html>");

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
