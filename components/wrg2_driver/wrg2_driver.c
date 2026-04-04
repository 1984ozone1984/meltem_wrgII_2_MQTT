#include "wrg2_driver.h"
#include "modbus_rtu.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "wrg2_driver";

/* UART hardware — XIAO ESP32-S3 + MAX485
 * TX  → GPIO43  RX  → GPIO44  DE/RE → GPIO2 (auto via UART RTS)
 */
#define WRG2_UART_PORT  UART_NUM_1
#define WRG2_GPIO_TX    43
#define WRG2_GPIO_RX    44
#define WRG2_GPIO_RTS   2

/* -----------------------------------------------------------------------
 * Register map — M-WRG-II uses LITERAL PDU addresses.
 * Datasheet §16.5. The map has gaps; each burst covers only a contiguous
 * block. A burst that spans an undefined address returns exception 0x02.
 * --------------------------------------------------------------------- */

/* Burst A: exhaust/outdoor/extract temps, humidity, CO2 — 41000..41007 */
#define BURST_A_BASE   41000
#define BURST_A_COUNT  8
#define A_TEMP_EXHAUST  0   /* 41000-41001  Float32 Fortluft  */
#define A_TEMP_OUTDOOR  2   /* 41002-41003  Float32 Außenluft */
#define A_TEMP_EXTRACT  4   /* 41004-41005  Float32 Abluft    */
#define A_HUM_EXTRACT   6   /* 41006  UINT16 %                */
#define A_CO2_EXTRACT   7   /* 41007  UINT16 ppm              */
                            /* 41008  undefined — gap         */

/* Burst B: supply temp + supply humidity — 41009..41011 */
#define BURST_B_BASE   41009
#define BURST_B_COUNT  3
#define B_TEMP_SUPPLY   0   /* 41009-41010  Float32 Zuluft */
#define B_HUM_SUPPLY    2   /* 41011  UINT16 %             */
                            /* 41012-41015  undefined — gap */

/* Burst C: error / filter / frost flags — 41016..41018 */
#define BURST_C_BASE   41016
#define BURST_C_COUNT  3
#define C_ERROR_FLAG    0   /* 41016  UINT8 */
#define C_FILTER_DUE    1   /* 41017  UINT8 */
#define C_FROST_ACTIVE  2   /* 41018  UINT8 */
                            /* 41019  undefined — gap */

/* Burst D: actual fan throughputs — 41020..41021 */
#define BURST_D_BASE   41020
#define BURST_D_COUNT  2
#define D_FAN_EXHAUST   0   /* 41020  UINT8 m³/h */
#define D_FAN_SUPPLY    1   /* 41021  UINT8 m³/h */
                            /* 41022-41026  undefined — gap */

/* Burst E: filter days remaining — 41027 (1 register) */
#define BURST_E_BASE   41027
#define BURST_E_COUNT  1
                            /* 41028-41029  undefined — gap */

/* Burst F: operating hours — 41030..41033 */
#define BURST_F_BASE   41030
#define BURST_F_COUNT  4
#define F_HOURS_DEV_H   0   /* 41030  UINT32 high word */
#define F_HOURS_DEV_L   1   /* 41031  UINT32 low word  */
#define F_HOURS_MOT_H   2   /* 41032  UINT32 high word */
#define F_HOURS_MOT_L   3   /* 41033  UINT32 low word  */

/* Burst G: operating mode + fan targets — 41120..41122 */
#define BURST_G_BASE   41120
#define BURST_G_COUNT  3
#define G_MODE          0   /* 41120  UINT8: 1=off 2=regulated 3=manual_bal 4=manual_unbal */
#define G_FAN_TGT_SUPP  1   /* 41121  UINT8: 0-200 → 0-100 m³/h */
#define G_FAN_TGT_EXHST 2   /* 41122  UINT8: 0-200 → 0-100 m³/h */

/* Burst H: humidity + CO2 config — 42000..42005 */
#define BURST_H_BASE   42000
#define BURST_H_COUNT  6
#define H_HUM_SETPOINT  0   /* 42000  UINT8 % (40-80, def 60)  */
#define H_HUM_FAN_MIN   1   /* 42001  UINT8 % (def 10)         */
#define H_HUM_FAN_MAX   2   /* 42002  UINT8 % (def 60)         */
#define H_CO2_SETPOINT  3   /* 42003  UINT16 ppm (def 800)     */
#define H_CO2_FAN_MIN   4   /* 42004  UINT8 % (def 10)         */
#define H_CO2_FAN_MAX   5   /* 42005  UINT8 % (def 60)         */
                            /* 42006  undefined — gap           */

