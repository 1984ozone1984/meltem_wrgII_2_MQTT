#include "modbus_rtu.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "modbus_rtu";

/* Max registers per request (Modbus spec: 125 for FC03/FC04) */
#define MAX_REGS 125

static uart_port_t      s_port  = UART_NUM_1;
static SemaphoreHandle_t s_mutex = NULL;

/* CRC16/Modbus: poly 0x8005 reflected = 0xA001, init 0xFFFF */
static uint16_t crc16(const uint8_t *buf, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

esp_err_t modbus_rtu_init(uart_port_t port, int tx_gpio, int rx_gpio,
                           int rts_gpio, uint32_t baud)
{
    s_port = port;

    uart_config_t cfg = {
        .baud_rate  = (int)baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_EVEN,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(port, &cfg));
    /* rts_gpio unused: Youmile module has internal auto-direction, no external DE/RE */
    ESP_ERROR_CHECK(uart_set_pin(port, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    /* 512-byte RX buffer; no TX ring buffer needed for short frames */
    ESP_ERROR_CHECK(uart_driver_install(port, 512, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_set_mode(port, UART_MODE_UART));

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "init: UART%d tx=%d rx=%d rts(DE)=%d baud=%lu",
             (int)port, tx_gpio, rx_gpio, rts_gpio, (unsigned long)baud);
    return ESP_OK;
}

esp_err_t modbus_rtu_read_regs(uint8_t slave, uint8_t fc,
                                 uint16_t start, uint16_t count,
                                 uint16_t *out)
{
    if (count == 0 || count > MAX_REGS || !out || !s_mutex) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Request frame: slave(1) + fc(1) + start(2) + count(2) + crc(2) = 8 bytes */
    uint8_t req[8];
    req[0] = slave;
    req[1] = fc;
    req[2] = (uint8_t)(start >> 8);
    req[3] = (uint8_t)(start & 0xFF);
    req[4] = (uint8_t)(count >> 8);
    req[5] = (uint8_t)(count & 0xFF);
    uint16_t req_crc = crc16(req, 6);
    req[6] = (uint8_t)(req_crc & 0xFF);   /* CRC low byte */
    req[7] = (uint8_t)(req_crc >> 8);     /* CRC high byte */

    /* Expected response: slave(1) + fc(1) + byte_count(1) + data(2*count) + crc(2) */
    int expected = 5 + 2 * (int)count;
    uint8_t resp[5 + 2 * MAX_REGS];

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        uart_flush_input(s_port);
        uart_write_bytes(s_port, req, sizeof(req));
        uart_wait_tx_done(s_port, pdMS_TO_TICKS(100));

        int n = uart_read_bytes(s_port, resp, expected, pdMS_TO_TICKS(300));
        if (n != expected) {
            if (n == 5 && (resp[1] & 0x80)) {
                /* Modbus exception response */
                ESP_LOGW(TAG, "slave %u fc %02X start %u: EXCEPTION code=0x%02X (bytes: %02X %02X %02X %02X %02X)",
                         slave, fc, start, resp[2],
                         resp[0], resp[1], resp[2], resp[3], resp[4]);
            } else if (n > 0) {
                ESP_LOGW(TAG, "slave %u fc %02X start %u: attempt %d short read %d/%d: "
                         "%02X %02X %02X %02X %02X",
                         slave, fc, start, attempt + 1, n, expected,
                         n>0?resp[0]:0, n>1?resp[1]:0, n>2?resp[2]:0,
                         n>3?resp[3]:0, n>4?resp[4]:0);
            } else {
                ESP_LOGW(TAG, "slave %u fc %02X start %u: attempt %d timeout (0/%d bytes)",
                         slave, fc, start, attempt + 1, expected);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uint16_t recv_crc = (uint16_t)resp[n - 2] | ((uint16_t)resp[n - 1] << 8);
        uint16_t calc_crc = crc16(resp, n - 2);
        if (recv_crc != calc_crc) {
            ESP_LOGW(TAG, "slave %u fc %02X start %u: attempt %d CRC mismatch (recv %04X calc %04X)",
                     slave, fc, start, attempt + 1, recv_crc, calc_crc);
            continue;
        }

        if (resp[0] != slave || resp[1] != fc || resp[2] != (uint8_t)(count * 2)) {
            ESP_LOGW(TAG, "slave %u fc %02X start %u: attempt %d bad header",
                     slave, fc, start, attempt + 1);
            continue;
        }

        /* Unpack big-endian register values */
        for (int i = 0; i < (int)count; i++) {
            out[i] = ((uint16_t)resp[3 + i * 2] << 8) | resp[4 + i * 2];
        }
        ret = ESP_OK;
        break;
    }

    xSemaphoreGive(s_mutex);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "slave %u fc %02X start %u count %u: all retries failed",
                 slave, fc, start, count);
    }
    return ret;
}

esp_err_t modbus_rtu_write_reg(uint8_t slave, uint16_t addr, uint16_t value)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;

    /* FC06 request = echo response: slave(1)+0x06(1)+addr(2)+val(2)+crc(2) */
    uint8_t req[8];
    req[0] = slave;
    req[1] = 0x06;
    req[2] = (uint8_t)(addr  >> 8);
    req[3] = (uint8_t)(addr  & 0xFF);
    req[4] = (uint8_t)(value >> 8);
    req[5] = (uint8_t)(value & 0xFF);
    uint16_t req_crc = crc16(req, 6);
    req[6] = (uint8_t)(req_crc & 0xFF);
    req[7] = (uint8_t)(req_crc >> 8);

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        uart_flush_input(s_port);
        uart_write_bytes(s_port, req, sizeof(req));
        uart_wait_tx_done(s_port, pdMS_TO_TICKS(100));

        uint8_t resp[8] = {0};
        int n = uart_read_bytes(s_port, resp, sizeof(resp), pdMS_TO_TICKS(300));

        if (n == 5 && (resp[1] & 0x80)) {
            ESP_LOGW(TAG, "write slave %u addr %u val %u: EXCEPTION code=0x%02X",
                     slave, addr, value, resp[2]);
            break; /* exception won't change on retry */
        }
        if (n != 8) {
            ESP_LOGW(TAG, "write slave %u addr %u: attempt %d short read %d/8",
                     slave, addr, attempt + 1, n);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        /* Validate CRC */
        uint16_t recv_crc = (uint16_t)resp[6] | ((uint16_t)resp[7] << 8);
        if (crc16(resp, 6) != recv_crc) {
            ESP_LOGW(TAG, "write slave %u addr %u: attempt %d CRC mismatch",
                     slave, addr, attempt + 1);
            continue;
        }
        /* Echo must match request (slave, fc, addr, value) */
        if (resp[0] != req[0] || resp[1] != req[1] ||
            resp[2] != req[2] || resp[3] != req[3] ||
            resp[4] != req[4] || resp[5] != req[5]) {
            ESP_LOGW(TAG, "write slave %u addr %u: attempt %d echo mismatch",
                     slave, addr, attempt + 1);
            continue;
        }
        ret = ESP_OK;
        break;
    }

    xSemaphoreGive(s_mutex);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "write slave %u addr %u val %u: all retries failed",
                 slave, addr, value);
    }
    return ret;
}
