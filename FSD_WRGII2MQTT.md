# Functional Specification Document (FSD)

## Project: WRGII2MQTT

**Version:** 1.0  
**Date:** 2026-04-01  
**Repository:** https://github.com/1984ozone1984/meltem_wrgII_2_MQTT

---

## 1. Overview

**Project Name:** WRGII2MQTT  
**Goal:** Read sensor data and control a Meltem WRG-II heat recovery ventilation unit via Modbus RTU (RS485), and expose it to Home Assistant over MQTT using an ESP32-based device.

The system bridges the proprietary RS485/Modbus interface of the Meltem WRG-II to the MQTT protocol over WiFi, enabling full integration with Home Assistant — including sensor readings, device control, and automatic entity discovery.

---

## 2. Hardware

### 2.1 Components

| Component | Description |
|-----------|-------------|
| Seeed Studio XIAO ESP32S3 | Microcontroller with WiFi |
| MAX485 Breakout Board | RS485 transceiver for Modbus RTU |
| Meltem WRG-II E-M-F | Heat recovery ventilation unit |

### 2.2 Pin Configuration

| Signal | ESP32S3 GPIO | XIAO Label |
|--------|-------------|------------|
| UART TX (to MAX485 DI) | GPIO43 | D6 |
| UART RX (from MAX485 RO) | GPIO44 | D7 |
| DE/RE (direction control) | TBD | TBD |
| GND | GND | GND |
| Power | USB-C (5V) or 3.3V pin | — |

### 2.3 Reference Documents

| Document | Path | Description |
|----------|------|-------------|
| Protocol Specification | `docs/Meltem BA-IA_M-WRG-II_P-M_E-M.PDF` | Meltem WRG-II Modbus specification |
| Controller Datasheet | `docs/esp32-s3_datasheet.PDF` | ESP32-S3 datasheet |
| Board Pinout | `docs/XIAO_ESP32S3_Sense_Pinout.xlsx` | XIAO ESP32S3 pin layout |

### 2.4 Interfaces

#### RS485 (UART/Modbus RTU)

| Parameter | Value |
|-----------|-------|
| Baud Rate | 9600 (configurable via NVS) |
| Parity | Even |
| Stop Bits | 1 |
| Data Bits | 8 |
| Direction Control | DE/RE pin (half-duplex) |

#### WiFi

| Parameter | Value |
|-----------|-------|
| Mode | Station (STA) |
| IP Assignment | DHCP |
| Protocol | 802.11 b/g/n (2.4 GHz) |

---

## 3. Software Architecture

### 3.1 Platform & Toolchain

- **Framework:** ESP-IDF (C)
- **Flashing / Debugging:** OTA (over-the-air) — USB is not used for flashing in production; all updates and log output are delivered wirelessly
- **Build System:** CMake via ESP-IDF

### 3.2 Module Overview

| Module | Responsibility |
|--------|---------------|
| `system_core` | Boot sequence, NVS init, watchdog, error handling |
| `wifi_manager` | WiFi connection, reconnection logic |
| `mqtt_client` | MQTT connect/publish/subscribe, reconnect logic |
| `modbus_rtu` | UART driver, Modbus RTU framing, CRC, retry/timeout |
| `wrgii_driver` | WRG-II register map, read/write abstraction |
| `ha_discovery` | Home Assistant MQTT discovery payload generation |
| `ota_manager` | OTA firmware update handling |
| `logger` | Unified logging over UART and optionally MQTT |

### 3.3 Data Flow

```
Boot
 └─► NVS Config Load
      └─► WiFi Connect
           └─► MQTT Connect
                └─► HA Discovery Publish
                     └─► Modbus Polling Loop
                          └─► WRG-II Register Read
                               └─► MQTT Publish (status topics)
                                    └─► Home Assistant Update

MQTT Subscribe (control topics)
 └─► Modbus Write Register
      └─► WRG-II Actuate
```

---

## 4. Functional Requirements

### 4.1 Modbus RTU

| Requirement | Detail |
|-------------|--------|
| Function Code Read | 0x03 — Read Holding Registers |
| Function Code Write | 0x06 — Write Single Register |
| Polling Interval | 5–30 seconds (configurable) |
| Retry Count | 3 retries on timeout or CRC error |
| Timeout | 1 second per request |
| Slave ID | Configurable via NVS |

### 4.2 MQTT

#### Topic Structure

```
wrgii/
  status/
    temperature_supply        # Supply air temperature (°C)
    temperature_extract       # Extract air temperature (°C)
    fan_speed                 # Current fan speed (RPM or level)
    bypass_state              # Bypass valve state (open/closed)
    operating_mode            # Current operating mode
  control/
    fan_level/set             # Set fan level (0–4 or as per WRG-II spec)
    bypass/set                # Set bypass state (ON/OFF)
    mode/set                  # Set operating mode
  availability                # online / offline (LWT)
```

#### Payload Format

- Sensor values: plain numeric string or JSON object `{"value": 21.5, "unit": "°C"}`
- Control commands: plain value string (e.g. `"2"`, `"ON"`)
- Availability: `"online"` / `"offline"`

#### MQTT Connection

| Parameter | Detail |
|-----------|--------|
| QoS | 1 (at least once) |
| Retain | Yes for status and availability |
| LWT | `wrgii/availability` → `"offline"` |
| Auth | Optional (username/password via NVS) |
| TLS | Optional (configurable) |

### 4.3 Home Assistant Integration

#### MQTT Discovery

Discovery messages are published once on connect to enable automatic entity creation in Home Assistant.

