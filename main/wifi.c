#include "wifi.h"

#include <ctype.h>
#include <string.h>

#include "esp_event.h"
#include "esp_bit_defs.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define CONNECT_REQUEST_BIT BIT0
#define CONNECTING_BIT BIT1
#define SCAN_REQUEST_BIT BIT2
#define RETRY_DELAY_MS 5000
#define SCAN_INTERVAL_MS 30000
#define SETTINGS_VERSION 1

static const char *TAG = "wifi_manager";
static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_queue;
static EventGroupHandle_t s_events;
static esp_netif_t *s_wifi_netif;
static esp_netif_t *s_ethernet_netif;
static esp_eth_handle_t s_ethernet_handle;
static esp_event_handler_instance_t s_wifi_handler;
static network_settings_t s_settings;
static network_status_t s_status;
static bool s_initialized;
static wifi_scan_result_t s_scan_result;
/* Worker-only scratch buffer avoids putting large AP records on task stack. */
static wifi_ap_record_t s_scan_records[WIFI_SCAN_MAX_APS];

typedef struct {
    network_settings_t settings;
    bool save;
    bool restart_ethernet;
} network_request_t;

/* Fixed-width, versioned format; do not persist compiler enum/struct layout. */
typedef struct {
    uint8_t version;
    uint8_t mode;
    char ssid[33];
    char password[65];
} stored_settings_t;

