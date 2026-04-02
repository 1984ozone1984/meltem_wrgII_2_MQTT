# WRG2MQTT — Implementation Plan

**Based on:** FSD_WRG2MQTT.md v1.0  
**Date:** 2026-04-01

---

## Preliminary Notes

The project starts from an empty repository. ESP-IDF v5.4 targeting `esp32s3`. The PDF register map (`docs/Meltem BA-IA_M-WRG-II_P-M_E-M.pdf`) must be consulted before Phase 2 begins. The DE/RE GPIO pin for the MAX485 direction control is TBD — must be assigned before Phase 2.

---

## 1. Project Directory Structure

```
wrg2mqtt/
├── CMakeLists.txt                  # Top-level: cmake_minimum_required + project()
├── sdkconfig.defaults              # Baseline Kconfig overrides (checked in)
├── partitions.csv                  # Custom partition table (OTA requires 2 app slots)
├── docs/
│   └── (reference PDFs)
├── main/
│   ├── CMakeLists.txt              # idf_component_register for main component
│   └── app_main.c                  # Entry point: calls init functions in sequence
└── components/
    ├── system_core/                # NVS init, watchdog init, error handling
    ├── config_manager/             # NVS read/write, global config struct
    ├── wifi_manager/               # STA connect, event-driven reconnect, backoff
    ├── mqtt_manager/               # MQTT init, LWT, pub/sub, reconnect
    ├── ha_discovery/               # Build + publish HA discovery JSON payloads
    ├── ota_manager/                # HTTPS OTA task, MQTT trigger handler
    ├── modbus_rtu/                 # Phase 2+: UART driver, RTU framing, CRC, retry
    └── wrg2_driver/               # Phase 2+: WRG-II register map, read/write API
```

### Custom Partition Table (mandatory for OTA)

`partitions.csv`:
```
# Name,   Type, SubType, Offset,   Size,    Flags
nvs,      data, nvs,     0x9000,   0x6000,
otadata,  data, ota,     0xf000,   0x2000,
ota_0,    app,  ota_0,   0x10000,  0x1E0000,
ota_1,    app,  ota_1,   0x1F0000, 0x1E0000,
```

### `sdkconfig.defaults` Baseline

```
CONFIG_ESPTOOLPY_FLASHSIZE="8MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_ESP_TASK_WDT_TIMEOUT_S=30
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=n
CONFIG_MQTT_SKIP_PUBLISH_IF_DISCONNECTED=y
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
```

---

## 2. FreeRTOS Task Layout

| Task | Stack | Priority | Role |
|------|-------|----------|------|
| `wifi_manager` internal | ESP-IDF managed | system | WiFi event loop |
| `mqtt_client` internal | ESP-IDF managed | system | MQTT send/receive |
| `polling_task` | 4096 B | 5 | Modbus read + MQTT publish |
| `control_task` | 4096 B | 6 | Modbus write from MQTT commands (Phase 3) |
| `ota_task` | 8192 B | 5 | OTA download (spawned on demand) |

---

## 3. Phase 1 — Infrastructure (No Modbus)

**Goal:** Full ESP32 infrastructure validated independently of WRG-II hardware. Connects to WiFi, MQTT, publishes HA discovery with mock data, supports OTA.

### Step 1.1 — Skeleton Project

**Files to create:**
- `CMakeLists.txt`
- `main/CMakeLists.txt`
- `main/app_main.c`
- `partitions.csv`
- `sdkconfig.defaults`

**Boot sequence in `app_main.c`:**
```c
void app_main(void) {
    system_core_init();     // NVS flash init, watchdog init
    config_manager_init();  // Load config from NVS into global struct
    wifi_manager_init();    // Start WiFi STA, block until IP obtained
    mqtt_manager_init();    // Connect MQTT, register event handler
    ha_discovery_publish(); // Publish all discovery topics
    ota_manager_init();     // Subscribe to OTA trigger topic
    xTaskCreate(polling_task, "poll", 4096, NULL, 5, NULL);
}
```

### Step 1.2 — `config_manager` Component

**NVS namespace:** `"wrg2_cfg"`

**Global config struct:**
```c
typedef struct {
    char wifi_ssid[64];
    char wifi_pass[64];
    char mqtt_url[128];     // e.g. "mqtt://192.168.1.x:1883"
    char mqtt_user[32];
    char mqtt_pass[32];
    uint8_t mb_slave_id;    // default 1
    uint32_t mb_baud;       // default 9600
    uint32_t poll_interval; // default 10 (seconds)
} wrg2_config_t;

extern wrg2_config_t g_config;
```

