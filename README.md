# WRG2MQTT

ESP-IDF v5.4 firmware for a **Seeed XIAO ESP32-S3** that bridges a **Meltem M-WRG-II** heat-recovery ventilation unit to **Home Assistant** via MQTT over WiFi.

Communicates with the ventilation unit over Modbus RTU (RS-485) and exposes all sensors and controls as MQTT topics with full Home Assistant auto-discovery.

---

## Hardware

| Item | Detail |
|------|--------|
| MCU | Seeed XIAO ESP32-S3 |
| RS-485 adapter | MAX485 breakout module (auto-direction) |
| UART | UART_NUM_1 — GPIO43 TX, GPIO44 RX |
| Ventilation unit | Meltem M-WRG-II P-M / P-M-F (Modbus RTU, 19200 8E1, slave 1) |

Wiring:
- MAX485 TX (DI) → GPIO43
- MAX485 RX (RO) → GPIO44
- MAX485 A/B → RS-485 terminals on the WRG-II unit

---

## Features

- **Sensor readout** — 4 temperatures, 2 humidity sensors, actual fan throughputs, error/filter/frost flags, operating hours, filter days remaining
- **Full control** — Off, Humidity (auto) mode, balanced manual fan, unbalanced supply/exhaust fan independently
- **Home Assistant auto-discovery** — entities appear automatically on MQTT connect; stale entities from previous firmware versions are cleaned up
- **Web portal** — always-on HTTP config and control page at `http://<hostname>.local`
- **OTA updates** — firmware update via MQTT trigger topic
- **WiFi provisioning** — falls back to AP mode (`WRG2-Setup`) on first boot or failed STA connection

---

## Home Assistant Entities

| Entity | Type | Topic |
|--------|------|-------|
| Supply Air Temperature | sensor | `wrg2/status/temperature_supply` |
| Extract Air Temperature | sensor | `wrg2/status/temperature_extract` |
| Exhaust Air Temperature | sensor | `wrg2/status/temperature_exhaust` |
| Outdoor Air Temperature | sensor | `wrg2/status/temperature_outdoor` |
| Extract Air Humidity | sensor | `wrg2/status/humidity_extract` |
| Supply Air Humidity | sensor | `wrg2/status/humidity_supply` |
| Supply Fan Speed | sensor (m³/h) | `wrg2/status/fan_supply_m3h` |
| Exhaust Fan Speed | sensor (m³/h) | `wrg2/status/fan_exhaust_m3h` |
| Error | binary_sensor | `wrg2/status/error` |
| Filter Due | binary_sensor | `wrg2/status/filter_due` |
| Frost Protection | binary_sensor | `wrg2/status/frost_active` |
| Operating Mode | sensor | `wrg2/status/operating_mode` |
| Switch Off | button | `wrg2/control/mode/set` → `"off"` |
| Humidity Control | button | `wrg2/control/mode/set` → `"humidity"` |
| Manual Balanced Fan | number (0–100 m³/h) | `wrg2/control/fan_balanced/set` |
| Unbalanced Supply Fan | number (0–100 m³/h) | `wrg2/control/fan_unbal_supply/set` |
| Unbalanced Exhaust Fan | number (0–100 m³/h) | `wrg2/control/fan_unbal_exhaust/set` |

---

## MQTT Topics

| Topic | Direction | Notes |
|-------|-----------|-------|
| `wrg2/availability` | pub | `online` / `offline` (LWT) |
| `wrg2/status/*` | pub | All sensor values, published every poll interval |
| `wrg2/control/mode/set` | sub | `"off"` or `"humidity"` |
| `wrg2/control/fan_balanced/set` | sub | Integer 0–100 m³/h → balanced mode (reg 41120=3) |
| `wrg2/control/fan_unbal_supply/set` | sub | Integer 0–100 m³/h → unbalanced mode (reg 41120=4), exhaust preserved |
| `wrg2/control/fan_unbal_exhaust/set` | sub | Integer 0–100 m³/h → unbalanced mode (reg 41120=4), supply preserved |
| `wrg2/ota/trigger` | sub | HTTP URL of firmware `.bin` for OTA update |

---

## Configuration

On first boot (or when WiFi credentials are missing), the device starts a WiFi access point:

- SSID: `WRG2-Setup` (open)
- IP: `192.168.4.1`
- Config page: `http://192.168.4.1/config`

After connecting to your network, the device is reachable at `http://<hostname>.local` (default: `http://wrg2mqtt.local`).

**Config options:**

| Field | Default | Notes |
|-------|---------|-------|
| Hostname | `wrg2mqtt` | mDNS name |
| WiFi SSID / Password | — | |
| MQTT URL | — | e.g. `mqtt://192.168.1.10:1883` |
| MQTT Username / Password | — | Optional |
| Modbus Slave ID | `1` | |
| Modbus Baud Rate | `19200` | |
| Poll Interval | `10` | Seconds |

All settings are stored in ESP32 NVS flash — no credentials in firmware.

---

## Building

Requires ESP-IDF v5.4.

```bash
# Set up IDF environment
. ~/esp/esp-idf/export.sh

# Build
idf.py build

# Flash
idf.py -p /dev/ttyACM0 flash

# Monitor
idf.py -p /dev/ttyACM0 monitor

# Erase NVS (reset all settings)
python3 -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 erase_region 0x9000 0x6000
```

If the port is busy (monitor running), flash directly:

```bash
cd build && python3 -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 8MB --flash_freq 80m \
  0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin \
  0xe000 ota_data_initial.bin 0x10000 wrg2mqtt.bin
```

---

## Register Map (M-WRG-II)

The firmware reads the following Modbus holding registers (FC03) using the device's literal PDU addresses:

| Burst | Registers | Contents |
|-------|-----------|----------|
| A | 41000–41007 | Exhaust/outdoor/extract temps (Float32), extract humidity, CO2 |
| B | 41009–41011 | Supply temp (Float32), supply humidity |
| C | 41016–41018 | Error flag, filter due, frost active |
| D | 41020–41021 | Actual fan throughputs (m³/h) |
| E | 41027 | Filter days remaining |
| F | 41030–41033 | Device and motor operating hours (UINT32) |
| G | 41120–41122 | Operating mode, supply fan target, exhaust fan target |
| H | 42000–42005 | Humidity and CO2 setpoints and fan range config |
| I | 42007–42009 | External input fan level, on/off delay config |

Write sequence (FC06):

- **Off**: 41120=1, 41132=0
- **Humidity (regulated)**: 41120=2, 41121=112, 41132=0
- **Balanced manual**: 41120=3, 41121=m³/h×2, 41132=0
- **Unbalanced manual**: 41120=4, 41121=supply×2, 41122=exhaust×2, 41132=0

Floats use little-endian word order (BYTEORDER_LITTLE_SWAP): register N is the low word, register N+1 is the high word.

---

## Variant Notes

**P-M-F / E-M-F variants** (humidity-controlled, no CO2 sensor):
- CO2 register returns `0x7FFF` — firmware treats this as "no sensor" and skips publishing the CO2 topic
- Mode 2 = humidity-regulated (no "automatic" mode available on this variant)
