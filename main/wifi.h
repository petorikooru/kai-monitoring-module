#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_eth.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Values intentionally match the LVGL dropdown option order. */
typedef enum {
    NETWORK_MODE_WIFI = 0,
    NETWORK_MODE_ETHERNET,
    NETWORK_MODE_BOTH,
} network_mode_t;

typedef struct {
    network_mode_t mode;
    char ssid[33];
    char password[65];
} network_settings_t;

typedef struct {
    bool busy;
    bool ethernet_available;
    bool wifi_enabled;
    bool ethernet_enabled;
    bool wifi_link;
    bool ethernet_link;
    esp_ip4_addr_t wifi_ip;
    esp_ip4_addr_t ethernet_ip;
    uint16_t disconnect_reason;
    uint32_t completed_requests;
    esp_err_t apply_error;
    esp_err_t save_error;
} network_status_t;

#define WIFI_SCAN_MAX_APS 20

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool open;
} wifi_scan_ap_t;

typedef struct {
    bool scanning;
    bool pending;
    uint16_t count;
    uint32_t revision;
    esp_err_t error;
    wifi_scan_ap_t aps[WIFI_SCAN_MAX_APS];
} wifi_scan_result_t;

/**
 * Initialize once, before ui_init().
 * Caller must initialize NVS, ESP-NETIF and the default event loop first.
 * Pass an installed, attached, STOPPED Ethernet driver and its netif, or
 * NULL, NULL for Wi-Fi only. This module owns driver start/stop thereafter.
 * This module creates and owns the default Wi-Fi STA netif and Wi-Fi driver.
 * Saved settings are applied asynchronously by the worker task.
 */
esp_err_t wifi_manager_init(esp_eth_handle_t ethernet_handle,
                            esp_netif_t *ethernet_netif);

/** Validate and queue one Apply/Save request without blocking the UI. */
esp_err_t wifi_manager_apply(const network_settings_t *settings);

/** Restart enabled interfaces using current settings; do not write NVS. */
esp_err_t wifi_manager_retry(void);

/** Nonblocking snapshot APIs. ESP_ERR_TIMEOUT means retry next LVGL tick. */
esp_err_t wifi_manager_get_settings(network_settings_t *settings);
esp_err_t wifi_manager_get_status(network_status_t *status);

/** Queue discovery scan. A stopped radio is powered temporarily, then stopped. */
esp_err_t wifi_manager_scan_now(void);
esp_err_t wifi_manager_get_scan_result(wifi_scan_result_t *result);

#ifdef __cplusplus
}
#endif