**NVS Key Map:**

| NVS Key | Field | Type |
|---------|-------|------|
| `wifi_ssid` | `wifi_ssid` | `nvs_get_str` |
| `wifi_pass` | `wifi_pass` | `nvs_get_str` |
| `mqtt_url` | `mqtt_url` | `nvs_get_str` |
| `mqtt_user` | `mqtt_user` | `nvs_get_str` |
| `mqtt_pass` | `mqtt_pass` | `nvs_get_str` |
| `mb_slave_id` | `mb_slave_id` | `nvs_get_u8` |
| `mb_baud` | `mb_baud` | `nvs_get_u32` |
| `poll_ivl` | `poll_interval` | `nvs_get_u32` |

ESP-IDF APIs: `nvs_open`, `nvs_get_str`, `nvs_get_u8`, `nvs_get_u32`, `nvs_set_*`, `nvs_commit`, `nvs_close`.

### Step 1.3 — `wifi_manager` Component

- Event-driven using `esp_event_loop_create_default()`
- `EventGroupHandle_t` signals `WIFI_CONNECTED_BIT` / `WIFI_FAIL_BIT`
- Reconnect: exponential backoff starting at 1 s, doubling, capped at 60 s
- ESP-IDF components: `esp_wifi`, `esp_netif`, `esp_event`

```c
void wifi_manager_init(void);
bool wifi_manager_is_connected(void);
```

### Step 1.4 — `mqtt_manager` Component

**Library:** Built-in `mqtt` component (`mqtt_client.h`)

**Key config:**
```c
esp_mqtt_client_config_t mqtt_cfg = {
    .broker.address.uri                      = g_config.mqtt_url,
    .credentials.username                    = g_config.mqtt_user,
    .credentials.authentication.password     = g_config.mqtt_pass,
    .session.last_will.topic                 = "wrg2/availability",
    .session.last_will.msg                   = "offline",
    .session.last_will.qos                   = 1,
    .session.last_will.retain                = 1,
    .network.reconnect_timeout_ms            = 5000,
};
```

**On `MQTT_EVENT_CONNECTED`:**
1. Publish `wrg2/availability` → `"online"` (QoS 1, retain)
2. Re-subscribe to all control topics
3. Call `ha_discovery_publish()`

**Exported API:**
```c
void mqtt_manager_init(void);
int  mqtt_publish(const char *topic, const char *payload, int qos, int retain);
bool mqtt_manager_is_connected(void);
```

### Step 1.5 — `ha_discovery` Component

Discovery published once on MQTT connect. All payloads: QoS 1, retain=1.

| HA Topic | Entity Type | State Topic |
|----------|-------------|-------------|
| `homeassistant/sensor/wrg2_supply_temp/config` | sensor | `wrg2/status/temperature_supply` |
| `homeassistant/sensor/wrg2_extract_temp/config` | sensor | `wrg2/status/temperature_extract` |
| `homeassistant/sensor/wrg2_fan_speed/config` | sensor | `wrg2/status/fan_speed` |
| `homeassistant/binary_sensor/wrg2_bypass/config` | binary_sensor | `wrg2/status/bypass_state` |
| `homeassistant/fan/wrg2_fan/config` | fan | `wrg2/status/fan_speed` |
| `homeassistant/select/wrg2_mode/config` | select | `wrg2/status/operating_mode` |

All payloads include:
```json
"availability_topic": "wrg2/availability",
"device": {
  "identifiers": ["wrg2mqtt"],
  "name": "Meltem WRG-II",
  "manufacturer": "Meltem",
  "model": "WRG-II"
}
```

### Step 1.6 — `ota_manager` Component

- **Trigger:** MQTT topic `wrg2/ota/trigger`, payload = firmware URL
- ESP-IDF API: `esp_https_ota()` from `esp_https_ota.h`
- OTA runs in a dedicated task (8 KB stack); on success calls `esp_restart()`
- Phase 1: plain HTTP (local LAN). TLS can be added later.

### Step 1.7 — Mock Polling Task + Watchdog

**Watchdog config:**
```c
esp_task_wdt_config_t wdt_cfg = {
    .timeout_ms    = 30000,
    .idle_core_mask = 0,
    .trigger_panic  = true,
};
esp_task_wdt_init(&wdt_cfg);
```

