#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "driver/uart.h"

/**
 * Initialize UART for Modbus RTU RS-485 half-duplex.
 * rts_gpio is used as DE/RE control (driven by UART hardware in half-duplex mode).
 */
esp_err_t modbus_rtu_init(uart_port_t port, int tx_gpio, int rx_gpio,
                           int rts_gpio, uint32_t baud);

/**
 * Read registers via FC03 (holding) or FC04 (input).
 * Retries up to 3 times on timeout or CRC error.
 * Returns ESP_OK on success, ESP_FAIL after all retries exhausted.
 */
esp_err_t modbus_rtu_read_regs(uint8_t slave, uint8_t fc,
                                 uint16_t start, uint16_t count,
                                 uint16_t *out);
