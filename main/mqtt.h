#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MQTT_PUBLISH_MAX_TOPIC 128
#define MQTT_PUBLISH_MAX_PAYLOAD 1024

typedef struct {
    char broker_uri[256];
    char username[128];
    char password[128];
    char client_id[64];
    char telemetry_topic[MQTT_PUBLISH_MAX_TOPIC];
    uint32_t publish_interval_ms;
    uint32_t max_sample_age_ms;
    uint8_t qos;
    bool retain;
    bool enabled;
} mqtt_settings_t;

typedef struct {
    const char *broker_uri;       /* mqtt://host:1883 or mqtts://host:8883 */
    const char *username;         /* NULL for anonymous */
    const char *password;
    const char *client_id;        /* NULL uses ESP-MQTT's unique default */
    const char *telemetry_topic;
    const char *ca_certificate;   /* NULL uses enabled certificate bundle for TLS */
    uint32_t publish_interval_ms; /* 0 -> 5000; minimum 1000 */
    uint32_t max_sample_age_ms;   /* 0 -> three publish intervals */
    int qos;                     /* 0, 1 or 2; choose 1 for telemetry */
    bool retain;
} mqtt_publisher_config_t;

typedef struct {
    bool started;
    bool connected;
    bool sample_valid;
    esp_err_t last_error;
    int error_type;
    int last_message_id;
    uint32_t enqueued_messages;
    uint32_t acknowledged_messages; /* MQTT_EVENT_PUBLISHED, QoS 1/2 only */
    uint32_t enqueue_failures;
    bool enabled;
    bool busy;
    uint32_t completed_requests;
    esp_err_t apply_error;
    esp_err_t save_error;
} mqtt_publisher_status_t;

/**
 * Initialize once after wifi_manager_init() and NVS initialization.
 * NULL config loads NVS or starts disabled for LVGL configuration.
 * Supplied config supplies first-boot defaults; saved NVS overrides them.
 * Worker starts MQTT when Wi-Fi or Ethernet has IPv4, stops when neither does.
 * Application-lifetime singleton; do not initialize from an LVGL callback.
 */
esp_err_t mqtt_publisher_init(const mqtt_publisher_config_t *config);
esp_err_t mqtt_get_settings(mqtt_settings_t *settings);
/** Queue runtime reconfiguration and NVS save; UI caller never waits on network. */
esp_err_t mqtt_apply_settings(const mqtt_settings_t *settings);

/** Update latest sensor sample; automatic task publishes it at configured interval. */
esp_err_t mqtt_set_readings(float temperature_c, float humidity_pct);

/**
 * Copy custom message into bounded RAM queue without waiting.
 * ESP_OK means locally accepted, NOT broker acknowledgement.
 * ESP_ERR_TIMEOUT means queue full; retry from application if required.
 * ESP_ERR_INVALID_STATE means disabled or reconfiguration pending.
 * Applying settings discards old queued messages and client outbox.
 * Empty payload: payload=NULL, length=0. Topic max 127 bytes, payload max 512.
 */
esp_err_t mqtt_publish(const char *topic, const void *payload, size_t length,
                        int qos, bool retain);

/** Nonblocking snapshot; ESP_ERR_TIMEOUT means retry next tick. */
esp_err_t mqtt_get_status(mqtt_publisher_status_t *status);

#ifdef __cplusplus
}
#endif