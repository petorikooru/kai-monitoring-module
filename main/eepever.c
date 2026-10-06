#include "eepever.h"
#include "config.h"
#include "mqtt.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#ifndef EPEVER_UART
#define EPEVER_UART UART_NUM_1
#endif
#ifndef EPEVER_BAUD
#define EPEVER_BAUD 115200
#endif
#ifndef EPEVER_SLAVE_ID
#define EPEVER_SLAVE_ID 1
#endif
#ifndef TTL_DE_GPIO
#define TTL_DE_GPIO (-1)
#endif

#define RESP_TIMEOUT_MS 300
#define FRAME_GAP_MS    20
#define MAX_QTY         30
#define FAIL_OFFLINE    3
#define STATS_EVERY     5
#define CFG_VERSION     1

static const char *TAG = "epever";

typedef struct {
    const char *key;
    const char *suffix;
    const char *unit;
} field_def_t;

_Static_assert(sizeof(epever_values_t) == EPEVER_FIELD_COUNT * sizeof(float),
               "epever_values_t layout");

static const field_def_t FIELDS[EPEVER_FIELD_COUNT] = {
    {"pv_voltage",          "pv/voltage",             "V"},
    {"pv_current",          "pv/current",             "A"},
    {"pv_power",            "pv/power",               "W"},
    {"bat_voltage",         "battery/voltage",        "V"},
    {"bat_charge_current",  "battery/charge_current", "A"},
    {"bat_charge_power",    "battery/charge_power",   "W"},
    {"bat_net_current",     "battery/net_current",    "A"},
    {"load_voltage",        "load/voltage",           "V"},
    {"load_current",        "load/current",           "A"},
    {"load_power",          "load/power",             "W"},
    {"bat_temp",            "temp/battery",           "C"},
    {"dev_temp",            "temp/device",            "C"},
    {"comp_temp",           "temp/components",        "C"},
    {"amb_temp",            "temp/ambient",           "C"},
    {"rem_bat_temp",        "temp/remote_battery",    "C"},
    {"soc",                 "battery/soc",            "%"},
    {"chg_stage",           "charge/stage",           ""},
    {"pv_v_max_today",      "stats/pv_v_max",         "V"},
    {"pv_v_min_today",      "stats/pv_v_min",         "V"},
    {"bat_v_max_today",     "stats/bat_v_max",        "V"},
    {"bat_v_min_today",     "stats/bat_v_min",        "V"},
    {"gen_today",           "energy/gen_today",       "kWh"},
    {"gen_month",           "energy/gen_month",       "kWh"},
    {"gen_year",            "energy/gen_year",        "kWh"},
    {"gen_total",           "energy/gen_total",       "kWh"},
    {"con_today",           "energy/con_today",       "kWh"},
    {"con_month",           "energy/con_month",       "kWh"},
    {"con_year",            "energy/con_year",        "kWh"},
    {"con_total",           "energy/con_total",       "kWh"},
    {"co2_saved",           "energy/co2",             "t"},
};

typedef struct {
    uint32_t version;
    epever_mqtt_cfg_t cfg;
} stored_cfg_t;

static SemaphoreHandle_t s_mutex;
static epever_data_t s_data;
static epever_mqtt_cfg_t s_cfg;
static stored_cfg_t s_stored;
static bool s_save_pending;
static uint32_t s_consec_fail;

/* ---------- helpers ---------- */

const char *epever_field_key(int idx)
{
    return (idx >= 0 && idx < EPEVER_FIELD_COUNT) ? FIELDS[idx].key : "";
}

const char *epever_field_unit(int idx)
{
    return (idx >= 0 && idx < EPEVER_FIELD_COUNT) ? FIELDS[idx].unit : "";
}

const char *epever_stage_text(float stage)
{
    switch ((int)stage & 3) {
    case 1:  return "Float";
    case 2:  return "Boost";
    case 3:  return "Equalize";
    default: return "Idle";
    }
}

static float f_u16(uint16_t r) { return r / 100.0f; }
static float f_s16(uint16_t r) { return (int16_t)r / 100.0f; }
static float f_u32(uint16_t lo, uint16_t hi)
{
    return (float)(((uint32_t)hi << 16) | lo) / 100.0f;
}
static float f_s32(uint16_t lo, uint16_t hi)
{
    return (float)(int32_t)(((uint32_t)hi << 16) | lo) / 100.0f;
}

/* ---------- Modbus RTU ---------- */

static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

