#pragma once
#include <stdbool.h>
#include <stddef.h>

/**
 * Initialize WiFi stack (netif, event loop, drivers).
 * Must be called once before wifi_manager_start().
 */
void wifi_manager_init(void);

/**
 * Start WiFi: tries STA with stored credentials (30 s timeout),
 * falls back to open AP "WRG2-Setup" on 192.168.4.1 if no credentials
 * or connection fails.
 */
void wifi_manager_start(void);

/** Returns true when connected in STA mode and IP assigned. */
bool wifi_manager_is_connected(void);

/** Returns true when running in AP (provisioning) mode. */
bool wifi_manager_is_ap_mode(void);

/** Copies current IP address string into buf. */
void wifi_manager_get_ip(char *buf, size_t len);

/** Copies the AP SSID string into buf. */
void wifi_manager_get_ap_ssid(char *buf, size_t len);
