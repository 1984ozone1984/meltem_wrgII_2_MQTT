#include "config_server.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "wrg2_driver.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
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
        "<nav><a href='/'>Status</a><a href='/control'>Control</a>"
        "<a href='/config'>Settings</a><a href='/ota'>OTA Update</a></nav>",
        CSS);

    if (!have_data) {
        n += snprintf(buf + n, 6144 - n,
            "<div class=box><p>Waiting for first Modbus read...</p></div>");
    } else {
        /* ── Temperatures ── */
        n += snprintf(buf + n, 6144 - n,
            "<div class=grid>"
            "<div class=box><h2>Temperatures</h2><table>"
            "<tr><th>Zulufttemperatur</th><td><span class=big>%.1f</span><span class=unit>°C</span></td></tr>"
            "<tr><th>Ablufttemperatur</th><td><span class=big>%.1f</span><span class=unit>°C</span></td></tr>"
            "<tr><th>Fortlufttemperatur</th><td><span class=big>%.1f</span><span class=unit>°C</span></td></tr>"
            "<tr><th>Au&#223;enlufttemperatur</th><td><span class=big>%.1f</span><span class=unit>°C</span></td></tr>"
            "</table></div>",
            d.temp_zuluft, d.temp_abluft, d.temp_fortluft, d.temp_aussenluft);

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
            "<tr><th>Feuchte Abluft</th><td><span class=big>%u</span><span class=unit>%%</span></td></tr>"
            "<tr><th>Feuchte Zuluft</th><td><span class=big>%u</span><span class=unit>%%</span></td></tr>"
            "<tr><th>CO2 extract</th><td>%s</td></tr>"
            "</table></div>",
            d.feuchte_abluft, d.feuchte_zuluft, co2_cell);

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

/* ── GET /control ─────────────────────────────────────────────────────────── */

