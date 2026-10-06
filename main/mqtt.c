#include "mqtt.h"
#include "wifi.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "sdkconfig.h"
#include "nvs.h"

#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) && CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#define MQTT_QUEUE_LENGTH 8
#define MQTT_OUTBOX_LIMIT 8192
#define MQTT_DEFAULT_INTERVAL_MS 5000

static const char *TAG = "mqtt_publisher";
static esp_mqtt_client_handle_t s_client;
static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_queue;
static mqtt_publisher_config_t s_config;
static mqtt_publisher_status_t s_status;
static bool s_initialized;
static mqtt_settings_t s_settings;
static QueueHandle_t s_settings_queue;
static mqtt_settings_t *s_client_settings;

typedef struct {
    float temperature_c;
    float humidity_pct;
    int64_t timestamp_us;
    uint32_t sequence;
    bool valid;
} sensor_sample_t;

typedef struct {
    char topic[MQTT_PUBLISH_MAX_TOPIC];
    char payload[MQTT_PUBLISH_MAX_PAYLOAD];
    size_t length;
    int qos;
    bool retain;
} publish_message_t;

static sensor_sample_t s_sample;

static bool valid_topic(const char *topic)
{
    return topic != NULL && topic[0] != '\0' &&
           strnlen(topic, MQTT_PUBLISH_MAX_TOPIC) < MQTT_PUBLISH_MAX_TOPIC &&
           strchr(topic, '+') == NULL && strchr(topic, '#') == NULL;
}

static void free_config(void)
{
    free((void *)s_config.broker_uri);
    free((void *)s_config.username);
    free((void *)s_config.password);
    free((void *)s_config.client_id);
    free((void *)s_config.telemetry_topic);
    free((void *)s_config.ca_certificate);
    memset(&s_config, 0, sizeof(s_config));
}

static bool copy_string(const char **destination, const char *source)
{
    *destination = source == NULL ? NULL : strdup(source);
    return source == NULL || *destination != NULL;
}

static void set_error(esp_err_t error)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.last_error = error;
    xSemaphoreGive(s_mutex);
}

/* No LVGL calls, MQTT lifecycle calls, or blocking network work here. */
static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;
    if (event == NULL || event->client != s_client) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_status.connected = true;
        s_status.last_error = ESP_OK;
        s_status.error_type = 0;
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_status.connected = false;
        break;
    case MQTT_EVENT_PUBLISHED:
        s_status.acknowledged_messages++;
        break;
    case MQTT_EVENT_ERROR:
        s_status.last_error = ESP_FAIL;
        if (event != NULL && event->error_handle != NULL) {
            s_status.error_type = (int)event->error_handle->error_type;
        }
        break;
    default:
        break;
    }
    xSemaphoreGive(s_mutex);
    if (event_id == MQTT_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "Broker connected");
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "Broker disconnected");
    } else if (event_id == MQTT_EVENT_ERROR) {
        ESP_LOGW(TAG, "MQTT error event");
    }
}