/* Burst I: external input config — 42007..42009 */
#define BURST_I_BASE   42007
#define BURST_I_COUNT  3
#define I_EXT_FAN_LEVEL 0   /* 42007  UINT8 % (def 60)  */
#define I_EXT_ON_DELAY  1   /* 42008  UINT8 min (def 1) */
#define I_EXT_OFF_DELAY 2   /* 42009  UINT8 min (def 15)*/

static uint8_t      s_slave     = 1;
static wrg2_data_t  s_last_data = {0};
static bool         s_data_valid = false;

/* Reconstruct IEEE 754 float from two Modbus registers.
 * M-WRG-II word order: r[1] is the high word, r[0] is the low word
 * (minimalmodbus BYTEORDER_LITTLE_SWAP). */
static float regs_to_float(const uint16_t *r)
{
    uint32_t u = ((uint32_t)r[1] << 16) | (uint32_t)r[0];
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

esp_err_t wrg2_driver_init(uint8_t slave_id, uint32_t baud)
{
    s_slave = slave_id;
    esp_err_t err = modbus_rtu_init(WRG2_UART_PORT, WRG2_GPIO_TX, WRG2_GPIO_RX,
                                    WRG2_GPIO_RTS, baud);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "init slave=%u baud=%lu", slave_id, (unsigned long)baud);
    return ESP_OK;
}

esp_err_t wrg2_read_all(wrg2_data_t *data)
{
    if (!data) return ESP_ERR_INVALID_ARG;

    uint16_t a[BURST_A_COUNT];
    uint16_t b[BURST_B_COUNT];
    uint16_t c[BURST_C_COUNT];
    uint16_t d[BURST_D_COUNT];
    uint16_t e[BURST_E_COUNT];
    uint16_t f[BURST_F_COUNT];
    uint16_t g[BURST_G_COUNT];
    uint16_t h[BURST_H_COUNT];
    uint16_t ii[BURST_I_COUNT];
    esp_err_t err;

    /* Mandatory bursts — abort on failure */
    err = modbus_rtu_read_regs(s_slave, 0x03, BURST_A_BASE, BURST_A_COUNT, a);
    if (err != ESP_OK) { ESP_LOGE(TAG, "burst A (temps/hum/co2) failed"); return err; }

    err = modbus_rtu_read_regs(s_slave, 0x03, BURST_B_BASE, BURST_B_COUNT, b);
    if (err != ESP_OK) { ESP_LOGE(TAG, "burst B (supply) failed"); return err; }

    err = modbus_rtu_read_regs(s_slave, 0x03, BURST_C_BASE, BURST_C_COUNT, c);
    if (err != ESP_OK) { ESP_LOGE(TAG, "burst C (flags) failed"); return err; }

    err = modbus_rtu_read_regs(s_slave, 0x03, BURST_D_BASE, BURST_D_COUNT, d);
    if (err != ESP_OK) { ESP_LOGE(TAG, "burst D (fan actual) failed"); return err; }

    err = modbus_rtu_read_regs(s_slave, 0x03, BURST_G_BASE, BURST_G_COUNT, g);
    if (err != ESP_OK) { ESP_LOGE(TAG, "burst G (mode) failed"); return err; }

    /* Parse mandatory fields */
    data->temp_exhaust      = regs_to_float(&a[A_TEMP_EXHAUST]);
    data->temp_outdoor      = regs_to_float(&a[A_TEMP_OUTDOOR]);
    data->temp_extract      = regs_to_float(&a[A_TEMP_EXTRACT]);
    data->temp_supply       = regs_to_float(&b[B_TEMP_SUPPLY]);
    data->humidity_extract  = a[A_HUM_EXTRACT];
    data->humidity_supply   = b[B_HUM_SUPPLY];
    data->co2_extract       = a[A_CO2_EXTRACT];
    data->error_flag        = (uint8_t)(c[C_ERROR_FLAG]   & 0xFF);
    data->filter_due        = (uint8_t)(c[C_FILTER_DUE]   & 0xFF);
    data->frost_active      = (uint8_t)(c[C_FROST_ACTIVE] & 0xFF);
    data->fan_exhaust_m3h   = (uint8_t)(d[D_FAN_EXHAUST]  & 0xFF);
    data->fan_supply_m3h    = (uint8_t)(d[D_FAN_SUPPLY]   & 0xFF);
    data->mode              = (uint8_t)(g[G_MODE]          & 0xFF);
    data->fan_target_supply  = (uint8_t)(g[G_FAN_TGT_SUPP] & 0xFF);
    data->fan_target_exhaust = (uint8_t)(g[G_FAN_TGT_EXHST]& 0xFF);

    /* Optional bursts — log warning on failure, keep previous or zero */
    err = modbus_rtu_read_regs(s_slave, 0x03, BURST_E_BASE, BURST_E_COUNT, e);
    data->filter_days_left = (err == ESP_OK) ? e[0] : 0;
    if (err != ESP_OK) ESP_LOGW(TAG, "burst E (filter days) failed — using 0");

    err = modbus_rtu_read_regs(s_slave, 0x03, BURST_F_BASE, BURST_F_COUNT, f);
    if (err == ESP_OK) {
        data->hours_device = ((uint32_t)f[F_HOURS_DEV_H] << 16) | f[F_HOURS_DEV_L];
        data->hours_motors = ((uint32_t)f[F_HOURS_MOT_H] << 16) | f[F_HOURS_MOT_L];
    } else {
        data->hours_device = 0;
        data->hours_motors = 0;
        ESP_LOGW(TAG, "burst F (hours) failed — using 0");
    }

    err = modbus_rtu_read_regs(s_slave, 0x03, BURST_H_BASE, BURST_H_COUNT, h);
    if (err == ESP_OK) {
        data->cfg_hum_setpoint = (uint8_t)(h[H_HUM_SETPOINT] & 0xFF);
        data->cfg_hum_fan_min  = (uint8_t)(h[H_HUM_FAN_MIN]  & 0xFF);
        data->cfg_hum_fan_max  = (uint8_t)(h[H_HUM_FAN_MAX]  & 0xFF);
        data->cfg_co2_setpoint = h[H_CO2_SETPOINT];
        data->cfg_co2_fan_min  = (uint8_t)(h[H_CO2_FAN_MIN]  & 0xFF);
        data->cfg_co2_fan_max  = (uint8_t)(h[H_CO2_FAN_MAX]  & 0xFF);
    } else {
        data->cfg_hum_setpoint = 60;
        data->cfg_hum_fan_min  = 10;
        data->cfg_hum_fan_max  = 60;
        data->cfg_co2_setpoint = 800;
        data->cfg_co2_fan_min  = 10;
        data->cfg_co2_fan_max  = 60;
        ESP_LOGW(TAG, "burst H (hum/CO2 config) failed — using defaults");
    }

    err = modbus_rtu_read_regs(s_slave, 0x03, BURST_I_BASE, BURST_I_COUNT, ii);
    if (err == ESP_OK) {
        data->cfg_ext_fan_level = (uint8_t)(ii[I_EXT_FAN_LEVEL] & 0xFF);
        data->cfg_ext_on_delay  = (uint8_t)(ii[I_EXT_ON_DELAY]  & 0xFF);
        data->cfg_ext_off_delay = (uint8_t)(ii[I_EXT_OFF_DELAY] & 0xFF);
    } else {
        data->cfg_ext_fan_level = 60;
        data->cfg_ext_on_delay  = 1;
        data->cfg_ext_off_delay = 15;
        ESP_LOGW(TAG, "burst I (ext input config) failed — using defaults");
    }

    /* Cache for wrg2_get_last_data() */
    memcpy(&s_last_data, data, sizeof(wrg2_data_t));
    s_data_valid = true;

    return ESP_OK;
}