```
homeassistant/sensor/wrgii_supply_temp/config
homeassistant/sensor/wrgii_extract_temp/config
homeassistant/sensor/wrgii_fan_speed/config
homeassistant/binary_sensor/wrgii_bypass/config
homeassistant/fan/wrgii_fan/config
homeassistant/select/wrgii_mode/config
```

#### Entities

| Entity | Type | Description |
|--------|------|-------------|
| Supply Air Temp | `sensor` | Temperature of supply air (°C) |
| Extract Air Temp | `sensor` | Temperature of extracted air (°C) |
| Fan Speed | `sensor` | Current fan speed |
| Bypass State | `binary_sensor` | Bypass valve open/closed |
| Fan Control | `fan` | Fan level control (read + write) |
| Operating Mode | `select` | Ventilation mode selection |

### 4.4 Control Functions

| Function | MQTT Topic | Modbus Action |
|----------|-----------|---------------|
| Set fan level | `wrgii/control/fan_level/set` | Write Single Register (0x06) |
| Set bypass | `wrgii/control/bypass/set` | Write Single Register (0x06) |
| Set operating mode | `wrgii/control/mode/set` | Write Single Register (0x06) |

---

## 5. Configuration

### 5.1 Storage

All runtime configuration is stored in **NVS (Non-Volatile Storage)** on the ESP32.

### 5.2 Configuration Parameters

| Parameter | Key | Default | Description |
|-----------|-----|---------|-------------|
| WiFi SSID | `wifi_ssid` | — | Network name |
| WiFi Password | `wifi_pass` | — | Network password |
| MQTT Broker URL | `mqtt_url` | — | e.g. `mqtt://192.168.1.x:1883` |
| MQTT Username | `mqtt_user` | — | Optional |
| MQTT Password | `mqtt_pass` | — | Optional |
| Modbus Slave ID | `mb_slave_id` | `1` | WRG-II device address |
| Modbus Baud Rate | `mb_baud` | `9600` | RS485 baud rate |
| Poll Interval (s) | `poll_interval` | `10` | Modbus polling interval |

---

## 6. OTA Updates

- OTA updates are delivered over WiFi using the ESP-IDF OTA API
- Triggered via a dedicated MQTT topic or HTTP endpoint (TBD)
- System logs over UART and optionally MQTT during normal operation
- USB is not used for flashing in the target deployment

---

## 7. Error Handling

| Scenario | Handling |
|----------|----------|
| Modbus timeout | Retry up to 3×, then log error and skip cycle |
| Modbus CRC error | Retry up to 3×, then log error |
| WiFi disconnection | Auto-reconnect with exponential backoff |
| MQTT disconnection | Auto-reconnect with exponential backoff |
| Watchdog timeout | System restart |

---

## 8. Non-Functional Requirements

### 8.1 Performance

| Metric | Target |
|--------|--------|
| RAM usage | < 70% of available heap |
| CPU load | < 50% average |
| Uptime | Continuous stable operation required |

### 8.2 Security

| Requirement | Detail |
|-------------|--------|
| No hardcoded credentials | All secrets stored in NVS only |
| MQTT authentication | Optional, configurable |
| TLS | Optional, configurable |

### 8.3 Logging

| Level | Usage |
|-------|-------|
| ERROR | Hardware failures, connection loss |
| WARN | Retries, unexpected states |
| INFO | Boot events, connection status, discovery |
| DEBUG | Modbus frames, MQTT payloads |

Output: UART (always), MQTT log topic (optional).

---

## 9. Implementation Phases

### Phase 1 — Infrastructure (No Modbus)

**Goal:** Validate ESP32 infrastructure independently of the WRG-II hardware.

- WiFi connect/reconnect
- MQTT connect/publish/subscribe
- OTA update mechanism
- NVS configuration read/write
- UART logging
- Watchdog
- Home Assistant discovery (with mock data)

### Phase 2 — Modbus Read

**Goal:** Establish Modbus RTU communication and read WRG-II status registers.

- UART/RS485 driver with DE/RE control
- Modbus RTU master implementation (Read Holding Registers 0x03)
- CRC validation, retry, timeout
- Read and publish: temperatures, fan speed, bypass state
- Validate register map against Meltem PDF specification

### Phase 3 — Modbus Write / Full Control

**Goal:** Enable full bidirectional control of the WRG-II.

- Write Single Register (0x06) implementation
- MQTT control topic handling
- Fan level, bypass, and mode control
- End-to-end Home Assistant integration test

---

## 10. Acceptance Criteria

| Criterion | Description |
|-----------|-------------|
| Stable WiFi | Device maintains WiFi connection and auto-reconnects |
| Stable MQTT | Device maintains MQTT connection and auto-reconnects |
| Correct Data | Sensor values match expected readings from WRG-II |
| HA Discovery | Entities appear automatically in Home Assistant |
| Control Works | Fan level and bypass can be set from Home Assistant |
| OTA Works | Firmware can be updated over WiFi |
| No Credentials in Code | All secrets loaded from NVS only |

---

## 11. Optional / Future Enhancements

- Web interface for NVS configuration (no re-flash required)
- Statistics dashboard (historical data, uptime, error counts)
- Support for additional Modbus registers as they are identified
- Support for multiple WRG-II units (multi-slave)

---

## 12. Open Items

- [ ] Complete register map from Meltem PDF (`docs/Meltem BA-IA_M-WRG-II_P-M_E-M.PDF`)
- [ ] Determine DE/RE GPIO pin assignment
- [ ] Define full set of supported WRG-II operating modes
- [ ] Decide OTA trigger mechanism (MQTT topic vs. HTTP)
- [ ] Home Assistant entity configuration optimization

---