static esp_err_t enqueue_message(const char *topic, const char *payload,
                                 size_t length, int qos, bool retain)
{
    /* Only this worker calls enqueue, so ESP-MQTT cannot stall LVGL callers. */
    int id = esp_mqtt_client_enqueue(s_client, topic, payload, (int)length,
                                     qos, retain, true);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (id >= 0) {
        s_status.last_message_id = id;
        s_status.enqueued_messages++;
    } else {
        s_status.enqueue_failures++;
        s_status.last_error = id == -2 ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    xSemaphoreGive(s_mutex);
    return id >= 0 ? ESP_OK : ESP_FAIL;
}

static bool copy_field(char *destination, size_t capacity, const char *source)
{
    if (source == NULL) {
        destination[0] = '\0';
        return true;
    }
    size_t length = strnlen(source, capacity);
    if (length >= capacity) {
        return false;
    }
    memcpy(destination, source, length + 1);
    return true;
}

static esp_err_t validate_mqtt_settings(const mqtt_settings_t *settings)
{
    if (settings == NULL ||
        strnlen(settings->broker_uri, sizeof(settings->broker_uri)) >= sizeof(settings->broker_uri) ||
        strnlen(settings->username, sizeof(settings->username)) >= sizeof(settings->username) ||
        strnlen(settings->password, sizeof(settings->password)) >= sizeof(settings->password) ||
        strnlen(settings->client_id, sizeof(settings->client_id)) >= sizeof(settings->client_id) ||
        strnlen(settings->telemetry_topic, sizeof(settings->telemetry_topic)) >= sizeof(settings->telemetry_topic) ||
        settings->qos > 2 || settings->publish_interval_ms < 1000 ||
        settings->publish_interval_ms > 3600000 || settings->max_sample_age_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!settings->enabled) {
        return ESP_OK;
    }
    if (!valid_topic(settings->telemetry_topic)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t prefix = strncmp(settings->broker_uri, "mqtts://", 8) == 0 ? 8
                    : strncmp(settings->broker_uri, "mqtt://", 7) == 0 ? 7 : 0;
    if (prefix == 0 || settings->broker_uri[prefix] == '\0' ||
        strpbrk(settings->broker_uri, " \t\r\n") != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
#if !defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) || !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (prefix == 8 && s_config.ca_certificate == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif
    return ESP_OK;
}

/* Versioned, fixed-width record, initialized to zero including padding. */
typedef struct {
    uint32_t version;
    mqtt_settings_t settings;
} stored_mqtt_settings_t;

static esp_err_t load_mqtt_settings(mqtt_settings_t *settings)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("kai_mqtt", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    stored_mqtt_settings_t stored = {0};
    size_t size = sizeof(stored);
    err = nvs_get_blob(handle, "settings", &stored, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (size != sizeof(stored) || stored.version != 1) {
        return ESP_ERR_INVALID_STATE;
    }
    err = validate_mqtt_settings(&stored.settings);
    if (err == ESP_OK) {
        *settings = stored.settings;
    }
    memset(&stored, 0, sizeof(stored));
    return err;
}

static esp_err_t save_mqtt_settings(const mqtt_settings_t *settings)
{
    stored_mqtt_settings_t stored = {0};
    stored.version = 1;
    stored.settings = *settings;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("kai_mqtt", NVS_READWRITE, &handle);
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

static esp_err_t create_mqtt_client(const mqtt_settings_t *settings,
                                    esp_mqtt_client_handle_t *client,
                                    mqtt_settings_t **owned_settings)
{
    *client = NULL;
    *owned_settings = NULL;
    esp_err_t err = validate_mqtt_settings(settings);
    if (err != ESP_OK || !settings->enabled) {
        return err;
    }
    *owned_settings = malloc(sizeof(**owned_settings));
    if (*owned_settings == NULL) {
        return ESP_ERR_NO_MEM;
    }
    **owned_settings = *settings;
    settings = *owned_settings;
    esp_mqtt_client_config_t config = {0};
    config.broker.address.uri = settings->broker_uri;
    config.credentials.username = settings->username[0] ? settings->username : NULL;
    config.credentials.authentication.password = settings->password[0] ? settings->password : NULL;
    config.credentials.client_id = settings->client_id[0] ? settings->client_id : NULL;
    config.network.reconnect_timeout_ms = 5000;
    config.network.timeout_ms = 5000;
    config.session.keepalive = 30;
    config.outbox.limit = MQTT_OUTBOX_LIMIT;
    if (strncmp(settings->broker_uri, "mqtts://", 8) == 0) {
        if (s_config.ca_certificate != NULL) {
            config.broker.verification.certificate = s_config.ca_certificate;
        } else {
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) && CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
            config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
#endif
        }
    }
    *client = esp_mqtt_client_init(&config);
    if (*client == NULL) {
        free(*owned_settings);
        *owned_settings = NULL;
        return ESP_FAIL;
    }
    err = esp_mqtt_client_register_event(*client, ESP_EVENT_ANY_ID,
                                        mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(*client);
        *client = NULL;
        free(*owned_settings);
        *owned_settings = NULL;
    }
    return err;
}

static esp_err_t reconfigure_mqtt(const mqtt_settings_t *settings, bool *started)
{
    /* Build candidate before touching working client. */
    esp_mqtt_client_handle_t candidate;
    mqtt_settings_t *candidate_settings;
    esp_err_t err = create_mqtt_client(settings, &candidate, &candidate_settings);
    if (err != ESP_OK) {
        return err;
    }
    if (*started) {
        err = esp_mqtt_client_stop(s_client);
        if (err != ESP_OK) {
            if (candidate != NULL) {
                esp_mqtt_client_destroy(candidate);
            }
            free(candidate_settings);
            return err;
        }
        *started = false;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.started = false;
        s_status.connected = false;
        xSemaphoreGive(s_mutex);
    }
    if (s_client != NULL) {
        err = esp_mqtt_client_destroy(s_client);
        if (err != ESP_OK) {
            if (candidate != NULL) {
                esp_mqtt_client_destroy(candidate);
            }
            free(candidate_settings);
            return err;
        }
    }
    free(s_client_settings);
    s_client_settings = candidate_settings;
    s_client = candidate;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_settings = *settings;
    s_status.enabled = settings->enabled;
    s_status.started = false;
    s_status.connected = false;
    s_status.last_error = ESP_OK;
    s_status.error_type = 0;
    /* Prevent queued messages from old settings reaching a new broker. */
    xQueueReset(s_queue);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

static void mqtt_task(void *arg)
{
    (void)arg;
    bool started = false;
    bool previous_connected = false;
    uint32_t active_ip = 0;
    int active_interface = 0;
    int64_t last_publish_us = 0;
    /* Scratch message is task-local and never exposed to producer tasks. */
    publish_message_t message;
    mqtt_settings_t requested;

    while (true) {
        if (xQueueReceive(s_settings_queue, &requested, 0) == pdTRUE) {
            esp_err_t apply_error = reconfigure_mqtt(&requested, &started);
            esp_err_t save_error = apply_error == ESP_OK
                ? save_mqtt_settings(&requested) : ESP_OK;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.apply_error = apply_error;
            s_status.save_error = save_error;
            s_status.completed_requests++;
            s_status.busy = false;
            xSemaphoreGive(s_mutex);
            if (apply_error == ESP_OK) {
                previous_connected = false;
                active_ip = 0;
                active_interface = 0;
                last_publish_us = 0;
            }
            memset(&requested, 0, sizeof(requested));
        }
        if (!s_settings.enabled || s_client == NULL) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        network_status_t network;
        if (wifi_manager_get_status(&network) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        /* Matches supplied netif priorities: Ethernet 200, Wi-Fi STA 100. */
        uint32_t next_ip = network.ethernet_ip.addr != 0
            ? network.ethernet_ip.addr : network.wifi_ip.addr;
        int next_interface = network.ethernet_ip.addr != 0 ? 2
                             : network.wifi_ip.addr != 0 ? 1 : 0;
        bool route_changed = next_ip != active_ip || next_interface != active_interface;
        if (started && (next_ip == 0 || route_changed)) {
            esp_err_t err = esp_mqtt_client_stop(s_client);
            if (err != ESP_OK) {
                set_error(err);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            started = false;
            previous_connected = false;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.started = false;
            s_status.connected = false;
            xSemaphoreGive(s_mutex);
        }
        if (!started && next_ip != 0) {
            esp_err_t err = esp_mqtt_client_start(s_client);
            if (err != ESP_OK) {
                set_error(err);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            started = true;
            active_ip = next_ip;
            active_interface = next_interface;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.started = true;
            xSemaphoreGive(s_mutex);
        }

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        bool connected = s_status.connected;
        sensor_sample_t sample = s_sample;
        xSemaphoreGive(s_mutex);
        if (connected && !previous_connected) {
            last_publish_us = esp_timer_get_time() -
                (int64_t)s_settings.publish_interval_ms * 1000;
        }
        previous_connected = connected;
        if (connected) {
            for (int n = 0; n < MQTT_QUEUE_LENGTH && xQueuePeek(s_queue, &message, 0) == pdTRUE; n++) {
                if (enqueue_message(message.topic, message.length == 0 ? NULL
                                      : message.payload, message.length,
                                      message.qos, message.retain) != ESP_OK) {
                    break;
                }
                xQueueReceive(s_queue, &message, 0);
            }
            int64_t now_us = esp_timer_get_time();
            int64_t age_us = now_us - sample.timestamp_us;
            if (sample.valid && age_us <= (int64_t)s_settings.max_sample_age_ms * 1000 &&
                now_us - last_publish_us >= (int64_t)s_settings.publish_interval_ms * 1000) {
                char payload[192];
                int length = snprintf(payload, sizeof(payload),
                    "{\"temperature_c\":%.2f,\"humidity_pct\":%.2f,"
                    "\"sample_sequence\":%lu,\"sample_age_ms\":%lld}",
                    (double)sample.temperature_c, (double)sample.humidity_pct,
                    (unsigned long)sample.sequence, (long long)(age_us / 1000));
                if (length > 0 && length < (int)sizeof(payload) &&
                    enqueue_message(s_settings.telemetry_topic, payload, (size_t)length,
                                    s_settings.qos, s_settings.retain) == ESP_OK) {
                    last_publish_us = now_us;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

esp_err_t mqtt_publisher_init(const mqtt_publisher_config_t *config)
{
    if (s_initialized || s_mutex != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    network_status_t network;
    esp_err_t err = wifi_manager_get_status(&network);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        return err;
    }
    mqtt_settings_t settings = {
        .telemetry_topic = "kai/monitoring/telemetry",
        .publish_interval_ms = MQTT_DEFAULT_INTERVAL_MS,
        .max_sample_age_ms = MQTT_DEFAULT_INTERVAL_MS * 3,
        .qos = 1,
    };
    if (config != NULL) {
        if (!copy_field(settings.broker_uri, sizeof(settings.broker_uri), config->broker_uri) ||
            !copy_field(settings.username, sizeof(settings.username), config->username) ||
            !copy_field(settings.password, sizeof(settings.password), config->password) ||
            !copy_field(settings.client_id, sizeof(settings.client_id), config->client_id) ||
            !copy_field(settings.telemetry_topic, sizeof(settings.telemetry_topic), config->telemetry_topic) ||
            config->qos < 0 || config->qos > 2) {
            return ESP_ERR_INVALID_ARG;
        }
        settings.enabled = true;
        settings.publish_interval_ms = config->publish_interval_ms == 0
            ? MQTT_DEFAULT_INTERVAL_MS : config->publish_interval_ms;
        settings.max_sample_age_ms = config->max_sample_age_ms == 0
            ? settings.publish_interval_ms * 3 : config->max_sample_age_ms;
        settings.qos = (uint8_t)config->qos;
        settings.retain = config->retain;
        if (!copy_string(&s_config.ca_certificate, config->ca_certificate)) {
            return ESP_ERR_NO_MEM;
        }
    }
    err = load_mqtt_settings(&settings);
    if (err != ESP_OK) {
        free_config();
        return err;
    }
    s_mutex = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(MQTT_QUEUE_LENGTH, sizeof(publish_message_t));
    s_settings_queue = xQueueCreate(1, sizeof(mqtt_settings_t));
    if (s_mutex == NULL || s_queue == NULL || s_settings_queue == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    err = create_mqtt_client(&settings, &s_client, &s_client_settings);
    if (err != ESP_OK) {
        goto fail;
    }
    s_settings = settings;
    s_status.enabled = settings.enabled;
    if (xTaskCreate(mqtt_task, "mqtt_pub", 6144, NULL, 4, NULL) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        if (s_client != NULL) {
            esp_mqtt_client_destroy(s_client);
            s_client = NULL;
        }
        free(s_client_settings);
        s_client_settings = NULL;
        goto fail;
    }
    s_initialized = true;
    memset(&settings, 0, sizeof(settings));
    return ESP_OK;

fail:
    if (s_settings_queue != NULL) {
        vQueueDelete(s_settings_queue);
        s_settings_queue = NULL;
    }
    if (s_queue != NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    free_config();
    return err;
}

esp_err_t mqtt_get_settings(mqtt_settings_t *settings)
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

esp_err_t mqtt_apply_settings(const mqtt_settings_t *settings)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = validate_mqtt_settings(settings);
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
    if (xQueueSend(s_settings_queue, settings, 0) != pdTRUE) {
        s_status.busy = false;
        err = ESP_ERR_TIMEOUT;
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t mqtt_set_readings(float temperature_c, float humidity_pct)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!isfinite(temperature_c) || !isfinite(humidity_pct) ||
        humidity_pct < 0.0f || humidity_pct > 100.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_sample.temperature_c = temperature_c;
    s_sample.humidity_pct = humidity_pct;
    s_sample.timestamp_us = esp_timer_get_time();
    s_sample.sequence++;
    s_sample.valid = true;
    s_status.sample_valid = true;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t mqtt_publish(const char *topic, const void *payload, size_t length,
                        int qos, bool retain)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    bool enabled = s_status.enabled;
    bool busy = s_status.busy;
    xSemaphoreGive(s_mutex);
    if (!enabled || busy) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!valid_topic(topic) || length > MQTT_PUBLISH_MAX_PAYLOAD ||
        (payload == NULL && length != 0) || qos < 0 || qos > 2) {
        return ESP_ERR_INVALID_ARG;
    }
    publish_message_t message = {.length = length, .qos = qos, .retain = retain};
    memcpy(message.topic, topic, strlen(topic));
    if (length != 0) {
        memcpy(message.payload, payload, length);
    }
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = !s_status.enabled || s_status.busy ? ESP_ERR_INVALID_STATE
        : xQueueSend(s_queue, &message, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t mqtt_get_status(mqtt_publisher_status_t *status)
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