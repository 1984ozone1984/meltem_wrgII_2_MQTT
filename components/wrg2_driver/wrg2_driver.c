#include "wrg2_driver.h"
#include "modbus_rtu.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "wrg2_driver";

/* UART hardware assignment for XIAO ESP32-S3 + MAX485
 * TX  → GPIO43 (XIAO D6/TX)
 * RX  → GPIO44 (XIAO D7/RX)
 * DE/RE → GPIO2 (XIAO D0) — verify solder jumper on board
 */
#define WRG2_UART_PORT  UART_NUM_1
#define WRG2_GPIO_TX    43
#define WRG2_GPIO_RX    44
#define WRG2_GPIO_RTS   2      /* MAX485 DE/RE control via UART RTS */

/* -----------------------------------------------------------------------
 * Register map — M-WRG-II uses LITERAL PDU addresses.
 * Datasheet register 41000 → PDU address 41000 (NOT 41000-40001=999).
 * The register map has gaps; bursts must not span undefined addresses
 * (device returns exception 0x02 if any address in a burst is invalid).
 * Datasheet section 16.5
 * --------------------------------------------------------------------- */

/* Burst A: exhaust/outdoor/extract temps + humidity + CO2 (41000-41007) */
#define BURST_A_BASE   41000
#define BURST_A_COUNT  8
#define A_TEMP_EXHAUST  0   /* 41000-41001: Float32, Fortluft  */
#define A_TEMP_OUTDOOR  2   /* 41002-41003: Float32, Außenluft */
#define A_TEMP_EXTRACT  4   /* 41004-41005: Float32, Abluft    */
#define A_HUM_EXTRACT   6   /* 41006: UINT16, %                */
#define A_CO2_EXTRACT   7   /* 41007: UINT16, ppm              */
                            /* 41008: undefined — gap          */

/* Burst B: supply temp + supply humidity (41009-41011) */
#define BURST_B_BASE   41009
#define BURST_B_COUNT  3
#define B_TEMP_SUPPLY   0   /* 41009-41010: Float32, Zuluft */
#define B_HUM_SUPPLY    2   /* 41011: UINT16, %             */
                            /* 41012-41015: undefined — gap */

/* Burst C: error/filter/frost flags (41016-41018) */
#define BURST_C_BASE   41016
#define BURST_C_COUNT  3
#define C_ERROR_FLAG    0   /* 41016: UINT8 */
#define C_FILTER_DUE    1   /* 41017: UINT8 */
#define C_FROST_ACTIVE  2   /* 41018: UINT8 */
                            /* 41019: undefined — gap */

/* Burst D: actual fan throughputs (41020-41021) */
#define BURST_D_BASE   41020
#define BURST_D_COUNT  2
#define D_FAN_EXHAUST   0   /* 41020: UINT8, m³/h Abluft */
#define D_FAN_SUPPLY    1   /* 41021: UINT8, m³/h Zuluft */

/* Burst E: operating mode + target fan levels (41120-41122) */
#define BURST_E_BASE   41120
#define BURST_E_COUNT  3
#define E_MODE          0   /* 41120: UINT8, 1=off 2=auto 3=manual_bal 4=manual_unbal */
#define E_FAN_TGT_SUPPLY 1  /* 41121: UINT8, 0-200 */
#define E_FAN_TGT_EXHST  2  /* 41122: UINT8, 0-200 */

static uint8_t s_slave = 1;

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
    esp_err_t err = modbus_rtu_init(WRG2_UART_PORT, WRG2_GPIO_TX, WRG2_GPIO_RX, WRG2_GPIO_RTS, baud);
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
    esp_err_t err;

    err = modbus_rtu_read_regs(s_slave, 0x03, BURST_A_BASE, BURST_A_COUNT, a);
    if (err != ESP_OK) { ESP_LOGE(TAG, "burst A (temps/hum) failed"); return err; }

    err = modbus_rtu_read_regs(s_slave, 0x03, BURST_B_BASE, BURST_B_COUNT, b);
    if (err != ESP_OK) { ESP_LOGE(TAG, "burst B (supply) failed"); return err; }

    err = modbus_rtu_read_regs(s_slave, 0x03, BURST_C_BASE, BURST_C_COUNT, c);
    if (err != ESP_OK) { ESP_LOGE(TAG, "burst C (flags) failed"); return err; }

    err = modbus_rtu_read_regs(s_slave, 0x03, BURST_D_BASE, BURST_D_COUNT, d);
    if (err != ESP_OK) { ESP_LOGE(TAG, "burst D (fan actual) failed"); return err; }

    err = modbus_rtu_read_regs(s_slave, 0x03, BURST_E_BASE, BURST_E_COUNT, e);
    if (err != ESP_OK) { ESP_LOGE(TAG, "burst E (mode) failed"); return err; }

    data->temp_exhaust       = regs_to_float(&a[A_TEMP_EXHAUST]);
    data->temp_outdoor       = regs_to_float(&a[A_TEMP_OUTDOOR]);
    data->temp_extract       = regs_to_float(&a[A_TEMP_EXTRACT]);
    data->temp_supply        = regs_to_float(&b[B_TEMP_SUPPLY]);
    data->humidity_extract   = a[A_HUM_EXTRACT];
    data->humidity_supply    = b[B_HUM_SUPPLY];
    data->co2_extract        = a[A_CO2_EXTRACT];
    data->error_flag         = (uint8_t)(c[C_ERROR_FLAG]    & 0xFF);
    data->filter_due         = (uint8_t)(c[C_FILTER_DUE]    & 0xFF);
    data->frost_active       = (uint8_t)(c[C_FROST_ACTIVE]  & 0xFF);
    data->fan_exhaust_m3h    = (uint8_t)(d[D_FAN_EXHAUST]   & 0xFF);
    data->fan_supply_m3h     = (uint8_t)(d[D_FAN_SUPPLY]    & 0xFF);
    data->filter_days_left   = 0;   /* 41027 not read — gap at 41022-41026 */
    data->hours_device       = 0;   /* 41030-41031 not read */
    data->hours_motors       = 0;   /* 41032-41033 not read */
    data->mode               = (uint8_t)(e[E_MODE]           & 0xFF);
    data->fan_target_supply  = (uint8_t)(e[E_FAN_TGT_SUPPLY] & 0xFF);
    data->fan_target_exhaust = (uint8_t)(e[E_FAN_TGT_EXHST]  & 0xFF);

    ESP_LOGD(TAG, "supply=%.1f extract=%.1f outdoor=%.1f exhaust=%.1f "
             "fan_s=%u fan_e=%u mode=%u",
             data->temp_supply, data->temp_extract,
             data->temp_outdoor, data->temp_exhaust,
             data->fan_supply_m3h, data->fan_exhaust_m3h, data->mode);

    return ESP_OK;
}