**Mock polling task (replaced in Phase 2):**
```c
void polling_task(void *arg) {
    esp_task_wdt_add(NULL);
    while (1) {
        esp_task_wdt_reset();
        if (mqtt_manager_is_connected()) {
            mqtt_publish("wrg2/status/temperature_supply", "21.5", 1, 1);
            mqtt_publish("wrg2/status/temperature_extract", "18.0", 1, 1);
            mqtt_publish("wrg2/status/fan_speed", "2", 1, 1);
            mqtt_publish("wrg2/status/bypass_state", "OFF", 1, 1);
            mqtt_publish("wrg2/status/operating_mode", "auto", 1, 1);
        }
        vTaskDelay(pdMS_TO_TICKS(g_config.poll_interval * 1000));
    }
}
```

### Phase 1 Completion Checklist

- [ ] `idf.py build` succeeds with custom partition table
- [ ] Device connects to WiFi, logs IP address
- [ ] Device connects to MQTT, publishes `wrg2/availability` = `"online"`
- [ ] All 6 HA entities auto-discovered
- [ ] Mock sensor values appear in HA at poll interval
- [ ] WiFi reconnects automatically after router restart
- [ ] MQTT reconnects automatically after broker restart
- [ ] OTA update via `wrg2/ota/trigger` topic works
- [ ] No credentials in source code

---

## 4. Phase 2 — Modbus Read

**Pre-conditions:** Register map extracted from Meltem PDF. DE/RE GPIO assigned.

### Step 2.1 — Hardware: UART Port and GPIO Pins

GPIO43/44 are default `UART_NUM_0` (console) pins — conflict. Resolution:

- **Use `UART_NUM_1`** remapped to GPIO43/44 via `uart_set_pin()`
- Set `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` to move console to USB-JTAG
- **DE/RE pin:** Assign to a free GPIO (e.g., GPIO2 = D0). Connect as hardware RTS pin:

```c
uart_set_pin(UART_NUM_1, GPIO43, GPIO44, GPIO_DE_RE, UART_PIN_NO_CHANGE);
uart_set_mode(UART_NUM_1, UART_MODE_RS485_HALF_DUPLEX);
```

`UART_MODE_RS485_HALF_DUPLEX` automatically drives DE/RE via hardware RTS — no software GPIO toggling needed.

### Step 2.2 — `modbus_rtu` Component

**Direct UART implementation** (not `esp-modbus`). Rationale: single slave, two function codes — ~250 lines of C, fully controllable.

**UART config:**
```c
uart_config_t uart_cfg = {
    .baud_rate  = g_config.mb_baud,
    .data_bits  = UART_DATA_8_BITS,
    .parity     = UART_PARITY_EVEN,
    .stop_bits  = UART_STOP_BITS_1,
    .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    .source_clk = UART_SCLK_DEFAULT,
};
uart_driver_install(UART_NUM_1, 256, 0, 0, NULL, 0);
uart_param_config(UART_NUM_1, &uart_cfg);
uart_set_rx_timeout(UART_NUM_1, 3);  // 3.5-char silence = frame boundary
```

**CRC16 (Modbus, polynomial 0xA001):**
```c
uint16_t modbus_crc16(const uint8_t *buf, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}
```

**Send-and-receive with retry:**
```c
esp_err_t modbus_rtu_read_registers(uint8_t slave, uint16_t addr,
                                     uint16_t count, uint16_t *out) {
    for (int attempt = 0; attempt < 3; attempt++) {
        uart_flush_input(UART_NUM_1);
        uart_write_bytes(UART_NUM_1, req_buf, req_len);
        uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(100));
        int rx_len = uart_read_bytes(UART_NUM_1, resp_buf, sizeof(resp_buf),
                                     pdMS_TO_TICKS(1000));
        if (rx_len < 5) continue;
        if (modbus_parse_read_response(resp_buf, rx_len, out, count) == ESP_OK)
            return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return ESP_ERR_TIMEOUT;
}
```

**Bus mutex:** `SemaphoreHandle_t` guards UART access. Required in Phase 3 when both polling and control tasks use the bus concurrently.

**Exported API:**
```c
void      modbus_rtu_init(void);
esp_err_t modbus_rtu_read_registers(uint8_t slave, uint16_t addr,
                                     uint16_t count, uint16_t *out);
esp_err_t modbus_rtu_write_register(uint8_t slave, uint16_t addr,
                                     uint16_t value);  // Phase 3
```

### Step 2.3 — `wrg2_driver` Component

