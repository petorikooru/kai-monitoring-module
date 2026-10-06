#pragma once

#include "esp_err.h"
#include "esp_eth.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install and attach W5500, but leave it stopped. Call after ESP-NETIF/event init. */
esp_err_t eth_init(void);
esp_eth_handle_t eth_get_handle(void);
esp_netif_t *eth_get_netif(void);

/* Existing UI retry callback name retained; restart is queued to wifi.c. */
void eth_start_or_retry(void);

#ifdef __cplusplus
}
#endif
