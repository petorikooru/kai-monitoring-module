#include "lvgl.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

// Local Files //////////////////////////
#include "config.h"
#include "eepever.h"
#include "wifi.h"
#include "mqtt.h"
#include "ui.h"
#include "ui_log.h"
#include "lcd_touch.h"
#include "net_eth.h"

LV_IMAGE_DECLARE(kai_logo);

// ---------- RGB565 palette (dark theme, from Theme.qml) ----------
#define C_SIDEBAR   0x0862 // #090F17
#define C_BG        0x08A4 // #0E1621
#define C_SURFACE   0x10E5 // #121C29
#define C_CARD      0x1926 // #192536
#define C_CARD2     0x2168 // #202F43
#define C_TEXT      0xFFDF // #F7FAFC
#define C_SUBTEXT   0xB61A // #B4C1D0
#define C_MUTED     0x7C74 // #7D8EA3
#define C_ACCENT    0xF364 // #F36F21
#define C_ACCENT_P  0xDAA2 // #D95712
#define C_ACCENT_T  0x1083 // #111318
#define C_SUCCESS   0x262B // #22C55E
#define C_INFO      0x3DFF // #38BDF8
#define C_WARNING   0xF4E1 // #F59E0B
#define C_DANGER    0xEA28 // #EF4444
#define C_BORDER    0x322B // #30445B
#define C_DIVIDER   0x21C9 // #27394D
#define C_CONTROL   0x2167 // #202D3E

#define NAV_H   56
#define PAD     16
#define CONTENT_H (LCD_V_RES - NAV_H)
#define TREND_POINTS 60

// RGB565 -> lv_color_t (exact when LV_COLOR_DEPTH == 16)
static inline lv_color_t c565(uint16_t c)
{
    uint8_t r = (c >> 8) & 0xF8; r |= r >> 5;
    uint8_t g = (c >> 3) & 0xFC; g |= g >> 6;
    uint8_t b = (c << 3) & 0xF8; b |= b >> 5;
    return lv_color_make(r, g, b);
}

typedef enum { PAGE_DASH, PAGE_TREND, PAGE_LOG, PAGE_SETTINGS, PAGE_COUNT } page_id_t;

static lv_obj_t *pages[PAGE_COUNT];
static lv_obj_t *nav_bar = NULL;
static lv_obj_t *nav_btns[PAGE_COUNT];
static lv_obj_t *splash_scr = NULL;

static lv_obj_t *temp_label = NULL;
static lv_obj_t *hum_label = NULL;
static lv_obj_t *eth_status_label = NULL;
static lv_obj_t *eth_ip_label = NULL;
static lv_obj_t *internet_interface_label = NULL;
static lv_obj_t *log_label = NULL;

static lv_obj_t *sol_status_label = NULL;
static lv_obj_t *sol_body_label = NULL;
static lv_obj_t *sol_chart = NULL;
static lv_chart_series_t *sol_ser_pv = NULL;
static lv_chart_series_t *sol_ser_bat = NULL;
static lv_obj_t *trend_grp_env = NULL;
static lv_obj_t *trend_grp_sol = NULL;
static lv_obj_t *trend_mode_label = NULL;
static bool trend_show_solar = false;
static uint32_t sol_last_seq = 0;

static lv_obj_t *trend_chart = NULL;
static lv_chart_series_t *ser_temp = NULL;
static lv_chart_series_t *ser_hum = NULL;

static eth_retry_cb_t retry_cb = NULL;

static void settings_keyboard_hide(void);

void ui_set_eth_retry_cb(eth_retry_cb_t cb) { retry_cb = cb; }

// ---------- shared styles (init once) ----------

static lv_style_t st_title, st_label, st_value, st_accent_txt, st_card, st_nav_btn, st_nav_btn_on;

static void init_styles(void)
{
    lv_style_init(&st_title);
    lv_style_set_text_font(&st_title, &lv_font_montserrat_24);
    lv_style_set_text_color(&st_title, c565(C_TEXT));

    lv_style_init(&st_label);
    lv_style_set_text_font(&st_label, &lv_font_montserrat_14);
    lv_style_set_text_color(&st_label, c565(C_SUBTEXT));

    lv_style_init(&st_value);
    lv_style_set_text_font(&st_value, &lv_font_montserrat_24);
    lv_style_set_text_color(&st_value, c565(C_TEXT));

    lv_style_init(&st_accent_txt);
    lv_style_set_text_font(&st_accent_txt, &lv_font_montserrat_14);
    lv_style_set_text_color(&st_accent_txt, c565(C_ACCENT));

    lv_style_init(&st_card);
    lv_style_set_radius(&st_card, 0);
    lv_style_set_bg_color(&st_card, c565(C_CARD));
    lv_style_set_bg_opa(&st_card, LV_OPA_COVER);
    lv_style_set_border_color(&st_card, c565(C_BORDER));
    lv_style_set_border_width(&st_card, 1);
    lv_style_set_pad_all(&st_card, 12);
    lv_style_set_shadow_width(&st_card, 0);

    // nav button: idle
    lv_style_init(&st_nav_btn);
    lv_style_set_radius(&st_nav_btn, 0);
    lv_style_set_bg_color(&st_nav_btn, c565(C_SIDEBAR));
    lv_style_set_bg_opa(&st_nav_btn, LV_OPA_COVER);
    lv_style_set_border_width(&st_nav_btn, 0);
    lv_style_set_shadow_width(&st_nav_btn, 0);
    lv_style_set_text_font(&st_nav_btn, &lv_font_montserrat_14);
    lv_style_set_text_color(&st_nav_btn, c565(C_MUTED));

    // nav button: active (accent bar on top edge)
    lv_style_init(&st_nav_btn_on);
    lv_style_set_bg_color(&st_nav_btn_on, c565(C_SURFACE));
    lv_style_set_text_color(&st_nav_btn_on, c565(C_ACCENT));
    lv_style_set_border_color(&st_nav_btn_on, c565(C_ACCENT));
    lv_style_set_border_width(&st_nav_btn_on, 3);
    lv_style_set_border_side(&st_nav_btn_on, LV_BORDER_SIDE_TOP);
}

// ---------- page switching ----------

static void show_page(page_id_t id)
{
    settings_keyboard_hide();
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (i == (int)id) lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        else              lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);

        if (i == (int)id) lv_obj_add_state(nav_btns[i], LV_STATE_CHECKED);
        else              lv_obj_clear_state(nav_btns[i], LV_STATE_CHECKED);
    }
}

static void nav_cb(lv_event_t *e)
{
    show_page((page_id_t)(intptr_t)lv_event_get_user_data(e));
}

static void splash_cb(lv_event_t *e)
{
    lv_obj_add_flag(splash_scr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(nav_bar, LV_OBJ_FLAG_HIDDEN);
    show_page(PAGE_DASH);
}

static void retry_btn_cb(lv_event_t *e)
{
    esp_err_t err = wifi_manager_retry();
    if (err != ESP_OK) {
        ui_log("Network retry: %s", esp_err_to_name(err));
    }
}

// ---------- shared widgets ----------

static lv_obj_t *make_page(void)
{
    lv_obj_t *p = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, LCD_H_RES, CONTENT_H);
    lv_obj_align(p, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(p, c565(C_BG), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

static lv_obj_t *page_header(lv_obj_t *parent, const char *title)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LCD_H_RES, 56);
    lv_obj_set_style_bg_color(bar, c565(C_SURFACE), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, c565(C_DIVIDER), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *lbl = lv_label_create(bar);
    lv_obj_add_style(lbl, &st_title, 0);
    lv_label_set_text(lbl, title);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, PAD, 0);
    return bar;
}

static lv_obj_t *my_card(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_add_style(c, &st_card, 0);
    lv_obj_set_size(c, w, h);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

// metric card: accent strip top, caption, big value
static lv_obj_t *metric_card(lv_obj_t *parent, int w, const char *caption, lv_obj_t **value_out)
{
    lv_obj_t *c = my_card(parent, w, 80);

    lv_obj_t *cap = lv_label_create(c);
    lv_obj_add_style(cap, &st_label, 0);
    lv_label_set_text(cap, caption);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 4);

    *value_out = lv_label_create(c);
    lv_obj_add_style(*value_out, &st_value, 0);
    lv_label_set_text(*value_out, "--");
    lv_obj_align(*value_out, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    return c;
}

static lv_obj_t *my_button(lv_obj_t *parent, const char *text, uint16_t bg, uint16_t fg)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, c565(bg), 0);
    lv_obj_set_style_bg_color(btn, c565(C_ACCENT_P), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, 40);
    lv_obj_set_style_pad_hor(btn, 16, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, c565(fg), 0);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    return btn;
}

// ---------- splash ----------