static esp_err_t validate_settings(const network_settings_t *settings,
                                   bool allow_empty_ssid)
{
    if (settings == NULL || settings->mode < NETWORK_MODE_WIFI ||
        settings->mode > NETWORK_MODE_BOTH) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ssid_len = strnlen(settings->ssid, sizeof(settings->ssid));
    size_t password_len = strnlen(settings->password, sizeof(settings->password));
    if (ssid_len > 32 || password_len > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    if (settings->mode != NETWORK_MODE_ETHERNET &&
        ssid_len == 0 && !allow_empty_ssid) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password_len != 0 && password_len < 8) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password_len == 64) {
        for (size_t i = 0; i < password_len; i++) {
            if (!isxdigit((unsigned char)settings->password[i])) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    } else {
        for (size_t i = 0; i < password_len; i++) {
            unsigned char character = (unsigned char)settings->password[i];
            if (character < 32 || character > 126) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    if (settings->mode != NETWORK_MODE_WIFI && s_ethernet_handle == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static esp_err_t load_settings(network_settings_t *settings)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("network", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    stored_settings_t stored = {0};
    size_t size = sizeof(stored);
    err = nvs_get_blob(handle, "settings", &stored, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (size != sizeof(stored) || stored.version != SETTINGS_VERSION) {
        return ESP_ERR_INVALID_STATE;
    }

    network_settings_t loaded = {.mode = (network_mode_t)stored.mode};
    memcpy(loaded.ssid, stored.ssid, sizeof(loaded.ssid));
    memcpy(loaded.password, stored.password, sizeof(loaded.password));
    err = validate_settings(&loaded, true);
    if (err == ESP_OK) {
        *settings = loaded;
    }
    return err;
}

static esp_err_t save_settings(const network_settings_t *settings)
{
    stored_settings_t stored = {
        .version = SETTINGS_VERSION,
        .mode = (uint8_t)settings->mode,
    };
    memcpy(stored.ssid, settings->ssid, sizeof(stored.ssid));
    memcpy(stored.password, settings->password, sizeof(stored.password));

    nvs_handle_t handle;
    esp_err_t err = nvs_open("network", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, "settings", &stored, sizeof(stored));
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    memset(&stored, 0, sizeof(stored));
    return err;
}

/* Never call LVGL or start/stop a driver from the ESP event-loop task. */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    if (event_id == WIFI_EVENT_STA_START ||
        event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            xEventGroupClearBits(s_events, CONNECTING_BIT);
            const wifi_event_sta_disconnected_t *event = event_data;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.disconnect_reason = event->reason;
            xSemaphoreGive(s_mutex);
        }
        xEventGroupSetBits(s_events, CONNECT_REQUEST_BIT);
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
        xEventGroupClearBits(s_events, CONNECT_REQUEST_BIT | CONNECTING_BIT);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.disconnect_reason = 0;
        xSemaphoreGive(s_mutex);
    } else if (event_id == WIFI_EVENT_STA_STOP) {
        xEventGroupClearBits(s_events, CONNECTING_BIT);
    }
}

static void refresh_status(bool wifi_started, bool ethernet_started)
{
    wifi_ap_record_t ap = {0};
    bool wifi_link = wifi_started && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    bool ethernet_link = ethernet_started &&
                         esp_netif_is_netif_up(s_ethernet_netif);
    esp_netif_ip_info_t wifi_ip = {0};
    esp_netif_ip_info_t ethernet_ip = {0};
    if (wifi_link) {
        esp_netif_get_ip_info(s_wifi_netif, &wifi_ip);
    }
    if (ethernet_link) {
        esp_netif_get_ip_info(s_ethernet_netif, &ethernet_ip);
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.wifi_enabled = wifi_started;
    s_status.ethernet_enabled = ethernet_started;
    s_status.wifi_link = wifi_link;
    s_status.ethernet_link = ethernet_link;
    s_status.wifi_ip = wifi_ip.ip;
    s_status.ethernet_ip = ethernet_ip.ip;
    xSemaphoreGive(s_mutex);
}

static esp_err_t apply_runtime(const network_settings_t *settings,
                                bool restart_ethernet, bool *wifi_started,
                                bool *ethernet_started)
{
    /* Stop first so changed credentials cannot race an old connection. */
    esp_err_t err;
    if (*wifi_started) {
        err = esp_wifi_stop();
        if (err != ESP_OK) {
            return err;
        }
        *wifi_started = false;
    }
    xEventGroupClearBits(s_events, CONNECT_REQUEST_BIT | CONNECTING_BIT);

    if (restart_ethernet && *ethernet_started) {
        err = esp_eth_stop(s_ethernet_handle);
        if (err != ESP_OK) {
            return err;
        }
        *ethernet_started = false;
    }

    bool want_ethernet = settings->mode != NETWORK_MODE_WIFI;
    if (want_ethernet != *ethernet_started) {
        err = want_ethernet ? esp_eth_start(s_ethernet_handle)
                           : esp_eth_stop(s_ethernet_handle);
        if (err != ESP_OK) {
            return err;
        }
        *ethernet_started = want_ethernet;
    }

    if (settings->mode != NETWORK_MODE_ETHERNET) {
        wifi_config_t config = {0};
        /* IDF accepts a full 32-byte SSID / 64-byte hex PSK without a NUL. */
        memcpy(config.sta.ssid, settings->ssid, strlen(settings->ssid));
        memcpy(config.sta.password, settings->password, strlen(settings->password));
        config.sta.threshold.authmode = settings->password[0] == '\0'
                                      ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        config.sta.pmf_cfg.required = false;
        config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        /* Empty SSID still starts STA radio for automatic discovery. */
        err = settings->ssid[0] != '\0'
            ? esp_wifi_set_config(WIFI_IF_STA, &config) : ESP_OK;
        memset(&config, 0, sizeof(config));
        if (err != ESP_OK) {
            return err;
        }
        err = esp_wifi_start();
        if (err != ESP_OK) {
            return err;
        }
        *wifi_started = true;
        if (settings->ssid[0] != '\0') {
            xEventGroupSetBits(s_events, CONNECT_REQUEST_BIT);
        }
    }
    return ESP_OK;
}

static esp_err_t scan_networks(bool *wifi_started)
{
    bool temporary_radio = !*wifi_started;
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_scan_result.scanning = true;
    xSemaphoreGive(s_mutex);

    if (temporary_radio) {
        err = esp_wifi_start();
        if (err == ESP_OK) {
            *wifi_started = true;
        }
    }
    if (err == ESP_OK) {
        wifi_scan_config_t config = {
            .show_hidden = false,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        };
        config.scan_time.active.min = 30;
        config.scan_time.active.max = 120;
        /* Blocking only this worker; LVGL and ESP event task remain responsive. */
        err = esp_wifi_scan_start(&config, true);
    }
    uint16_t count = WIFI_SCAN_MAX_APS;
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(&count, s_scan_records);
    }
    if (err != ESP_OK) {
        /* Free driver AP storage on failed retrieval/aborted scan as well. */
        esp_wifi_clear_ap_list();
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (err == ESP_OK) {
        s_scan_result.count = 0;
        memset(s_scan_result.aps, 0, sizeof(s_scan_result.aps));
        for (uint16_t i = 0; i < count; i++) {
            const wifi_ap_record_t *record = &s_scan_records[i];
            if (record->ssid[0] == '\0') {
                continue;
            }
            uint16_t index;
            for (index = 0; index < s_scan_result.count; index++) {
                if (strncmp(s_scan_result.aps[index].ssid,
                            (const char *)record->ssid, 32) == 0) {
                    break;
                }
            }
            if (index == s_scan_result.count) {
                s_scan_result.count++;
            } else if (record->rssi <= s_scan_result.aps[index].rssi) {
                continue;
            }
            wifi_scan_ap_t *ap = &s_scan_result.aps[index];
            memcpy(ap->ssid, record->ssid, 32);
            ap->ssid[32] = '\0';
            ap->rssi = record->rssi;
            ap->open = record->authmode == WIFI_AUTH_OPEN;
        }
    }
    s_scan_result.error = err;
    s_scan_result.revision++;
    s_scan_result.scanning = false;
    xSemaphoreGive(s_mutex);

    if (temporary_radio && *wifi_started) {
        esp_err_t stop_err = esp_wifi_stop();
        xEventGroupClearBits(s_events, CONNECT_REQUEST_BIT | CONNECTING_BIT);
        if (stop_err != ESP_OK) {
            /* Cannot hide a failed stop as a successful discovery. */
            ESP_LOGE(TAG, "Discovery radio stop: %s", esp_err_to_name(stop_err));
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_scan_result.error = stop_err;
            xSemaphoreGive(s_mutex);
            return stop_err;
        }
        *wifi_started = false;
    }
    return err;
}

static void network_task(void *arg)
{
    (void)arg;
    bool wifi_started = false;
    bool ethernet_started = false;
    TickType_t last_attempt = xTaskGetTickCount() - pdMS_TO_TICKS(RETRY_DELAY_MS);
    network_request_t request = {.settings = s_settings};
    bool have_credentials = s_settings.mode != NETWORK_MODE_ETHERNET &&
                            s_settings.ssid[0] != '\0';
    TickType_t last_scan = xTaskGetTickCount() - pdMS_TO_TICKS(SCAN_INTERVAL_MS);

    /* Initial boot request is already marked busy before the task starts. */
    bool boot = true;
    while (true) {
        if (boot || xQueueReceive(s_queue, &request, pdMS_TO_TICKS(250)) == pdTRUE) {
            esp_err_t apply_err = apply_runtime(&request.settings,
                                                request.restart_ethernet, &wifi_started,
                                                &ethernet_started);
            esp_err_t save_err = ESP_OK;
            if (request.save && apply_err == ESP_OK) {
                save_err = save_settings(&request.settings);
            }
            refresh_status(wifi_started, ethernet_started);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            /* Retain requested values on failure so the user can retry them. */
            s_settings = request.settings;
            have_credentials = request.settings.mode != NETWORK_MODE_ETHERNET &&
                               request.settings.ssid[0] != '\0';
            s_status.apply_error = apply_err;
            s_status.save_error = save_err;
            s_status.completed_requests++;
            s_status.busy = false;
            xSemaphoreGive(s_mutex);
            if (apply_err != ESP_OK || save_err != ESP_OK) {
                ESP_LOGE(TAG, "Apply: %s; save: %s", esp_err_to_name(apply_err),
                         esp_err_to_name(save_err));
            }
            boot = false;
            last_attempt = xTaskGetTickCount() - pdMS_TO_TICKS(RETRY_DELAY_MS);
            last_scan = xTaskGetTickCount() - pdMS_TO_TICKS(SCAN_INTERVAL_MS);
            memset(&request, 0, sizeof(request));
        }

        TickType_t now = xTaskGetTickCount();
        EventBits_t bits = xEventGroupGetBits(s_events);
        bool scan_due = (bits & SCAN_REQUEST_BIT) != 0 ||
            (wifi_started && (TickType_t)(now - last_scan) >=
                             pdMS_TO_TICKS(SCAN_INTERVAL_MS));
        /* Finish current connection attempt before discovery can run. */
        if (scan_due && (bits & CONNECTING_BIT) == 0) {
            xEventGroupClearBits(s_events, SCAN_REQUEST_BIT);
            esp_err_t err = scan_networks(&wifi_started);
            last_scan = xTaskGetTickCount();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Scan: %s", esp_err_to_name(err));
            }
        }
        now = xTaskGetTickCount();
        if (wifi_started && have_credentials &&
            (xEventGroupGetBits(s_events) & CONNECTING_BIT) == 0 &&
            (xEventGroupGetBits(s_events) & CONNECT_REQUEST_BIT) != 0 &&
            (TickType_t)(now - last_attempt) >= pdMS_TO_TICKS(RETRY_DELAY_MS)) {
            /* Clear BEFORE connect, preserving a subsequent disconnect event. */
            xEventGroupClearBits(s_events, CONNECT_REQUEST_BIT);
            xEventGroupSetBits(s_events, CONNECTING_BIT);
            esp_err_t err = esp_wifi_connect();
            last_attempt = now;
            if (err != ESP_OK) {
                xEventGroupClearBits(s_events, CONNECTING_BIT);
                ESP_LOGW(TAG, "Connect: %s", esp_err_to_name(err));
                xEventGroupSetBits(s_events, CONNECT_REQUEST_BIT);
            }
        }
        refresh_status(wifi_started, ethernet_started);
    }
}

esp_err_t wifi_manager_init(esp_eth_handle_t ethernet_handle,
                            esp_netif_t *ethernet_netif)
{
    if (s_initialized || s_mutex != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((ethernet_handle == NULL) != (ethernet_netif == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_ethernet_handle = ethernet_handle;
    s_ethernet_netif = ethernet_netif;
    s_settings.mode = ethernet_handle == NULL ? NETWORK_MODE_WIFI
                                              : NETWORK_MODE_ETHERNET;
    esp_err_t err = load_settings(&s_settings);
    if (err != ESP_OK) {
        return err;
    }

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    s_mutex = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(1, sizeof(network_request_t));
    s_events = xEventGroupCreate();
    if (s_mutex == NULL || s_queue == NULL || s_events == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    s_wifi_netif = esp_netif_create_default_wifi_sta();
    if (s_wifi_netif == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    err = esp_wifi_init(&config);
    if (err != ESP_OK) {
        goto fail_netif;
    }
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        goto fail_wifi;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        goto fail_wifi;
    }
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, NULL,
                                              &s_wifi_handler);
    if (err != ESP_OK) {
        goto fail_wifi;
    }

    s_status = (network_status_t) {
        .busy = true,
        .ethernet_available = ethernet_handle != NULL,
    };
    if (xTaskCreate(network_task, "network", 4096, NULL, 5, NULL) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              s_wifi_handler);
        goto fail_wifi;
    }
    s_initialized = true;
    return ESP_OK;

fail_wifi:
    esp_wifi_deinit();
fail_netif:
    esp_netif_destroy_default_wifi(s_wifi_netif);
    s_wifi_netif = NULL;
fail:
    if (s_events != NULL) {
        vEventGroupDelete(s_events);
        s_events = NULL;
    }
    if (s_queue != NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    return err;
}

static esp_err_t queue_request(const network_request_t *request)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = validate_settings(&request->settings, !request->save);
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_status.busy) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_status.busy = true;
    if (xQueueSend(s_queue, request, 0) != pdTRUE) {
        s_status.busy = false;
        err = ESP_ERR_TIMEOUT;
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t wifi_manager_apply(const network_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    network_request_t request = {.settings = *settings, .save = true};
    return queue_request(&request);
}

esp_err_t wifi_manager_retry(void)
{
    network_request_t request = {.restart_ethernet = true};
    esp_err_t err = wifi_manager_get_settings(&request.settings);
    if (err != ESP_OK) {
        return err;
    }
    return queue_request(&request);
}

esp_err_t wifi_manager_get_settings(network_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *settings = s_settings;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t wifi_manager_get_status(network_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *status = s_status;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t wifi_manager_scan_now(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupSetBits(s_events, SCAN_REQUEST_BIT);
    return ESP_OK;
}

esp_err_t wifi_manager_get_scan_result(wifi_scan_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *result = s_scan_result;
    result->pending = (xEventGroupGetBits(s_events) & SCAN_REQUEST_BIT) != 0;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}