static esp_err_t uart_setup(void)
{
    const uart_config_t cfg = {
        .baud_rate = EPEVER_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(EPEVER_UART, 256, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_param_config(EPEVER_UART, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(EPEVER_UART, TTL_TX_GPIO, TTL_RX_GPIO,
                       TTL_DE_GPIO >= 0 ? TTL_DE_GPIO : UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }
    if (TTL_DE_GPIO >= 0) {
        err = uart_set_mode(EPEVER_UART, UART_MODE_RS485_HALF_DUPLEX);
    }
    return err;
}

/* FC 0x04, read input registers. */
static esp_err_t mb_read(uint16_t addr, uint16_t qty, uint16_t *out)
{
    if (qty == 0 || qty > MAX_QTY) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t req[8] = {EPEVER_SLAVE_ID, 0x04, (uint8_t)(addr >> 8), (uint8_t)addr,
                      (uint8_t)(qty >> 8), (uint8_t)qty};
    uint16_t c = crc16(req, 6);
    req[6] = (uint8_t)(c & 0xFF);
    req[7] = (uint8_t)(c >> 8);

    uart_flush_input(EPEVER_UART);
    if (uart_write_bytes(EPEVER_UART, req, sizeof(req)) != (int)sizeof(req)) {
        return ESP_FAIL;
    }
    uart_wait_tx_done(EPEVER_UART, pdMS_TO_TICKS(50));

    uint8_t rsp[5 + 2 * MAX_QTY];
    TickType_t to = pdMS_TO_TICKS(RESP_TIMEOUT_MS);
    if (uart_read_bytes(EPEVER_UART, rsp, 3, to) != 3) {
        return ESP_ERR_TIMEOUT;
    }
    if (rsp[0] != EPEVER_SLAVE_ID) {
        uart_flush_input(EPEVER_UART);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (rsp[1] == (0x80 | 0x04)) {           /* exception frame */
        uart_read_bytes(EPEVER_UART, rsp + 3, 2, to);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (rsp[1] != 0x04 || rsp[2] != qty * 2) {
        uart_flush_input(EPEVER_UART);
        return ESP_ERR_INVALID_RESPONSE;
    }
    int bc = rsp[2];
    if (uart_read_bytes(EPEVER_UART, rsp + 3, bc + 2, to) != bc + 2) {
        return ESP_ERR_TIMEOUT;
    }
    uint16_t rx_crc = (uint16_t)(rsp[3 + bc] | (rsp[4 + bc] << 8));
    if (crc16(rsp, 3 + bc) != rx_crc) {
        return ESP_ERR_INVALID_CRC;
    }
    for (int i = 0; i < qty; i++) {
        out[i] = (uint16_t)((rsp[3 + 2 * i] << 8) | rsp[4 + 2 * i]);
    }
    return ESP_OK;
}

static void gap(void)
{
    vTaskDelay(pdMS_TO_TICKS(FRAME_GAP_MS));
}

/* ---------- poll ---------- */

static void poll_once(uint32_t cycle)
{
    epever_data_t d;
    uint16_t r[MAX_QTY];
    esp_err_t err;
    esp_err_t first_err = ESP_OK;
    bool core_ok = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    d = s_data;
    xSemaphoreGive(s_mutex);

    /* rated values, once */
    if (!d.rated_valid && mb_read(0x3000, 15, r) == ESP_OK) {
        d.rated_pv_v = f_u16(r[0]);
        d.rated_pv_a = f_u16(r[1]);
        d.rated_pv_w = f_u32(r[2], r[3]);
        d.rated_bat_v = f_u16(r[4]);
        d.rated_chg_a = f_u16(r[5]);
        d.rated_load_a = f_u16(r[14]);
        d.rated_valid = true;
        gap();
    }

    /* 0x3100..0x310F: PV, battery, load */
    err = mb_read(0x3100, 16, r);
    if (err == ESP_OK) {
        epever_values_t *v = &d.v;
        v->pv_v = f_u16(r[0]);
        v->pv_a = f_u16(r[1]);
        v->pv_w = f_u32(r[2], r[3]);
        v->bat_v = f_u16(r[4]);
        v->bat_a = f_u16(r[5]);
        v->bat_w = f_u32(r[6], r[7]);
        v->load_v = f_u16(r[12]);
        v->load_a = f_u16(r[13]);
        v->load_w = f_u32(r[14], r[15]);
        core_ok = true;
    } else {
        first_err = err;
    }

    if (core_ok) {
        gap();
        /* 0x3110..0x3112: temps */
        err = mb_read(0x3110, 3, r);
        if (err == ESP_OK) {
            d.v.bat_temp = f_s16(r[0]);
            d.v.dev_temp = f_s16(r[1]);
            d.v.comp_temp = f_s16(r[2]);
        } else if (first_err == ESP_OK) {
            first_err = err;
        }
        gap();

        /* 0x311A: SOC %, 0x311B: remote battery temp */
        err = mb_read(0x311A, 2, r);
        if (err == ESP_OK) {
            d.v.soc = (float)r[0];
            d.v.rem_bat_temp = f_s16(r[1]);
        } else if (first_err == ESP_OK) {
            first_err = err;
        }
        gap();

        /* 0x3200..0x3202: status words */
        err = mb_read(0x3200, 3, r);
        if (err == ESP_OK) {
            d.bat_status = r[0];
            d.chg_status = r[1];
            d.dis_status = r[2];
            d.v.chg_stage = (float)((r[1] >> 2) & 3);
        } else if (first_err == ESP_OK) {
            first_err = err;
        }

        /* 0x3300..0x331D: statistics, slower cadence */
        if (cycle % STATS_EVERY == 0) {
            gap();
            err = mb_read(0x3300, 30, r);
            if (err == ESP_OK) {
                epever_values_t *v = &d.v;
                v->pv_v_max = f_u16(r[0]);
                v->pv_v_min = f_u16(r[1]);
                v->bat_v_max = f_u16(r[2]);
                v->bat_v_min = f_u16(r[3]);
                v->con_day = f_u32(r[4], r[5]);
                v->con_month = f_u32(r[6], r[7]);
                v->con_year = f_u32(r[8], r[9]);
                v->con_total = f_u32(r[10], r[11]);
                v->gen_day = f_u32(r[12], r[13]);
                v->gen_month = f_u32(r[14], r[15]);
                v->gen_year = f_u32(r[16], r[17]);
                v->gen_total = f_u32(r[18], r[19]);
                v->co2 = f_u32(r[20], r[21]);
                v->bat_net_a = f_s32(r[26], r[27]);
                v->amb_temp = f_s16(r[29]);
            } else if (first_err == ESP_OK) {
                first_err = err;
            }
        }
    }

    d.last_err = first_err;
    if (core_ok) {
        s_consec_fail = 0;
        d.online = true;
        d.sequence++;
        d.ok_cycles++;
    } else {
        d.err_cycles++;
        if (++s_consec_fail >= FAIL_OFFLINE) {
            d.online = false;
        }
        ESP_LOGW(TAG, "poll: %s", esp_err_to_name(first_err));
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_data = d;
    xSemaphoreGive(s_mutex);
}

/* ---------- MQTT config ---------- */

void epever_mqtt_defaults(epever_mqtt_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->interval_s = 10;
    c->field_mask = (uint32_t)((1ull << EPEVER_FIELD_COUNT) - 1);
    snprintf(c->base, sizeof(c->base), "kai/epever");
    for (int i = 0; i < EPEVER_FIELD_COUNT; i++) {
        snprintf(c->suffix[i], EPEVER_SUFFIX_MAX, "%s", FIELDS[i].suffix);
    }
}

static bool part_ok(const char *s, size_t cap)
{
    size_t n = strnlen(s, cap);
    if (n >= cap || strpbrk(s, "+#") != NULL) {
        return false;
    }
    if (n > 0 && (s[0] == '/' || s[n - 1] == '/')) {
        return false;
    }
    return true;
}

static bool cfg_valid(const epever_mqtt_cfg_t *c)
{
    if (c == NULL || c->interval_s < 1 || c->interval_s > 3600) {
        return false;
    }
    if (!part_ok(c->base, sizeof(c->base))) {
        return false;
    }
    if (c->enabled && c->base[0] == '\0') {
        return false;
    }
    for (int i = 0; i < EPEVER_FIELD_COUNT; i++) {
        if (!part_ok(c->suffix[i], EPEVER_SUFFIX_MAX)) {
            return false;
        }
        if (c->enabled && !c->json_mode && (c->field_mask & (1u << i)) &&
            c->suffix[i][0] == '\0') {
            return false;
        }
    }
    return true;
}

static esp_err_t cfg_load(epever_mqtt_cfg_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("kai_epever", NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    memset(&s_stored, 0, sizeof(s_stored));
    size_t size = sizeof(s_stored);
    err = nvs_get_blob(h, "cfg", &s_stored, &size);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (size != sizeof(s_stored) || s_stored.version != CFG_VERSION ||
        !cfg_valid(&s_stored.cfg)) {
        return ESP_ERR_INVALID_STATE;
    }
    *out = s_stored.cfg;
    return ESP_OK;
}

static esp_err_t cfg_save(const epever_mqtt_cfg_t *c)
{
    memset(&s_stored, 0, sizeof(s_stored));
    s_stored.version = CFG_VERSION;
    s_stored.cfg = *c;
    nvs_handle_t h;
    esp_err_t err = nvs_open("kai_epever", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "cfg", &s_stored, sizeof(s_stored));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    return err;
}

esp_err_t epever_get_mqtt_cfg(epever_mqtt_cfg_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *cfg = s_cfg;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t epever_apply_mqtt_cfg(const epever_mqtt_cfg_t *cfg)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!cfg_valid(cfg)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_cfg = *cfg;
    s_save_pending = true;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t epever_get_data(epever_data_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *out = s_data;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

/* ---------- MQTT publish ---------- */

/* false = hard fail (stop cycle) */
static bool pub(const char *topic, const char *payload, int qos, bool retain)
{
    for (int t = 0; t < 20; t++) {
        esp_err_t e = mqtt_publish(topic, payload, strlen(payload), qos, retain);
        if (e == ESP_OK) {
            return true;
        }
        if (e != ESP_ERR_TIMEOUT) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));      /* queue full, worker drains */
    }
    return false;
}

static void publish_cycle(const epever_data_t *d, const epever_mqtt_cfg_t *c)
{
    mqtt_publisher_status_t st;
    if (mqtt_get_status(&st) != ESP_OK || !st.enabled || !st.connected) {
        return;
    }
    int qos = 1;
    bool retain = false;
    mqtt_settings_t ms;
    if (mqtt_get_settings(&ms) == ESP_OK) {
        qos = ms.qos;
        retain = ms.retain;
    }
    memset(&ms, 0, sizeof(ms));              /* had password */

    if (c->json_mode) {
        char buf[MQTT_PUBLISH_MAX_PAYLOAD];
        int n = snprintf(buf, sizeof(buf), "{\"online\":%s,\"seq\":%lu",
                         d->online ? "true" : "false", (unsigned long)d->sequence);
        for (int i = 0; i < EPEVER_FIELD_COUNT && n > 0; i++) {
            if (!(c->field_mask & (1u << i))) {
                continue;
            }
            int w = snprintf(buf + n, sizeof(buf) - n, ",\"%s\":%.2f",
                             FIELDS[i].key, d->v.f[i]);
            if (w < 0 || w >= (int)sizeof(buf) - n) {
                break;
            }
            n += w;
        }
        if (n > 0 && n < (int)sizeof(buf) - 1) {
            buf[n++] = '}';
            buf[n] = '\0';
            pub(c->base, buf, qos, retain);
        }
        return;
    }

    char topic[MQTT_PUBLISH_MAX_TOPIC];
    char val[24];
    for (int i = 0; i < EPEVER_FIELD_COUNT; i++) {
        if (!(c->field_mask & (1u << i)) || c->suffix[i][0] == '\0') {
            continue;
        }
        int tn = snprintf(topic, sizeof(topic), "%s/%s", c->base, c->suffix[i]);
        if (tn <= 0 || tn >= (int)sizeof(topic)) {
            continue;
        }
        snprintf(val, sizeof(val), "%.2f", d->v.f[i]);
        if (!pub(topic, val, qos, retain)) {
            break;
        }
    }
}

/* ---------- task ---------- */

static void epever_task(void *arg)
{
    (void)arg;
    static epever_mqtt_cfg_t cfg;
    static epever_data_t data;
    uint32_t cycle = 0;
    TickType_t last_pub = xTaskGetTickCount();

    while (true) {
        poll_once(cycle++);

        bool save = false;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        cfg = s_cfg;
        data = s_data;
        save = s_save_pending;
        s_save_pending = false;
        xSemaphoreGive(s_mutex);

        if (save) {
            esp_err_t e = cfg_save(&cfg);
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "cfg save: %s", esp_err_to_name(e));
            }
        }
        if (cfg.enabled && data.online && data.sequence > 0 &&
            (TickType_t)(xTaskGetTickCount() - last_pub) >=
                pdMS_TO_TICKS(cfg.interval_s * 1000)) {
            publish_cycle(&data, &cfg);
            last_pub = xTaskGetTickCount();
        }
        vTaskDelay(pdMS_TO_TICKS(EPEVER_POLL_MS));
    }
}

esp_err_t epever_init(void)
{
    if (s_mutex != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    epever_mqtt_defaults(&s_cfg);
    esp_err_t err = cfg_load(&s_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cfg load: %s, defaults", esp_err_to_name(err));
        epever_mqtt_defaults(&s_cfg);
    }
    err = uart_setup();
    if (err != ESP_OK) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }
    if (xTaskCreate(epever_task, "epever", 8192, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UART%d TX=%d RX=%d %d baud id=%d", (int)EPEVER_UART,
             TTL_TX_GPIO, TTL_RX_GPIO, EPEVER_BAUD, EPEVER_SLAVE_ID);
    return ESP_OK;
}