static esp_err_t control_get(httpd_req_t *req)
{
    wrg2_data_t d = {0};
    bool have = wrg2_get_last_data(&d);

    char *buf = malloc(6144);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    int n = 0;
    n += snprintf(buf + n, 6144 - n,
        "<!DOCTYPE html><html><head>"
        "<meta charset=UTF-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>WRG2MQTT Control</title><style>%s</style></head><body>"
        "<h1>M-WRG-II Control</h1>"
        "<nav><a href='/'>Status</a><a href='/control'>Control</a>"
        "<a href='/config'>Settings</a><a href='/ota'>OTA Update</a></nav>",
        CSS);

    if (!have) {
        n += snprintf(buf + n, 6144 - n,
            "<div class=box><p>Waiting for first Modbus read...</p></div>");
        n += snprintf(buf + n, 6144 - n, "</body></html>");
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
        free(buf);
        return ESP_OK;
    }

    /* Current mode badge */
    const char *cur_mode = mode_str(d.mode, d.fan_target_supply);
    n += snprintf(buf + n, 6144 - n,
        "<div class=box style='border-left:4px solid #1a73e8'>"
        "<b>Current mode:</b> %s &nbsp;"
        "<span style='color:#888;font-size:.9em'>"
        "(supply %um&#179;/h actual, exhaust %um&#179;/h actual)</span>"
        "</div>",
        cur_mode, d.fan_supply_m3h, d.fan_exhaust_m3h);

    /* ── Mode: Off (41120=1, 41132=0; 41121 not used) ── */
    n += snprintf(buf + n, 6144 - n,
        "<div class=box><h2>Off &mdash; writes: 41120=1, 41132=0</h2>"
        "<form method=POST action=/control/mode>"
        "<input type=hidden name=mode value=off>"
        "<button type=submit class=r>Switch Off</button>"
        "</form></div>");

    /* ── Mode: Humidity (41120=2, 41121=112, 41132=0) ── */
    n += snprintf(buf + n, 6144 - n,
        "<div class=box><h2>Humidity Controlled &mdash; writes: 41120=2, 41121=112, 41132=0</h2>"
        "<form method=POST action=/control/mode>"
        "<input type=hidden name=mode value=humidity>"
        "<button type=submit>Enable Humidity Control</button>"
        "</form></div>");

    /* ── Mode 3: Balanced manual (41120=3, 41121=fan*2, 41132=0) ── */
    n += snprintf(buf + n, 6144 - n,
        "<div class=box><h2>Manual Balanced &mdash; writes: 41120=3, 41121=speed&times;2, 41132=0</h2>"
        "<form method=POST action=/control/fan>"
        "<label>Fan speed &mdash; supply &amp; exhaust equal (0&ndash;100 m&#179;/h)</label>"
        "<input type=number name=fan min=0 max=100 step=5 value=%u>"
        "<button type=submit>Set Balanced Speed</button>"
        "</form></div>",
        d.fan_target_supply / 2);

    /* ── Mode 4: Unbalanced manual (41120=4, 41121=sup*2, 41122=exh*2, 41132=0) ── */
    n += snprintf(buf + n, 6144 - n,
        "<div class=box><h2>Manual Unbalanced &mdash; writes: 41120=4, 41121=supply&times;2, 41122=exhaust&times;2, 41132=0</h2>"
        "<form method=POST action=/control/fan_unbal>"
        "<label>Supply (Zuluft) 0&ndash;100 m&#179;/h</label>"
        "<input type=number name=supply min=0 max=100 step=5 value=%u>"
        "<label>Exhaust (Fortluft) 0&ndash;100 m&#179;/h</label>"
        "<input type=number name=exhaust min=0 max=100 step=5 value=%u>"
        "<button type=submit>Set Unbalanced Speed</button>"
        "</form></div>",
        d.fan_target_supply  / 2,
        d.fan_target_exhaust / 2);

    /* ── Config 42xxx ── */
    n += snprintf(buf + n, 6144 - n,
        "<div class=grid>"

        "<div class=box><h2>Humidity Control (42000-42002)</h2>"
        "<form method=POST action=/control/cfg_hum>"
        "<label>Setpoint 40&ndash;80%%</label>"
        "<input type=number name=sp min=40 max=80 step=1 value=%u>"
        "<label>Min fan level 0&ndash;100%%</label>"
        "<input type=number name=fmin min=0 max=100 step=10 value=%u>"
        "<label>Max fan level 10&ndash;100%%</label>"
        "<input type=number name=fmax min=10 max=100 step=10 value=%u>"
        "<button type=submit>Save</button>"
        "</form></div>",
        d.cfg_hum_setpoint, d.cfg_hum_fan_min, d.cfg_hum_fan_max);

    n += snprintf(buf + n, 6144 - n,
        "<div class=box><h2>CO2 Control (42003-42005)</h2>"
        "<form method=POST action=/control/cfg_co2>"
        "<label>Setpoint 500&ndash;1200 ppm</label>"
        "<input type=number name=sp min=500 max=1200 step=1 value=%u>"
        "<label>Min fan level 0&ndash;100%%</label>"
        "<input type=number name=fmin min=0 max=100 step=10 value=%u>"
        "<label>Max fan level 10&ndash;100%%</label>"
        "<input type=number name=fmax min=10 max=100 step=10 value=%u>"
        "<button type=submit>Save</button>"
        "</form></div>",
        d.cfg_co2_setpoint, d.cfg_co2_fan_min, d.cfg_co2_fan_max);

    n += snprintf(buf + n, 6144 - n,
        "<div class=box><h2>External Input (42007-42009)</h2>"
        "<form method=POST action=/control/cfg_ext>"
        "<label>Fan level 10&ndash;100%%</label>"
        "<input type=number name=fan min=10 max=100 step=10 value=%u>"
        "<label>On-delay 0&ndash;240 min</label>"
        "<input type=number name=on min=0 max=240 step=1 value=%u>"
        "<label>Off-delay 0&ndash;240 min</label>"
        "<input type=number name=off min=0 max=240 step=1 value=%u>"
        "<button type=submit>Save</button>"
        "</form></div>"
        "</div>",
        d.cfg_ext_fan_level, d.cfg_ext_on_delay, d.cfg_ext_off_delay);

    n += snprintf(buf + n, 6144 - n, "</body></html>");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

/* ── POST helpers for control ─────────────────────────────────────────────── */

/* Called by mqtt_manager and config_server via extern declarations */
extern void wrg2_enqueue_mode(const char *payload);
extern void wrg2_enqueue_fan_balanced(const char *payload);
extern void wrg2_enqueue_fan_unbal_supply(const char *payload);
extern void wrg2_enqueue_fan_unbal_exhaust(const char *payload);

static esp_err_t send_ok(httpd_req_t *req, const char *msg, const char *back)
{
    char buf[300];
    snprintf(buf, sizeof(buf),
        "<!DOCTYPE html><html><head><meta charset=UTF-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>OK</title></head>"
        "<body style='font-family:sans-serif;padding:20px'>"
        "<h2>&#10003; %s</h2><p><a href='%s'>Back</a></p></body></html>",
        msg, back);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t ctrl_mode_post(httpd_req_t *req)
{
    char body[64]; read_body(req, body, sizeof(body));
    char mode[32] = {0};
    get_field(body, "mode", mode, sizeof(mode));
    if (mode[0]) wrg2_enqueue_mode(mode);
    return send_ok(req, "Mode command queued", "/control");
}

static esp_err_t ctrl_fan_post(httpd_req_t *req)
{
    char body[32]; read_body(req, body, sizeof(body));
    char val[16] = {0};
    get_field(body, "fan", val, sizeof(val));
    if (val[0]) wrg2_enqueue_fan_balanced(val);
    return send_ok(req, "Fan level command queued", "/control");
}

static esp_err_t ctrl_fan_unbal_post(httpd_req_t *req)
{
    char body[64]; read_body(req, body, sizeof(body));
    char sup[16] = {0}, exh[16] = {0};
    get_field(body, "supply",  sup, sizeof(sup));
    get_field(body, "exhaust", exh, sizeof(exh));
    /* Encode as "supply,exhaust" — control_task handles via CMD_FAN + exhaust path */
    if (sup[0] && exh[0]) {
        int s = atoi(sup), e = atoi(exh);
        /* Directly call the driver — config_server runs in its own task,
           Modbus mutex serializes access safely */
        wrg2_set_mode_unbalanced((uint8_t)(s < 0 ? 0 : s > 100 ? 100 : s),
                                 (uint8_t)(e < 0 ? 0 : e > 100 ? 100 : e));
    }
    return send_ok(req, "Unbalanced fan command sent", "/control");
}

static esp_err_t ctrl_cfg_hum_post(httpd_req_t *req)
{
    char body[64]; read_body(req, body, sizeof(body));
    char sp[16]={0}, fmin[16]={0}, fmax[16]={0};
    get_field(body, "sp",   sp,   sizeof(sp));
    get_field(body, "fmin", fmin, sizeof(fmin));
    get_field(body, "fmax", fmax, sizeof(fmax));
    if (sp[0])   wrg2_write_config(42000, (uint16_t)atoi(sp));
    if (fmin[0]) wrg2_write_config(42001, (uint16_t)atoi(fmin));
    if (fmax[0]) wrg2_write_config(42002, (uint16_t)atoi(fmax));
    return send_ok(req, "Humidity config saved", "/control");
}

static esp_err_t ctrl_cfg_co2_post(httpd_req_t *req)
{
    char body[64]; read_body(req, body, sizeof(body));
    char sp[16]={0}, fmin[16]={0}, fmax[16]={0};
    get_field(body, "sp",   sp,   sizeof(sp));
    get_field(body, "fmin", fmin, sizeof(fmin));
    get_field(body, "fmax", fmax, sizeof(fmax));
    if (sp[0])   wrg2_write_config(42003, (uint16_t)atoi(sp));
    if (fmin[0]) wrg2_write_config(42004, (uint16_t)atoi(fmin));
    if (fmax[0]) wrg2_write_config(42005, (uint16_t)atoi(fmax));
    return send_ok(req, "CO2 config saved", "/control");
}

static esp_err_t ctrl_cfg_ext_post(httpd_req_t *req)
{
    char body[64]; read_body(req, body, sizeof(body));
    char fan[16]={0}, on[16]={0}, off_[16]={0};
    get_field(body, "fan", fan,  sizeof(fan));
    get_field(body, "on",  on,   sizeof(on));
    get_field(body, "off", off_, sizeof(off_));
    if (fan[0])  wrg2_write_config(42007, (uint16_t)atoi(fan));
    if (on[0])   wrg2_write_config(42008, (uint16_t)atoi(on));
    if (off_[0]) wrg2_write_config(42009, (uint16_t)atoi(off_));
    return send_ok(req, "External input config saved", "/control");
}

/* ── GET /config ──────────────────────────────────────────────────────────── */

static esp_err_t config_get(httpd_req_t *req)
{
    char *buf = malloc(5120);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    snprintf(buf, 5120,
        "<!DOCTYPE html><html><head>"
        "<meta charset=UTF-8><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>WRG2MQTT Settings</title><style>%s</style></head><body>"
        "<h1>WRG2MQTT Settings</h1>"
        "<nav><a href='/'>Status</a><a href='/control'>Control</a>"
        "<a href='/config'>Settings</a><a href='/ota'>OTA Update</a></nav>"

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

        "<div class=box><h2>Modbus &amp; Polling</h2>"
        "<form method=POST action=/config/modbus>"
        "<label>Slave ID</label>"
        "<input type=number name=slave_id value='%u' min=1 max=247>"
        "<label>Baud Rate</label>"
        "<input type=number name=baud value='%lu' min=1200 max=115200>"
        "<label>Poll Interval (s) — how often to read Modbus</label>"
        "<input type=number name=poll_ivl value='%lu' min=1 max=300>"
        "<label>Publish Interval (s) — how often to send to MQTT</label>"
        "<input type=number name=pub_ivl value='%lu' min=1 max=3600>"
        "<p style='color:#888;font-size:.9em'>Publish interval should be &ge; poll interval. "
        "Takes effect immediately (no reboot needed).</p>"
        "<label>TX GPIO</label>"
        "<input type=number name=gpio_tx value='%u' min=0 max=47>"
        "<label>RX GPIO</label>"
        "<input type=number name=gpio_rx value='%u' min=0 max=47>"
        "<label>RTS/DE GPIO (set to 255 to disable)</label>"
        "<input type=number name=gpio_rts value='%u' min=0 max=255>"
        "<p style='color:#888;font-size:.9em'>GPIO changes take effect after reboot. "
        "Default: TX=43, RX=44, RTS=2.</p>"
        "<button type=submit>Save Modbus &amp; Polling</button>"
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
        g_config.mqtt_user,
        (unsigned)g_config.mb_slave_id,
        (unsigned long)g_config.mb_baud,
        (unsigned long)g_config.poll_interval,
        (unsigned long)g_config.pub_interval,
        (unsigned)g_config.mb_gpio_tx,
        (unsigned)g_config.mb_gpio_rx,
        (unsigned)g_config.mb_gpio_rts);

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

/* ── POST /config/modbus ──────────────────────────────────────────────────── */

static esp_err_t config_modbus_post(httpd_req_t *req)
{
    char body[256];
    read_body(req, body, sizeof(body));

    char s_slave[8] = {0}, s_baud[8] = {0}, s_poll[8] = {0}, s_pub[8] = {0};
    char s_tx[8] = {0}, s_rx[8] = {0}, s_rts[8] = {0};
    get_field(body, "slave_id", s_slave, sizeof(s_slave));
    get_field(body, "baud",     s_baud,  sizeof(s_baud));
    get_field(body, "poll_ivl", s_poll,  sizeof(s_poll));
    get_field(body, "pub_ivl",  s_pub,   sizeof(s_pub));
    get_field(body, "gpio_tx",  s_tx,    sizeof(s_tx));
    get_field(body, "gpio_rx",  s_rx,    sizeof(s_rx));
    get_field(body, "gpio_rts", s_rts,   sizeof(s_rts));

    uint8_t  slave_id      = (uint8_t)atoi(s_slave);
    uint32_t baud          = (uint32_t)atoi(s_baud);
    uint32_t poll_interval = (uint32_t)atoi(s_poll);
    uint32_t pub_interval  = (uint32_t)atoi(s_pub);
    uint8_t  gpio_tx       = s_tx[0]  ? (uint8_t)atoi(s_tx)  : g_config.mb_gpio_tx;
    uint8_t  gpio_rx       = s_rx[0]  ? (uint8_t)atoi(s_rx)  : g_config.mb_gpio_rx;
    uint8_t  gpio_rts      = s_rts[0] ? (uint8_t)atoi(s_rts) : g_config.mb_gpio_rts;

    /* Clamp to sane ranges */
    if (slave_id < 1)            slave_id = 1;
    if (slave_id > 247)          slave_id = 247;
    if (baud < 1200)             baud = 1200;
    if (baud > 115200)           baud = 115200;
    if (poll_interval < 1)       poll_interval = 1;
    if (poll_interval > 300)     poll_interval = 300;
    if (pub_interval < 1)        pub_interval = 1;
    if (pub_interval > 3600)     pub_interval = 3600;
    if (pub_interval < poll_interval) pub_interval = poll_interval;
    /* gpio_tx/rx clamped to valid ESP32-S3 range */
    if (gpio_tx > 47)  gpio_tx  = 47;
    if (gpio_rx > 47)  gpio_rx  = 47;
    /* gpio_rts: 255 means disabled (UART_PIN_NO_CHANGE) */

    bool gpio_changed = (gpio_tx  != g_config.mb_gpio_tx  ||
                         gpio_rx  != g_config.mb_gpio_rx  ||
                         gpio_rts != g_config.mb_gpio_rts);

    config_manager_save_modbus(slave_id, baud, poll_interval, pub_interval,
                               gpio_tx, gpio_rx, gpio_rts);

    const char *resp = gpio_changed
        ? "<!DOCTYPE html><html><head><meta charset=UTF-8>"
          "<meta name=viewport content='width=device-width,initial-scale=1'>"
          "<title>Saved</title></head><body style='font-family:sans-serif;padding:20px'>"
          "<h2>&#10003; Modbus &amp; Polling settings saved</h2>"
          "<p>GPIO pin changes take effect after reboot.</p>"
          "<p><a href='/config'>Back to settings</a></p>"
          "</body></html>"
        : "<!DOCTYPE html><html><head><meta charset=UTF-8>"
          "<meta name=viewport content='width=device-width,initial-scale=1'>"
          "<title>Saved</title></head><body style='font-family:sans-serif;padding:20px'>"
          "<h2>&#10003; Modbus &amp; Polling settings saved</h2>"
          "<p>New intervals take effect immediately.</p>"
          "<p><a href='/config'>Back to settings</a></p>"
          "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── GET /ota ─────────────────────────────────────────────────────────────── */

static esp_err_t ota_get(httpd_req_t *req)
{
    /* Inline the JS so there are no external dependencies */
    static const char PAGE[] =
        "<!DOCTYPE html><html><head>"
        "<meta charset=UTF-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>WRG2MQTT OTA Update</title>"
        "<style>"
        "body{font-family:sans-serif;margin:0;padding:16px;background:#f4f4f4;}"
        ".box{background:#fff;border-radius:8px;padding:20px;margin:14px 0;"
             "box-shadow:0 2px 6px rgba(0,0,0,.12);}"
        "h1{margin:0 0 4px;font-size:1.4em;color:#222;}"
        "h2{font-size:1em;color:#555;margin:0 0 10px;text-transform:uppercase;"
           "letter-spacing:.05em;border-bottom:2px solid #eee;padding-bottom:6px;}"
        "label{display:block;font-weight:bold;margin:10px 0 3px;color:#333;}"
        "input[type=file]{width:100%;padding:8px;border:1px solid #ccc;border-radius:4px;"
                         "box-sizing:border-box;font-size:1em;background:#fff;}"
        "button{background:#1a73e8;color:#fff;padding:10px 20px;border:none;"
                "border-radius:4px;cursor:pointer;font-size:1em;margin-top:14px;}"
        "button:hover{background:#1558b0;}"
        "button:disabled{background:#aaa;cursor:default;}"
        "nav a{display:inline-block;margin:0 8px 12px 0;padding:8px 14px;"
              "background:#1a73e8;color:#fff;text-decoration:none;border-radius:4px;}"
        "nav a:hover{background:#1558b0;}"
        "progress{width:100%;height:20px;margin-top:12px;}"
        "#status{margin-top:8px;font-weight:bold;}"
        ".ok{color:#188038;} .err{color:#d93025;} .info{color:#555;}"
        "</style></head><body>"
        "<h1>WRG2MQTT OTA Update</h1>"
        "<nav><a href='/'>Status</a><a href='/control'>Control</a>"
        "<a href='/config'>Settings</a><a href='/ota'>OTA Update</a></nav>"
        "<div class=box><h2>Flash Firmware</h2>"
        "<p class=info>Select a <code>.bin</code> firmware file built for this device. "
        "The device will reboot automatically after a successful flash.</p>"
        "<label>Firmware file</label>"
        "<input type=file id=fw accept=.bin>"
        "<button id=btn onclick=doUpload()>Flash Firmware</button>"
        "<progress id=bar value=0 max=100 style='display:none'></progress>"
        "<div id=status></div>"
        "</div>"
        "<script>"
        "function doUpload(){"
          "var f=document.getElementById('fw').files[0];"
          "if(!f){alert('No file selected');return;}"
          "var btn=document.getElementById('btn');"
          "var bar=document.getElementById('bar');"
          "var st=document.getElementById('status');"
          "btn.disabled=true;"
          "bar.style.display='';"
          "bar.value=0;"
          "st.textContent='Uploading...';"
          "st.className='info';"
          "var xhr=new XMLHttpRequest();"
          "xhr.open('POST','/ota/upload');"
          "xhr.setRequestHeader('Content-Type','application/octet-stream');"
          "xhr.upload.onprogress=function(e){"
            "if(e.lengthComputable){"
              "var p=Math.round(e.loaded/e.total*100);"
              "bar.value=p;"
              "st.textContent='Uploading... '+p+'% ('+Math.round(e.loaded/1024)+'/'+"
                "Math.round(e.total/1024)+' KB)';"
            "}"
          "};"
          "xhr.onload=function(){"
            "if(xhr.status===200){"
              "bar.value=100;"
              "st.className='ok';"
              "st.textContent='Flash successful! Device is rebooting...';"
            "}else{"
              "st.className='err';"
              "st.textContent='Error: '+xhr.responseText;"
              "btn.disabled=false;"
            "}"
          "};"
          "xhr.onerror=function(){"
            "st.className='err';"
            "st.textContent='Upload failed — check device connection';"
            "btn.disabled=false;"
          "};"
          "xhr.send(f);"
        "}"
        "</script>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── POST /ota/upload ─────────────────────────────────────────────────────── */

#define OTA_BUF_SIZE 4096

static esp_err_t ota_upload_post(httpd_req_t *req)
{
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "OTA: no update partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA: writing to partition '%s' at offset 0x%lx, size 0x%lx",
             update_partition->label,
             (unsigned long)update_partition->address,
             (unsigned long)update_partition->size);

    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    int written   = 0;
    bool ok       = true;

    ESP_LOGI(TAG, "OTA: firmware size %d bytes", remaining);

    while (remaining > 0) {
        int to_recv = (remaining < OTA_BUF_SIZE) ? remaining : OTA_BUF_SIZE;
        int received = httpd_req_recv(req, buf, to_recv);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;  /* retry on timeout */
            ESP_LOGE(TAG, "OTA: recv error %d", received);
            ok = false;
            break;
        }
        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA: write failed: %s", esp_err_to_name(err));
            ok = false;
            break;
        }
        remaining -= received;
        written   += received;
        ESP_LOGD(TAG, "OTA: written %d / %d bytes", written, req->content_len);
    }

    free(buf);

    if (!ok) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
        return ESP_FAIL;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            err == ESP_ERR_OTA_VALIDATE_FAILED
                                ? "Image validation failed — wrong binary?"
                                : "OTA end failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: set boot partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: success — %d bytes written, rebooting", written);
    httpd_resp_sendstr(req, "OK");

    xTaskCreate(reboot_task, "ota_reboot", 1024, NULL, 5, NULL);
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
    cfg.lru_purge_enable  = true;
    cfg.max_uri_handlers  = 18;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",                  .method = HTTP_GET,  .handler = root_get             },
        { .uri = "/control",           .method = HTTP_GET,  .handler = control_get          },
        { .uri = "/control/mode",      .method = HTTP_POST, .handler = ctrl_mode_post       },
        { .uri = "/control/fan",       .method = HTTP_POST, .handler = ctrl_fan_post        },
        { .uri = "/control/fan_unbal", .method = HTTP_POST, .handler = ctrl_fan_unbal_post  },
        { .uri = "/control/cfg_hum",   .method = HTTP_POST, .handler = ctrl_cfg_hum_post    },
        { .uri = "/control/cfg_co2",   .method = HTTP_POST, .handler = ctrl_cfg_co2_post    },
        { .uri = "/control/cfg_ext",   .method = HTTP_POST, .handler = ctrl_cfg_ext_post    },
        { .uri = "/config",            .method = HTTP_GET,  .handler = config_get           },
        { .uri = "/config/hostname",   .method = HTTP_POST, .handler = config_hostname_post },
        { .uri = "/config/wifi",       .method = HTTP_POST, .handler = config_wifi_post     },
        { .uri = "/config/mqtt",       .method = HTTP_POST, .handler = config_mqtt_post     },
        { .uri = "/config/modbus",     .method = HTTP_POST, .handler = config_modbus_post   },
        { .uri = "/reboot",            .method = HTTP_POST, .handler = reboot_post          },
        { .uri = "/ota",               .method = HTTP_GET,  .handler = ota_get              },
        { .uri = "/ota/upload",        .method = HTTP_POST, .handler = ota_upload_post      },
    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    ESP_LOGI(TAG, "started on port 80");
    return ESP_OK;
}
