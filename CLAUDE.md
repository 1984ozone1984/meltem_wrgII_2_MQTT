# WRG2MQTT — Claude Code Session Memory

## Project
ESP-IDF v5.4 firmware for **Seeed XIAO ESP32-S3** that bridges a **Meltem M-WRG-II** heat-recovery ventilation unit (Modbus RTU) to **Home Assistant** via MQTT.

GitHub: `git@github.com:1984ozone1984/meltem_wrgII_2_MQTT.git`

---

## Hardware

| Item | Detail |
|------|--------|
| MCU | Seeed XIAO ESP32-S3 (ESP32-S3, 8 MB PSRAM, PCB antenna) |
| Flash device | `/dev/ttyACM0` (USB JTAG/serial, ID 303a:1001) |
| RS-485 | MAX485 on UART_NUM_1, GPIO43 TX / GPIO44 RX |
| DE/RE pin | TBD — candidate GPIO2 (D0), confirm against pinout before Phase 2 |
| Baud | 9600, 8E1 |
| Slave ID | 1 (default, NVS-configurable) |

**Antenna gotcha:** XIAO ESP32-S3 has a solder jumper selecting PCB trace vs IPEX connector. Must be on **OB** (on-board) pad or WiFi AP is invisible.

**Flash commands:**
```bash
# Build
idf.py build

# Flash (port may be busy — kill monitor first)
idf.py -p /dev/ttyACM0 flash

# Monitor
idf.py -p /dev/ttyACM0 monitor

# Erase NVS only (useful for clean credential reset)
python3 -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 erase_region 0x9000 0x6000

# Flash from build/ dir if idf.py flash fails due to busy port
cd build && python3 -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 8MB --flash_freq 80m \
  0x0 bootloader/bootloader.bin 0x10000 wrg2mqtt.bin \
  0x8000 partition_table/partition-table.bin 0xe000 ota_data_initial.bin
```

---

## Phase Status

### ✅ Phase 1 — Infrastructure (commit 983e70c)

All components build and run. Verified on hardware.

| Component | File | Status |
|-----------|------|--------|
| `system_core` | NVS flash init + task watchdog | ✅ |
| `config_manager` | NVS namespace `wrg2_cfg`, hostname/WiFi/MQTT/Modbus config | ✅ |
| `wifi_manager` | STA → AP fallback, mDNS `hostname.local`, AT country code, full TX power | ✅ |
| `config_server` | HTTP portal port 80, hostname/WiFi/MQTT forms | ✅ |
| `mqtt_manager` | MQTT client, LWT `wrg2/availability`, HA discovery on connect | ✅ |
| `ha_discovery` | 6 HA entities published on MQTT connect | ✅ |
| `ota_manager` | HTTP OTA via `wrg2/ota/trigger` MQTT topic | ✅ |
| `app_main` | Mock polling task (21.5°C, 18°C, fan=2, bypass=OFF, mode=auto) | ✅ |

**Provisioning flow:**
1. Fresh device → AP `WRG2-Setup` (open, 192.168.4.1) appears immediately
2. Connect + open `http://192.168.4.1/config` → set hostname, WiFi, MQTT
3. Save WiFi → reboots into STA, connects, MQTT online, HA discovers entities
4. Device reachable at `http://wrg2mqtt.local` (default hostname)

**WiFi AP visibility fix (learned the hard way):**
- `esp_wifi_set_country_code("AT", false)` — `false` = don't scan beacons for country, apply AT rules immediately → full TX power
- `esp_wifi_set_max_tx_power(78)` after `esp_wifi_start()` — IDF 5.x defaults to "world safe" reduced power
- mDNS requires `espressif/mdns` managed component (not built into IDF 5.4) → declared in `main/idf_component.yml`

### 🔲 Phase 2 — Modbus Read

**Pre-conditions (must do before coding):**
1. Extract register map from `docs/Meltem BA-IA_M-WRG-II_P-M_E-M.pdf`
   ```bash
   pdftotext "docs/Meltem BA-IA_M-WRG-II_P-M_E-M.pdf" -
   ```
2. Confirm DE/RE GPIO — check `docs/XIAO_ESP32S3_Sense_Pinout.xlsx`, candidate GPIO2
3. Console already moved to USB-JTAG (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`), so GPIO43/44 are free for UART_NUM_1

**Components to create:**
- `components/modbus_rtu/` — direct UART driver (not esp-modbus), FC03 read, CRC16, 3× retry, bus mutex
- `components/wrg2_driver/` — register map, `wrg2_read_all()`, `wrg2_data_t` struct

**Replace** mock `polling_task` in `app_main.c` with real `wrg2_read_all()` call.

### 🔲 Phase 3 — Modbus Write / Full Control

- FC06 write single register
- MQTT control topics → `QueueHandle_t` → `control_task`
- Read-after-write confirmation

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
| `mb_baud` | u32 | `9600` | |
| `poll_ivl` | u32 | `10` | seconds |

---

## MQTT Topics

| Topic | Direction | Payload |
|-------|-----------|---------|
| `wrg2/availability` | pub | `online` / `offline` (LWT) |
| `wrg2/status/temperature_supply` | pub | float string |
| `wrg2/status/temperature_extract` | pub | float string |
| `wrg2/status/fan_speed` | pub | int string |
| `wrg2/status/bypass_state` | pub | `ON` / `OFF` |
| `wrg2/status/operating_mode` | pub | `auto` / `manual` / `boost` / `away` |
| `wrg2/control/fan_level/set` | sub | Phase 3 |
| `wrg2/control/bypass/set` | sub | Phase 3 |
| `wrg2/control/mode/set` | sub | Phase 3 |
| `wrg2/ota/trigger` | sub | firmware URL |

---

## Key Design Decisions

- **Direct UART Modbus** (not `esp-modbus`) — single slave, two function codes, ~250 lines, fully controllable
- **UART_NUM_1** on GPIO43/44, `UART_MODE_RS485_HALF_DUPLEX` for automatic DE/RE via hardware RTS
- **No credentials in source** — all secrets NVS-only, never logged
- **Config portal always running** — available in both STA and AP mode on port 80
- **MQTT reconnect** handles broker restarts — `ha_discovery_publish()` called on every `MQTT_EVENT_CONNECTED`
