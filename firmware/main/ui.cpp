/*
 * UI for the Tab5 ControlHome panel, ported from the "Tab5 ControlHome" design
 * (artboards "Ekran główny 1280×720" and "Tryb zegara").
 */
#include <initializer_list>
#include <math.h>
#include <esp_timer.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include <bsp/m5stack_tab5.h>
#include <driver/ledc.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "layout.h"
#include <esp_heap_caps.h>
#include "alarm.h"
#include "uiwatch.h"
#include "lang.h"
#include "scene.h"
// Clock faces from Tab5 Studio (only in builds made by the Studio)
#if __has_include("studio/scene.h")
#include "studio/scene.h"
#endif

// Scene on screen: one sent live from Tab5 Studio (preview), else the one compiled in, else none (layout)
static SceneFace live_faces[2];
static bool live_scene;
static int scene_live_n[2];  // elements of the scene on each face (scene_build)
static const SceneFace* active_faces()
{
    if (live_scene) return live_faces;
#ifdef STUDIO_SCENE
    return studio_faces;
#else
    return NULL;
#endif
}
#include "settings.h"
#include "ui.h"
#include "weather.h"
#include "wifi.h"

extern "C" {
extern const lv_font_t icon_menu52;
LV_FONT_DECLARE(inter_16);
LV_FONT_DECLARE(inter_m16);
LV_FONT_DECLARE(inter_sb24);
LV_FONT_DECLARE(inter_sb28);
LV_FONT_DECLARE(inter_30);
LV_FONT_DECLARE(inter_sb30);
LV_FONT_DECLARE(inter_m64);
LV_FONT_DECLARE(icons_48);
LV_FONT_DECLARE(icons_56);
LV_FONT_DECLARE(icons_40);
LV_IMAGE_DECLARE(img_splash);
LV_FONT_DECLARE(splash_sb88);
}

// Lucide glyphs (lucide-static font)
#define ICON_MENU "\xEE\x84\x99"
#define ICON_TIMER "\xEE\x87\xA0"  // lucide timer (built-in icon font)
#define ICON_BULB "\xEE\x87\x82"
#define ICON_MOON "\xEE\x84\xA2"
#define ICON_SUN "\xEE\x85\xBC"
#define ICON_THERMO "\xEE\x86\x86"
#define ICON_DROP "\xEE\x82\xB8"
#define ICON_GAUGE "\xEE\x86\xBF"

#define HEX(c) lv_color_hex(0x##c)

// Clock dark mode (sun/moon icon or 23:00–5:40): backlight duty below the BSP's 1 % step
// (12-bit LEDC on channel 1, see m5stack_tab5.c) plus a dark overlay over the content
static const uint32_t NIGHT_DUTY       = 12;  // of 4095, ~0.3 %
static const lv_opa_t NIGHT_DIM        = LV_OPA_50;


static lv_obj_t* night_overlay[2];  // [0] digital clock, [1] big digital clock
static lv_obj_t* night_overlay3;     // Wariant-3 (made with its screen)
static lv_obj_t* big_temp[3];       // big digital clock: thermometer icon, value, unit


static lv_obj_t* scr_clock;
static lv_obj_t* scr_big;     // big HH:MM digits and the temperature only
static const uint32_t SETTINGS_IDLE_MS = 120000;  // untouched settings go back to the clock
static lv_obj_t* seg_display;
static lv_obj_t* big_display;

// Plain container: no theme styles, not clickable, not scrollable.
static lv_obj_t* box(lv_obj_t* parent)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void flex(lv_obj_t* o, lv_flex_flow_t flow, int gap)
{
    lv_obj_set_layout(o, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_set_style_pad_row(o, gap, 0);
    lv_obj_set_style_pad_column(o, gap, 0);
}

static lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color, const char* text)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}


/* -------------------------------------------------------------------------- */
/*                                Clock screen                                */
/* -------------------------------------------------------------------------- */

// Digit color from the web page (default turquoise #5BE7E1); dark mode uses it at ~45 %,
// unlit segments are a faint tint of it over the background
static lv_color_t led() { return lv_color_hex(config().digit_color); }
static lv_color_t led_night() { return lv_color_mix(led(), lv_color_black(), 115); }
static lv_color_t led_off() { return lv_color_mix(led(), HEX(0A0A0A), 10); }
// Sensor values: the digit color unless a separate one is chosen on the web page; dimmed the same way at night
static lv_color_t value_color(bool night)
{
    lv_color_t c = config().sensor_follow ? led() : lv_color_hex(config().sensor_color);
    return night ? lv_color_mix(c, lv_color_black(), 115) : c;
}
static bool night_mode                 = false;

static lv_color_t led_on() { return night_mode ? led_night() : led(); }
static const float SKEW          = 0.1405f;      // tan(8°), like CSS skewX(-8deg)
static const int GROUP_W         = 858;
static const int GROUP_H         = 280;
static const int HHMM_W          = 716;  // HH:MM width at scale 1

struct SegCtx {
    lv_layer_t* layer;
    float cx, cy;     // skew origin (group center)
    bool only_lit;    // big clock: unlit segments are not drawn
};

// Filled convex polygon, skewed around the group center. Drawn twice so the
// antialiased seams between fan triangles disappear (colors are opaque).
static void fill_poly(const SegCtx& c, const float* xs, const float* ys, int n, lv_color_t color)
{
    float px[8], py[8], mx = 0, my = 0;
    for (int k = 0; k < n; k++) {
        px[k] = xs[k] - SKEW * (ys[k] - c.cy);
        py[k] = ys[k];
        mx += px[k];
        my += py[k];
    }
    mx /= n;
    my /= n;

    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.bg_color = color;
    d.bg_opa   = LV_OPA_COVER;
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < n; k++) {
            int k2    = (k + 1) % n;
            d.p[0].x  = (int32_t)lroundf(mx);
            d.p[0].y  = (int32_t)lroundf(my);
            d.p[1].x  = (int32_t)lroundf(px[k]);
            d.p[1].y  = (int32_t)lroundf(py[k]);
            d.p[2].x  = (int32_t)lroundf(px[k2]);
            d.p[2].y  = (int32_t)lroundf(py[k2]);
            lv_draw_triangle(c.layer, &d);
        }
    }
}

static void draw_digit(const SegCtx& c, float x0, float y0, float s, int n)
{
    static const char* MAP[10] = {"abcdef", "bc", "abdeg", "abcdg", "bcfg", "acdfg", "acdefg", "abc", "abcdefg", "abcdfg"};
    const float W = 150 * s, H = 280 * s, t = 28 * s, g = 3 * s;
    const float hw = W - t - 2 * g, vh = H / 2 - t / 2 - 2 * g;
    struct Seg {
        char id;
        float x, y, w, h;
        bool horizontal;
    } segs[7] = {
        {'a', t / 2 + g, 0, hw, t, true},          {'g', t / 2 + g, H / 2 - t / 2, hw, t, true},
        {'d', t / 2 + g, H - t, hw, t, true},      {'f', 0, t / 2 + g, t, vh, false},
        {'b', W - t, t / 2 + g, t, vh, false},     {'e', 0, H / 2 + g, t, vh, false},
        {'c', W - t, H / 2 + g, t, vh, false},
    };
    for (const Seg& sg : segs) {
        float x = x0 + sg.x, y = y0 + sg.y, w = sg.w, h = sg.h;
        float xs[6], ys[6];
        if (sg.horizontal) {
            float xv[6] = {x + h / 2, x + w - h / 2, x + w, x + w - h / 2, x + h / 2, x};
            float yv[6] = {y, y, y + h / 2, y + h, y + h, y + h / 2};
            memcpy(xs, xv, sizeof(xs));
            memcpy(ys, yv, sizeof(ys));
        } else {
            float xv[6] = {x + w / 2, x + w, x + w, x + w / 2, x, x};
            float yv[6] = {y, y + w / 2, y + h - w / 2, y + h, y + h - w / 2, y + w / 2};
            memcpy(xs, xv, sizeof(xs));
            memcpy(ys, yv, sizeof(ys));
        }
        bool lit = strchr(MAP[n], sg.id) != NULL;
        if (!lit && c.only_lit) continue;
        fill_poly(c, xs, ys, 6, lit ? led_on() : led_off());
    }
}

static void seg_draw_cb(lv_event_t* e)
{
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);

    // Position and size from the layout (web editor); the display box covers the whole screen
    if (active_faces()) return;  // a Studio scene draws its own clocks
    bool big              = lv_event_get_user_data(e) != NULL;
    const LayoutClock& lc = big ? layout_current().full_clock : layout_current().basic_clock;
    if (!lc.visible || lc.font[0]) return;  // a clock in a TTF font is a label (build_layout)
    bool secs  = lc.seconds;
    float s    = lc.size / 100.0f;
    float gw   = (secs ? GROUP_W : HHMM_W) * s;
    float gx   = a.x1 + lc.x, gy = a.y1 + lc.y;
    SegCtx c;
    c.layer    = lv_event_get_layer(e);
    c.cx       = gx + gw / 2.0f;
    c.cy       = gy + GROUP_H * s / 2.0f;
    c.only_lit = big;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    // Layout from the design: digit 150 px, gap 22, colon 28, seconds at 36 % scale aligned to the bottom
    draw_digit(c, gx + 0, gy, s, tm.tm_hour / 10);
    draw_digit(c, gx + 172 * s, gy, s, tm.tm_hour % 10);
    bool colon_lit   = tm.tm_sec % 2 == 0;
    lv_color_t colon = colon_lit ? led_on() : led_off();
    for (int k = 0; k < 2 && (colon_lit || !big); k++) {
        float x = gx + 344 * s, y = gy + (79 + k * 94) * s, d = 28 * s;
        float xs[4] = {x, x + d, x + d, x};
        float ys[4] = {y, y, y + d, y + d};
        fill_poly(c, xs, ys, 4, colon);
    }
    draw_digit(c, gx + 394 * s, gy, s, tm.tm_min / 10);
    draw_digit(c, gx + 566 * s, gy, s, tm.tm_min % 10);
    if (!secs) return;
    float sy = gy + (GROUP_H - 280 * 0.36f) * s;
    draw_digit(c, gx + 738 * s, sy, 0.36f * s, tm.tm_sec / 10);
    draw_digit(c, gx + 804 * s, sy, 0.36f * s, tm.tm_sec % 10);
}

static lv_obj_t* sensor_values[3];  // temperature, humidity, pressure

// Backlight: only the digital clocks dim in dark mode; on the logo faces dark mode is just a black theme.
// target = the screen being shown.
static bool screen_off();
static bool splash_on;

static void set_duty(uint32_t duty)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static bool alarm_shown();

static void apply_backlight(lv_obj_t* target = NULL)
{
    if (!target) target = lv_screen_active();
    if (alarm_shown()) {
        bsp_display_brightness_set(100);  // a ringing alarm is always bright
    } else if (screen_off()) {
        set_duty(0);
    } else if (splash_on) {
        bsp_display_brightness_set(config().brightness);  // the intro is never dimmed
    } else if (night_mode && (target == scr_clock || target == scr_big)) {
        set_duty(NIGHT_DUTY);
    } else {
        bsp_display_brightness_set(config().brightness);
    }
}

static void menu_theme();

static void color_items(bool night);
static void scene_colors(bool night);
static void scene_build();
static void scene_update_time();
static void scene_sensors(const WeatherData& w);

