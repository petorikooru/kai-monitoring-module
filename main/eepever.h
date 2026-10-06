#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EPEVER_FIELD_COUNT 30
#define EPEVER_BASE_MAX    64
#define EPEVER_SUFFIX_MAX  32
#define EPEVER_POLL_MS     2000

/* Order must match FIELDS[] in epever.c. Units: V A W C % kWh t */
typedef union {
    float f[EPEVER_FIELD_COUNT];
    struct {
        float pv_v, pv_a, pv_w;
        float bat_v, bat_a, bat_w;
        float bat_net_a;                 /* signed, + = charging */
        float load_v, load_a, load_w;
        float bat_temp, dev_temp, comp_temp, amb_temp, rem_bat_temp;
        float soc;
        float chg_stage;                 /* 0 idle, 1 float, 2 boost, 3 equalize */
        float pv_v_max, pv_v_min, bat_v_max, bat_v_min;   /* today */
        float gen_day, gen_month, gen_year, gen_total;
        float con_day, con_month, con_year, con_total;
        float co2;
    };
} epever_values_t;

typedef struct {
    epever_values_t v;
    uint16_t bat_status;     /* raw 0x3200 */
    uint16_t chg_status;     /* raw 0x3201 */
    uint16_t dis_status;     /* raw 0x3202 */
    bool online;
    uint32_t sequence;       /* ++ per good poll */
    uint32_t ok_cycles;
    uint32_t err_cycles;
    esp_err_t last_err;
    bool rated_valid;        /* read once */
    float rated_pv_v, rated_pv_a, rated_pv_w;
    float rated_bat_v, rated_chg_a, rated_load_a;
} epever_data_t;

typedef struct {
    bool enabled;
    bool json_mode;          /* true: one JSON on <base>; false: <base>/<suffix> per field */
    uint32_t interval_s;     /* 1..3600 */
    uint32_t field_mask;     /* bit i = field i published */
    char base[EPEVER_BASE_MAX];
    char suffix[EPEVER_FIELD_COUNT][EPEVER_SUFFIX_MAX];
} epever_mqtt_cfg_t;

/* Call after nvs_flash_init() and mqtt_publisher_init(). */
esp_err_t epever_init(void);

/* Nonblocking. ESP_ERR_TIMEOUT -> retry next tick. */
esp_err_t epever_get_data(epever_data_t *out);
esp_err_t epever_get_mqtt_cfg(epever_mqtt_cfg_t *cfg);
/* Validate, copy to RAM, NVS save done by worker. Never blocks UI. */
esp_err_t epever_apply_mqtt_cfg(const epever_mqtt_cfg_t *cfg);
void epever_mqtt_defaults(epever_mqtt_cfg_t *cfg);

const char *epever_field_key(int idx);
const char *epever_field_unit(int idx);
const char *epever_stage_text(float stage);

#ifdef __cplusplus
}
#endif