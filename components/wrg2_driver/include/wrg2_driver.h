#pragma once
#include <stdint.h>
#include "esp_err.h"

/**
 * Live sensor and status data read from the M-WRG-II via Modbus.
 *
 * Temperature fields map to Modbus Float32 registers (two consecutive
 * holding registers, word-swapped: r[1] is high word, r[0] is low word).
 * UINT8 fields are stored as the low byte of a 16-bit holding register.
 *
 * Air-path terminology:
 *   supply  (Zuluft)    — conditioned air delivered into the house
 *   extract (Abluft)    — stale air drawn from the house
 *   exhaust (Fortluft)  — extract air expelled to the outside
 *   outdoor (Außenluft) — fresh air drawn in from outside
 */
typedef struct {
    /* Temperatures (°C) */
    float    temp_supply;         /* Zuluft     reg 41009-41010 */
    float    temp_extract;        /* Abluft     reg 41004-41005 */
    float    temp_exhaust;        /* Fortluft   reg 41000-41001 */
    float    temp_outdoor;        /* Außenluft  reg 41002-41003 */

    /* Humidity (%) */
    uint16_t humidity_extract;    /* Feuchte Abluft  reg 41006 */
    uint16_t humidity_supply;     /* Feuchte Zuluft  reg 41011 */

    /* Air quality */
    uint16_t co2_extract;         /* CO2 Abluft, ppm reg 41007 */

    /* Status flags */
    uint8_t  error_flag;          /* 0=OK, 1=error     reg 41016 */
    uint8_t  filter_due;          /* 1=change needed   reg 41017 */
    uint8_t  frost_active;        /* 1=frost protection reg 41018 */

    /* Actual fan speeds (m³/h) */
    uint8_t  fan_exhaust_m3h;     /* Lüftungsstufe Abluft  reg 41020 */
    uint8_t  fan_supply_m3h;      /* Lüftungsstufe Zuluft  reg 41021 */

    /* Maintenance */
    uint16_t filter_days_left;    /* reg 41027 */
    uint32_t hours_device;        /* reg 41030-41031 */
    uint32_t hours_motors;        /* reg 41032-41033 */

    /* Operating mode (reg 41120) */
    /*   1 = off           */
    /*   2 = auto (humidity/CO2 regulation) */
    /*   3 = balanced manual fan level      */
    /*   4 = unbalanced manual fan level    */
    uint8_t  mode;

    /* Target fan levels, 0-200 (maps to 0-100 m³/h) */
    uint8_t  fan_target_supply;   /* reg 41121 */
    uint8_t  fan_target_exhaust;  /* reg 41122 */
} wrg2_data_t;

/**
 * Initialize the Modbus RTU driver and configure UART.
 * Must be called before wrg2_read_all().
 */
esp_err_t wrg2_driver_init(uint8_t slave_id, uint32_t baud);

/**
 * Read all sensor and status data from the M-WRG-II.
 * Performs two Modbus burst reads. Each read retries up to 3 times.
 * Returns ESP_OK on success.
 */
esp_err_t wrg2_read_all(wrg2_data_t *data);