static void set_night(bool on)
{
    // A real switch to day / night mode brings that mode's color preset (if one is chosen in the schedule)
    static int shown = -1;
    if (shown != (int)on) {
        shown = on;
        if (preset_apply(on ? config().night_preset : config().day_preset)) config_save();
    }
    night_mode = on;
    apply_backlight();
    lv_obj_invalidate(seg_display);
    lv_obj_invalidate(big_display);
    color_items(on);
    // Dark mode on the digital clocks: same layout, dimmed backlight plus a dark overlay
    for (lv_obj_t* o : night_overlay) {
        if (on) {
            lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (night_overlay3) {
        if (on) lv_obj_remove_flag(night_overlay3, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(night_overlay3, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_bg_color(scr_clock, on ? lv_color_black() : HEX(050505), 0);
    lv_obj_set_style_bg_color(scr_big, on ? lv_color_black() : HEX(050505), 0);
    scene_colors(on);
    menu_theme();
}

// True when the current time is in [start, end), which may run past midnight (minutes after midnight)
static bool in_window(int start, int end)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int minutes = tm.tm_hour * 60 + tm.tm_min;
    if (start == end) return false;
    return start < end ? minutes >= start && minutes < end : minutes >= start || minutes < end;
}

// Automatic dark mode (23:00–5:40 unless changed on the web page). Applied only when the window
// starts or ends, so a manual toggle stays until the next boundary.
static int night_last = -1;  // -1 = not checked yet, so the current state is applied

static void check_night_schedule()
{
    bool night = in_window(config().night_start, config().night_end);
    if (night_last != (int)night) {
        night_last = night;
        set_night(night);
        // The night window may bring its own clock face, and the morning the start face back (only over a clock
        // face: an open settings screen or alarm stays)
        int nf = config().night_face;
        if (nf >= 0 && nf < ui_face_count() && ui_face() >= 0) ui_show_face(night ? nf : config().default_face);
    }
}

/* ------------------------------- Screen off ------------------------------- */

// The backlight goes off in the configured window or on request from the web page. A tap on the dark
// screen lights it for a minute; the tap itself is caught so it does not act on the clock.
static const uint32_t WAKE_MS = 60000;
static bool off_wanted;
static bool off_woken;       // lit by a tap while off_wanted
static int off_last = -1;
static lv_obj_t* off_catcher;  // on the top layer, visible while the screen is off

static bool screen_off()
{
    return off_wanted && !off_woken;
}

static void refresh_screen_off()
{
    if (screen_off()) {
        lv_obj_remove_flag(off_catcher, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(off_catcher, LV_OBJ_FLAG_HIDDEN);
    }
    apply_backlight();
}

static void set_screen_on(bool on)
{
    off_wanted = !on;
    off_woken  = false;
    refresh_screen_off();
}

static void on_off_tap(lv_event_t* e)
{
    LV_UNUSED(e);
    lv_indev_t* indev = lv_indev_active();
    if (indev) lv_indev_wait_release(indev);
    off_woken = true;
    refresh_screen_off();
}

static void check_screen_off()
{
    const PanelConfig& c = config();
    bool in              = c.off_enabled && in_window(c.off_start, c.off_end);
    if (off_last != (int)in) {
        off_last = in;
        set_screen_on(!in);
    }
    if (off_woken && !alarm_shown() && lv_display_get_inactive_time(NULL) >= WAKE_MS) {
        off_woken = false;
        refresh_screen_off();
    }
}

static void build_off_catcher()
{
    off_catcher = box(lv_layer_top());
    lv_obj_set_size(off_catcher, 1280, 720);
    lv_obj_add_flag(off_catcher, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(off_catcher, on_off_tap, LV_EVENT_PRESSED, NULL);
    lv_obj_add_flag(off_catcher, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------------------------- Alarm --------------------------------- */
// Full-screen view of a ringing alarm on the top layer (above everything, also above the dark screen)

static lv_obj_t* alarm_view;
static lv_obj_t* timer_view;  // see Timer below

// Alarm and timer screens designed in Tab5 Studio (live from Studio, or compiled in); NULL = built-in look
struct SceneLive;
static lv_obj_t* scene_view(const SceneFace& f, SceneLive** list, int* n);
static void scene_view_refresh(SceneLive* list, int n, const char* alarm_label, int timer_left, bool timer_running);
static SceneFace live_alarm, live_timer;
static bool live_alarm_on, live_timer_on;
static SceneLive *alarm_live, *timer_live;
static int alarm_live_n, timer_live_n;
static char alarm_label_now[24];

static const SceneFace* alarm_scene()
{
    if (live_alarm_on) return &live_alarm;
#ifdef STUDIO_ALARM
    return &studio_alarm_face;
#else
    return NULL;
#endif
}

static const SceneFace* timer_scene()
{
    if (live_timer_on) return &live_timer;
#ifdef STUDIO_TIMER
    return &studio_timer_face;
#else
    return NULL;
#endif
}
static lv_obj_t* alarm_time;  // LED digits (built-in screen)
static void scene_led_cb(lv_event_t* e);
static lv_obj_t* alarm_name;
static lv_obj_t* alarm_snooze_lbl;

static bool alarm_shown()
{
    return alarm_view && !lv_obj_has_flag(alarm_view, LV_OBJ_FLAG_HIDDEN);
}

static void on_alarm_snooze(lv_event_t* e)
{
    LV_UNUSED(e);
    alarm_snooze();
}

static void on_alarm_stop(lv_event_t* e)
{
    LV_UNUSED(e);
    alarm_dismiss();
}

// Alarm and timer screens: the look of the settings and Tab5 Studio (16 px corners; primary = yellow fill,
// secondary = dark fill with a thin border)
#define SCR_BG HEX(0B0D10)
#define BTN_FILL HEX(1A1E23)
#define BTN_LINE HEX(262B31)
#define BTN_TEXT HEX(E9E4DA)
#define BTN_ACCENT HEX(FFC83D)
#define BTN_INK HEX(1A1408)

static lv_obj_t* screen_button(lv_obj_t* parent, const char* text, int w, int h, bool primary, lv_event_cb_t cb, intptr_t user,
                               lv_obj_t** lbl = NULL)
{
    lv_obj_t* b = box(parent);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_bg_color(b, primary ? BTN_ACCENT : BTN_FILL, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    if (primary) {
        lv_obj_set_style_bg_color(b, HEX(D9A92F), LV_STATE_PRESSED);
    } else {
        lv_obj_set_style_border_color(b, BTN_LINE, 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_bg_color(b, HEX(262B31), LV_STATE_PRESSED);
    }
    lv_obj_t* l = label(b, h >= 90 ? &inter_sb30 : &inter_sb24, primary ? BTN_INK : BTN_TEXT, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void*)user);
    if (lbl) *lbl = l;
    return b;
}

static void build_alarm_view()
{
    if (const SceneFace* sc = alarm_scene()) {
        alarm_view = scene_view(*sc, &alarm_live, &alarm_live_n);
        alarm_time = alarm_name = alarm_snooze_lbl = NULL;
        lv_obj_add_flag(alarm_view, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    alarm_view = box(lv_layer_top());
    lv_obj_set_size(alarm_view, 1280, 720);
    lv_obj_set_style_bg_color(alarm_view, SCR_BG, 0);
    lv_obj_set_style_bg_opa(alarm_view, LV_OPA_COVER, 0);
    lv_obj_add_flag(alarm_view, LV_OBJ_FLAG_CLICKABLE);  // nothing below reacts while it rings
    flex(alarm_view, LV_FLEX_FLOW_COLUMN, 18);
    lv_obj_set_flex_align(alarm_view, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // The time in LED digits 270 px high, drawn like an LED clock element of Studio (faint unlit segments)
    static const SceneEl led = [] {
        SceneEl e = {};
        e.type = SC_CLOCK_LED, e.opa = 255, e.flags = SC_UNLIT;
        e.w = HHMM_W * 270 / 280, e.h = e.size = 270;
        e.color = SC_COLOR | 0xFFC83D;
        return e;
    }();
    int pad    = SKEW * led.size / 2 + 4;
    alarm_time = box(alarm_view);
    lv_obj_set_size(alarm_time, led.w + 2 * pad, led.h);
    lv_obj_add_event_cb(alarm_time, scene_led_cb, LV_EVENT_DRAW_MAIN, (void*)&led);
    alarm_name = label(alarm_view, &inter_30, BTN_TEXT, "");
    lv_obj_set_style_margin_bottom(alarm_name, 48, 0);

    lv_obj_t* row = box(alarm_view);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    flex(row, LV_FLEX_FLOW_ROW, 24);
    screen_button(row, "", 400, 110, true, on_alarm_snooze, 0, &alarm_snooze_lbl);
    screen_button(row, TR("Wyłącz", "Stop"), 400, 110, false, on_alarm_stop, 0);

    lv_obj_add_flag(alarm_view, LV_OBJ_FLAG_HIDDEN);
}

static void update_alarm_time()
{
    if (!alarm_shown()) return;
    if (!alarm_time) {  // designed screen: its clocks and the alarm name
        scene_view_refresh(alarm_live, alarm_live_n, alarm_label_now, -1, false);
        return;
    }
    static int shown = -1;  // minute of the day on screen
    time_t now       = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int m = tm.tm_hour * 60 + tm.tm_min;
    if (m != shown || lv_obj_has_flag(alarm_time, LV_OBJ_FLAG_HIDDEN)) {
        shown = m;
        lv_obj_invalidate(alarm_time);  // scene_led_cb draws the time now
    }
}

void ui_alarm_show(const char* label_text, int snooze_min)
{
    uiwatch_crumb("alarm: build view");
    if (!alarm_view) build_alarm_view();
    uiwatch_crumb("alarm: fill view");
    strlcpy(alarm_label_now, label_text && label_text[0] ? label_text : TR("Budzik", "Alarm"), sizeof(alarm_label_now));
    if (alarm_name) lv_label_set_text(alarm_name, alarm_label_now);
    if (alarm_snooze_lbl) {
        if (snooze_min > 0) lv_label_set_text_fmt(alarm_snooze_lbl, TR("Drzemka %d min", "Snooze %d min"), snooze_min);
        else lv_label_set_text(alarm_snooze_lbl, TR("Drzemka", "Snooze"));
    }
    lv_obj_remove_flag(alarm_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(alarm_view);
    uiwatch_crumb("alarm: time on view");
    update_alarm_time();
    off_woken = true;  // light the screen if it was dark
    refresh_screen_off();
}

lv_obj_t* ui_alarm_view(void)
{
    if (alarm_shown()) return alarm_view;
    if (timer_view && !lv_obj_has_flag(timer_view, LV_OBJ_FLAG_HIDDEN)) return timer_view;
    return NULL;
}


void ui_alarm_hide(void)
{
    if (!alarm_view) return;
    lv_obj_add_flag(alarm_view, LV_OBJ_FLAG_HIDDEN);
    apply_backlight();
}

/* ---------------------------------- Timer --------------------------------- */
// Hold on a clock face: set a time, start; at zero the alarm view rings ("Minutnik"). The timer keeps running when
// its view is closed; a small pill in the corner shows the time left and opens it again.

static lv_obj_t* timer_time;    // row with minutes : seconds
static lv_obj_t* timer_min;
static lv_obj_t* timer_sec;
static int timer_field;           // what − / + change: 0 minutes, 1 seconds
static lv_obj_t* timer_go_lbl;   // Start / Pauza
static lv_obj_t* timer_pill;
static lv_obj_t* timer_pill_lbl;
static bool timer_running;
static int64_t timer_end_us;
static int timer_left, timer_total;  // seconds
static bool timer_paused;          // stopped with time left: opening the timer keeps it
static const int TIMER_MAX = 99 * 60 + 59;

static int timer_remaining()
{
    if (!timer_running) return timer_left;
    int64_t left = timer_end_us - esp_timer_get_time();
    return left <= 0 ? 0 : (int)((left + 999999) / 1000000);
}

static bool face_has_timer_badge(lv_obj_t* scr);

static void timer_refresh()
{
    if (!timer_view) return;
    int r = timer_remaining();
    if (!timer_min) {  // designed screen
        scene_view_refresh(timer_live, timer_live_n, NULL, r, timer_running);
        bool pill = timer_running && lv_obj_has_flag(timer_view, LV_OBJ_FLAG_HIDDEN) && !face_has_timer_badge(lv_screen_active());
        if (pill) {
            lv_label_set_text_fmt(timer_pill_lbl, "%02d:%02d", r / 60, r % 60);
            lv_obj_remove_flag(timer_pill, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(timer_pill, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    char t[16], part[16];
    snprintf(t, sizeof(t), "%02d:%02d", r / 60, r % 60);
    snprintf(part, sizeof(part), "%02d", r / 60);
    if (strcmp(lv_label_get_text(timer_min), part) != 0) lv_label_set_text(timer_min, part);
    snprintf(part, sizeof(part), "%02d", r % 60);
    if (strcmp(lv_label_get_text(timer_sec), part) != 0) lv_label_set_text(timer_sec, part);
    // the part − / + change is yellow, the other white (both yellow while counting)
    lv_obj_set_style_text_color(timer_min, timer_running || timer_field == 0 ? HEX(FFC83D) : HEX(E9E4DA), 0);
    lv_obj_set_style_text_color(timer_sec, timer_running || timer_field == 1 ? HEX(FFC83D) : HEX(E9E4DA), 0);
    lv_label_set_text(timer_go_lbl, timer_running ? TR("Pauza", "Pause") : TR("Start", "Start"));
    bool pill = timer_running && lv_obj_has_flag(timer_view, LV_OBJ_FLAG_HIDDEN) && !face_has_timer_badge(lv_screen_active());
    if (pill) {
        lv_label_set_text(timer_pill_lbl, t);
        lv_obj_remove_flag(timer_pill, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(timer_pill, LV_OBJ_FLAG_HIDDEN);
    }
}

static void timer_set(int seconds)
{
    seconds = LV_CLAMP(0, seconds, TIMER_MAX);
    if (timer_running) {
        timer_end_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    } else {
        timer_left = seconds;
    }
    if (seconds > timer_total || !timer_running) timer_total = seconds;
    timer_refresh();
}

static void on_timer_preset(lv_event_t* e)
{
    timer_running = false;
    timer_paused  = false;
    timer_left = timer_total = (int)(intptr_t)lv_event_get_user_data(e);
    timer_refresh();
}

// − / + : user data = direction (−1 / +1); minutes by 1, seconds by 5; holding repeats
static void on_timer_adjust(lv_event_t* e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    timer_set(timer_remaining() + dir * (timer_field ? 5 : 60));
}

static void on_timer_field(lv_event_t* e)
{
    timer_field = (int)(intptr_t)lv_event_get_user_data(e);
    timer_refresh();
}

static void on_timer_go(lv_event_t* e)
{
    LV_UNUSED(e);
    if (timer_running) {
        timer_left    = timer_remaining();
        timer_running = false;
        timer_paused  = true;
    } else if (timer_left > 0) {
        timer_end_us  = esp_timer_get_time() + (int64_t)timer_left * 1000000;
        timer_running = true;
        timer_paused  = false;
    }
    timer_refresh();
}

static void on_timer_reset(lv_event_t* e)
{
    LV_UNUSED(e);
    timer_running = false;
    timer_paused  = false;
    timer_left    = timer_total;
    timer_refresh();
}

static void on_timer_close(lv_event_t* e)
{
    LV_UNUSED(e);
    lv_obj_add_flag(timer_view, LV_OBJ_FLAG_HIDDEN);
    timer_refresh();
}

static void timer_open();
static void on_timer_pill(lv_event_t* e)
{
    LV_UNUSED(e);
    timer_open();
}

static void build_timer_pill();
static void build_timer_view()
{
    build_timer_pill();
    if (const SceneFace* sc = timer_scene()) {
        timer_view = scene_view(*sc, &timer_live, &timer_live_n);
        timer_time = timer_min = timer_sec = timer_go_lbl = NULL;
        lv_obj_add_flag(timer_view, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    timer_view = box(lv_layer_top());
    lv_obj_set_size(timer_view, 1280, 720);
    lv_obj_set_style_bg_color(timer_view, SCR_BG, 0);
    lv_obj_set_style_bg_opa(timer_view, LV_OPA_COVER, 0);
    lv_obj_add_flag(timer_view, LV_OBJ_FLAG_CLICKABLE);

    // Header: "‹ Powrót" as a whole button on the left, the timer icon and the title on the right
    lv_obj_t* back = screen_button(timer_view, "", 210, 72, false, on_timer_close, 0);
    lv_obj_set_pos(back, 24, 24);
    lv_obj_clean(back);
    flex(back, LV_FLEX_FLOW_ROW, 6);
    lv_obj_set_flex_align(back, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_right(back, 10, 0);
    label(back, &icons_40, BTN_TEXT, "\xEE\x81\xB2");  // chevron left
    label(back, &inter_sb30, BTN_TEXT, TR("Powrót", "Back"));
    lv_obj_t* title = label(timer_view, &inter_sb30, BTN_TEXT, TR("Minutnik", "Timer"));
    lv_obj_align(title, LV_ALIGN_TOP_RIGHT, -40, 42);
    lv_obj_update_layout(title);
    lv_obj_t* ticon = label(timer_view, layout_font_fixed("icons", 40), BTN_ACCENT, ICON_TIMER);
    lv_obj_align_to(ticon, title, LV_ALIGN_OUT_LEFT_MID, -14, 0);

    size_t ttf_size;
    const uint8_t* ttf   = layout_font_data("rajdhani", &ttf_size);  // Rajdhani Bold (built in)
    const lv_font_t* big = ttf ? layout_font_fixed("rajdhani", 250) : &inter_m64;  // made once: the view is rebuilt with each new design

    // − 05:00 +
    lv_obj_t* row = box(timer_view);
    lv_obj_set_size(row, 1280, 260);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 120);
    flex(row, LV_FLEX_FLOW_ROW, 56);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int k = 0; k < 2; k++) {
        lv_obj_t* b = screen_button(row, "", 120, 120, false, on_timer_adjust, k ? 1 : -1);
        lv_obj_add_event_cb(b, on_timer_adjust, LV_EVENT_LONG_PRESSED_REPEAT, (void*)(intptr_t)(k ? 1 : -1));
        for (int bar = 0; bar < (k ? 2 : 1); bar++) {  // − and + drawn as bars
            lv_obj_t* l = box(b);
            lv_obj_set_size(l, bar ? 6 : 40, bar ? 40 : 6);
            lv_obj_set_style_radius(l, 3, 0);
            lv_obj_set_style_bg_color(l, BTN_TEXT, 0);
            lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
            lv_obj_center(l);
        }
        if (!k) {
            // 05 : 00 — tap the minutes or the seconds to choose what − / + change
            timer_time = box(row);
            lv_obj_set_size(timer_time, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            flex(timer_time, LV_FLEX_FLOW_ROW, 0);
            lv_obj_set_flex_align(timer_time, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            timer_min = label(timer_time, big, HEX(FFC83D), "05");
            label(timer_time, big, HEX(FFC83D), ":");
            timer_sec = label(timer_time, big, HEX(E9E4DA), "00");
            for (int f = 0; f < 2; f++) {
                lv_obj_t* part = f ? timer_sec : timer_min;
                lv_obj_add_flag(part, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_ext_click_area(part, 20);
                lv_obj_add_event_cb(part, on_timer_field, LV_EVENT_CLICKED, (void*)(intptr_t)f);
            }
        }
    }

    // 5 · 10 · 15 min in a strip, like the segmented controls of the settings
    lv_obj_t* presets = box(timer_view);
    lv_obj_set_size(presets, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(presets, LV_ALIGN_TOP_MID, 0, 420);
    lv_obj_set_style_bg_color(presets, HEX(0E0F12), 0);
    lv_obj_set_style_bg_opa(presets, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(presets, 18, 0);
    lv_obj_set_style_pad_all(presets, 5, 0);
    flex(presets, LV_FLEX_FLOW_ROW, 5);
    static const int mins[] = {5, 10, 15};
    for (int m : mins) {
        char t[12];
        snprintf(t, sizeof(t), "%d min", m);
        lv_obj_t* b = screen_button(presets, t, 160, 72, false, on_timer_preset, m * 60);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, HEX(22262B), 0);
    }

    lv_obj_t* actions = box(timer_view);
    lv_obj_set_size(actions, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(actions, LV_ALIGN_TOP_MID, 0, 540);
    flex(actions, LV_FLEX_FLOW_ROW, 24);
    screen_button(actions, TR("Start", "Start"), 340, 100, true, on_timer_go, 0, &timer_go_lbl);
    screen_button(actions, TR("Reset", "Reset"), 220, 100, false, on_timer_reset, 0);
    lv_obj_add_flag(timer_view, LV_OBJ_FLAG_HIDDEN);

}

// Pill in the top-left corner of the clock while the timer runs in the background
static void build_timer_pill()
{
    if (timer_pill) return;
    timer_pill = box(lv_layer_top());
    lv_obj_set_size(timer_pill, LV_SIZE_CONTENT, 52);
    lv_obj_set_pos(timer_pill, 24, 20);
    lv_obj_set_style_pad_hor(timer_pill, 20, 0);
    lv_obj_set_style_radius(timer_pill, 16, 0);
    lv_obj_set_style_bg_color(timer_pill, HEX(1A1E23), 0);
    lv_obj_set_style_bg_opa(timer_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(timer_pill, HEX(FFC83D), 0);
    lv_obj_set_style_border_width(timer_pill, 2, 0);
    lv_obj_add_flag(timer_pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(timer_pill, on_timer_pill, LV_EVENT_CLICKED, NULL);
    flex(timer_pill, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_flex_align(timer_pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // Stopwatch icon: ring, crown and hand
    label(timer_pill, layout_font_fixed("icons", 28), HEX(FFC83D), ICON_TIMER);
    timer_pill_lbl = label(timer_pill, &inter_sb24, HEX(FFC83D), "");
    lv_obj_add_flag(timer_pill, LV_OBJ_FLAG_HIDDEN);
}

static void timer_open()
{
    if (!timer_view) build_timer_view();
    if (!timer_running && !timer_paused) timer_left = 0;  // a new timer starts from 00:00
    lv_obj_remove_flag(timer_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(timer_view);
    if (alarm_shown()) lv_obj_move_foreground(alarm_view);  // a ringing alarm stays on top
    timer_refresh();
}

// Twice a second (tick_cb): count down, ring at zero
static void timer_tick()
{
    if (!timer_view) return;
    if (timer_running && timer_remaining() == 0) {
        timer_running = false;
        timer_paused  = false;
        timer_left    = timer_total;
        lv_obj_add_flag(timer_view, LV_OBJ_FLAG_HIDDEN);
        alarm_ring_timer();
    }
    timer_refresh();
}

// Two taps within 400 ms on any clock face switch dark / light mode
static void on_clock_tap(lv_event_t* e)
{
    LV_UNUSED(e);
    static uint32_t last_tap = 0;
    if (last_tap != 0 && lv_tick_elaps(last_tap) < 400) {
        last_tap = 0;
        set_night(!night_mode);
    } else {
        last_tap = lv_tick_get();
    }
}

/* --------------------------------- Layout --------------------------------- */

static lv_obj_t* layout_layer[2];  // [0] Basic, [1] Full: holds the elements placed by the layout
static lv_obj_t* date_label[2];
static uint32_t sensors_shown;     // weather version on screen (0 = redraw)

// UTF-8 for an icon codepoint
static const char* icon_text(uint16_t cp)
{
    static char buf[4][4];
    static int n;
    char* b = buf[n++ % 4];
    if (!cp) return "";
    b[0] = 0xE0 | (cp >> 12), b[1] = 0x80 | ((cp >> 6) & 0x3F), b[2] = 0x80 | (cp & 0x3F), b[3] = 0;
    return b;
}

// One placed element (sensor or temperature) with its labels, for recoloring
struct Item {
    lv_obj_t *icon, *value, *unit;
    const LayoutText* t;
    lv_color_t icon_day;  // automatic icon color in light mode
    bool full;            // on the Full face: automatic colors follow the digits
};
static Item items[4];     // [0..2] Basic sensors, [3] Full temperature
static const LayoutText* date_t[2];
static lv_obj_t* clock_label[2][2];  // [face][HH:MM, seconds] when the clock uses a TTF font

static lv_color_t dim(lv_color_t c, bool night) { return night ? lv_color_mix(c, lv_color_black(), 115) : c; }
static bool custom(uint32_t c) { return c & LAYOUT_COLOR; }
static lv_color_t custom_color(uint32_t c) { return lv_color_hex(c & 0xFFFFFF); }

// Colors of every placed element for light / dark mode: chosen colors, or the automatic ones
static void color_items(bool night)
{
    for (const Item& it : items) {
        if (!it.value) continue;
        const LayoutText& t = *it.t;
        lv_color_t ic = custom(t.icon_color) ? dim(custom_color(t.icon_color), night)
                        : it.full            ? led_on()
                        : night              ? led_night()
                                             : it.icon_day;
        lv_color_t vc = custom(t.value_color) ? dim(custom_color(t.value_color), night) : value_color(night);
        lv_color_t uc = custom(t.unit_color) ? dim(custom_color(t.unit_color), night)
                        : it.full            ? led_on()
                        : night              ? led_night()
                                             : HEX(8A929C);
        lv_obj_set_style_text_color(it.icon, ic, 0);
        lv_obj_set_style_text_color(it.value, vc, 0);
        lv_obj_set_style_text_color(it.unit, uc, 0);
    }
    for (int f = 0; f < 2; f++) {
        if (date_label[f] && date_t[f])
            lv_obj_set_style_text_color(date_label[f], custom(date_t[f]->value_color) ? dim(custom_color(date_t[f]->value_color), night)
                                                       : night ? led_night() : HEX(8A929C), 0);
        for (lv_obj_t* l : clock_label[f])
            if (l) lv_obj_set_style_text_color(l, led_on(), 0);
    }
}

// Icon, value and unit at the layout position; returns the value label. Structure: row → icon, box → value, unit.
static lv_obj_t* layout_item(lv_obj_t* parent, const LayoutText& t, Item& it, lv_color_t icon_day, bool full)
{
    lv_obj_t* row = box(parent);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_pos(row, t.x, t.y);
    flex(row, LV_FLEX_FLOW_ROW, t.size * 0.3f);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    int isz        = t.icon_size ? t.icon_size : (int)(t.size * 0.875f);
    int usz        = t.unit_size ? t.unit_size : (int)(t.size * 0.47f);
    lv_obj_t* icon = label(row, layout_icon_font(isz), icon_day, icon_text(t.icon));
    if (!t.icon) lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* value = box(row);
    lv_obj_set_size(value, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    flex(value, LV_FLEX_FLOW_ROW, t.size * 0.16f);
    lv_obj_set_flex_align(value, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_t* v = label(value, layout_font(t.font, t.size), HEX(F2F3F5), "—");
    lv_obj_t* u = label(value, layout_font(t.font, usz), HEX(8A929C), t.unit);
    lv_obj_set_style_pad_bottom(u, t.size * 0.14f, 0);
    if (!t.unit[0]) lv_obj_add_flag(u, LV_OBJ_FLAG_HIDDEN);
    if (!t.visible) lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    it = {icon, v, u, &t, icon_day, full};
    return v;
}

static lv_obj_t* layout_date(lv_obj_t* parent, const LayoutText& t)
{
    lv_obj_t* l = label(parent, layout_font(t.font, t.size), HEX(8A929C), "");
    lv_obj_set_pos(l, t.x, t.y);
    if (!t.visible) lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

// Clock in a TTF font: HH:MM label (font size = 2.8 × size %, the height of the LED digits) and smaller seconds
static void layout_clock_text(lv_obj_t* parent, const LayoutClock& c, lv_obj_t** out)
{
    out[0] = out[1] = NULL;
    if (!c.font[0] || !c.visible) return;
    int px         = c.size * 2.8f;
    lv_obj_t* row  = box(parent);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_pos(row, c.x, c.y);
    flex(row, LV_FLEX_FLOW_ROW, px * 0.06f);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    out[0] = label(row, layout_font(c.font, px), led(), "");
    if (c.seconds) {
        out[1] = label(row, layout_font(c.font, px * 0.36f), led(), "");
        lv_obj_set_style_pad_bottom(out[1], px * 0.12f, 0);
    }
}

static void update_clock_labels()
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char hm[8], ss[4];
    snprintf(hm, sizeof(hm), "%02d:%02d", tm.tm_hour, tm.tm_min);
    snprintf(ss, sizeof(ss), "%02d", tm.tm_sec);
    for (auto& f : clock_label) {
        if (f[0] && strcmp(lv_label_get_text(f[0]), hm) != 0) lv_label_set_text(f[0], hm);
        if (f[1] && strcmp(lv_label_get_text(f[1]), ss) != 0) lv_label_set_text(f[1], ss);
    }
}

// Day and month names in the language of the program ("Piątek, 31 października" / "Friday, 31 October")
static const char* day_name(int wday)
{
    static const char* const pl[] = {"Niedziela", "Poniedziałek", "Wtorek", "Środa", "Czwartek", "Piątek", "Sobota"};
    static const char* const en[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    return (g_lang_en ? en : pl)[wday];
}

// Short day names for the forecast ("Pn", "Wt" / "Mon", "Tue")
static const char* day_short(int wday)
{
    static const char* const pl[] = {"Nd", "Pn", "Wt", "Śr", "Cz", "Pt", "So"};
    static const char* const en[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    return (g_lang_en ? en : pl)[wday];
}

// Capital letters in place: ASCII and the Polish letters (two UTF-8 bytes each: lower → upper)
static void upper_pl(char* buf)
{
    static const unsigned char pl[][4] = {{0xC4, 0x85, 0xC4, 0x84}, {0xC4, 0x87, 0xC4, 0x86}, {0xC4, 0x99, 0xC4, 0x98},
                                          {0xC5, 0x82, 0xC5, 0x81}, {0xC5, 0x84, 0xC5, 0x83}, {0xC3, 0xB3, 0xC3, 0x93},
                                          {0xC5, 0x9B, 0xC5, 0x9A}, {0xC5, 0xBA, 0xC5, 0xB9}, {0xC5, 0xBC, 0xC5, 0xBB}};
    for (unsigned char* p = (unsigned char*)buf; *p; p++) {
        if (*p >= 'a' && *p <= 'z') {
            *p -= 32;
        } else if (*p >= 0xC0 && p[1]) {
            for (const auto& m : pl)
                if (p[0] == m[0] && p[1] == m[1]) p[0] = m[2], p[1] = m[3];
            p++;
        }
    }
}

static const char* month_name(int mon)
{
    static const char* const pl[] = {"stycznia", "lutego", "marca", "kwietnia", "maja", "czerwca", "lipca", "sierpnia", "września",
                                     "października", "listopada", "grudnia"};
    static const char* const en[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September",
                                     "October", "November", "December"};
    return (g_lang_en ? en : pl)[mon];
}

// "Piątek, 31 października" on both faces (when shown)
static void update_dates()
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[48];
    snprintf(buf, sizeof(buf), "%s, %d %s", day_name(tm.tm_wday), tm.tm_mday, month_name(tm.tm_mon));
    for (lv_obj_t* l : date_label)
        if (l && strcmp(lv_label_get_text(l), buf) != 0) lv_label_set_text(l, buf);
    update_clock_labels();
    scene_update_time();
}

static void scene_build();
static void scene_update_time();

// (Re)creates the clock text, sensors, temperature and date of both faces from layout_current()
static void build_layout()
{
    if (active_faces()) {
        scene_build();
        return;
    }
    scene_live_n[0] = scene_live_n[1] = 0;
    const Layout& L = layout_current();
    layout_fonts_begin();
    for (lv_obj_t* l : layout_layer) lv_obj_clean(l);
    static const lv_color_t icon_day[] = {HEX(FF9E6B), HEX(7CC4FF), HEX(B79CFF)};
    layout_clock_text(layout_layer[0], L.basic_clock, clock_label[0]);
    for (int k = 0; k < 3; k++) sensor_values[k] = layout_item(layout_layer[0], L.basic_sensor[k], items[k], icon_day[k], false);
    date_label[0] = layout_date(layout_layer[0], L.basic_date);
    date_t[0]     = &L.basic_date;
    layout_clock_text(layout_layer[1], L.full_clock, clock_label[1]);
    big_temp[1]   = layout_item(layout_layer[1], L.full_temp, items[3], led(), true);
    big_temp[0]   = items[3].icon;
    big_temp[2]   = items[3].unit;
    date_label[1] = layout_date(layout_layer[1], L.full_date);
    date_t[1]     = &L.full_date;
    layout_fonts_end();
    sensors_shown = 0;
    update_dates();
    lv_obj_invalidate(seg_display);
    lv_obj_invalidate(big_display);
}

/* ---------------------------------- Scene --------------------------------- */
// Faces from Tab5 Studio (scene.h). Without a scene these functions do nothing.

struct SceneLive {
    const SceneEl* el;
    lv_obj_t* obj;   // label (text-like elements), draw box (LED clock) or shape
    lv_obj_t* unit;  // sensor unit label
    lv_point_precise_t pts[2];  // line end points (lv_line keeps a pointer to them)
};
static SceneLive (*scene_live)[64];  // [2][64] in PSRAM, made by the first scene_build (internal RAM is short at boot)
// Wariant-3 (live scene only): its screen is made on first use, its elements exist only while it is shown
static SceneFace live_third;
static bool live_third_on;
static lv_obj_t* scr_third;
static lv_obj_t* third_layer;
static SceneLive* third_live;  // [64] in PSRAM
static int third_live_n;
static bool third_built;
static void third_make();
static void scene_paint(const SceneLive& L);
static void scene_time(const SceneLive& L, const struct tm& tm);
static bool scene_make(lv_obj_t* parent, const SceneEl& el, SceneLive& L);
static bool scene_night;

static lv_color_t scene_color(uint32_t c, lv_color_t automatic)
{
    lv_color_t col = c & SC_COLOR ? lv_color_hex(c & 0xFFFFFF) : automatic;
    return scene_night ? lv_color_mix(col, lv_color_black(), 115) : col;
}

// A full screen on the top layer from a designed scene (alarm, timer); its elements go to *list
static lv_obj_t* scene_view(const SceneFace& f, SceneLive** list, int* n)
{
    lv_obj_t* v = box(lv_layer_top());
    lv_obj_set_size(v, 1280, 720);
    lv_obj_set_style_bg_color(v, lv_color_hex(f.bg), 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
    lv_obj_add_flag(v, LV_OBJ_FLAG_CLICKABLE);  // nothing below reacts
    if (!*list) *list = (SceneLive*)heap_caps_calloc(64, sizeof(SceneLive), MALLOC_CAP_SPIRAM);
    *n = 0;
    for (int i = 0; *list && i < f.count && *n < 64; i++) {
        SceneLive& L = (*list)[*n];
        if (!scene_make(v, f.els[i], L)) continue;
        scene_paint(L);
        (*n)++;
    }
    return v;
}

// Clocks, the alarm name, the time left and Start / Pauza on a designed screen
static void scene_view_refresh(SceneLive* list, int n, const char* alarm_label, int timer_left, bool timer_running)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    for (int i = 0; list && i < n; i++) {
        const SceneLive& L = list[i];
        char buf[24];
        switch (L.el->type) {
            case SC_ALARM_LABEL:
                if (alarm_label && strcmp(lv_label_get_text(L.obj), alarm_label)) lv_label_set_text(L.obj, alarm_label);
                break;
            case SC_TIMER_TIME:
                if (timer_left < 0) break;
                snprintf(buf, sizeof(buf), "%02d:%02d", timer_left / 60, timer_left % 60);
                if (strcmp(lv_label_get_text(L.obj), buf)) lv_label_set_text(L.obj, buf);
                break;
            case SC_TIMER_GO:
                lv_label_set_text(L.obj, timer_running ? TR("Pauza", "Pause") : TR("Start", "Start"));
                break;
            default: scene_time(L, tm); break;
        }
    }
}

// Analog dial: marks every hour (and minute), hands from the center, second hand in color2.
// The still part (face, rim, marks) is drawn once into a picture (DialCache) and only copied after that; each
// second only the areas the moving hands cross are redrawn (scene_time). Drawing the whole dial every second
// took so long that taps and swipes were missed.
struct DialColors {
    lv_color_t hands, marks, face, sec;
};

static DialColors dial_colors(const SceneEl* el)
{
    DialColors c;
    c.hands = scene_color(el->color, led());
    c.marks = scene_color(el->color3, c.hands);
    c.face  = scene_color(el->color4, lv_color_black());
    c.sec   = scene_color(el->color2, HEX(FF4B4B));
    return c;
}

// One line from the center at angle a0 (radians, 12 o'clock = 0), from radius r0 to r1
static void dial_line(lv_layer_t* layer, float cx, float cy, float a0, float r0, float r1, float w, lv_color_t c, bool round)
{
    float s = sinf(a0), k = cosf(a0);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = c, d.width = (int32_t)w, d.opa = LV_OPA_COVER, d.round_start = d.round_end = round;
    d.p1 = {(lv_value_precise_t)lroundf(cx + s * r0), (lv_value_precise_t)lroundf(cy - k * r0)};
    d.p2 = {(lv_value_precise_t)lroundf(cx + s * r1), (lv_value_precise_t)lroundf(cy - k * r1)};
    lv_draw_line(layer, &d);
}

// Face, rim and marks inside the square a (the object's opacity style fades the whole drawing)
static void dial_still(lv_layer_t* layer, const SceneEl* el, const lv_area_t& a, const DialColors& c)
{
    float r = lv_area_get_width(&a) / 2.0f, cx = a.x1 + r, cy = a.y1 + r;
    if (el->color4 || el->size2) {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.radius       = LV_RADIUS_CIRCLE;
        d.bg_opa       = el->color4 ? LV_OPA_COVER : LV_OPA_TRANSP;
        d.bg_color     = c.face;
        d.border_width = el->size2;
        d.border_color = c.marks;
        d.border_opa   = LV_OPA_COVER;
        lv_draw_rect(layer, &d, &a);
    }
    float rim = el->size2 + r * 0.04f;
    if (el->source)
        for (int i = 0; i < 60; i++) {
            bool hour = i % 5 == 0;
            if (!hour && el->source < 2) continue;
            float len = hour ? r * 0.12f : r * 0.05f;
            dial_line(layer, cx, cy, i * (float)M_PI / 30, r - rim - len, r - rim, hour ? LV_MAX(2.0f, r / 36) : LV_MAX(1.0f, r / 110), c.marks, false);
        }
}

struct DialHands {
    float hour, min, sec;  // angles in radians
};

static DialHands dial_hands_now()
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return {(tm.tm_hour % 12 + tm.tm_min / 60.0f) * (float)M_PI / 6, (tm.tm_min + tm.tm_sec / 60.0f) * (float)M_PI / 30,
            tm.tm_sec * (float)M_PI / 30};
}

// Hand sizes: {from, to, width} as parts of the radius (hour, minute, second hand)
static void dial_hand_geom(int which, float r, float* r0, float* r1, float* w)
{
    if (which == 0) *r0 = -r * 0.08f, *r1 = r * 0.5f, *w = LV_MAX(3.0f, r / 16);
    else if (which == 1) *r0 = -r * 0.08f, *r1 = r * 0.78f, *w = LV_MAX(2.0f, r / 26);
    else *r0 = -r * 0.16f, *r1 = r * 0.86f, *w = LV_MAX(1.0f, r / 90);
}

static void dial_hands(lv_layer_t* layer, const SceneEl* el, const lv_area_t& a, const DialColors& c, const DialHands& h)
{
    float r = lv_area_get_width(&a) / 2.0f, cx = a.x1 + r, cy = a.y1 + r, r0, r1, w;
    dial_hand_geom(0, r, &r0, &r1, &w);
    dial_line(layer, cx, cy, h.hour, r0, r1, w, c.hands, true);
    dial_hand_geom(1, r, &r0, &r1, &w);
    dial_line(layer, cx, cy, h.min, r0, r1, w, c.hands, true);
    bool secs = el->flags & SC_SECONDS;
    if (secs) {
        dial_hand_geom(2, r, &r0, &r1, &w);
        dial_line(layer, cx, cy, h.sec, r0, r1, w, c.sec, true);
    }
    // Center cap
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius    = LV_RADIUS_CIRCLE;
    d.bg_color  = secs ? c.sec : c.hands;
    d.bg_opa    = LV_OPA_COVER;
    int cap     = LV_MAX(3, (int)(r / 22));
    lv_area_t q = {(int)cx - cap, (int)cy - cap, (int)cx + cap, (int)cy + cap};
    lv_draw_rect(layer, &d, &q);
}

struct DialCache {
    lv_obj_t* obj;
    const SceneEl* el;
    lv_draw_buf_t buf;
    uint8_t* data;   // ARGB8888, w * w * 4 bytes in PSRAM
    int32_t w;
    uint32_t key;    // colors and sizes the picture was drawn with (0 = none yet)
    bool building;
    bool has_hands;  // hands holds what is on screen
    DialHands hands;
};
static const int MAX_DIALS = 6;
static DialCache* s_dials;       // [MAX_DIALS] in PSRAM
static lv_obj_t* s_dial_canvas;  // hidden canvas the still parts are drawn with

static uint32_t dial_key(const SceneEl* el, const DialColors& c, int32_t w)
{
    uint32_t k = 2166136261u;
    for (uint32_t v : {lv_color_to_u32(c.marks), lv_color_to_u32(c.face), (uint32_t)el->size2, (uint32_t)el->source, (uint32_t)el->color4, (uint32_t)w})
        k = (k ^ v) * 16777619u;
    return k | 1;
}

static DialCache* dial_find(lv_obj_t* obj, bool add)
{
    if (!s_dials) s_dials = (DialCache*)heap_caps_calloc(MAX_DIALS, sizeof(DialCache), MALLOC_CAP_SPIRAM);
    if (!s_dials) return NULL;
    for (int i = 0; i < MAX_DIALS; i++)
        if (s_dials[i].obj == obj) return &s_dials[i];
    if (!add) return NULL;
    for (int i = 0; i < MAX_DIALS; i++)
        if (!s_dials[i].obj) {
            s_dials[i].obj = obj;
            return &s_dials[i];
        }
    return NULL;  // more dials than slots: those are drawn the slow way
}

static void dial_forget(lv_event_t* e)
{
    DialCache* c = dial_find((lv_obj_t*)lv_event_get_target(e), false);
    if (!c) return;
    if (c->data) lv_image_cache_drop(&c->buf);
    free(c->data);
    *c = {};
}

// Draws the still part into the picture (LVGL task, outside a refresh: from lv_async_call)
static void dial_build(void* arg)
{
    DialCache* c = dial_find((lv_obj_t*)arg, false);
    if (!c || !c->el) return;
    c->building   = false;
    int32_t w     = lv_obj_get_width(c->obj);
    DialColors cl = dial_colors(c->el);
    if (w <= 0) return;
    if (!c->data || c->w != w) {
        free(c->data);
        c->data = (uint8_t*)heap_caps_malloc((size_t)w * w * 4, MALLOC_CAP_SPIRAM);
        c->w    = w;
        if (!c->data) return;
    }
    lv_draw_buf_init(&c->buf, w, w, LV_COLOR_FORMAT_ARGB8888, w * 4, c->data, (uint32_t)w * w * 4);
    memset(c->data, 0, (size_t)w * w * 4);
    if (!s_dial_canvas) {
        s_dial_canvas = lv_canvas_create(lv_layer_top());
        lv_obj_add_flag(s_dial_canvas, LV_OBJ_FLAG_HIDDEN);
    }
    lv_canvas_set_draw_buf(s_dial_canvas, &c->buf);
    lv_layer_t layer;
    lv_canvas_init_layer(s_dial_canvas, &layer);
    lv_area_t a = {0, 0, w - 1, w - 1};
    dial_still(&layer, c->el, a, cl);
    lv_canvas_finish_layer(s_dial_canvas, &layer);
    lv_image_cache_drop(&c->buf);
    c->key = dial_key(c->el, cl, w);
    lv_obj_invalidate(c->obj);
}

static void scene_analog_cb(lv_event_t* e)
{
    lv_obj_t* obj     = (lv_obj_t*)lv_event_get_target(e);
    const SceneEl* el = (const SceneEl*)lv_event_get_user_data(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    DialColors cl = dial_colors(el);
    DialCache* c  = dial_find(obj, true);
    if (c && c->data && c->key == dial_key(el, cl, lv_area_get_width(&a))) {
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.src = &c->buf;
        lv_draw_image(layer, &d, &a);
    } else {
        dial_still(layer, el, a, cl);  // this time the slow way; the picture is made after this frame
        if (c && !c->building) {
            c->el       = el;
            c->building = true;
            lv_async_call(dial_build, obj);
        }
    }
    DialHands h = c && c->has_hands ? c->hands : dial_hands_now();
    if (c) c->hands = h, c->has_hands = true;
    dial_hands(layer, el, a, cl, h);
}

// Bounding box of a hand (absolute coordinates), with room for its width and anti-aliasing
static lv_area_t dial_hand_area(const lv_area_t& a, int which, float angle)
{
    float r = lv_area_get_width(&a) / 2.0f, cx = a.x1 + r, cy = a.y1 + r, r0, r1, w;
    dial_hand_geom(which, r, &r0, &r1, &w);
    float s = sinf(angle), k = cosf(angle), pad = w / 2 + 3;
    float x0 = cx + s * r0, y0 = cy - k * r0, x1 = cx + s * r1, y1 = cy - k * r1;
    float cap = LV_MAX(3, (int)(r / 22)) + 2;
    lv_area_t out = {(int32_t)floorf(LV_MIN(LV_MIN(x0, x1) - pad, cx - cap)), (int32_t)floorf(LV_MIN(LV_MIN(y0, y1) - pad, cy - cap)),
                     (int32_t)ceilf(LV_MAX(LV_MAX(x0, x1) + pad, cx + cap)), (int32_t)ceilf(LV_MAX(LV_MAX(y0, y1) + pad, cy + cap))};
    return out;
}

// The new second: redraw only where the hands were and where they go
static void dial_tick(lv_obj_t* obj, const SceneEl* el)
{
    DialCache* c = dial_find(obj, false);
    if (!c || !c->has_hands) {
        lv_obj_invalidate(obj);
        return;
    }
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    DialHands now = dial_hands_now(), was = c->hands;
    const float olds[3] = {was.hour, was.min, was.sec}, news[3] = {now.hour, now.min, now.sec};
    for (int i = 0; i < 3; i++) {
        if (i == 2 && !(el->flags & SC_SECONDS)) continue;
        if (olds[i] == news[i]) continue;
        lv_area_t o = dial_hand_area(a, i, olds[i]), n = dial_hand_area(a, i, news[i]);
        lv_obj_invalidate_area(obj, &o);
        lv_obj_invalidate_area(obj, &n);
    }
    c->hands = now;  // drawn by scene_analog_cb in the next frame
}

static void scene_led_cb(lv_event_t* e)
{
    const SceneEl* el = (const SceneEl*)lv_event_get_user_data(e);
    lv_obj_t* obj     = (lv_obj_t*)lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    float s   = el->size / 280.0f;  // size = digit height in px
    bool secs = el->flags & SC_SECONDS;
    float gw  = (secs ? GROUP_W : HHMM_W) * s;
    float pad = SKEW * el->size / 2 + 4;  // the slant reaches past the digit box on both sides
    SegCtx c;
    c.layer    = lv_event_get_layer(e);
    c.cx       = a.x1 + pad + gw / 2.0f;
    c.cy       = a.y1 + GROUP_H * s / 2.0f;
    c.only_lit = !(el->flags & SC_UNLIT);
    float gx = a.x1 + pad, gy = a.y1;
    // Colors: the element's own or the palette; draw_digit uses led_on() / led_off(), so set them for this call
    lv_color_t on  = scene_color(el->color, led());
    lv_color_t off = lv_color_mix(on, HEX(0A0A0A), 10);
    time_t now     = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    auto digit = [&](float x, float y, float k, int n) {
        static const char* MAP[10] = {"abcdef", "bc", "abdeg", "abcdg", "bcfg", "acdfg", "acdefg", "abc", "abcdefg", "abcdfg"};
        const float W = 150 * k, H = 280 * k, t = 28 * k, g = 3 * k, hw = W - t - 2 * g, vh = H / 2 - t / 2 - 2 * g;
        struct Seg { char id; float x, y, w, h; bool hor; } segs[7] = {
            {'a', t / 2 + g, 0, hw, t, true}, {'g', t / 2 + g, H / 2 - t / 2, hw, t, true}, {'d', t / 2 + g, H - t, hw, t, true},
            {'f', 0, t / 2 + g, t, vh, false}, {'b', W - t, t / 2 + g, t, vh, false}, {'e', 0, H / 2 + g, t, vh, false},
            {'c', W - t, H / 2 + g, t, vh, false}};
        for (const Seg& sg : segs) {
            float X = x + sg.x, Y = y + sg.y, w = sg.w, h = sg.h, xs[6], ys[6];
            if (sg.hor) {
                float xv[6] = {X + h / 2, X + w - h / 2, X + w, X + w - h / 2, X + h / 2, X};
                float yv[6] = {Y, Y, Y + h / 2, Y + h, Y + h, Y + h / 2};
                memcpy(xs, xv, sizeof(xs)), memcpy(ys, yv, sizeof(ys));
            } else {
                float xv[6] = {X + w / 2, X + w, X + w, X + w / 2, X, X};
                float yv[6] = {Y, Y + w / 2, Y + h - w / 2, Y + h, Y + h - w / 2, Y + w / 2};
                memcpy(xs, xv, sizeof(xs)), memcpy(ys, yv, sizeof(ys));
            }
            bool lit = strchr(MAP[n], sg.id) != NULL;
            if (!lit && c.only_lit) continue;
            fill_poly(c, xs, ys, 6, lit ? on : off);
        }
    };
    digit(gx, gy, s, tm.tm_hour / 10);
    digit(gx + 172 * s, gy, s, tm.tm_hour % 10);
    bool colon_lit = tm.tm_sec % 2 == 0;
    for (int k = 0; k < 2 && (colon_lit || !c.only_lit); k++) {
        float x = gx + 344 * s, y = gy + (79 + k * 94) * s, d = 28 * s;
        float xs[4] = {x, x + d, x + d, x}, ys[4] = {y, y, y + d, y + d};
        fill_poly(c, xs, ys, 4, colon_lit ? on : off);
    }
    digit(gx + 394 * s, gy, s, tm.tm_min / 10);
    digit(gx + 566 * s, gy, s, tm.tm_min % 10);
    if (!secs) return;
    float sy = gy + (GROUP_H - 280 * 0.36f) * s;
    digit(gx + 738 * s, sy, 0.36f * s, tm.tm_sec / 10);
    digit(gx + 804 * s, sy, 0.36f * s, tm.tm_sec % 10);
}

/* --------------------------------- Loaders -------------------------------- */
// Animated elements for the intro (and any screen): bar filling up, dots, spinner, pulsing ring, wave

static void anim_width(void* o, int32_t v) { lv_obj_set_width((lv_obj_t*)o, v); }
static void anim_height(void* o, int32_t v) { lv_obj_set_height((lv_obj_t*)o, v); }
static void anim_opa(void* o, int32_t v) { lv_obj_set_style_opa((lv_obj_t*)o, v, 0); }
static void anim_scale(void* o, int32_t v)
{
    lv_obj_set_style_transform_scale((lv_obj_t*)o, v, 0);
    lv_obj_set_style_opa((lv_obj_t*)o, (lv_opa_t)LV_CLAMP(0, 255 - (v - 128) * 2, 255), 0);
}

static void loader_anim(lv_obj_t* o, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, uint32_t ms, uint32_t delay, bool back)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, delay);
    if (back) lv_anim_set_playback_duration(&a, ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static lv_obj_t* dot(lv_obj_t* parent, int w, int h, lv_color_t c, int radius)
{
    lv_obj_t* d = box(parent);
    lv_obj_set_size(d, w, h);
    lv_obj_set_style_radius(d, radius, 0);
    lv_obj_set_style_bg_color(d, c, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    return d;
}

static lv_obj_t* scene_loader(lv_obj_t* parent, const SceneEl& el)
{
    lv_color_t c     = el.color & SC_COLOR ? lv_color_hex(el.color & 0xFFFFFF) : led();
    lv_color_t track = el.color2 & SC_COLOR ? lv_color_hex(el.color2 & 0xFFFFFF) : lv_color_mix(c, lv_color_black(), 50);
    uint32_t ms      = el.size2 ? el.size2 * 100 : 1200;
    int w = LV_MAX(el.w, 4), h = LV_MAX(el.h, 4);
    lv_obj_t* o = box(parent);
    lv_obj_set_size(o, w, h);
    switch (el.source) {
        case 0: {  // bar: track with a fill growing over the time (once, or again and again)
            lv_obj_set_style_radius(o, h / 2, 0);
            lv_obj_set_style_bg_color(o, track, 0);
            lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
            lv_obj_t* fill = dot(o, 0, h, c, h / 2);
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, fill);
            lv_anim_set_exec_cb(&a, anim_width);
            lv_anim_set_values(&a, 0, w);
            lv_anim_set_duration(&a, ms);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
            if (el.flags & SC_LOOP) lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);
            break;
        }
        case 1: {  // three dots fading in turn
            flex(o, LV_FLEX_FLOW_ROW, h / 2);
            lv_obj_set_flex_align(o, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            for (int k = 0; k < 3; k++) loader_anim(dot(o, h, h, c, LV_RADIUS_CIRCLE), anim_opa, 60, 255, ms / 2, k * ms / 6, true);
            break;
        }
        case 2: {  // spinner
            lv_obj_t* sp = lv_spinner_create(o);
            lv_spinner_set_anim_params(sp, ms, 240);
            lv_obj_set_size(sp, w, w);
            lv_obj_center(sp);
            int t = LV_MAX(2, w / 12);
            lv_obj_set_style_arc_width(sp, t, LV_PART_MAIN);
            lv_obj_set_style_arc_width(sp, t, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(sp, track, LV_PART_MAIN);
            lv_obj_set_style_arc_color(sp, c, LV_PART_INDICATOR);
            lv_obj_set_style_arc_rounded(sp, true, LV_PART_INDICATOR);
            break;
        }
        case 3: {  // ring growing and fading, a dot in the middle
            lv_obj_t* ring = box(o);
            lv_obj_set_size(ring, w, w);
            lv_obj_center(ring);
            lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_color(ring, c, 0);
            lv_obj_set_style_border_width(ring, LV_MAX(2, w / 16), 0);
            lv_obj_set_style_transform_pivot_x(ring, w / 2, 0);
            lv_obj_set_style_transform_pivot_y(ring, w / 2, 0);
            loader_anim(ring, anim_scale, 128, 256, ms, 0, false);
            lv_obj_t* core = dot(o, w / 4, w / 4, c, LV_RADIUS_CIRCLE);
            lv_obj_center(core);
            break;
        }
        default: {  // wave: five bars going up and down in turn
            flex(o, LV_FLEX_FLOW_ROW, w / 14);
            lv_obj_set_flex_align(o, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            int bw = LV_MAX(2, w / 8);
            for (int k = 0; k < 5; k++) loader_anim(dot(o, bw, h, c, bw / 2), anim_height, h / 4, h, ms / 2, k * ms / 10, true);
            break;
        }
    }
    return o;
}

// A tap on an element of the alarm or timer screen
static void scene_action_cb(lv_event_t* e)
{
    const SceneEl* el = (const SceneEl*)lv_event_get_user_data(e);
    switch (el->action) {
        case SA_SNOOZE: alarm_ringing() ? alarm_snooze() : ui_alarm_hide(); break;
        case SA_STOP: alarm_ringing() ? alarm_dismiss() : ui_alarm_hide(); break;
        case SA_TIMER_GO: on_timer_go(e); break;
        case SA_TIMER_RESET: on_timer_reset(e); break;
        case SA_TIMER_PLUS: timer_set(timer_remaining() + 60); break;
        case SA_TIMER_MINUS: timer_set(timer_remaining() - 60); break;
        case SA_TIMER_PLUS_S: timer_set(timer_remaining() + 5); break;
        case SA_TIMER_MINUS_S: timer_set(timer_remaining() - 5); break;
        case SA_TIMER_SET:
            timer_running = false;
            timer_left = timer_total = LV_MAX(1, el->action_val) * 60;
            timer_refresh();
            break;
        case SA_TIMER_CLOSE: on_timer_close(e); break;
        default: break;
    }
}

static bool scene_make_el(lv_obj_t* parent, const SceneEl& el, SceneLive& L);

// One scene element on `parent`; fills L (what the color / time / sensor updates need). false = not shown.
static bool scene_make(lv_obj_t* parent, const SceneEl& el, SceneLive& L)
{
    if (!scene_make_el(parent, el, L)) return false;
    if (el.action) {
        lv_obj_t* hit = L.unit ? lv_obj_get_parent(L.obj) : L.obj;  // a sensor: the whole row
        lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(hit, 16);
        lv_obj_add_event_cb(hit, scene_action_cb, LV_EVENT_CLICKED, (void*)&el);
        if (el.action == SA_TIMER_PLUS || el.action == SA_TIMER_MINUS || el.action == SA_TIMER_PLUS_S || el.action == SA_TIMER_MINUS_S)
            lv_obj_add_event_cb(hit, scene_action_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void*)&el);
    }
    return true;
}

static bool scene_make_el(lv_obj_t* parent, const SceneEl& el, SceneLive& L)
{
    lv_obj_t *o = NULL, *unit = NULL;
    switch (el.type) {
        case SC_LOADER:
            o = scene_loader(parent, el);
            break;
        case SC_ANALOG:
            o = box(parent);
            lv_obj_set_size(o, el.w, el.w);
            lv_obj_add_event_cb(o, scene_analog_cb, LV_EVENT_DRAW_MAIN, (void*)&el);
            lv_obj_add_event_cb(o, dial_forget, LV_EVENT_DELETE, NULL);
            break;
        case SC_CLOCK_LED: {
            // Drawing box wider than the digits by the slant on each side (see scene_led_cb)
            int pad = SKEW * el.size / 2 + 4;
            o       = box(parent);
            lv_obj_set_size(o, el.w + 2 * pad, el.h);
            lv_obj_add_event_cb(o, scene_led_cb, LV_EVENT_DRAW_MAIN, (void*)&el);
            lv_obj_set_pos(o, el.x - pad, el.y);
            lv_obj_set_style_opa(o, el.opa, 0);
            L.el = &el, L.obj = o, L.unit = NULL;
            return true;
        }
        case SC_CLOCK_TEXT:
        case SC_DATE:
        case SC_PLACE:
        case SC_ALARM_LABEL:
        case SC_TIMER_TIME:
        case SC_TIMER_GO:
        case SC_TEXT:
            o = label(parent, layout_font(el.font, el.size), lv_color_white(), el.type == SC_TEXT ? el.text : "");
            if (el.w > 0) {
                lv_obj_set_width(o, el.w);
                lv_obj_set_style_text_align(o, el.align == 1 ? LV_TEXT_ALIGN_CENTER : el.align == 2 ? LV_TEXT_ALIGN_RIGHT
                                                                                                     : LV_TEXT_ALIGN_LEFT, 0);
            }
            break;
        case SC_SENSOR: {
            o = box(parent);  // row: value + unit, bottoms aligned
            lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            flex(o, LV_FLEX_FLOW_ROW, el.size * 0.16f);
            lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
            lv_obj_t* v = label(o, layout_font(el.font, el.size), lv_color_white(), "—");
            if (el.text && el.text[0]) {
                unit = label(o, layout_font(el.font, el.size2 ? el.size2 : el.size * 0.47f), lv_color_white(), el.text);
                lv_obj_set_style_pad_bottom(unit, el.size * 0.14f, 0);
            }
            L.el = &el, L.obj = v, L.unit = unit;
            lv_obj_set_pos(o, el.x, el.y);
            lv_obj_set_style_opa(o, el.opa, 0);
            return true;
        }
        case SC_ICON: {
            char t[4] = {(char)(0xE0 | (el.icon >> 12)), (char)(0x80 | ((el.icon >> 6) & 0x3F)), (char)(0x80 | (el.icon & 0x3F)), 0};
            o = label(parent, layout_font("studio_icons", el.size), lv_color_white(), t);
            break;
        }
        case SC_WEATHER:
            // Icon (studio icon font) or sunrise / sunset in a font; filled by scene_sensors when weather comes
            o = el.source <= 1 ? label(parent, layout_font("studio_icons", el.size), lv_color_white(), "")
                               : label(parent, layout_font(el.font, el.size), lv_color_white(), "--:--");
            break;
        case SC_FORECAST: {
            // Column: day name, icon, max and min in a row; filled by scene_forecast when weather comes
            o = box(parent);
            lv_obj_set_size(o, el.w > 0 ? el.w : LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            flex(o, LV_FLEX_FLOW_COLUMN, el.size * 0.2f);
            lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            label(o, layout_font(el.font, el.size), lv_color_white(), "--");
            label(o, layout_font("studio_icons", el.size2 ? el.size2 : el.size * 8 / 5), lv_color_white(), "");
            lv_obj_t* temps = box(o);
            lv_obj_set_size(temps, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            flex(temps, LV_FLEX_FLOW_ROW, el.size * 0.35f);
            label(temps, layout_font(el.font, el.size), lv_color_white(), "--");
            label(temps, layout_font(el.font, el.size), lv_color_white(), "--");
            L.el = &el, L.obj = o, L.unit = NULL;
            lv_obj_set_pos(o, el.x, el.y);
            lv_obj_set_style_opa(o, el.opa, 0);
            return true;
        }
        case SC_ALARM_BADGE:
        case SC_TIMER_BADGE: {
            // Row: icon and / or time, vertically centered; hidden until there is something to show (scene_badge)
            o = box(parent);
            lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            flex(o, LV_FLEX_FLOW_ROW, el.size * 0.25f);
            lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            if (el.source != 2 && el.icon) {
                char t[4] = {(char)(0xE0 | (el.icon >> 12)), (char)(0x80 | ((el.icon >> 6) & 0x3F)), (char)(0x80 | (el.icon & 0x3F)), 0};
                label(o, layout_font("studio_icons", el.size2 ? el.size2 : el.size), lv_color_white(), t);
            }
            if (el.source != 1) unit = label(o, layout_font(el.font, el.size), lv_color_white(), "");
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
            L.el = &el, L.obj = o, L.unit = unit;
            lv_obj_set_pos(o, el.x, el.y);
            lv_obj_set_style_opa(o, el.opa, 0);
            return true;
        }
        case SC_IMAGE:
            o = lv_image_create(parent);
            lv_image_set_src(o, el.img);
            lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
            break;
        case SC_RECT:
        case SC_ELLIPSE:
            o = box(parent);
            lv_obj_set_size(o, el.w, el.h);
            lv_obj_set_style_radius(o, el.type == SC_ELLIPSE ? LV_RADIUS_CIRCLE : el.size2, 0);
            if (el.color & SC_COLOR) {
                lv_obj_set_style_bg_color(o, lv_color_hex(el.color & 0xFFFFFF), 0);
                lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
            }
            if (el.size && (el.color2 & SC_COLOR)) {
                lv_obj_set_style_border_width(o, el.size, 0);
                lv_obj_set_style_border_color(o, lv_color_hex(el.color2 & 0xFFFFFF), 0);
            }
            break;
        case SC_LINE: {
            o                      = lv_line_create(parent);
            int16_t ox = el.w < 0 ? -el.w : 0, oy = el.h < 0 ? -el.h : 0;  // the object starts at the top-left point
            L.pts[0] = {ox, oy};
            L.pts[1] = {ox + el.w, oy + el.h};
            lv_line_set_points(o, L.pts, 2);
            lv_obj_set_style_line_width(o, el.size, 0);
            lv_obj_set_style_line_rounded(o, el.flags & SC_ROUND, 0);
            lv_obj_set_style_line_color(o, lv_color_hex(el.color & 0xFFFFFF), 0);
            lv_obj_set_pos(o, el.x - ox, el.y - oy);
            lv_obj_set_style_opa(o, el.opa, 0);
            L.el  = &el, L.obj = o, L.unit = NULL;
            return true;
        }
    }
    if (!o) return false;
    lv_obj_set_pos(o, el.x, el.y);
    lv_obj_set_style_opa(o, el.opa, 0);
    L.el = &el, L.obj = o, L.unit = unit;
    return true;
}

static void scene_build()
{
    const SceneFace* faces = active_faces();
    if (!faces) return;
    if (!scene_live) scene_live = (SceneLive(*)[64])heap_caps_calloc(2, sizeof(*scene_live), MALLOC_CAP_SPIRAM);
    if (!scene_live) return;
    // The layout's objects go with the layers: forget them so nothing touches deleted objects
    for (Item& it : items) it = {};
    for (int k = 0; k < 3; k++) sensor_values[k] = big_temp[k] = NULL;
    for (int f = 0; f < 2; f++) date_label[f] = clock_label[f][0] = clock_label[f][1] = NULL, date_t[f] = NULL;
    layout_fonts_begin();
    for (int f = 0; f < 2; f++) {
        const SceneFace& face = faces[f];
        lv_obj_clean(layout_layer[f]);
        scene_live_n[f] = 0;
        for (int i = 0; i < face.count && scene_live_n[f] < 64; i++) {
            if (scene_make(layout_layer[f], face.els[i], scene_live[f][scene_live_n[f]])) scene_live_n[f]++;
        }
    }
    if (third_built) third_make();  // its fonts must count as used, or layout_fonts_end() frees them
    layout_fonts_end();
    sensors_shown = 0;
    scene_colors(night_mode);
    scene_update_time();
}

// Colors for light / dark mode (dark: everything dimmed like the built-in faces)
static void scene_colors(bool night)
{
    const SceneFace* faces = active_faces();
    if (!faces) return;
    scene_night = night;
    for (int f = 0; f < 2; f++) {
        lv_obj_t* scr = f ? scr_big : scr_clock;
        lv_color_t bg = lv_color_hex(faces[f].bg);
        lv_obj_set_style_bg_color(scr, night ? lv_color_mix(bg, lv_color_black(), 60) : bg, 0);
        for (int i = 0; i < scene_live_n[f]; i++) scene_paint(scene_live[f][i]);
    }
    if (third_built) {
        lv_color_t bg = lv_color_hex(live_third.bg);
        lv_obj_set_style_bg_color(scr_third, night ? lv_color_mix(bg, lv_color_black(), 60) : bg, 0);
        for (int i = 0; i < third_live_n; i++) scene_paint(third_live[i]);
    }
}

// Text colors of one element (automatic colors follow the palette)
static void scene_paint(const SceneLive& L)
{
    switch (L.el->type) {
        case SC_CLOCK_TEXT: lv_obj_set_style_text_color(L.obj, scene_color(L.el->color, led()), 0); break;
        case SC_SENSOR:
            lv_obj_set_style_text_color(L.obj, scene_color(L.el->color, value_color(false)), 0);
            if (L.unit) lv_obj_set_style_text_color(L.unit, scene_color(L.el->color2, HEX(8A929C)), 0);
            break;
        case SC_DATE:
        case SC_PLACE:
        case SC_TEXT: lv_obj_set_style_text_color(L.obj, scene_color(L.el->color, HEX(8A929C)), 0); break;
        case SC_ALARM_LABEL:
        case SC_TIMER_GO: lv_obj_set_style_text_color(L.obj, scene_color(L.el->color, HEX(E9E4DA)), 0); break;
        case SC_TIMER_TIME: lv_obj_set_style_text_color(L.obj, scene_color(L.el->color, led()), 0); break;
        case SC_ICON:
        case SC_WEATHER: lv_obj_set_style_text_color(L.obj, scene_color(L.el->color, led()), 0); break;
        case SC_FORECAST: {
            lv_color_t text  = scene_color(L.el->color, led());
            lv_obj_t* temps  = lv_obj_get_child(L.obj, 2);
            lv_obj_set_style_text_color(lv_obj_get_child(L.obj, 0), text, 0);
            lv_obj_set_style_text_color(lv_obj_get_child(L.obj, 1), scene_color(L.el->color2, text), 0);
            lv_obj_set_style_text_color(lv_obj_get_child(temps, 0), text, 0);
            lv_obj_set_style_text_color(lv_obj_get_child(temps, 1), scene_color(L.el->color3, HEX(8A929C)), 0);
            break;
        }
        case SC_ALARM_BADGE:
        case SC_TIMER_BADGE: {
            lv_color_t text = scene_color(L.el->color, led());
            if (L.el->source != 2 && L.el->icon) lv_obj_set_style_text_color(lv_obj_get_child(L.obj, 0), scene_color(L.el->color2, text), 0);
            if (L.unit) lv_obj_set_style_text_color(L.unit, text, 0);
            break;
        }
        case SC_CLOCK_LED:
        case SC_ANALOG: lv_obj_invalidate(L.obj); break;
        default: break;
    }
}

static void scene_update_time()
{
    if (!active_faces()) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    for (int f = 0; f < 2; f++)
        for (int i = 0; i < scene_live_n[f]; i++) scene_time(scene_live[f][i], tm);
    for (int i = 0; third_built && i < third_live_n; i++) scene_time(third_live[i], tm);
}

// Alarm / timer badge: shown while there is an alarm on or the timer runs, with the time if it has one
static void scene_badge(const SceneLive& L)
{
    char buf[24] = "";
    bool show;
    if (L.el->type == SC_ALARM_BADGE) {
        int h = 0, m = 0;
        show = alarm_indicator(&h, &m);
        snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
    } else {
        show  = timer_running;
        int r = timer_remaining();
        snprintf(buf, sizeof(buf), "%02d:%02d", r / 60, r % 60);
    }
    if (show == lv_obj_has_flag(L.obj, LV_OBJ_FLAG_HIDDEN)) {
        if (show) lv_obj_remove_flag(L.obj, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(L.obj, LV_OBJ_FLAG_HIDDEN);
    }
    if (show && L.unit && strcmp(lv_label_get_text(L.unit), buf) != 0) lv_label_set_text(L.unit, buf);
}

// Time, date or place text of one element (clocks drawn by hand are just redrawn)
static void scene_time(const SceneLive& L, const struct tm& tm)
{
    char buf[64] = "";
    if (L.el->type == SC_ANALOG) {
        dial_tick(L.obj, L.el);
        return;
    }
    if (L.el->type == SC_ALARM_BADGE || L.el->type == SC_TIMER_BADGE) {
        scene_badge(L);
        return;
    }
    if (L.el->type == SC_CLOCK_LED) {
        lv_obj_invalidate(L.obj);
        return;
    } else if (L.el->type == SC_CLOCK_TEXT) {
        if (L.el->flags & SC_SECONDS) snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
        else snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    } else if (L.el->type == SC_DATE || L.el->type == SC_PLACE) {
        if (L.el->type == SC_PLACE) {
            char postcode[8];
            weather_location(postcode, buf);
        } else switch (L.el->source) {
            case 1: snprintf(buf, sizeof(buf), "%02d.%02d.%04d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900); break;
            case 2: snprintf(buf, sizeof(buf), "%s", day_name(tm.tm_wday)); break;
            case 3: snprintf(buf, sizeof(buf), "%d %s", tm.tm_mday, month_name(tm.tm_mon)); break;
            default: snprintf(buf, sizeof(buf), "%s, %d %s", day_name(tm.tm_wday), tm.tm_mday, month_name(tm.tm_mon));
        }
        if (L.el->flags & SC_UPPER) upper_pl(buf);
    } else {
        return;
    }
    if (strcmp(lv_label_get_text(L.obj), buf) != 0) lv_label_set_text(L.obj, buf);
}

// Lucide glyph for a WMO weather code (night: moon instead of sun)
static uint16_t weather_glyph(int code, bool day)
{
    if (code <= 0) return day ? 0xE17C : 0xE122;                         // sun / moon (also unknown)
    if (code <= 2) return day ? 0xE216 : 0xE215;                         // cloud-sun / cloud-moon
    if (code == 3) return 0xE08C;                                        // cloud
    if (code == 45 || code == 48) return 0xE214;                         // cloud-fog
    if (code >= 51 && code <= 57) return 0xE08E;                         // cloud-drizzle
    if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return 0xE092;  // cloud-rain
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return 0xE094;    // cloud-snow
    if (code >= 95) return 0xE090;                                       // cloud-lightning
    return 0xE08C;
}

static void scene_weather(const SceneLive& L, const WeatherData& w)
{
    char buf[16] = "";
    if (L.el->source <= 1) {
        uint16_t cp = L.el->source == 0 ? weather_glyph(w.code, w.is_day) : weather_glyph(w.code_day[1], true);
        buf[0] = 0xE0 | (cp >> 12), buf[1] = 0x80 | ((cp >> 6) & 0x3F), buf[2] = 0x80 | (cp & 0x3F), buf[3] = 0;
    } else {
        int m = L.el->source == 2 ? w.sunrise : w.sunset;
        if (m >= 0) snprintf(buf, sizeof(buf), "%02d:%02d", m / 60, m % 60);
        else strcpy(buf, "--:--");
    }
    if (strcmp(lv_label_get_text(L.obj), buf) != 0) lv_label_set_text(L.obj, buf);
}

static void set_text(lv_obj_t* l, const char* t)
{
    if (strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

static void scene_forecast(const SceneLive& L, const WeatherData& w)
{
    int d = L.el->source < WEATHER_DAYS ? L.el->source : WEATHER_DAYS - 1;
    char buf[32] = "--";
    if (w.wday[d] >= 0) {
        // the first day is "Dziś": the forecast always starts today, not on a fixed weekday
        strlcpy(buf, d == 0 ? TR("Dziś", "Today") : L.el->flags & SC_LONG ? day_name(w.wday[d]) : day_short(w.wday[d]), sizeof(buf));
        if (L.el->flags & SC_UPPER) upper_pl(buf);
    }
    set_text(lv_obj_get_child(L.obj, 0), buf);
    uint16_t cp = weather_glyph(w.code_day[d], true);
    char icon[4] = {(char)(0xE0 | (cp >> 12)), (char)(0x80 | ((cp >> 6) & 0x3F)), (char)(0x80 | (cp & 0x3F)), 0};
    set_text(lv_obj_get_child(L.obj, 1), icon);
    lv_obj_t* temps = lv_obj_get_child(L.obj, 2);
    snprintf(buf, sizeof(buf), "%ld°", lroundf(w.t_max[d]) + 0L);  // whole degrees, never "-0°"
    set_text(lv_obj_get_child(temps, 0), buf);
    snprintf(buf, sizeof(buf), "%ld°", lroundf(w.t_min[d]) + 0L);
    set_text(lv_obj_get_child(temps, 1), buf);
}

static void scene_sensor(const SceneLive& L, const WeatherData& w)
{
    if (L.el->type == SC_WEATHER) return scene_weather(L, w);
    if (L.el->type == SC_FORECAST) return scene_forecast(L, w);
    if (L.el->type != SC_SENSOR) return;
    float v;
    switch (L.el->source) {
        case 1: v = w.humidity; break;
        case 2: v = w.pressure; break;
        case 3: v = w.t_max[0]; break;
        case 4: v = w.t_min[0]; break;
        case 5: v = w.t_max[1]; break;
        case 6: v = w.t_min[1]; break;
        default: v = w.temperature;
    }
    char buf[24];
    if (fabsf(v) < 0.5f * powf(10.0f, -L.el->decimals)) v = 0;  // no "-0,0"
    snprintf(buf, sizeof(buf), "%.*f", L.el->decimals, v);
    char* dot = strchr(buf, '.');
    if (dot) *dot = ',';  // Polish decimal comma
    lv_label_set_text(L.obj, buf);
}

static void scene_sensors(const WeatherData& w)
{
    if (!active_faces() || !w.version) return;
    for (int f = 0; f < 2; f++)
        for (int i = 0; i < scene_live_n[f]; i++) scene_sensor(scene_live[f][i], w);
    for (int i = 0; third_built && i < third_live_n; i++) scene_sensor(third_live[i], w);
}

// Dimming layer for dark mode, on top of everything; taps pass through to the screen
static lv_obj_t* add_night_overlay(lv_obj_t* scr)
{
    lv_obj_t* o = box(scr);
    lv_obj_set_size(o, 1280, 720);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, NIGHT_DIM, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

static void build_clock_screen()
{
    scr_clock = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr_clock);
    lv_obj_set_style_bg_color(scr_clock, HEX(050505), 0);
    lv_obj_set_style_bg_opa(scr_clock, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr_clock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_clock, on_clock_tap, LV_EVENT_SHORT_CLICKED, NULL);

    // LED digits straight on the background, drawn where the layout puts them
    seg_display = box(scr_clock);
    lv_obj_set_size(seg_display, 1280, 720);
    lv_obj_add_event_cb(seg_display, seg_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    // Sensors and date, (re)built from the layout by build_layout()
    layout_layer[0] = box(scr_clock);
    lv_obj_set_size(layout_layer[0], 1280, 720);

    night_overlay[0] = add_night_overlay(scr_clock);
}

// Big digital clock: HH:MM at 1.5x, only lit segments, temperature in the top left in the digit color
static void build_big_screen()
{
    scr_big = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr_big);
    lv_obj_set_style_bg_color(scr_big, HEX(050505), 0);
    lv_obj_set_style_bg_opa(scr_big, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr_big, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_big, on_clock_tap, LV_EVENT_SHORT_CLICKED, NULL);

    big_display = box(scr_big);
    lv_obj_set_size(big_display, 1280, 720);
    lv_obj_add_event_cb(big_display, seg_draw_cb, LV_EVENT_DRAW_MAIN, (void*)1);
    layout_layer[1] = box(scr_big);
    lv_obj_set_size(layout_layer[1], 1280, 720);

    night_overlay[1] = add_night_overlay(scr_big);
}

static lv_obj_t* settings_prev;  // screen to return to from the settings
static void third_build();
static void third_unbuild();

// Shows a screen, refreshing its contents first. No transition animation: full-screen
// software rendering on this display is too slow for a smooth slide.
static void show_screen(lv_obj_t* target)
{
    if (target == scr_clock) {
        lv_obj_invalidate(seg_display);
    } else if (target == scr_big) {
        lv_obj_invalidate(big_display);
    } else if (target && target == scr_third) {
        third_build();
    }
    lv_screen_load(target);
    if (target != scr_third) third_unbuild();  // Wariant-3 keeps its elements only while it is shown
    apply_backlight(target);
}

static int face_count()
{
    return live_third_on ? 3 : 2;
}

static void third_create();

static lv_obj_t* face(int k)
{
    if (k == 2 && live_third_on) {
        if (!scr_third) third_create();
        return scr_third;
    }
    return k == 1 ? scr_big : scr_clock;
}

// Index of a clock face, -1 for any other screen
static int face_index(lv_obj_t* scr)
{
    if (!scr) return -1;
    if (scr == scr_clock) return 0;
    if (scr == scr_big) return 1;
    if (scr == scr_third && live_third_on) return 2;
    return -1;
}

// Swipe left / right switches between the digital clock and the big digital clock
static void on_clock_swipe(lv_event_t* e)
{
    LV_UNUSED(e);
    lv_indev_t* indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT) return;
    lv_indev_wait_release(indev);  // no tap on the new screen when the finger lifts

    int k = face_index(lv_screen_active());
    if (k < 0) return;
    k = (k + (dir == LV_DIR_LEFT ? 1 : face_count() - 1)) % face_count();
    show_screen(face(k));
}

/* ------------------------------- Menu button ------------------------------ */

static lv_obj_t* menu_icons[3];
static int menu_icon_n;

static void on_menu(lv_event_t* e)
{
    LV_UNUSED(e);
    settings_prev = lv_screen_active();
    settings_on_open();
    show_screen(settings_screen());
}

static void on_settings_back()
{
    show_screen(settings_prev ? settings_prev : scr_clock);
}

// Hamburger in the top right corner of a clock face
static void add_menu_button(lv_obj_t* scr)
{
    lv_obj_t* b = box(scr);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 12);
    lv_obj_set_size(b, 84, 84);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_add_event_cb(b, on_menu, LV_EVENT_CLICKED, NULL);
    lv_obj_t* icon = menu_icons[menu_icon_n++] = label(b, &icon_menu52, HEX(6E757E), ICON_MENU);
    lv_obj_center(icon);
}

// Subtle gray; nearly black on the dimmed night clock
static void menu_theme()
{
    for (int k = 0; k < menu_icon_n; k++) {
        lv_color_t c = night_mode ? HEX(2A2928) : HEX(6E757E);
        lv_obj_set_style_text_color(menu_icons[k], c, 0);
    }
}

/* -------------------------------- Wariant-3 ------------------------------- */
// A third clock face from a Studio design (live or saved scene). The screen stays once made; its elements are
// built when it is shown and deleted when another screen comes, so it costs no memory while not on screen.

static void third_create()
{
    scr_third = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr_third);
    lv_obj_set_style_bg_color(scr_third, HEX(050505), 0);
    lv_obj_set_style_bg_opa(scr_third, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr_third, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_third, on_clock_tap, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(scr_third, on_clock_swipe, LV_EVENT_GESTURE, NULL);
    third_layer = box(scr_third);
    lv_obj_set_size(third_layer, 1280, 720);
    night_overlay3 = add_night_overlay(scr_third);
    add_menu_button(scr_third);
    menu_theme();
}

// (Re)makes the elements (the caller's font generation, or none: they then count for the current one)
static void third_make()
{
    lv_obj_clean(third_layer);
    third_live_n = 0;
    if (!third_live) third_live = (SceneLive*)heap_caps_calloc(64, sizeof(SceneLive), MALLOC_CAP_SPIRAM);
    for (int i = 0; third_live && i < live_third.count && third_live_n < 64; i++)
        if (scene_make(third_layer, live_third.els[i], third_live[third_live_n])) third_live_n++;
    third_built = true;
}

static void third_build()
{
    if (!scr_third || third_built || !live_third_on) return;
    third_make();
    scene_colors(night_mode);
    if (night_mode) lv_obj_remove_flag(night_overlay3, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(night_overlay3, LV_OBJ_FLAG_HIDDEN);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    for (int i = 0; i < third_live_n; i++) scene_time(third_live[i], tm);
    WeatherData w = weather_get();
    if (w.version) scene_sensors(w);
}

static void third_unbuild()
{
    if (!third_built) return;
    lv_obj_clean(third_layer);
    third_live_n = 0;
    third_built  = false;
}

// The face shows its own timer badge: the corner pill stays away there
static bool face_has_timer_badge(lv_obj_t* scr)
{
    const SceneLive* list = NULL;
    int n                 = 0;
    if (scene_live && (scr == scr_clock || scr == scr_big)) list = scene_live[scr == scr_big], n = scene_live_n[scr == scr_big];
    else if (scr && scr == scr_third && third_built) list = third_live, n = third_live_n;
    for (int i = 0; i < n; i++)
        if (list[i].el->type == SC_TIMER_BADGE) return true;
    return false;
}

/* -------------------------------------------------------------------------- */
/*                                   Ticker                                   */
/* -------------------------------------------------------------------------- */

// Outdoor values (Open-Meteo for now); Polish decimal comma for the temperature
static void refresh_sensors()
{
    WeatherData w = weather_get();
    if (w.version == sensors_shown) return;
    sensors_shown = w.version;

    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", w.temperature);
    char* dot = strchr(buf, '.');
    if (dot) *dot = ',';
    if (sensor_values[0]) {
        lv_label_set_text(sensor_values[0], buf);
        lv_label_set_text(big_temp[1], buf);
        lv_label_set_text_fmt(sensor_values[1], "%d", (int)lroundf(w.humidity));
        lv_label_set_text_fmt(sensor_values[2], "%d", (int)lroundf(w.pressure));
    }
    scene_sensors(w);
}


static void tick_cb(lv_timer_t* t)
{
    LV_UNUSED(t);
    // The LVGL task is watched by the task watchdog: if the UI stops for 5 s the
    // device reboots (with a backtrace in the log) instead of staying frozen.
    static bool watched = false;
    if (!watched) watched = esp_task_wdt_add(NULL) == ESP_OK;
    esp_task_wdt_reset();
    check_night_schedule();
    check_screen_off();
    refresh_sensors();
    update_dates();
    alarm_tick();
    update_alarm_time();
    timer_tick();
    lv_obj_t* active = lv_screen_active();
    // Automatic face change while nobody touches the screen
    static uint32_t cycled_at = 0;
    uint32_t period           = config().cycle_min * 60000u;
    if (period && face_index(active) >= 0 && !splash_on && lv_display_get_inactive_time(NULL) >= period &&
        lv_tick_elaps(cycled_at) >= period) {
        cycled_at = lv_tick_get();
        show_screen(face((face_index(active) + 1) % face_count()));
        active = lv_screen_active();
    }
    if (active == scr_clock) {
        lv_obj_invalidate(seg_display);
    } else if (active == scr_big) {
        lv_obj_invalidate(big_display);
    } else if (active == settings_screen()) {
        if (lv_display_get_inactive_time(NULL) >= SETTINGS_IDLE_MS) on_settings_back();
    }
}

/* -------------------------------------------------------------------------- */
/*                                Start screen                                */
/* -------------------------------------------------------------------------- */

static const uint32_t SPLASH_MS = 4000;
static const lv_color_t SPLASH_ACCENT = HEX(FFC83D);  // warm yellow

static void set_width(void* obj, int32_t w)
{
    lv_obj_set_width((lv_obj_t*)obj, w);
}

static lv_obj_t* splash_return;  // clock face to go back to (NULL at boot: the default face)

static lv_obj_t* intro_scr;
static lv_font_t* splash_font;  // Inter Light 40 for the credit line of the built-in intro

static void on_splash_done(lv_timer_t* t)
{
    lv_obj_t* splash = (lv_obj_t*)lv_timer_get_user_data(t);
    if (splash == intro_scr) intro_scr = NULL;
    splash_on = false;
    show_screen(splash_return ? splash_return : face(config().default_face));
    lv_obj_delete(splash);
    if (splash_font) {  // the built-in intro's credit font
        lv_tiny_ttf_destroy(splash_font);
        splash_font = NULL;
    }
}

// Intro designed in Tab5 Studio: sent live from Studio, or compiled in (studio/scene.h defines STUDIO_INTRO)
static SceneFace live_intro;
static bool live_intro_on;
static SceneLive* intro_live;  // [64] in PSRAM

static const SceneFace* intro_scene()
{
    if (live_intro_on) return &live_intro;
#ifdef STUDIO_INTRO
    return &studio_intro_face;
#else
    return NULL;
#endif
}

// New alarm / timer looks from Studio. The old views may point at the elements being replaced: build them again
// (an open one opens again in the new look).
void ui_scene_live_screens(const SceneFace* alarm, const SceneFace* timer)
{
    bool alarm_open = alarm_shown();
    bool timer_open_now = timer_view && !lv_obj_has_flag(timer_view, LV_OBJ_FLAG_HIDDEN);
    if (alarm_view) {
        lv_obj_delete(alarm_view);
        alarm_view = NULL;
    }
    if (timer_view) {
        lv_obj_delete(timer_view);
        timer_view = NULL;
    }
    live_alarm_on = alarm != NULL;
    if (alarm) live_alarm = *alarm;
    live_timer_on = timer != NULL;
    if (timer) live_timer = *timer;
    if (timer_open_now) timer_open();
    if (alarm_open) {
        char label[24];
        strlcpy(label, alarm_label_now, sizeof(label));
        ui_alarm_show(label, 0);
    }
}

void ui_preview_screen(int which)
{
    if (which == 1) {  // the alarm screen without sound; Wyłącz / Drzemka close it
        ui_alarm_show(TR("Podgląd budzika", "Alarm preview"), 0);
    } else if (which == 2) {
        timer_open();
    }
}

static lv_timer_t* intro_timer;  // ends the designed intro on screen (intro_scr)

void ui_scene_live_intro(const SceneFace* f)
{
    if (intro_scr && live_intro_on) {  // playing the live intro: finish it now
        lv_timer_delete(intro_timer);
        splash_on = false;
        show_screen(splash_return ? splash_return : face(config().default_face));
        lv_obj_delete(intro_scr);
        intro_scr = NULL, intro_timer = NULL;
    }
    live_intro_on = f != NULL;
    if (f) live_intro = *f;
}

static void show_scene_intro(const SceneFace& in)
{
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(in.bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    if (!intro_live) intro_live = (SceneLive*)heap_caps_calloc(64, sizeof(SceneLive), MALLOC_CAP_SPIRAM);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    WeatherData w = weather_get();
    for (int i = 0, n = 0; intro_live && i < in.count && n < 64; i++) {
        SceneLive& L = intro_live[n];
        if (!scene_make(scr, in.els[i], L)) continue;
        scene_paint(L);
        scene_time(L, tm);
        if (L.el->type == SC_SENSOR && w.version) {
            float v = L.el->source == 0 ? w.temperature : L.el->source == 1 ? w.humidity : w.pressure;
            char buf[24];
            snprintf(buf, sizeof(buf), "%.*f", L.el->decimals, v);
            char* dot = strchr(buf, '.');
            if (dot) *dot = ',';
            lv_label_set_text(L.obj, buf);
        }
        n++;
    }
    lv_screen_load(scr);
    splash_on = true;
    apply_backlight();
    lv_timer_t* t = lv_timer_create(on_splash_done, in.ms > 0 ? in.ms : SPLASH_MS, scr);
    lv_timer_set_repeat_count(t, 1);
    intro_scr = scr, intro_timer = t;
}

// Logo next to MARCIASMEDIA with a line and the credit line under the name; shown at boot, then the clock
static void show_splash()
{
    if (const SceneFace* in = intro_scene()) {
        show_scene_intro(*in);
        return;
    }
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    // Black with a faint warm glow towards the bottom
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(scr, HEX(1A1310), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Logo on the left; MARCIASMEDIA, the line and the credit line in a column to its right
    lv_obj_t* brand = box(scr);
    lv_obj_set_size(brand, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    flex(brand, LV_FLEX_FLOW_ROW, 48);
    lv_obj_set_flex_align(brand, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(brand);
    lv_obj_t* logo = lv_image_create(brand);
    lv_image_set_src(logo, &img_splash);
    lv_obj_set_style_image_recolor(logo, SPLASH_ACCENT, 0);
    lv_obj_set_style_image_recolor_opa(logo, LV_OPA_COVER, 0);

    lv_obj_t* col = box(brand);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    flex(col, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_t* name = label(col, &splash_sb88, HEX(E9E4DA), "MARCIASMEDIA");
    lv_obj_set_style_text_letter_space(name, 6, 0);

    lv_obj_update_layout(name);
    int32_t w     = lv_obj_get_width(name);
    lv_obj_t* line = box(col);
    lv_obj_set_size(line, w, 2);
    lv_obj_set_style_bg_color(line, HEX(2E2724), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_top(line, 24, 0);
    lv_obj_set_style_margin_bottom(line, 24, 0);

    // Loader: a slightly lighter bar filling the line over the splash time
    lv_obj_t* bar = box(line);
    lv_obj_set_size(bar, 0, 2);
    // Yellow loader with a darker tail and a bright head
    lv_obj_set_style_bg_color(bar, HEX(4A3A10), 0);
    lv_obj_set_style_bg_grad_color(bar, HEX(FFE07A), 0);
    lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bar);
    lv_anim_set_exec_cb(&a, set_width);
    lv_anim_set_values(&a, 0, w);
    lv_anim_set_duration(&a, SPLASH_MS - 300);  // full just before the clock appears
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    // Credit line as wide as MARCIASMEDIA: gray text, the name in off-white, the year in yellow (Inter Light, built-in TTF)
    if (!splash_font) {  // its own copy: layout fonts may be freed by a rebuild while the intro shows
        size_t size;
        const uint8_t* data = layout_font_data("inter_light", &size);
        if (data) splash_font = lv_tiny_ttf_create_data_ex(data, size, 40, LV_FONT_KERNING_NORMAL, 64);
    }
    const lv_font_t* light = splash_font ? splash_font : LV_FONT_DEFAULT;
    lv_obj_t* credit       = box(col);
    lv_obj_set_size(credit, w, LV_SIZE_CONTENT);
    flex(credit, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_flex_align(credit, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    label(credit, light, HEX(8A8580), "Design by");
    label(credit, light, HEX(E9E4DA), "Jakub W.");
    label(credit, light, HEX(8A8580), "best year old");
    label(credit, light, SPLASH_ACCENT, "1976");

    lv_screen_load(scr);
    splash_on = true;
    apply_backlight();
    lv_timer_t* t = lv_timer_create(on_splash_done, SPLASH_MS, scr);
    lv_timer_set_repeat_count(t, 1);
}

// Holding a finger on a clock face for a second plays the start screen again, then returns
static void on_clock_hold(lv_event_t* e)
{
    LV_UNUSED(e);
    lv_indev_t* indev = lv_indev_active();
    if (indev) lv_indev_wait_release(indev);
    timer_open();  // the intro is on the web page and in the settings
}

void ui_create(void)
{
    build_clock_screen();
    build_big_screen();
    build_layout();
    settings_build(on_settings_back);
    for (lv_obj_t* scr : {scr_clock, scr_big}) {
        add_menu_button(scr);
        lv_obj_add_event_cb(scr, on_clock_swipe, LV_EVENT_GESTURE, NULL);
    }
    menu_theme();
    for (lv_obj_t* scr : {scr_clock, scr_big})
        lv_obj_add_event_cb(scr, on_clock_hold, LV_EVENT_LONG_PRESSED, NULL);
    for (lv_indev_t* i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) lv_indev_set_long_press_time(i, 1000);
    build_off_catcher();
    show_splash();
    lv_timer_create(tick_cb, 500, NULL);
}

void ui_set_night(bool on)
{
    set_night(on);
}

static lv_obj_t* scr_update;
static lv_obj_t* update_label;
static lv_obj_t* update_prev;

void ui_show_update(int percent)
{
    if (!scr_update) {
        scr_update = lv_obj_create(NULL);
        lv_obj_remove_style_all(scr_update);
        lv_obj_set_style_bg_color(scr_update, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(scr_update, LV_OPA_COVER, 0);
        lv_obj_remove_flag(scr_update, LV_OBJ_FLAG_SCROLLABLE);
        update_label = lv_label_create(scr_update);
        lv_obj_set_style_text_font(update_label, &inter_m64, 0);
        lv_obj_set_style_text_color(update_label, lv_color_hex(0xE9E4DA), 0);
        lv_obj_align(update_label, LV_ALIGN_CENTER, 0, -30);
        lv_obj_t* hint = lv_label_create(scr_update);
        lv_obj_set_style_text_font(hint, &inter_sb30, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(0x9AA0A6), 0);
        lv_label_set_text(hint, TR("Nie wyłączaj Tab5", "Do not switch the Tab5 off"));
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, 50);
    }
    // three dots: the Inter bitmap fonts have no "…" glyph
    lv_label_set_text_fmt(update_label, "%s %d%%", TR("Aktualizacja...", "Updating..."), percent);
    if (lv_screen_active() != scr_update) {
        update_prev = lv_screen_active();
        lv_screen_load(scr_update);
    }
}

void ui_hide_update(void)
{
    if (lv_screen_active() == scr_update && update_prev) show_screen(update_prev);
}

/* ------------------------------ Web page API ------------------------------ */

// The clock face that is shown, or the one the intro / settings screen returns to
static lv_obj_t* current_face()
{
    lv_obj_t* active = lv_screen_active();
    if (splash_on && splash_return) return splash_return;
    if (active == settings_screen() && settings_prev) return settings_prev;
    return face_index(active) >= 0 ? active : face(config().default_face);
}

int ui_face(void)
{
    return face_index(current_face());
}

void ui_show_face(int k)
{
    splash_return = face(k);  // also where a running intro returns to
    if (lv_screen_active() == settings_screen()) {
        settings_prev = face(k);  // chosen in the settings: shown when they close
    } else if (!splash_on) {
        show_screen(face(k));
    }
}

bool ui_night(void)
{
    return night_mode;
}

bool ui_screen_on(void)
{
    return !screen_off();
}

bool ui_intro_playing(void)
{
    return splash_on;
}

void ui_set_screen(bool on)
{
    set_screen_on(on);
}

void ui_play_intro(void)
{
    set_screen_on(true);
    if (splash_on) return;
    splash_return = current_face();
    show_splash();
}

void ui_config_changed(void)
{
    night_last = -1;  // new windows apply right away
    off_last   = -1;
    check_night_schedule();
    check_screen_off();
    set_night(night_mode);  // repaints everything in the current digit color
}

void ui_layout_changed(void)
{
    build_layout();
    set_night(night_mode);  // colors of the new elements
    refresh_sensors();
}

void ui_scene_live(const SceneFace faces[2])
{
    live_faces[0] = faces[0];
    live_faces[1] = faces[1];
    live_scene    = true;
    ui_layout_changed();
}

void ui_scene_live_third(const SceneFace* f)
{
    bool shown = scr_third && lv_screen_active() == scr_third;
    if (shown) show_screen(scr_clock);  // its elements are about to change (or the variant goes away)
    third_unbuild();
    live_third_on = f != NULL && f->els;
    if (live_third_on) live_third = *f;
    if (!live_third_on) {
        if (settings_prev == scr_third) settings_prev = scr_clock;
        if (splash_return == scr_third) splash_return = NULL;
    } else if (shown) {
        show_screen(scr_third);
    }
}

int ui_face_count(void)
{
    return face_count();
}

void ui_scene_live_preset(const SceneFace faces[2])
{
    // Before ui_create(): it builds the faces from these right away
    live_faces[0] = faces[0];
    live_faces[1] = faces[1];
    live_scene    = true;
}

void ui_scene_live_stop(void)
{
    if (!live_scene) return;
    live_scene = false;
    ui_layout_changed();
    // Back to the built-in look: the background of the layout faces
    lv_obj_set_style_bg_color(scr_clock, night_mode ? lv_color_black() : HEX(050505), 0);
    lv_obj_set_style_bg_color(scr_big, night_mode ? lv_color_black() : HEX(050505), 0);
}

bool ui_scene_is_live(void)
{
    return live_scene;
}

void ui_timer(int open, int start_s, bool cancel)
{
    if (open || start_s > 0) timer_open();
    if (cancel) {
        timer_running = false;
        timer_paused  = false;
        timer_left    = timer_total;
    }
    if (start_s > 0) {
        timer_total = timer_left = LV_MIN(start_s, TIMER_MAX);
        timer_end_us  = esp_timer_get_time() + (int64_t)timer_left * 1000000;
        timer_running = true;
    }
    if (open < 0 && timer_view) lv_obj_add_flag(timer_view, LV_OBJ_FLAG_HIDDEN);
    timer_refresh();
}

int ui_timer_left(bool* running)
{
    *running = timer_running;
    return timer_remaining();
}

void ui_timer_restart(int seconds)
{
    build_timer_pill();
    if (!timer_view) build_timer_view();
    timer_total = timer_left = LV_CLAMP(1, seconds, TIMER_MAX);
    timer_end_us  = esp_timer_get_time() + (int64_t)timer_left * 1000000;
    timer_running = true;
    lv_obj_add_flag(timer_view, LV_OBJ_FLAG_HIDDEN);
    timer_refresh();
}
