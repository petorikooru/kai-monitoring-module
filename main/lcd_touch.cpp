#include "lcd_touch.h"
#include "config.h"
#include "ui.h"
#include <cassert>
#include <cstdint>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/ledc.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/panel/Panel_ST7796.hpp>
#include <lgfx/v1/touch/Touch_XPT2046.hpp>
#include "lvgl.h"

// Uses the pin definitions in your existing config.h.
// LVGL 9; ESP-IDF; LovyanGFX.
// No other driver may independently initialize SPI2_HOST.
static constexpr int BACKLIGHT_ON = 1;
static const char *TAG = "lcd_touch";

class LGFX : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7796 _panel;
    lgfx::Bus_SPI _bus;
    lgfx::Touch_XPT2046 _touch;

public:
    LGFX()
    {
        {
            auto cfg = _bus.config();
            cfg.spi_host = SPI2_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 79000000;
            cfg.freq_read = 8000000;
            cfg.spi_3wire = false;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = SPI_SCLK_GPIO;
            cfg.pin_mosi = SPI_MOSI_GPIO;
            cfg.pin_miso = SPI_MISO_GPIO;
            cfg.pin_dc = LCD_DC;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs = CS_LCD_GPIO;
            cfg.pin_rst = LCD_RST;
            cfg.pin_busy = -1;
            cfg.memory_width = LCD_H_RES;
            cfg.memory_height = LCD_V_RES;
            cfg.panel_width = LCD_H_RES;
            cfg.panel_height = LCD_V_RES;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.offset_rotation = 0;
            cfg.invert = true;
            cfg.rgb_order = true;
            cfg.bus_shared = true;
            cfg.readable = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _touch.config();
            // Approximate bounds. Later try x_min ~300, x_max ~3800 (same y).
            cfg.x_min = 145;
            cfg.x_max = 3915;
            cfg.y_min = 215;
            cfg.y_max = 3950;
            cfg.pin_int = TCH_IRQ_GPIO;
            cfg.pin_cs = CS_TCH_GPIO;
            cfg.pin_sclk = SPI_SCLK_GPIO;
            cfg.pin_mosi = SPI_MOSI_GPIO;
            cfg.pin_miso = SPI_MISO_GPIO;
            cfg.freq = 400000;
            cfg.spi_host = SPI2_HOST;
            cfg.bus_shared = true;
            cfg.offset_rotation = 180;
            _touch.config(cfg);
            _panel.setTouch(&_touch);
        }
        setPanel(&_panel);
    }
};

static LGFX gfx;
static SemaphoreHandle_t ui_mutex = nullptr;
static uint8_t *lvgl_buf1 = nullptr;
static esp_timer_handle_t tick_timer = nullptr;

// ---------- backlight + idle timeout ----------
static constexpr uint32_t DIM_LEAD_MS = 5000;
static constexpr uint8_t  BL_FULL_PCT = 100;
static constexpr uint8_t  BL_DIM_PCT  = 25;

typedef enum { BL_FULL, BL_DIM, BL_OFF } bl_state_t;
static bl_state_t bl_state = BL_FULL;
static volatile uint32_t timeout_ms = 60000;   // default 1 min
static lv_display_t *g_disp = nullptr;

