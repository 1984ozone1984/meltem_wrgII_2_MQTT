# WRG2MQTT — Claude Code Session Memory

## Project
ESP-IDF v5.4 firmware for **Seeed XIAO ESP32-S3** that bridges a **Meltem M-WRG-II P-M-F** heat-recovery ventilation unit (Modbus RTU) to **Home Assistant** via MQTT.

GitHub: `git@github.com:1984ozone1984/meltem_wrgII_2_MQTT.git`

---

## Hardware

| Item | Detail |
|------|--------|
| MCU | Seeed XIAO ESP32-S3 (ESP32-S3, 8 MB flash, PCB antenna) |
| Flash device | `/dev/ttyACM0` (USB JTAG/serial, ID 303a:1001) |
| RS-485 | MAX485 auto-direction module on UART_NUM_1, GPIO43 TX / GPIO44 RX |
| Baud | 19200, 8E1 (M-WRG-II factory default) |
| Slave ID | 1 (default, NVS-configurable) |
| Variant | P-M-F — no CO2 sensor (returns 0x7FFF), no automatic mode, humidity-regulated |

**Antenna:** XIAO ESP32-S3 has a solder jumper — must be on **OB** (on-board) pad or WiFi AP is invisible.

**Console:** moved to USB-JTAG (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`), so GPIO43/44 are free for UART_NUM_1.

**Flash commands:**
```bash
idf.py build
idf.py -p /dev/ttyACM0 flash
idf.py -p /dev/ttyACM0 monitor

# Erase NVS (clean credential reset)
python3 -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 erase_region 0x9000 0x6000

# Flash directly if port busy (monitor running)
cd build && python3 -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 8MB --flash_freq 80m \
  0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin \
  0xe000 ota_data_initial.bin 0x10000 wrg2mqtt.bin
```

---

## Implementation Status — ALL PHASES COMPLETE ✅

### Components

| Component | Status | Notes |
|-----------|--------|-------|
| `system_core` | ✅ | NVS init, task watchdog 60s timeout, trigger_panic=true |
| `config_manager` | ✅ | NVS namespace `wrg2_cfg`, all fields including pub_interval |
| `wifi_manager` | ✅ | STA → AP fallback, mDNS, AT country code, full TX power, infinite reconnect + supervisor self-reboot |
| `config_server` | ✅ | HTTP portal port 80, all config forms + control page |
| `modbus_rtu` | ✅ | Direct UART driver, FC03 read, FC06 write, CRC16, 3× retry, mutex |
| `wrg2_driver` | ✅ | 9-burst register map, wrg2_read_all(), wrg2_set_mode*(), wrg2_write_config() |
| `mqtt_manager` | ✅ | MQTT client, LWT, subscriptions, HA discovery on every connect |
| `ha_discovery` | ✅ | 26 entities, entity_category grouping, stale entity cleanup |
| `ota_manager` | ✅ | HTTP OTA via wrg2/ota/trigger |
| `app_main` | ✅ | polling_task + control_task, publish interval gating |

### Key behaviours
- **Polling task**: reads Modbus every `poll_interval` seconds; publishes MQTT only when `pub_interval` seconds have elapsed. Control writes always trigger an immediate read-back publish.
- **Control queue**: depth-8 FreeRTOS queue decouples MQTT event handler from Modbus task. `CMD_WRITE_REG` handles all config register writes generically.
- **Watchdog**: polling_task subscribed to TWDT (60s). Modbus UART hang → panic-reset.
- **Reboot**: via web UI button, MQTT `wrg2/control/reboot`, or HA button entity.
- **WiFi AP fix**: `esp_wifi_set_country_code("AT", false)` + `esp_wifi_set_max_tx_power(78)` needed for full TX power in IDF 5.x.
- **WiFi long-run stability** (fixes "WiFi dies after 12–24 h"): disconnect handler does 10 fast retries, then — once provisioned (got IP at least once) — **never gives up**; the `wifi_sup` supervisor task paces reconnects every 30 s and `esp_restart()`s after 600 s offline. Old code set `WIFI_FAIL_BIT` and stopped reconnecting forever after 10 drops. `s_provisioned` flag gates this so bad credentials still fall back to AP mode at boot.
- **MQTT outbox cap**: `.outbox.limit = 8 KB` + `.session.keepalive = 30` prevent unbounded heap growth from queued QoS-1 retained publishes during a WiFi/broker outage (the "memory overload" failure mode).

---

## NVS Layout

Namespace: `wrg2_cfg`

| Key | Type | Default | Notes |
|-----|------|---------|-------|
| `hostname` | str | `wrg2mqtt` | mDNS name |
| `wifi_ssid` | str | `` | |
| `wifi_pass` | str | `` | |
| `mqtt_url` | str | `` | e.g. `mqtt://192.168.1.x:1883` |
| `mqtt_user` | str | `` | |
| `mqtt_pass` | str | `` | |
| `mb_slave_id` | u8 | `1` | |
| `mb_baud` | u32 | `19200` | |
| `poll_ivl` | u32 | `10` | Modbus read interval (seconds) |
| `pub_ivl` | u32 | `30` | MQTT publish interval (seconds, ≥ poll_ivl) |

---

## Modbus Register Map

**M-WRG-II uses literal PDU addresses** (not standard address-1 convention).
Register map has gaps — burst reads across undefined addresses return exception 0x02.

| Burst | Registers | Contents |
|-------|-----------|----------|
| A | 41000–41007 | Exhaust/outdoor/extract temps (Float32), extract humidity, CO2 |
| B | 41009–41011 | Supply temp (Float32), supply humidity |
| C | 41016–41018 | Error flag, filter due, frost active |
| D | 41020–41021 | Actual fan throughputs (m³/h) |
| E | 41027 | Filter days remaining |
| F | 41030–41033 | Device and motor operating hours (UINT32) |
| G | 41120–41122 | Operating mode, supply fan target, exhaust fan target |
| H | 42000–42005 | Humidity/CO2 setpoints and fan range config |
| I | 42007–42009 | External input fan level, on/off delay config |

**Float/UINT32 word order:** BYTEORDER_LITTLE_SWAP — register N = low word, N+1 = high word.

**Write sequences (FC06, commit reg 41132=0 always last):**
- Off: 41120=1, 41132=0
- Humidity: 41120=2, 41121=112, 41132=0
- Balanced: 41120=3, 41121=m³/h×2, 41132=0
- Unbalanced: 41120=4, 41121=supply×2, 41122=exhaust×2, 41132=0
- Config: write single register (42000–42009), no commit needed

---

## MQTT Topics

### Published (every pub_interval, retained)

| Topic | Payload |
|-------|---------|
| `wrg2/availability` | `online` / `offline` (LWT) |
| `wrg2/status/temperature_supply` | float °C |
| `wrg2/status/temperature_extract` | float °C |
| `wrg2/status/temperature_exhaust` | float °C |
| `wrg2/status/temperature_outdoor` | float °C |
| `wrg2/status/humidity_extract` | integer % |
| `wrg2/status/humidity_supply` | integer % |
| `wrg2/status/fan_supply_m3h` | integer m³/h |
| `wrg2/status/fan_exhaust_m3h` | integer m³/h |
| `wrg2/status/fan_supply_target` | integer m³/h |
| `wrg2/status/fan_exhaust_target` | integer m³/h |
| `wrg2/status/operating_mode` | `off`/`humidity`/`manual`/`manual_unbal` |
| `wrg2/status/error` | `ON`/`OFF` |
| `wrg2/status/filter_due` | `ON`/`OFF` |
| `wrg2/status/frost_active` | `ON`/`OFF` |
| `wrg2/status/filter_days_left` | integer days |
| `wrg2/status/hours_device` | integer h |
| `wrg2/status/hours_motors` | integer h |
| `wrg2/config/hum_setpoint` | integer % |
| `wrg2/config/hum_fan_min` | integer % |
| `wrg2/config/hum_fan_max` | integer % |
| `wrg2/config/ext_fan_level` | integer % |
| `wrg2/config/ext_on_delay` | integer min |
| `wrg2/config/ext_off_delay` | integer min |

### Subscribed (control)

| Topic | Effect |
|-------|--------|
| `wrg2/control/mode/set` | `"off"` or `"humidity"` |
| `wrg2/control/fan_balanced/set` | integer 0–100 m³/h → mode 3 |
| `wrg2/control/fan_unbal_supply/set` | integer 0–100 m³/h → mode 4 |
| `wrg2/control/fan_unbal_exhaust/set` | integer 0–100 m³/h → mode 4 |
| `wrg2/config/hum_setpoint/set` | write reg 42000 |
| `wrg2/config/hum_fan_min/set` | write reg 42001 |
| `wrg2/config/hum_fan_max/set` | write reg 42002 |
| `wrg2/config/ext_fan_level/set` | write reg 42007 |
| `wrg2/config/ext_on_delay/set` | write reg 42008 |
| `wrg2/config/ext_off_delay/set` | write reg 42009 |
| `wrg2/control/reboot` | esp_restart() after 200ms |
| `wrg2/ota/trigger` | HTTP OTA firmware update |

---

## HA Discovery Entities (26 total)

**Main card:** 4 temp sensors, extract/supply humidity, supply/exhaust fan speed, Switch Off button, Humidity Control button, Manual Balanced Fan number, Unbalanced Supply Fan number, Unbalanced Exhaust Fan number

**Diagnostic section:** Operating Mode sensor, Error binary_sensor, Filter Due binary_sensor, Frost Protection binary_sensor, Filter Days Remaining sensor, Device Operating Hours sensor, Motor Operating Hours sensor, Reboot button

**Configuration section:** Humidity Start Setpoint number (42000), Humidity Min Fan Level number (42001), Humidity Max Fan Level number (42002), Ext Input Fan Level number (42007), Ext Input On Delay number (42008), Ext Input Off Delay number (42009)

Stale entities deleted on every connect: Phase 1 topics (wrg2_supply_temp, wrg2_extract_temp, wrg2_fan_speed, wrg2_bypass, wrg2_fan) + replaced Phase 3 topics (wrg2_mode select, wrg2_fan_level, wrg2_fan_exhaust_level).

---

## Key Design Decisions

- **Direct UART Modbus** (not `esp-modbus`) — single slave, two function codes, fully controllable
- **UART_MODE_UART** (not RS485_HALF_DUPLEX) — MAX485 module handles DE/RE automatically
- **No credentials in source** — all secrets NVS-only, never logged
- **Config portal always running** — port 80, both STA and AP mode
- **MQTT reconnect** — `ha_discovery_publish()` called on every `MQTT_EVENT_CONNECTED`
- **publish interval ≥ poll interval** — enforced in config_server POST handler