bool wrg2_get_last_data(wrg2_data_t *out)
{
    if (!s_data_valid || !out) return false;
    memcpy(out, &s_last_data, sizeof(wrg2_data_t));
    return true;
}

esp_err_t wrg2_set_mode(uint8_t mode, uint8_t fan_target)
{
    /* Datasheet §16.7: write 41120, 41121, then 41132=0 in order.
     * 41132 (commit) must always be written last. */
    esp_err_t err;

    err = modbus_rtu_write_reg(s_slave, 41120, mode);
    if (err != ESP_OK) { ESP_LOGE(TAG, "set_mode: write 41120 failed"); return err; }

    err = modbus_rtu_write_reg(s_slave, 41121, fan_target);
    if (err != ESP_OK) { ESP_LOGE(TAG, "set_mode: write 41121 failed"); return err; }

    err = modbus_rtu_write_reg(s_slave, 41132, 0);
    if (err != ESP_OK) { ESP_LOGE(TAG, "set_mode: write 41132 (commit) failed"); return err; }

    ESP_LOGI(TAG, "set_mode OK: mode=%u fan_target=%u", mode, fan_target);
    return ESP_OK;
}

esp_err_t wrg2_set_fan_level(uint8_t m3h)
{
    if (m3h > 100) m3h = 100;
    return wrg2_set_mode(3, (uint8_t)(m3h * 2));
}
