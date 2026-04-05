#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * All registers from Meltem M-WRG-II Modbus map (datasheet §16.5).
 *
 * Floats: two consecutive holding registers, word-swapped
 *   (r[1] is the high IEEE-754 word, r[0] is the low word).
 * UINT8 fields occupy one 16-bit register, value in the low byte.
 * Register addresses are LITERAL PDU values (41000 not 999).
 */
typedef struct {
    /* ── Temperatures (°C) ── */
    float    temp_zuluft;         /* 41009-41010  Zulufttemperatur    */
    float    temp_abluft;         /* 41004-41005  Ablufttemperatur    */
    float    temp_fortluft;       /* 41000-41001  Fortlufttemperatur  */
    float    temp_aussenluft;     /* 41002-41003  Außenlufttemperatur */

    /* ── Humidity (%) ── */
    uint16_t feuchte_abluft;      /* 41006  Feuchte Abluft */
    uint16_t feuchte_zuluft;      /* 41011  Feuchte Zuluft */

    /* ── Air quality ── */
    uint16_t co2_extract;         /* 41007  CO2 Abluft, ppm */

    /* ── Status flags ── */
    uint8_t  error_flag;          /* 41016  0=OK, 1=error */
    uint8_t  filter_due;          /* 41017  0=OK, 1=filter change needed */
    uint8_t  frost_active;        /* 41018  0=inactive, 1=frost protection on */

    /* ── Actual fan throughputs (m³/h) ── */
    uint8_t  fan_exhaust_m3h;     /* 41020  Lüftungsstufe Abluft */
    uint8_t  fan_supply_m3h;      /* 41021  Lüftungsstufe Zuluft */

    /* ── Maintenance ── */
    uint16_t filter_days_left;    /* 41027  days until filter change */
    uint32_t hours_device;        /* 41030-41031  device operating hours */
    uint32_t hours_motors;        /* 41032-41033  motor operating hours */

    /* ── Operating mode (41120) ──
     *   1 = off
     *   2 = regulated (sub-mode via fan_target: 16=auto, 112=humidity, 144=CO2)
     *   3 = manual balanced
     *   4 = manual unbalanced                                                */
    uint8_t  mode;
    uint8_t  fan_target_supply;   /* 41121  0-200 → 0-100 m³/h */
    uint8_t  fan_target_exhaust;  /* 41122  0-200 → 0-100 m³/h (unbalanced) */

    /* ── Configuration registers (42xxx) ── */
    uint8_t  cfg_hum_setpoint;    /* 42000  Rel. Feuchte Startpunkt  %  (40-80, def 60)  */
    uint8_t  cfg_hum_fan_min;     /* 42001  Min. Lüftungsstufe Feuchte  %  (def 10)       */
    uint8_t  cfg_hum_fan_max;     /* 42002  Max. Lüftungsstufe Feuchte  %  (def 60)       */
    uint16_t cfg_co2_setpoint;    /* 42003  CO2 Startpunkt  ppm  (500-1200, def 800)      */
    uint8_t  cfg_co2_fan_min;     /* 42004  Min. Lüftungsstufe CO2  %  (def 10)           */
    uint8_t  cfg_co2_fan_max;     /* 42005  Max. Lüftungsstufe CO2  %  (def 60)           */
    uint8_t  cfg_ext_fan_level;   /* 42007  Lüftungsstufe Ext. Eingang  %  (def 60)       */
    uint8_t  cfg_ext_on_delay;    /* 42008  Einschaltverzögerung Ext.  min  (def 1)       */
    uint8_t  cfg_ext_off_delay;   /* 42009  Nachlaufzeit Ext.  min  (def 15)              */
} wrg2_data_t;

/** Initialize UART/Modbus driver. Must be called before wrg2_read_all(). */
esp_err_t wrg2_driver_init(uint8_t slave_id, uint32_t baud,
                            int gpio_tx, int gpio_rx, int gpio_rts);

/**
 * Read all registers from the M-WRG-II (9 burst reads, retries 3× each).
 * On success the result is also cached for wrg2_get_last_data().
 */
esp_err_t wrg2_read_all(wrg2_data_t *data);

/**
 * Copy the last successfully read data into *out.
 * Returns false if no successful read has happened yet.
 */
bool wrg2_get_last_data(wrg2_data_t *out);

/**
 * Set operating mode. Writes 41120 (mode), 41121 (fan_target), then
 * 41132 (commit=0) in sequence as required by the datasheet.
 *
 * Common mode+fan_target combinations:
 *   mode=1, fan=0   → Off
 *   mode=2, fan=112 → Humidity controlled  (P-M-F / E-M-F)
 *   mode=2, fan=144 → CO2 controlled       (P-M-FC / E-M-FC only)
 *   mode=2, fan=16  → Automatic            (P-M-FC / E-M-FC only)
 *   mode=3, fan=N   → Manual balanced, N ∈ [0,200] → [0,100] m³/h
 *
 * Returns ESP_OK only if all three writes succeed.
 */
esp_err_t wrg2_set_mode(uint8_t mode, uint8_t fan_target);

/**
 * Set balanced manual fan speed. Equivalent to wrg2_set_mode(3, m3h*2).
 * m3h is clamped to [0, 100].
 */
esp_err_t wrg2_set_fan_level(uint8_t m3h);

/**
 * Set unbalanced manual fan speeds (§16.7.2).
 * Writes 41120=4, 41121=supply*2, 41122=exhaust*2, 41132=0.
 * Both arguments are clamped to [0, 100] m³/h.
 */
esp_err_t wrg2_set_mode_unbalanced(uint8_t supply_m3h, uint8_t exhaust_m3h);

/**
 * Write a single configuration register (42xxx range).
 * Thin wrapper around modbus_rtu_write_reg for use by the web control page.
 * Returns ESP_OK on success.
 */
esp_err_t wrg2_write_config(uint16_t addr, uint16_t value);