static void build_splash(void)
{
    splash_scr = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(splash_scr);
    lv_obj_set_size(splash_scr, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(splash_scr, c565(C_SIDEBAR), 0);
    lv_obj_set_style_bg_opa(splash_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(splash_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(splash_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(splash_scr, splash_cb, LV_EVENT_CLICKED, NULL);

    // placeholder logo: swap for lv_img_create + LV_IMAGE_DECLARE(your_logo)
    lv_obj_t *logo = lv_image_create(splash_scr);
    lv_image_set_src(logo, &kai_logo);
    lv_image_set_scale(logo, 192);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -50);
    lv_obj_clear_flag(logo, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *name = lv_label_create(splash_scr);
    lv_obj_add_style(name, &st_title, 0);
    lv_label_set_text(name, "Monitoring Module");
    lv_obj_align(name, LV_ALIGN_CENTER, 0, 30);

    lv_obj_t *hint = lv_label_create(splash_scr);
    lv_obj_add_style(hint, &st_label, 0);
    lv_obj_set_style_text_color(hint, c565(C_MUTED), 0);
    lv_label_set_text(hint, "touch to continue");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -40);
}

// ---------- bottom nav ----------

static void build_nav(void)
{
    static const char *names[PAGE_COUNT] = { "Home", "Trend", "Logs", "Settings" };

    nav_bar = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(nav_bar);
    lv_obj_set_size(nav_bar, LCD_H_RES, NAV_H);
    lv_obj_align(nav_bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(nav_bar, c565(C_SIDEBAR), 0);
    lv_obj_set_style_bg_opa(nav_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(nav_bar, c565(C_DIVIDER), 0);
    lv_obj_set_style_border_width(nav_bar, 1, 0);
    lv_obj_set_style_border_side(nav_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_flex_flow(nav_bar, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(nav_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(nav_bar, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_t *b = lv_button_create(nav_bar);
        lv_obj_remove_style_all(b);
        lv_obj_add_style(b, &st_nav_btn, 0);
        lv_obj_add_style(b, &st_nav_btn_on, LV_STATE_CHECKED);
        lv_obj_set_size(b, LCD_H_RES / PAGE_COUNT, NAV_H);
        lv_obj_add_event_cb(b, nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, names[i]);
        lv_obj_center(l);
        nav_btns[i] = b;
    }
}

// ---------- dashboard ----------

static void build_dashboard(lv_obj_t *parent)
{
    page_header(parent, "Dashboard");

    const int card_w = (LCD_H_RES - PAD * 3) / 2;

    lv_obj_t *t = metric_card(parent, card_w, "TEMP", &temp_label);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, PAD, 72);

    lv_obj_t *h = metric_card(parent, card_w, "HUMIDITY", &hum_label);
    lv_obj_align(h, LV_ALIGN_TOP_RIGHT, -PAD, 72);

    lv_obj_t *eth_card = my_card(parent, LCD_H_RES - PAD * 2, 130);
    lv_obj_align(eth_card, LV_ALIGN_TOP_MID, 0, 170);

    lv_obj_t *eth_title = lv_label_create(eth_card);
    lv_obj_add_style(eth_title, &st_accent_txt, 0);
    lv_label_set_text(eth_title, "INTERNET");
    lv_obj_align(eth_title, LV_ALIGN_TOP_LEFT, 0, 0);

    eth_status_label = lv_label_create(eth_card);
    lv_obj_add_style(eth_status_label, &st_value, 0);
    lv_label_set_text(eth_status_label, "Disconnected");
    lv_obj_align(eth_status_label, LV_ALIGN_TOP_LEFT, 0, 26);

    internet_interface_label = lv_label_create(eth_card);
    lv_obj_add_style(internet_interface_label, &st_label, 0);
    lv_label_set_text(internet_interface_label, "Through: None");
    lv_obj_align(internet_interface_label, LV_ALIGN_TOP_LEFT, 0, 62);

    eth_ip_label = lv_label_create(eth_card);
    lv_obj_add_style(eth_ip_label, &st_label, 0);
    lv_label_set_text(eth_ip_label, "IP: --");
    lv_obj_align(eth_ip_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *retry_btn = my_button(eth_card, "Retry", C_ACCENT, C_ACCENT_T);
    lv_obj_align(retry_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(retry_btn, retry_btn_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_add_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_style_pad_bottom(parent, PAD, 0);

    lv_obj_t *sol = my_card(parent, LCD_H_RES - PAD * 2, LV_SIZE_CONTENT);
    lv_obj_align(sol, LV_ALIGN_TOP_MID, 0, 316);

    lv_obj_t *sol_title = lv_label_create(sol);
    lv_obj_add_style(sol_title, &st_accent_txt, 0);
    lv_label_set_text(sol_title, "SOLAR (EPEVER)");
    lv_obj_align(sol_title, LV_ALIGN_TOP_LEFT, 0, 0);

    sol_status_label = lv_label_create(sol);
    lv_obj_add_style(sol_status_label, &st_label, 0);
    lv_label_set_text(sol_status_label, "No data");
    lv_obj_align(sol_status_label, LV_ALIGN_TOP_RIGHT, 0, 0);

    sol_body_label = lv_label_create(sol);
    lv_obj_add_style(sol_body_label, &st_label, 0);
    lv_obj_set_width(sol_body_label, LCD_H_RES - PAD * 2 - 26);
    lv_label_set_text(sol_body_label, "Waiting for controller...");
    lv_obj_align(sol_body_label, LV_ALIGN_TOP_LEFT, 0, 26);
}

static void make_axis(lv_obj_t *parent, lv_obj_t *chart, bool right,
                      int32_t min, int32_t max, lv_color_t col)
{
    lv_obj_update_layout(parent);   // chart size/pos final before read

    lv_obj_t *s = lv_scale_create(parent);
    lv_scale_set_mode(s, right ? LV_SCALE_MODE_VERTICAL_RIGHT : LV_SCALE_MODE_VERTICAL_LEFT);
    lv_obj_set_size(s, 32, 200);    // same as chart height, hardcode
    lv_obj_set_style_pad_all(s, 0, 0);
    lv_scale_set_range(s, min, max);
    lv_scale_set_total_tick_count(s, 6);
    lv_scale_set_major_tick_every(s, 1);
    lv_scale_set_label_show(s, true);

    // label text lives on INDICATOR part in v9
    lv_obj_set_style_text_font(s, &lv_font_montserrat_14, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(s, col, LV_PART_INDICATOR);
    lv_obj_set_style_length(s, 4, LV_PART_INDICATOR);   // major tick len
    lv_obj_set_style_length(s, 3, LV_PART_ITEMS);       // minor tick len
    lv_obj_set_style_line_color(s, col, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(s, col, LV_PART_ITEMS);
    lv_obj_set_style_line_color(s, col, LV_PART_MAIN);

    lv_obj_align_to(s, chart, right ? LV_ALIGN_OUT_RIGHT_MID : LV_ALIGN_OUT_LEFT_MID, 0, 0);
}

static void chart_ranges(lv_obj_t *c, int32_t m1, int32_t m2)
{
#if LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR < 3
    lv_chart_set_range(c, LV_CHART_AXIS_PRIMARY_Y, 0, m1);
    lv_chart_set_range(c, LV_CHART_AXIS_SECONDARY_Y, 0, m2);
#else
    lv_chart_set_axis_range(c, LV_CHART_AXIS_PRIMARY_Y, 0, m1);
    lv_chart_set_axis_range(c, LV_CHART_AXIS_SECONDARY_Y, 0, m2);
#endif
}

static void chart_clear(lv_obj_t *c, lv_chart_series_t *s)
{
#if LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR < 3
    lv_chart_set_all_value(c, s, LV_CHART_POINT_NONE);
#else
    lv_chart_set_all_values(c, s, LV_CHART_POINT_NONE);
#endif
}

/* one legend + dual-axis chart, own container so it can be hidden as a unit */
static lv_obj_t *trend_group(lv_obj_t *card,
                             const char *n1, uint16_t col1,
                             const char *n2, uint16_t col2,
                             int32_t cmax1, int32_t cmax2,
                             int32_t lmax1, int32_t lmax2,
                             lv_obj_t **chart_out,
                             lv_chart_series_t **s1, lv_chart_series_t **s2)
{
    lv_obj_t *g = lv_obj_create(card);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, lv_pct(100), lv_pct(100));
    lv_obj_align(g, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    lv_obj_t *l1 = lv_label_create(g);
    lv_obj_set_style_text_font(l1, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l1, c565(col1), 0);
    lv_label_set_text(l1, n1);
    lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *l2 = lv_label_create(g);
    lv_obj_set_style_text_font(l2, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l2, c565(col2), 0);
    lv_label_set_text(l2, n2);
    lv_obj_align(l2, LV_ALIGN_TOP_RIGHT, 0, 0);

    lv_obj_t *ch = lv_chart_create(g);
    lv_obj_set_size(ch, LCD_H_RES - PAD * 2 - 16 - 64, 200);
    lv_obj_align(ch, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_pad_all(ch, 0, 0);
    lv_obj_set_style_radius(ch, 0, 0);
    lv_obj_set_style_bg_color(ch, c565(C_SURFACE), 0);
    lv_obj_set_style_bg_opa(ch, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ch, 0, 0);
    lv_obj_set_style_line_color(ch, c565(C_DIVIDER), LV_PART_MAIN);
    lv_obj_set_style_size(ch, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(ch, 2, LV_PART_ITEMS);
    lv_chart_set_type(ch, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(ch, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_point_count(ch, TREND_POINTS);
    lv_chart_set_div_line_count(ch, 5, 6);
    chart_ranges(ch, cmax1, cmax2);

    *s1 = lv_chart_add_series(ch, c565(col1), LV_CHART_AXIS_PRIMARY_Y);
    *s2 = lv_chart_add_series(ch, c565(col2), LV_CHART_AXIS_SECONDARY_Y);
    chart_clear(ch, *s1);
    chart_clear(ch, *s2);

    make_axis(g, ch, false, 0, lmax1, c565(col1));
    make_axis(g, ch, true,  0, lmax2, c565(col2));
    *chart_out = ch;
    return g;
}

static void trend_mode_cb(lv_event_t *e)
{
    (void)e;
    trend_show_solar = !trend_show_solar;
    if (trend_show_solar) {
        lv_obj_add_flag(trend_grp_env, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(trend_grp_sol, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(trend_grp_sol, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(trend_grp_env, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(trend_mode_label, trend_show_solar ? "Solar" : "Env");
}

static void build_trend_screen(lv_obj_t *parent)
{
    lv_obj_t *hdr = page_header(parent, "Trend");
    lv_obj_t *mode_btn = my_button(hdr, "Env", C_CONTROL, C_TEXT);
    lv_obj_set_height(mode_btn, 36);
    lv_obj_align(mode_btn, LV_ALIGN_RIGHT_MID, -PAD, 0);
    lv_obj_add_event_cb(mode_btn, trend_mode_cb, LV_EVENT_CLICKED, NULL);
    trend_mode_label = lv_obj_get_child(mode_btn, 0);

    lv_obj_t *card = my_card(parent, LCD_H_RES - PAD * 2, CONTENT_H - 56 - PAD * 2);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_set_style_pad_all(card, 8, 0);

    /* env: temp C 0-50 (left), hum % 0-100 (right) */
    trend_grp_env = trend_group(card, "Temp C", C_ACCENT, "Hum %", C_INFO,
                                50, 100, 50, 100,
                                &trend_chart, &ser_temp, &ser_hum);

    /* solar: PV W 0-300 (left), battery V 0-30 (right, stored x10) */
    trend_grp_sol = trend_group(card, "PV W", C_ACCENT, "Batt V", C_INFO,
                                300, 300, 300, 30,
                                &sol_chart, &sol_ser_pv, &sol_ser_bat);
    lv_obj_add_flag(trend_grp_sol, LV_OBJ_FLAG_HIDDEN);
}

// ---------- logs ----------

static void build_log_screen(lv_obj_t *parent)
{
    page_header(parent, "Logs");

    lv_obj_t *log_card = my_card(parent, LCD_H_RES - PAD * 2, CONTENT_H - 56 - PAD * 2);
    lv_obj_align(log_card, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_add_flag(log_card, LV_OBJ_FLAG_SCROLLABLE);

    log_label = lv_label_create(log_card);
    lv_obj_add_style(log_label, &st_label, 0);
    lv_label_set_long_mode(log_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(log_label, LCD_H_RES - PAD * 2 - 24 - 2);
    lv_label_set_text(log_label, "no logs yet");
    lv_obj_align(log_label, LV_ALIGN_TOP_LEFT, 0, 0);

    ui_log_bind_label(log_label);
}

// ---------- settings ----------

/* These widgets are owned exclusively by the LVGL task. */
static lv_obj_t *s_settings_body;
static lv_obj_t *s_network_mode;
static lv_obj_t *s_ssid;
static lv_obj_t *s_password;
static lv_obj_t *s_show_password;
static lv_obj_t *s_network_status;
static lv_obj_t *s_network_result;
static lv_obj_t *s_apply_button;
static lv_obj_t *s_keyboard;
static bool s_settings_loaded;
static bool s_waiting_for_apply;
static uint32_t s_completed_before_apply;
static bool s_status_seen;
static network_status_t s_previous_status;
static lv_obj_t *s_ap_dropdown;
static lv_obj_t *s_scan_button;
static lv_obj_t *s_scan_label;
static lv_obj_t *s_mqtt_status;
static lv_obj_t *s_mqtt_enabled;
static lv_obj_t *s_mqtt_uri;
static lv_obj_t *s_mqtt_topic;
static lv_obj_t *s_mqtt_username;
static lv_obj_t *s_mqtt_password;
static lv_obj_t *s_mqtt_client_id;
static lv_obj_t *s_mqtt_interval;
static lv_obj_t *s_mqtt_qos;
static lv_obj_t *s_mqtt_retain;
static lv_obj_t *s_mqtt_save;
static lv_obj_t *s_mqtt_result;
static lv_obj_t *s_ep_status;
static lv_obj_t *s_ep_enabled;
static lv_obj_t *s_ep_json;
static lv_obj_t *s_ep_base;
static lv_obj_t *s_ep_interval;
static lv_obj_t *s_ep_field_dd;
static lv_obj_t *s_ep_field_en;
static lv_obj_t *s_ep_field_topic;
static lv_obj_t *s_ep_save;
static lv_obj_t *s_ep_result;
static epever_mqtt_cfg_t s_ep_cfg;
static bool s_ep_loaded;
static int s_ep_sel;
static bool s_mqtt_loaded;
static bool s_mqtt_waiting;
static uint32_t s_mqtt_before_apply;
static mqtt_settings_t s_mqtt_settings_snapshot;
static wifi_scan_result_t s_ui_scan;
static wifi_scan_result_t s_scan_snapshot;
static uint32_t s_last_scan_request_ms;

static void style_control(lv_obj_t *obj)
{
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, c565(C_CONTROL), LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, c565(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(obj, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, c565(C_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_50, LV_PART_MAIN | LV_STATE_DISABLED);
}

static void dropdown_style_cb(lv_event_t *event)
{
    lv_obj_t *list = lv_dropdown_get_list(lv_event_get_target_obj(event));
    if (list != NULL) {
        style_control(list);
        lv_obj_set_style_bg_color(list, c565(C_ACCENT),
                                 LV_PART_SELECTED | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(list, c565(C_ACCENT_T),
                                   LV_PART_SELECTED | LV_STATE_CHECKED);
    }
}

static void set_disabled(lv_obj_t *obj, bool disabled)
{
    if (disabled) {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
    }
}

static void settings_keyboard_hide(void)
{
    if (s_keyboard == NULL) {
        return;
    }
    lv_obj_t *textarea = lv_keyboard_get_textarea(s_keyboard);
    lv_keyboard_set_textarea(s_keyboard, NULL);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(s_settings_body, CONTENT_H - 72 - PAD);
    if (textarea != NULL) {
        lv_obj_remove_state(textarea, LV_STATE_FOCUSED);
    }
}

static void textarea_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        settings_keyboard_hide();
        return;
    }
    if (code != LV_EVENT_FOCUSED && code != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_t *textarea = lv_event_get_target_obj(event);
    if (lv_obj_has_state(textarea, LV_STATE_DISABLED)) {
        return;
    }
    int32_t keyboard_height = CONTENT_H / 2;
    lv_obj_set_height(s_settings_body, CONTENT_H - 72 - PAD - keyboard_height);
    lv_keyboard_set_mode(s_keyboard,
                         (textarea == s_mqtt_interval || textarea == s_ep_interval)
                         ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_keyboard, textarea);
    lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_keyboard);
    lv_obj_update_layout(s_settings_body);
    lv_obj_scroll_to_view(textarea, LV_ANIM_ON);
}

static void keyboard_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        settings_keyboard_hide();
    }
}

static void network_mode_cb(lv_event_t *event)
{
    (void)event;
    bool ethernet_only = lv_dropdown_get_selected(s_network_mode) ==
                         NETWORK_MODE_ETHERNET;
    set_disabled(s_ssid, ethernet_only);
    set_disabled(s_password, ethernet_only);
    set_disabled(s_show_password, ethernet_only);
    set_disabled(s_scan_button, ethernet_only);
    set_disabled(s_ap_dropdown, ethernet_only || s_ui_scan.count == 0);
    if (!ethernet_only && wifi_manager_scan_now() == ESP_OK) {
        s_last_scan_request_ms = lv_tick_get();
    }
    settings_keyboard_hide();
}

static void scan_button_cb(lv_event_t *event)
{
    (void)event;
    esp_err_t err = wifi_manager_scan_now();
    if (err == ESP_OK) {
        s_last_scan_request_ms = lv_tick_get();
        lv_label_set_text(s_scan_label, "Scan queued...");
    } else {
        lv_label_set_text_fmt(s_scan_label, "Scan: %s", esp_err_to_name(err));
    }
}

static void ap_dropdown_cb(lv_event_t *event)
{
    (void)event;
    uint32_t selected = lv_dropdown_get_selected(s_ap_dropdown);
    if (selected == 0 || selected > s_ui_scan.count) {
        return;
    }
    const wifi_scan_ap_t *ap = &s_ui_scan.aps[selected - 1];
    if (ap->open || strcmp(lv_textarea_get_text(s_ssid), ap->ssid) != 0) {
        lv_textarea_set_text(s_password, "");
    }
    lv_textarea_set_text(s_ssid, ap->ssid);
}

static void update_scan_ui(void)
{
    if (wifi_manager_get_scan_result(&s_scan_snapshot) != ESP_OK) {
        return;
    }
    if (s_scan_snapshot.scanning) {
        lv_label_set_text(s_scan_label, "Scanning...");
    } else if (s_scan_snapshot.pending) {
        lv_label_set_text(s_scan_label, "Scan queued; waiting for connection attempt.");
    } else if (s_scan_snapshot.error != ESP_OK) {
        lv_label_set_text_fmt(s_scan_label, "Scan: %s",
                              esp_err_to_name(s_scan_snapshot.error));
    } else if (s_scan_snapshot.revision != 0) {
        lv_label_set_text_fmt(s_scan_label, "%u networks; auto refresh: 30 s",
                              (unsigned int)s_scan_snapshot.count);
    }
    if (s_scan_snapshot.revision == s_ui_scan.revision ||
        lv_dropdown_is_open(s_ap_dropdown)) {
        return;
    }
    /* Preserve index->SSID mapping while user has list open. */
    s_ui_scan = s_scan_snapshot;
    static char options[64 + WIFI_SCAN_MAX_APS * 64];
    size_t used = (size_t)snprintf(options, sizeof(options), "Select network");
    uint32_t selected = 0;
    for (uint16_t i = 0; i < s_ui_scan.count; i++) {
        char display_ssid[33];
        memcpy(display_ssid, s_ui_scan.aps[i].ssid, sizeof(display_ssid));
        for (size_t j = 0; j < 32 && display_ssid[j] != '\0'; j++) {
            unsigned char character = (unsigned char)display_ssid[j];
            if (character < 32 || character == 127) {
                display_ssid[j] = ' ';
            }
        }
        int written = snprintf(options + used, sizeof(options) - used,
            "\n%s (%d dBm, %s)", display_ssid, (int)s_ui_scan.aps[i].rssi,
            s_ui_scan.aps[i].open ? "open" : "secured");
        if (written < 0 || (size_t)written >= sizeof(options) - used) {
            break;
        }
        used += (size_t)written;
        if (strcmp(lv_textarea_get_text(s_ssid), s_ui_scan.aps[i].ssid) == 0) {
            selected = (uint32_t)i + 1;
        }
    }
    lv_dropdown_set_options(s_ap_dropdown, options);
    lv_dropdown_set_selected(s_ap_dropdown, selected);
    bool ethernet_only = lv_dropdown_get_selected(s_network_mode) == NETWORK_MODE_ETHERNET;
    set_disabled(s_ap_dropdown, ethernet_only || s_ui_scan.count == 0);
}

static bool mqtt_copy_text(char *destination, size_t capacity, lv_obj_t *textarea)
{
    const char *text = lv_textarea_get_text(textarea);
    size_t length = strlen(text);
    if (length >= capacity) {
        return false;
    }
    memcpy(destination, text, length + 1);
    return true;
}

static void mqtt_save_cb(lv_event_t *event)
{
    (void)event;
    mqtt_settings_t settings = {0};
    if (!mqtt_copy_text(settings.broker_uri, sizeof(settings.broker_uri), s_mqtt_uri) ||
        !mqtt_copy_text(settings.username, sizeof(settings.username), s_mqtt_username) ||
        !mqtt_copy_text(settings.password, sizeof(settings.password), s_mqtt_password) ||
        !mqtt_copy_text(settings.client_id, sizeof(settings.client_id), s_mqtt_client_id) ||
        !mqtt_copy_text(settings.telemetry_topic, sizeof(settings.telemetry_topic), s_mqtt_topic)) {
        lv_label_set_text(s_mqtt_result, "MQTT field exceeds byte limit.");
        return;
    }
    const char *interval = lv_textarea_get_text(s_mqtt_interval);
    char *end;
    unsigned long seconds = strtoul(interval, &end, 10);
    if (interval[0] == '\0' || *end != '\0' || seconds < 1 || seconds > 3600) {
        lv_label_set_text(s_mqtt_result, "Publish interval: 1-3600 seconds.");
        return;
    }
    settings.publish_interval_ms = (uint32_t)seconds * 1000;
    settings.max_sample_age_ms = settings.publish_interval_ms * 3;
    settings.enabled = lv_obj_has_state(s_mqtt_enabled, LV_STATE_CHECKED);
    settings.retain = lv_obj_has_state(s_mqtt_retain, LV_STATE_CHECKED);
    settings.qos = (uint8_t)lv_dropdown_get_selected(s_mqtt_qos);
    mqtt_publisher_status_t status;
    esp_err_t err = mqtt_get_status(&status);
    if (err == ESP_OK) {
        s_mqtt_before_apply = status.completed_requests;
        err = mqtt_apply_settings(&settings);
    }
    memset(&settings, 0, sizeof(settings));
    if (err == ESP_OK) {
        s_mqtt_waiting = true;
        settings_keyboard_hide();
        set_disabled(s_mqtt_save, true);
        lv_label_set_text(s_mqtt_result, "Applying MQTT settings...");
    } else if (err == ESP_ERR_INVALID_ARG) {
        lv_label_set_text(s_mqtt_result,
            "Enabled MQTT needs mqtt:// or mqtts:// broker and topic without + or #.");
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        lv_label_set_text(s_mqtt_result, "TLS needs trusted CA or enabled certificate bundle.");
    } else {
        lv_label_set_text_fmt(s_mqtt_result, "MQTT save: %s", esp_err_to_name(err));
    }
}

static void update_mqtt_ui(void)
{
    if (!s_mqtt_loaded && mqtt_get_settings(&s_mqtt_settings_snapshot) == ESP_OK) {
        const mqtt_settings_t *settings = &s_mqtt_settings_snapshot;
        lv_textarea_set_text(s_mqtt_uri, settings->broker_uri);
        lv_textarea_set_text(s_mqtt_username, settings->username);
        lv_textarea_set_text(s_mqtt_password, settings->password);
        lv_textarea_set_text(s_mqtt_client_id, settings->client_id);
        lv_textarea_set_text(s_mqtt_topic, settings->telemetry_topic);
        char interval[16];
        snprintf(interval, sizeof(interval), "%lu",
                 (unsigned long)(settings->publish_interval_ms / 1000));
        lv_textarea_set_text(s_mqtt_interval, interval);
        lv_dropdown_set_selected(s_mqtt_qos, settings->qos);
        if (settings->enabled) {
            lv_obj_add_state(s_mqtt_enabled, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_mqtt_enabled, LV_STATE_CHECKED);
        }
        if (settings->retain) {
            lv_obj_add_state(s_mqtt_retain, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_mqtt_retain, LV_STATE_CHECKED);
        }
        memset(&s_mqtt_settings_snapshot, 0, sizeof(s_mqtt_settings_snapshot));
        s_mqtt_loaded = true;
    }
    mqtt_publisher_status_t status;
    esp_err_t err = mqtt_get_status(&status);
    if (err == ESP_ERR_INVALID_STATE) {
        lv_label_set_text(s_mqtt_status, "MQTT: call mqtt_publisher_init(NULL) at boot");
        set_disabled(s_mqtt_save, true);
    } else if (err == ESP_OK) {
        lv_label_set_text_fmt(s_mqtt_status,
            "MQTT: %s\nQueued to ESP-MQTT: %lu\nBroker ACKs: %lu\nLast error: %s",
            !status.enabled ? "Disabled" : status.connected ? "Connected"
                : status.started ? "Connecting" : "Waiting for IP",
            (unsigned long)status.enqueued_messages,
            (unsigned long)status.acknowledged_messages,
            esp_err_to_name(status.last_error));
        set_disabled(s_mqtt_save, status.busy || !s_mqtt_loaded);
        if (s_mqtt_waiting && !status.busy &&
            status.completed_requests != s_mqtt_before_apply) {
            s_mqtt_waiting = false;
            if (status.apply_error != ESP_OK) {
                lv_label_set_text_fmt(s_mqtt_result, "MQTT apply failed: %s",
                                     esp_err_to_name(status.apply_error));
            } else if (status.save_error != ESP_OK) {
                lv_label_set_text_fmt(s_mqtt_result, "Applied; NVS save failed: %s",
                                     esp_err_to_name(status.save_error));
            } else {
                lv_label_set_text(s_mqtt_result, "MQTT settings saved.");
            }
        }
    }
}

static void show_password_cb(lv_event_t *event)
{
    (void)event;
    lv_textarea_set_password_mode(s_password,
        !lv_obj_has_state(s_show_password, LV_STATE_CHECKED));
}

static void network_apply_cb(lv_event_t *event)
{
    (void)event;
    const char *ssid = lv_textarea_get_text(s_ssid);
    const char *password = lv_textarea_get_text(s_password);
    network_settings_t settings = {
        .mode = (network_mode_t)lv_dropdown_get_selected(s_network_mode),
    };
    size_t ssid_len = strlen(ssid);
    size_t password_len = strlen(password);
    /* LVGL limits characters; the Wi-Fi driver limits BYTES. Check both. */
    if (ssid_len > 32 || password_len > 64) {
        lv_label_set_text(s_network_result, "SSID max 32 bytes; password max 64 bytes.");
        return;
    }
    if (settings.mode != NETWORK_MODE_ETHERNET && ssid_len == 0) {
        lv_label_set_text(s_network_result, "Enter Wi-Fi SSID.");
        return;
    }
    memcpy(settings.ssid, ssid, ssid_len);
    memcpy(settings.password, password, password_len);

    network_status_t status;
    esp_err_t err = wifi_manager_get_status(&status);
    if (err == ESP_OK && status.busy) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        s_completed_before_apply = status.completed_requests;
        err = wifi_manager_apply(&settings);
    }
    memset(&settings, 0, sizeof(settings));
    if (err == ESP_OK) {
        s_waiting_for_apply = true;
        settings_keyboard_hide();
        lv_label_set_text(s_network_result, "Applying...");
        set_disabled(s_apply_button, true);
    } else if (err == ESP_ERR_INVALID_ARG) {
        lv_label_set_text(s_network_result,
                          "Password: empty, 8-63 characters, or 64 hex digits.");
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        lv_label_set_text(s_network_result, "Ethernet driver unavailable.");
    } else {
        lv_label_set_text_fmt(s_network_result, "Apply: %s", esp_err_to_name(err));
    }
}

static const char *link_text(bool enabled, bool linked, esp_ip4_addr_t ip)
{
    if (!enabled) {
        return "Off";
    }
    if (!linked) {
        return "Disconnected";
    }
    return ip.addr == 0 ? "Waiting for DHCP" : "Connected";
}

/* LVGL timer callbacks already run inside the project's LVGL task/lock. */
static void network_ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update_scan_ui();
    update_mqtt_ui();
    if (!s_settings_loaded) {
        network_settings_t settings;
        if (wifi_manager_get_settings(&settings) == ESP_OK) {
            lv_dropdown_set_selected(s_network_mode, (uint32_t)settings.mode);
            lv_textarea_set_text(s_ssid, settings.ssid);
            lv_textarea_set_text(s_password, settings.password);
            memset(&settings, 0, sizeof(settings));
            s_settings_loaded = true;
            network_mode_cb(NULL);
        }
    }

    network_status_t status;
    if (wifi_manager_get_status(&status) != ESP_OK) {
        return;
    }
    if (!lv_obj_has_flag(pages[PAGE_SETTINGS], LV_OBJ_FLAG_HIDDEN) &&
        lv_dropdown_get_selected(s_network_mode) != NETWORK_MODE_ETHERNET &&
        (uint32_t)(lv_tick_get() - s_last_scan_request_ms) >= 30000 &&
        !s_scan_snapshot.scanning && !s_scan_snapshot.pending) {
        if (wifi_manager_scan_now() == ESP_OK) {
            s_last_scan_request_ms = lv_tick_get();
        }
    }
    if (s_status_seen && status.ethernet_link != s_previous_status.ethernet_link) {
        ui_log("Ethernet: %s", status.ethernet_link ? "Link Up" : "Link Down");
    }
    if (s_status_seen && status.wifi_link != s_previous_status.wifi_link) {
        ui_log("Wi-Fi: %s", status.wifi_link ? "Connected" : "Disconnected");
    }
    s_previous_status = status;
    s_status_seen = true;
    const char *wifi_text = link_text(status.wifi_enabled, status.wifi_link,
                                      status.wifi_ip);
    const char *ethernet_text = status.ethernet_available
        ? link_text(status.ethernet_enabled, status.ethernet_link, status.ethernet_ip)
        : "Unavailable";
    char text[256];
    snprintf(text, sizeof(text),
             "Wi-Fi: %s\nWi-Fi IP: " IPSTR "\nEthernet: %s\nEthernet IP: " IPSTR
             "\nWi-Fi disconnect reason: %u",
             wifi_text, IP2STR(&status.wifi_ip),
             ethernet_text, IP2STR(&status.ethernet_ip),
             (unsigned int)status.disconnect_reason);
    if (strcmp(lv_label_get_text(s_network_status), text) != 0) {
        lv_label_set_text(s_network_status, text);
    }
    /* Display actual default route, including automatic interface failover. */
    if (eth_status_label != NULL) {
        esp_netif_t *route = esp_netif_get_default_netif();
        esp_netif_ip_info_t ip_info = {0};
        esp_netif_t *wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        const char *interface_name = "None";
        bool connected = route != NULL && esp_netif_is_netif_up(route) &&
                         esp_netif_get_ip_info(route, &ip_info) == ESP_OK &&
                         ip_info.ip.addr != 0;
        if (connected) {
            if (route == wifi_netif) {
                interface_name = "Wi-Fi";
            } else if (route == eth_get_netif()) {
                interface_name = "Ethernet";
            } else {
                connected = false;
            }
        }
        lv_label_set_text(eth_status_label,
                          connected ? "Connected" : "Disconnected");
        lv_obj_set_style_text_color(eth_status_label,
                                   c565(connected ? C_SUCCESS : C_DANGER), 0);
        lv_label_set_text_fmt(internet_interface_label, "Through: %s", interface_name);
        if (connected) {
            lv_label_set_text_fmt(eth_ip_label, "IP: " IPSTR, IP2STR(&ip_info.ip));
        } else {
            lv_label_set_text(eth_ip_label, "IP: --");
        }
    }
    set_disabled(s_apply_button, status.busy || !s_settings_loaded);
    if (s_waiting_for_apply && !status.busy &&
        status.completed_requests != s_completed_before_apply) {
        s_waiting_for_apply = false;
        if (status.apply_error != ESP_OK) {
            lv_label_set_text_fmt(s_network_result, "Apply failed: %s",
                                  esp_err_to_name(status.apply_error));
        } else if (status.save_error != ESP_OK) {
            lv_label_set_text_fmt(s_network_result, "Applied; save failed: %s",
                                  esp_err_to_name(status.save_error));
        } else {
            lv_label_set_text(s_network_result, "Saved. Check connection status above.");
        }
    } else if (!s_waiting_for_apply && status.apply_error != ESP_OK) {
        lv_label_set_text_fmt(s_network_result, "Apply failed: %s",
                              esp_err_to_name(status.apply_error));
    }
}

static lv_obj_t *settings_card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *card = my_card(parent, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_t *label = lv_label_create(card);
    lv_obj_add_style(label, &st_accent_txt, 0);
    lv_label_set_text(label, title);
    return card;
}

static lv_obj_t *settings_textarea(lv_obj_t *parent, const char *caption,
                                  uint32_t max_length, bool password)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_add_style(label, &st_label, 0);
    lv_label_set_text(label, caption);
    lv_obj_t *textarea = lv_textarea_create(parent);
    style_control(textarea);
    lv_obj_set_width(textarea, lv_pct(100));
    lv_textarea_set_one_line(textarea, true);
    lv_textarea_set_max_length(textarea, max_length);
    lv_textarea_set_password_mode(textarea, password);
    lv_obj_add_event_cb(textarea, textarea_cb, LV_EVENT_ALL, NULL);
    return textarea;
}


static const uint32_t timeout_opts_s[] = { 15, 30, 60, 120, 300, 600, 0 };
#define TIMEOUT_OPT_COUNT (sizeof(timeout_opts_s) / sizeof(timeout_opts_s[0]))

static void timeout_dd_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target_obj(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_VALUE_CHANGED) {
        uint16_t idx = lv_dropdown_get_selected(dd);
        if (idx < TIMEOUT_OPT_COUNT) {
            display_set_timeout(timeout_opts_s[idx]);
            ui_log("Screen timeout: %lu s", (unsigned long)timeout_opts_s[idx]);
        }
    } else if (code == LV_EVENT_READY) {   // list just opened, style it
        lv_obj_t *list = lv_dropdown_get_list(dd);
        if (!list) return;
        lv_obj_set_style_bg_color(list, c565(C_CONTROL), 0);
        lv_obj_set_style_text_color(list, c565(C_TEXT), 0);
        lv_obj_set_style_text_font(list, &lv_font_montserrat_14, 0);
        lv_obj_set_style_border_color(list, c565(C_BORDER), 0);
        lv_obj_set_style_radius(list, 0, 0);
        lv_obj_set_style_bg_color(list, c565(C_ACCENT), LV_STATE_CHECKED);
        lv_obj_set_style_text_color(list, c565(C_ACCENT_T), LV_STATE_CHECKED);
    }
}

// ---------- touch test ----------

#define CAL_TARGETS 5
#define CAL_MARGIN  30
#define CAL_XHAIR   28

static lv_obj_t *cal_overlay = NULL;
static lv_obj_t *cal_target = NULL;
static lv_obj_t *cal_dot = NULL;
static lv_obj_t *cal_info = NULL;
static int cal_step;
static float cal_err_sum;
static float cal_err_max;

static void cal_target_pos(int step, int32_t *x, int32_t *y)
{
    switch (step) {
    case 0:  *x = CAL_MARGIN;               *y = CAL_MARGIN;               break;
    case 1:  *x = LCD_H_RES - CAL_MARGIN;   *y = CAL_MARGIN;               break;
    case 2:  *x = LCD_H_RES - CAL_MARGIN;   *y = LCD_V_RES - CAL_MARGIN;   break;
    case 3:  *x = CAL_MARGIN;               *y = LCD_V_RES - CAL_MARGIN;   break;
    default: *x = LCD_H_RES / 2;            *y = LCD_V_RES / 2;            break;
    }
}

static void cal_show_target(void)
{
    if (cal_step >= CAL_TARGETS) {
        lv_obj_add_flag(cal_target, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int32_t x, y;
    cal_target_pos(cal_step, &x, &y);
    lv_obj_set_pos(cal_target, x - CAL_XHAIR / 2, y - CAL_XHAIR / 2);
    lv_obj_remove_flag(cal_target, LV_OBJ_FLAG_HIDDEN);
}

static void cal_reset(void)
{
    cal_step = 0;
    cal_err_sum = 0;
    cal_err_max = 0;
    lv_obj_add_flag(cal_dot, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(cal_info, "Tap target 1/5");
    cal_show_target();
}

static void cal_close(void)
{
    if (cal_overlay == NULL) return;
    lv_obj_t *o = cal_overlay;
    cal_overlay = NULL;
    cal_target = cal_dot = cal_info = NULL;
    lv_obj_delete_async(o);     // may run from child's event cb
}

static void cal_close_cb(lv_event_t *e)   { (void)e; cal_close(); }
static void cal_restart_cb(lv_event_t *e) { (void)e; cal_reset(); }

static void cal_touch_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev == NULL || cal_overlay == NULL) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_obj_set_pos(cal_dot, p.x - 4, p.y - 4);
    lv_obj_remove_flag(cal_dot, LV_OBJ_FLAG_HIDDEN);

    if (lv_event_get_code(e) != LV_EVENT_PRESSED || cal_step >= CAL_TARGETS) return;

    int32_t tx, ty;
    cal_target_pos(cal_step, &tx, &ty);
    int32_t dx = p.x - tx, dy = p.y - ty;
    float err = sqrtf((float)(dx * dx + dy * dy));
    cal_err_sum += err;
    if (err > cal_err_max) cal_err_max = err;
    cal_step++;

    if (cal_step < CAL_TARGETS) {
        lv_label_set_text_fmt(cal_info, "Tap target %d/%d\nlast: dx=%+d dy=%+d px",
                              cal_step + 1, CAL_TARGETS, (int)dx, (int)dy);
    } else {
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "Done\navg err %.1f px\nmax err %.1f px\nDraw on screen to test",
                 (double)(cal_err_sum / CAL_TARGETS), (double)cal_err_max);
        lv_label_set_text_fmt(cal_info, "%s", buf);
    }
    cal_show_target();

    char lg[48];
    snprintf(lg, sizeof(lg), "%.1f", (double)err);
    ui_log("Touch test: dx=%+d dy=%+d (%s px)", (int)dx, (int)dy, lg);
}

static void cal_open_cb(lv_event_t *e)
{
    (void)e;
    if (cal_overlay != NULL) return;
    settings_keyboard_hide();

    cal_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(cal_overlay);
    lv_obj_set_size(cal_overlay, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(cal_overlay, c565(C_BG), 0);
    lv_obj_set_style_bg_opa(cal_overlay, LV_OPA_COVER, 0);
    lv_obj_remove_flag(cal_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cal_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cal_overlay, cal_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(cal_overlay, cal_touch_cb, LV_EVENT_PRESSING, NULL);

    cal_info = lv_label_create(cal_overlay);
    lv_obj_add_style(cal_info, &st_label, 0);
    lv_obj_set_style_text_align(cal_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(cal_info, 200);
    lv_obj_align(cal_info, LV_ALIGN_TOP_MID, 0, 50);

    // crosshair = two bars
    cal_target = lv_obj_create(cal_overlay);
    lv_obj_remove_style_all(cal_target);
    lv_obj_set_size(cal_target, CAL_XHAIR, CAL_XHAIR);
    lv_obj_remove_flag(cal_target, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *bar = lv_obj_create(cal_target);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, i ? 2 : CAL_XHAIR, i ? CAL_XHAIR : 2);
        lv_obj_set_style_bg_color(bar, c565(C_ACCENT), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(bar);
    }

    cal_dot = lv_obj_create(cal_overlay);
    lv_obj_remove_style_all(cal_dot);
    lv_obj_set_size(cal_dot, 8, 8);
    lv_obj_set_style_radius(cal_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cal_dot, c565(C_SUCCESS), 0);
    lv_obj_set_style_bg_opa(cal_dot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(cal_dot, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *restart = my_button(cal_overlay, "Restart", C_CONTROL, C_TEXT);
    lv_obj_align(restart, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_obj_add_event_cb(restart, cal_restart_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *close = my_button(cal_overlay, "Close", C_ACCENT, C_ACCENT_T);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_add_event_cb(close, cal_close_cb, LV_EVENT_CLICKED, NULL);

    cal_reset();
}

static void set_check(lv_obj_t *o, bool on)
{
    if (on) {
        lv_obj_add_state(o, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(o, LV_STATE_CHECKED);
    }
}

/* widgets -> RAM copy for selected field */
static void ep_store_field(void)
{
    snprintf(s_ep_cfg.suffix[s_ep_sel], EPEVER_SUFFIX_MAX, "%s",
             lv_textarea_get_text(s_ep_field_topic));
    if (lv_obj_has_state(s_ep_field_en, LV_STATE_CHECKED)) {
        s_ep_cfg.field_mask |= (1u << s_ep_sel);
    } else {
        s_ep_cfg.field_mask &= ~(1u << s_ep_sel);
    }
}

static void ep_load_field(void)
{
    lv_textarea_set_text(s_ep_field_topic, s_ep_cfg.suffix[s_ep_sel]);
    set_check(s_ep_field_en, (s_ep_cfg.field_mask >> s_ep_sel) & 1u);
}

static void ep_load_all(void)
{
    char b[12];
    s_ep_sel = 0;
    lv_dropdown_set_selected(s_ep_field_dd, 0);
    set_check(s_ep_enabled, s_ep_cfg.enabled);
    set_check(s_ep_json, s_ep_cfg.json_mode);
    lv_textarea_set_text(s_ep_base, s_ep_cfg.base);
    snprintf(b, sizeof(b), "%lu", (unsigned long)s_ep_cfg.interval_s);
    lv_textarea_set_text(s_ep_interval, b);
    ep_load_field();
}

static void ep_field_dd_cb(lv_event_t *event)
{
    (void)event;
    ep_store_field();
    s_ep_sel = (int)lv_dropdown_get_selected(s_ep_field_dd);
    ep_load_field();
}

static void ep_defaults_cb(lv_event_t *event)
{
    (void)event;
    epever_mqtt_defaults(&s_ep_cfg);
    ep_load_all();
    lv_label_set_text(s_ep_result, "Defaults loaded. Press Save to keep.");
}

static void ep_save_cb(lv_event_t *event)
{
    (void)event;
    ep_store_field();
    const char *base = lv_textarea_get_text(s_ep_base);
    if (strlen(base) >= EPEVER_BASE_MAX) {
        lv_label_set_text(s_ep_result, "Base topic too long.");
        return;
    }
    const char *interval = lv_textarea_get_text(s_ep_interval);
    char *end;
    unsigned long seconds = strtoul(interval, &end, 10);
    if (interval[0] == '\0' || *end != '\0' || seconds < 1 || seconds > 3600) {
        lv_label_set_text(s_ep_result, "Publish interval: 1-3600 seconds.");
        return;
    }
    snprintf(s_ep_cfg.base, sizeof(s_ep_cfg.base), "%s", base);
    s_ep_cfg.interval_s = (uint32_t)seconds;
    s_ep_cfg.enabled = lv_obj_has_state(s_ep_enabled, LV_STATE_CHECKED);
    s_ep_cfg.json_mode = lv_obj_has_state(s_ep_json, LV_STATE_CHECKED);

    esp_err_t err = epever_apply_mqtt_cfg(&s_ep_cfg);
    if (err == ESP_OK) {
        settings_keyboard_hide();
        lv_label_set_text(s_ep_result, "EPever MQTT saved.");
    } else if (err == ESP_ERR_INVALID_ARG) {
        lv_label_set_text(s_ep_result,
            "Bad topic: no + or #, no leading/trailing /, enabled field needs name.");
    } else {
        lv_label_set_text_fmt(s_ep_result, "EPever save: %s", esp_err_to_name(err));
    }
}

static void update_ep_ui(void)
{
    if (!s_ep_loaded && epever_get_mqtt_cfg(&s_ep_cfg) == ESP_OK) {
        ep_load_all();
        s_ep_loaded = true;
    }
    set_disabled(s_ep_save, !s_ep_loaded);

    epever_data_t d;
    if (epever_get_data(&d) != ESP_OK) {
        return;
    }
    char text[160];
    snprintf(text, sizeof(text), "Link: %s\nOK polls: %lu  errors: %lu\nLast error: %s",
             d.online ? "online" : "offline",
             (unsigned long)d.ok_cycles, (unsigned long)d.err_cycles,
             esp_err_to_name(d.last_err));
    if (strcmp(lv_label_get_text(s_ep_status), text) != 0) {
        lv_label_set_text(s_ep_status, text);
    }
}

static void build_settings_screen(lv_obj_t *parent)
{
    page_header(parent, "Settings");

    s_settings_body = lv_obj_create(parent);
    lv_obj_remove_style_all(s_settings_body);
    lv_obj_set_size(s_settings_body, LCD_H_RES - PAD * 2, CONTENT_H - 72 - PAD);
    lv_obj_align(s_settings_body, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_set_flex_flow(s_settings_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_settings_body, PAD, 0);
    lv_obj_set_scroll_dir(s_settings_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_settings_body, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *label;

    lv_obj_t *network_card = settings_card(s_settings_body, "NETWORK");
    label = lv_label_create(network_card);
    lv_obj_add_style(label, &st_label, 0);
    lv_label_set_text(label, "Interface");
    s_network_mode = lv_dropdown_create(network_card);
    lv_dropdown_set_options(s_network_mode, "Wi-Fi\nEthernet\nBoth");
    lv_obj_set_width(s_network_mode, lv_pct(100));
    style_control(s_network_mode);
    lv_obj_add_event_cb(s_network_mode, network_mode_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_network_mode, dropdown_style_cb, LV_EVENT_READY, NULL);

    label = lv_label_create(network_card);
    lv_obj_add_style(label, &st_label, 0);
    lv_label_set_text(label, "Available Wi-Fi networks");
    s_ap_dropdown = lv_dropdown_create(network_card);
    style_control(s_ap_dropdown);
    lv_obj_set_width(s_ap_dropdown, lv_pct(100));
    lv_dropdown_set_options(s_ap_dropdown, "Select network");
    set_disabled(s_ap_dropdown, true);
    lv_obj_add_event_cb(s_ap_dropdown, ap_dropdown_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_ap_dropdown, dropdown_style_cb, LV_EVENT_READY, NULL);
    s_scan_button = my_button(network_card, "Scan now", C_CONTROL, C_TEXT);
    lv_obj_add_event_cb(s_scan_button, scan_button_cb, LV_EVENT_CLICKED, NULL);
    s_scan_label = lv_label_create(network_card);
    lv_obj_add_style(s_scan_label, &st_label, 0);
    lv_obj_set_width(s_scan_label, lv_pct(100));
    lv_label_set_long_mode(s_scan_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_scan_label, "Select Wi-Fi/Both for automatic discovery.");

    s_ssid = settings_textarea(network_card, "Wi-Fi SSID", 32, false);
    s_password = settings_textarea(network_card,
                                   "Password (empty for open network)", 64, true);
    /* Wi-Fi passphrase/hex PSK is entered as printable ASCII. */
    lv_textarea_set_accepted_chars(s_password,
        " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
        "abcdefghijklmnopqrstuvwxyz{|}~");
    s_show_password = lv_checkbox_create(network_card);
    lv_checkbox_set_text(s_show_password, "Show password");
    lv_obj_add_style(s_show_password, &st_label, 0);
    lv_obj_add_event_cb(s_show_password, show_password_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_network_status = lv_label_create(network_card);
    lv_obj_add_style(s_network_status, &st_label, 0);
    lv_obj_set_width(s_network_status, lv_pct(100));
    lv_label_set_long_mode(s_network_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_network_status, "Waiting for network manager...");

    s_apply_button = my_button(network_card, "Connect / Save", C_ACCENT, C_ACCENT_T);
    lv_obj_set_width(s_apply_button, lv_pct(100));
    set_disabled(s_apply_button, true);
    lv_obj_add_event_cb(s_apply_button, network_apply_cb, LV_EVENT_CLICKED, NULL);

    s_network_result = lv_label_create(network_card);
    lv_obj_add_style(s_network_result, &st_label, 0);
    lv_obj_set_width(s_network_result, lv_pct(100));
    lv_label_set_long_mode(s_network_result, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_network_result, "Choose interface, then Connect / Save.");

    lv_obj_t *mqtt_card = settings_card(s_settings_body, "MQTT PUBLISHER");
    s_mqtt_enabled = lv_checkbox_create(mqtt_card);
    lv_checkbox_set_text(s_mqtt_enabled, "Enable MQTT publisher");
    lv_obj_add_style(s_mqtt_enabled, &st_label, 0);
    s_mqtt_uri = settings_textarea(mqtt_card, "Broker URI (includes port)", 255, false);
    lv_textarea_set_placeholder_text(s_mqtt_uri, "mqtt://192.168.1.50:1883");
    s_mqtt_topic = settings_textarea(mqtt_card, "Publish topic", 127, false);
    s_mqtt_username = settings_textarea(mqtt_card, "Username (optional)", 127, false);
    s_mqtt_password = settings_textarea(mqtt_card, "Password (optional)", 127, true);
    s_mqtt_client_id = settings_textarea(mqtt_card, "Client ID (empty = automatic)", 63, false);
    s_mqtt_interval = settings_textarea(mqtt_card, "Publish interval (seconds)", 4, false);
    lv_textarea_set_accepted_chars(s_mqtt_interval, "0123456789");
    label = lv_label_create(mqtt_card);
    lv_obj_add_style(label, &st_label, 0);
    lv_label_set_text(label, "QoS");
    s_mqtt_qos = lv_dropdown_create(mqtt_card);
    style_control(s_mqtt_qos);
    lv_obj_set_width(s_mqtt_qos, lv_pct(100));
    lv_dropdown_set_options(s_mqtt_qos, "0 - At most once\n1 - At least once\n2 - Exactly once");
    lv_obj_add_event_cb(s_mqtt_qos, dropdown_style_cb, LV_EVENT_READY, NULL);
    s_mqtt_retain = lv_checkbox_create(mqtt_card);
    lv_checkbox_set_text(s_mqtt_retain, "Retain last telemetry");
    lv_obj_add_style(s_mqtt_retain, &st_label, 0);
    s_mqtt_save = my_button(mqtt_card, "Save MQTT", C_ACCENT, C_ACCENT_T);
    set_disabled(s_mqtt_save, true);
    lv_obj_add_event_cb(s_mqtt_save, mqtt_save_cb, LV_EVENT_CLICKED, NULL);
    s_mqtt_result = lv_label_create(mqtt_card);
    lv_obj_add_style(s_mqtt_result, &st_label, 0);
    lv_obj_set_width(s_mqtt_result, lv_pct(100));
    lv_label_set_long_mode(s_mqtt_result, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_mqtt_result, "Edit settings, then Save MQTT.");
    s_mqtt_status = lv_label_create(mqtt_card);
    lv_obj_add_style(s_mqtt_status, &st_label, 0);
    lv_obj_set_width(s_mqtt_status, lv_pct(100));
    lv_label_set_long_mode(s_mqtt_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_mqtt_status, "MQTT: not initialized");
        lv_obj_t *ep_card = settings_card(s_settings_body, "EPEVER SOLAR");
    s_ep_status = lv_label_create(ep_card);
    lv_obj_add_style(s_ep_status, &st_label, 0);
    lv_obj_set_width(s_ep_status, lv_pct(100));
    lv_label_set_long_mode(s_ep_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_ep_status, "EPever: waiting");

    s_ep_enabled = lv_checkbox_create(ep_card);
    lv_checkbox_set_text(s_ep_enabled, "Publish EPever to MQTT");
    lv_obj_add_style(s_ep_enabled, &st_label, 0);

    s_ep_json = lv_checkbox_create(ep_card);
    lv_checkbox_set_text(s_ep_json, "One JSON topic (off = per field)");
    lv_obj_add_style(s_ep_json, &st_label, 0);

    s_ep_base = settings_textarea(ep_card, "Base topic", EPEVER_BASE_MAX - 1, false);
    lv_textarea_set_placeholder_text(s_ep_base, "kai/epever");
    s_ep_interval = settings_textarea(ep_card, "Publish interval (seconds)", 4, false);
    lv_textarea_set_accepted_chars(s_ep_interval, "0123456789");

    label = lv_label_create(ep_card);
    lv_obj_add_style(label, &st_label, 0);
    lv_label_set_text(label, "Field");
    static char ep_opts[EPEVER_FIELD_COUNT * 24];
    ep_opts[0] = '\0';
    for (int i = 0; i < EPEVER_FIELD_COUNT; i++) {
        strlcat(ep_opts, epever_field_key(i), sizeof(ep_opts));
        if (i < EPEVER_FIELD_COUNT - 1) {
            strlcat(ep_opts, "\n", sizeof(ep_opts));
        }
    }
    s_ep_field_dd = lv_dropdown_create(ep_card);
    lv_dropdown_set_options(s_ep_field_dd, ep_opts);
    lv_obj_set_width(s_ep_field_dd, lv_pct(100));
    style_control(s_ep_field_dd);
    lv_obj_add_event_cb(s_ep_field_dd, ep_field_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_ep_field_dd, dropdown_style_cb, LV_EVENT_READY, NULL);

    s_ep_field_en = lv_checkbox_create(ep_card);
    lv_checkbox_set_text(s_ep_field_en, "Field enabled");
    lv_obj_add_style(s_ep_field_en, &st_label, 0);

    s_ep_field_topic = settings_textarea(ep_card, "Field topic (<base>/<this>)",
                                         EPEVER_SUFFIX_MAX - 1, false);

    s_ep_save = my_button(ep_card, "Save EPever MQTT", C_ACCENT, C_ACCENT_T);
    lv_obj_set_width(s_ep_save, lv_pct(100));
    set_disabled(s_ep_save, true);
    lv_obj_add_event_cb(s_ep_save, ep_save_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ep_def = my_button(ep_card, "Reset to defaults", C_CONTROL, C_TEXT);
    lv_obj_set_width(ep_def, lv_pct(100));
    lv_obj_add_event_cb(ep_def, ep_defaults_cb, LV_EVENT_CLICKED, NULL);

    s_ep_result = lv_label_create(ep_card);
    lv_obj_add_style(s_ep_result, &st_label, 0);
    lv_obj_set_width(s_ep_result, lv_pct(100));
    lv_label_set_long_mode(s_ep_result, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_ep_result, "Pick field, edit topic, Save.");

    lv_obj_t *display_card = settings_card(s_settings_body, "DISPLAY");
    label = lv_label_create(display_card);
    lv_obj_add_style(label, &st_label, 0);
    lv_label_set_text(label, "Screen off after");
    lv_obj_t *timeout = lv_dropdown_create(display_card);
    lv_dropdown_set_options(timeout, "15 s\n30 s\n1 min\n2 min\n5 min\n10 min\nNever");
    lv_obj_set_width(timeout, lv_pct(100));
    style_control(timeout);
    uint32_t current = display_get_timeout();
    for (uint16_t i = 0; i < TIMEOUT_OPT_COUNT; i++) {
        if (timeout_opts_s[i] == current) {
            lv_dropdown_set_selected(timeout, i);
            break;
        }
    }
    lv_obj_add_event_cb(timeout, timeout_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(timeout, dropdown_style_cb, LV_EVENT_READY, NULL);
    
    lv_obj_t *touch_card = settings_card(s_settings_body, "TOUCH");
    lv_obj_t *cal_btn = my_button(touch_card, "Touch calibration test", C_ACCENT, C_ACCENT_T);
    lv_obj_set_width(cal_btn, lv_pct(100));
    lv_obj_add_event_cb(cal_btn, cal_open_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *about_card = settings_card(s_settings_body, "ABOUT");
    label = lv_label_create(about_card);
    lv_obj_add_style(label, &st_label, 0);
    lv_label_set_text(label, "KAI Monitoring v2.3.0");

    s_keyboard = lv_keyboard_create(parent);
    lv_obj_set_size(s_keyboard, LCD_H_RES, CONTENT_H / 2);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    style_control(s_keyboard);
    lv_obj_set_style_bg_color(s_keyboard, c565(C_CONTROL), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_keyboard, c565(C_TEXT), LV_PART_ITEMS);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_keyboard, keyboard_cb, LV_EVENT_ALL, NULL);

    lv_timer_create(network_ui_timer_cb, 500, NULL);
    network_ui_timer_cb(NULL);
    update_ep_ui();
}


void ui_show_splash(void)
{
    if (ui_lock(0)) {
        cal_close();
        settings_keyboard_hide();
        lv_obj_clear_flag(splash_scr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(nav_bar, LV_OBJ_FLAG_HIDDEN);

        ui_unlock();
    }
}

static void sol_update_body(const epever_data_t *d)
{
    const epever_values_t *v = &d->v;
    char b[640];
    snprintf(b, sizeof(b),
        "PV    %.1f V   %.2f A   %.0f W\n"
        "BATT  %.2f V   %.2f A   %.0f W\n"
        "LOAD  %.2f V   %.2f A   %.0f W\n"
        "SOC %.0f %%     Stage: %s\n"
        "Net battery current: %.2f A\n"
        "Temp C  bat %.1f  dev %.1f\n"
        "        comp %.1f  amb %.1f\n"
        "Today  gen %.2f  use %.2f kWh\n"
        "Month  gen %.2f  use %.2f kWh\n"
        "Total  gen %.1f  use %.1f kWh\n"
        "PV today   max %.1f  min %.1f V\n"
        "Batt today max %.2f  min %.2f V\n"
        "CO2 saved %.2f t",
        v->pv_v, v->pv_a, v->pv_w,
        v->bat_v, v->bat_a, v->bat_w,
        v->load_v, v->load_a, v->load_w,
        v->soc, epever_stage_text(v->chg_stage),
        v->bat_net_a,
        v->bat_temp, v->dev_temp,
        v->comp_temp, v->amb_temp,
        v->gen_day, v->con_day,
        v->gen_month, v->con_month,
        v->gen_total, v->con_total,
        v->pv_v_max, v->pv_v_min,
        v->bat_v_max, v->bat_v_min,
        v->co2);
    lv_label_set_text(sol_body_label, b);
}

/* LVGL timer: already inside LVGL lock */
static void sol_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    epever_data_t d;
    if (epever_get_data(&d) != ESP_OK) {
        return;
    }
    lv_label_set_text(sol_status_label,
                      d.online ? "Online" : (d.sequence ? "Offline" : "No data"));
    lv_obj_set_style_text_color(sol_status_label,
                                c565(d.online ? C_SUCCESS : C_DANGER), 0);
    if (d.sequence == 0 || d.sequence == sol_last_seq) {
        return;
    }
    sol_last_seq = d.sequence;
    sol_update_body(&d);
    lv_chart_set_next_value(sol_chart, sol_ser_pv, (int32_t)(d.v.pv_w + 0.5f));
    lv_chart_set_next_value(sol_chart, sol_ser_bat, (int32_t)(d.v.bat_v * 10.0f + 0.5f));
}

// ---------- init ----------

void ui_init(void)
{
    if (ui_lock(0)) {
        init_styles();

        lv_obj_set_style_bg_color(lv_screen_active(), c565(C_BG), 0);

        for (int i = 0; i < PAGE_COUNT; i++) pages[i] = make_page();

        build_dashboard(pages[PAGE_DASH]);
        build_log_screen(pages[PAGE_LOG]);
        build_trend_screen(pages[PAGE_TREND]);
        build_settings_screen(pages[PAGE_SETTINGS]);
        build_nav();
        build_splash();

        lv_timer_create(sol_timer_cb, 500, NULL);

        ui_unlock();
    }
}

// ---------- setters ----------

void ui_push_trend(int temp_c, int hum_pct)
{
    if (ui_lock(0)) {
        lv_chart_set_next_value(trend_chart, ser_temp, temp_c);
        lv_chart_set_next_value(trend_chart, ser_hum,  hum_pct);
        ui_unlock();
    }
}

void ui_set_temp(const char *text)
{
    if (ui_lock(0)) { lv_label_set_text(temp_label, text); ui_unlock(); }
}
void ui_set_hum(const char *text)
{
    if (ui_lock(0)) { lv_label_set_text(hum_label, text); ui_unlock(); }
}
void ui_set_eth_status(const char *text)
{
    (void)text; /* Dashboard refreshed from default route by LVGL timer. */
}
void ui_set_eth_ip(const char *text)
{
    (void)text; /* Dashboard refreshed from default route by LVGL timer. */
}