static void backlight_init(void)
{
    ledc_timer_config_t t = {};
    t.speed_mode      = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = LEDC_TIMER_8_BIT;
    t.timer_num       = LEDC_TIMER_0;
    t.freq_hz         = 5000;
    t.clk_cfg         = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    ledc_channel_config_t c = {};
    c.gpio_num   = LCD_LED;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel    = LEDC_CHANNEL_0;
    c.timer_sel  = LEDC_TIMER_0;
    c.duty       = 0;
    c.hpoint     = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

static void backlight_set(uint8_t pct)
{
    uint32_t duty = (uint32_t)pct * 255 / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void display_set_timeout(uint32_t seconds) {
    timeout_ms = seconds * 1000;
}
uint32_t display_get_timeout(void) {
    return timeout_ms / 1000;
}

// call from lvgl_task, lock held
static void power_tick(void)
{
    uint32_t to = timeout_ms;
    bl_state_t want = BL_FULL;

    if (to != 0) {
        uint32_t idle   = lv_display_get_inactive_time(g_disp);
        uint32_t dim_at = to > DIM_LEAD_MS ? to - DIM_LEAD_MS : 0;
        if (idle >= to) {
            want = BL_OFF;
            ui_show_splash();
        }
        else if (idle >= dim_at) {
            want = BL_DIM;
        }
    }
    if (want == bl_state) return;

    bl_state = want;
    backlight_set(want == BL_FULL ? BL_FULL_PCT :
                  want == BL_DIM  ? BL_DIM_PCT  : 0);
}

bool ui_lock(uint32_t timeout_ms)
{
    if (ui_mutex == nullptr) return false;
    TickType_t ticks = timeout_ms == 0
        ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms != 0 && ticks == 0) ticks = 1;
    return xSemaphoreTakeRecursive(ui_mutex, ticks) == pdTRUE;
}

void ui_unlock(void)
{
    assert(ui_mutex != nullptr);
    xSemaphoreGiveRecursive(ui_mutex);
}

static void lvgl_flush_cb(lv_display_t *disp,
                          const lv_area_t *area,
                          uint8_t *px_map)
{
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;

    gfx.startWrite();
    gfx.setAddrWindow(area->x1, area->y1, w, h);
    gfx.pushPixelsDMA(reinterpret_cast<uint16_t *>(px_map), w * h);
    // LVGL must not reuse its buffer until DMA has completed.
    gfx.waitDMA();
    gfx.endWrite();
    lv_display_flush_ready(disp);
}

static void lvgl_touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static uint16_t last_x = 0, last_y = 0;
    static bool swallow = false;

    uint16_t x = 0, y = 0;
    bool pressed = gfx.getTouch(&x, &y);

    // touch while screen off: wake only, don't click hidden widget
    if (pressed && bl_state == BL_OFF) {
        swallow = true;
        lv_display_trigger_activity(g_disp);
    }
    if (!pressed) swallow = false;

    if (pressed) { last_x = x; last_y = y; }

    data->state = (pressed && !swallow)
        ? LV_INDEV_STATE_PRESSED
        : LV_INDEV_STATE_RELEASED;
    data->point.x = last_x;
    data->point.y = last_y;
}

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(5);
}

static void lvgl_task(void *arg)
{
    (void)arg;

    while (true) {
        if (ui_lock(0)) {
            lv_timer_handler();
            power_tick();
            ui_unlock();
        }
        TickType_t delay_ticks = pdMS_TO_TICKS(10);
        vTaskDelay(delay_ticks > 0 ? delay_ticks : 1);
    }
}

static void set_output(int pin, int level)
{
    const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
    ESP_ERROR_CHECK(gpio_set_direction(gpio, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(gpio, level));
}

// STUB: HW touch cal disabled while debugging dead touch. Keeps ui.c linking.
void display_run_touch_cal(void)
{
    ESP_LOGW(TAG, "HW touch cal disabled");
}

void lcd_touch_init(void)
{
    assert(ui_mutex == nullptr);
    ui_mutex = xSemaphoreCreateRecursiveMutex();
    assert(ui_mutex != nullptr);

    backlight_init();
    set_output(CS_LCD_GPIO, 1);
    set_output(CS_TCH_GPIO, 1);
    ESP_ERROR_CHECK(gpio_set_direction(
        static_cast<gpio_num_t>(TCH_IRQ_GPIO), GPIO_MODE_INPUT));
    ESP_ERROR_CHECK(gpio_set_pull_mode(
        static_cast<gpio_num_t>(TCH_IRQ_GPIO), GPIO_PULLUP_ONLY));

    gfx.init();
    gfx.setRotation(0);
    gfx.setColorDepth(16);
    // LVGL RGB565 pixels are native-endian; send their MSB first.
    gfx.setSwapBytes(true);
    gfx.fillScreen(TFT_BLACK);
    gfx.waitDMA();

    backlight_set(BL_FULL_PCT);

    lv_init();
    // RGB565 is exactly two bytes per pixel, regardless of lv_color_t size.
    const size_t buf_bytes = LCD_H_RES * 40 * sizeof(uint16_t);
    lvgl_buf1 = static_cast<uint8_t *>(heap_caps_malloc(
        buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    assert(lvgl_buf1 != nullptr);

    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    assert(disp != nullptr);
    g_disp = disp;   // was missing: power_tick / touch_cb used null g_disp
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    lv_display_set_buffers(disp, lvgl_buf1, nullptr, buf_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    assert(indev != nullptr);
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, disp);
    lv_indev_set_read_cb(indev, lvgl_touch_cb);

    esp_timer_create_args_t tick_timer_args = {};
    tick_timer_args.callback = lvgl_tick_cb;
    tick_timer_args.arg = nullptr;
    tick_timer_args.dispatch_method = ESP_TIMER_TASK;
    tick_timer_args.name = "lvgl_tick";
    tick_timer_args.skip_unhandled_events = false;
    ESP_ERROR_CHECK(esp_timer_create(&tick_timer_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 5000));

    const BaseType_t result = xTaskCreate(
        lvgl_task, "lvgl_task", 8192, nullptr, 4, nullptr);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LVGL task");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    ESP_LOGI(TAG, "LCD 79 MHz; touch 400 kHz; shared SPI2 initialized");
}