**`wrg2_registers.h`** (addresses TBD from Meltem PDF):
```c
#define WRG2_REG_TEMP_SUPPLY       0x0000  // Supply air temp (x10 °C) — VERIFY
#define WRG2_REG_TEMP_EXTRACT      0x0001  // Extract air temp (x10 °C) — VERIFY
#define WRG2_REG_FAN_SPEED         0x0002  // Fan speed — VERIFY
#define WRG2_REG_BYPASS_STATE      0x0003  // 0=closed, 1=open — VERIFY
#define WRG2_REG_OPERATING_MODE    0x0004  // Operating mode — VERIFY
#define WRG2_REG_FAN_LEVEL_SET     0x0010  // Writable: fan level — VERIFY
#define WRG2_REG_BYPASS_SET        0x0011  // Writable: bypass — VERIFY
#define WRG2_REG_MODE_SET          0x0012  // Writable: mode — VERIFY
```

**`wrg2_data_t` struct:**
```c
typedef struct {
    float    temp_supply;
    float    temp_extract;
    uint16_t fan_speed;
    bool     bypass_open;
    uint16_t operating_mode;
} wrg2_data_t;
```

`wrg2_read_all()` reads a contiguous block in a single FC03 request, then maps raw values (applying scaling factors from the PDF) to the struct.

### Step 2.4 — Polling Task (Phase 2 — Real Data)

Replace the mock `polling_task` with one that calls `wrg2_read_all()` and publishes real register values to MQTT status topics.

### Phase 2 Completion Checklist

- [ ] Raw Modbus frames verified (logic analyser or UART monitor)
- [ ] FC03 response CRC passes, register values are sane
- [ ] Temperature readings match a reference thermometer
- [ ] Fan speed matches unit display (if readable)
- [ ] 3× retry logic exercised by disconnecting RS485 cable
- [ ] Values appear in HA sensor entities

---

## 5. Phase 3 — Modbus Write / Full Control

### Step 3.1 — FC06 Write Single Register

Add to `modbus_rtu.c`. Request is 8 bytes: `[slave, 0x06, addr_hi, addr_lo, val_hi, val_lo, crc_lo, crc_hi]`. Response is an echo of the request — validate it matches.

### Step 3.2 — MQTT Control Topic Handler

Subscribe at `MQTT_EVENT_CONNECTED` (QoS 1):
```
wrg2/control/fan_level/set
wrg2/control/bypass/set
wrg2/control/mode/set
wrg2/ota/trigger
```

**Important:** Do not block in the MQTT event callback. Post to a `QueueHandle_t`:
```c
typedef struct {
    uint8_t  type;   // CTRL_FAN, CTRL_BYPASS, CTRL_MODE
    uint16_t value;
} ctrl_cmd_t;
```

A `control_task` (priority 6) dequeues commands and calls `wrg2_set_*()`.

### Step 3.3 — Read-After-Write Confirmation

After every write, immediately read back the register to confirm acceptance. Publish confirmed value to the status topic so HA always reflects actual device state.

### Phase 3 Completion Checklist

- [ ] Fan level change from HA updates WRG-II immediately
- [ ] Bypass ON/OFF from HA toggles the valve
- [ ] Mode select in HA updates WRG-II mode register
- [ ] Read-after-write confirms values
- [ ] Concurrent Modbus access (polling + control) is safe via mutex
- [ ] Full E2E: HA → MQTT → Modbus write → read back → MQTT → HA

---

## 6. Open Items / Risks

### Critical — Must Resolve Before Phase 2

| # | Item | Resolution |
|---|------|------------|
| 1 | **Meltem Register Map** | Run `pdftotext "docs/Meltem BA-IA_M-WRG-II_P-M_E-M.pdf" -` to extract register addresses, scaling factors, and valid values. Populate `wrg2_registers.h`. |
| 2 | **DE/RE GPIO Assignment** | Confirm a free GPIO against `docs/XIAO_ESP32S3_Sense_Pinout.xlsx`. Candidate: GPIO2 (D0). |
| 3 | **UART Port Conflict** | GPIO43/44 = default UART0 (console). Set `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` and use `UART_NUM_1` remapped to these pins. |

### Risks

| # | Risk | Mitigation |
|---|------|------------|
| 4 | WRG-II Modbus slave address ≠ 1 | Make slave ID configurable via NVS (already in plan) |
| 5 | OTA partition table mismatch on first flash | Full `idf.py erase-flash` before first OTA-capable flash |
| 6 | Modbus response timeout too tight/loose | Monitor actual response times with logic analyser in Phase 2; tune `uart_read_bytes()` timeout |
| 7 | WRG-II operating mode values unknown | Extract valid mode values from Meltem PDF before populating HA `select` options |
| 8 | MQTT broker requires TLS | Start Phase 1 with plain `mqtt://`; add TLS as separate step if needed |

---
