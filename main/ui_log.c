#include "ui_log.h"
#include "lcd_touch.h"
#include "lvgl.h"
#include "esp_log.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "Startup";

#define LOG_LINES 12
static char log_buf[LOG_LINES][48];
static int log_idx = 0;
static lv_obj_t *log_label = NULL;

void ui_log_bind_label(void *lv_label_obj)
{
    log_label = (lv_obj_t *)lv_label_obj;
}

static void refresh_label(void)
{
    if (log_label == NULL) return;
    if (ui_lock(0)) {
        char combined[LOG_LINES * 50] = {0};
        int start = log_idx > LOG_LINES ? log_idx - LOG_LINES : 0;
        for (int i = start; i < log_idx; i++) {
            strcat(combined, log_buf[i % LOG_LINES]);
            strcat(combined, "\n");
        }
        lv_label_set_text(log_label, combined);
        ui_unlock();
    }
}

void ui_log(const char *fmt, ...)
{
    char line[48];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    ESP_LOGI(TAG, "%s", line);

    strncpy(log_buf[log_idx % LOG_LINES], line, sizeof(log_buf[0]) - 1);
    log_buf[log_idx % LOG_LINES][sizeof(log_buf[0]) - 1] = '\0';
    log_idx++;

    refresh_label();
}