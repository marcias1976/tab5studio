#include <ctype.h>
#include <initializer_list>
#include <stdio.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include "config.h"
#include "layout.h"
#include "settings.h"
#include "alarm.h"
#include "lang.h"
#include "ui.h"
#include "weather.h"
#include "version.h"
#include "wifi.h"

LV_FONT_DECLARE(inter_20);
LV_FONT_DECLARE(inter_sb24);
LV_FONT_DECLARE(inter_30);
LV_FONT_DECLARE(inter_sb28);
LV_FONT_DECLARE(inter_sb30);
LV_FONT_DECLARE(inter_m64);
LV_FONT_DECLARE(kb_30);
LV_FONT_DECLARE(icons_40);

#define HEX(c) lv_color_hex(0x##c)

#define ICON_BACK "\xEE\x81\xB2"
#define ICON_SETTINGS "\xEE\x85\x98"  // lucide settings (gear), built-in icon font
#define ICON_TRASH "\xEE\x86\x8E"
#define ICON_LOCK "\xEE\x84\x8F"
#define ICON_WIFI "\xEE\x86\xAE"
#define ICON_REFRESH "\xEE\x85\x89"
#define ICON_CHECK "\xEE\x81\xB0"
#define ICON_PIN "\xEE\x84\x95"

static const lv_color_t BG     = HEX(0B0D10);
static const lv_color_t PANEL  = HEX(15181C);
static const lv_color_t LINE   = HEX(262A30);
static const lv_color_t TEXT   = HEX(E9E4DA);
static const lv_color_t DIM    = HEX(7A7F87);
static const lv_color_t ACCENT = HEX(F5B13D);
static const lv_color_t GOOD   = HEX(6FCF8E);
static const lv_color_t BAD    = HEX(FF6B6B);

static void (*s_on_back)(void);
static lv_obj_t* scr;
static lv_obj_t* wifi_status;
static lv_obj_t* saved_title;
static lv_obj_t* saved_list;
static lv_obj_t* scan_list;
static lv_obj_t* place_label;
static lv_obj_t* postcode_label;
static lv_obj_t* postcode_ta;
static lv_obj_t* postcode_btn;
static lv_obj_t* postcode_msg;
static lv_obj_t* overlay;      // password dialog
static lv_obj_t* pass_title;
static lv_obj_t* pass_ta;
static lv_obj_t* pass_show;  // label of the Show / Hide password button
static lv_obj_t* keyboard;

static char saved_cache[WIFI_MAX_SAVED][33];
static int saved_cache_n = -1;
static char current_cache[33];
static WifiAp scan_aps[20];
static int scan_shown = -2;  // -2 = nothing drawn yet, -1 = "searching" drawn
static char join_ssid[33];
static bool join_watch;
static bool join_failed;  // failure message stays until something connects
static SearchState search_seen = SEARCH_IDLE;
static bool search_done;          // a place was just picked: keep the "Weather for" message
static bool search_mine;          // the search was started here (the web page has its own)
static lv_obj_t* place_results;  // buttons of the found places
// Preset name dialog (defined with the panel settings below)

/* -------------------------------- Helpers -------------------------------- */

static lv_obj_t* box(lv_obj_t* parent)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color, const char* text)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

static void column(lv_obj_t* o, int gap)
{
    lv_obj_set_layout(o, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(o, gap, 0);
}

static void row(lv_obj_t* o, int gap)
{
    lv_obj_set_layout(o, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, gap, 0);
}

static lv_obj_t* button(lv_obj_t* parent, const char* text, lv_color_t bg, lv_color_t fg, lv_event_cb_t cb, void* user)
{
    lv_obj_t* b = box(parent);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_pad_hor(b, 28, 0);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_t* l = label(b, &inter_sb24, fg, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

static lv_obj_t* icon_button(lv_obj_t* parent, const char* icon, lv_color_t color, lv_event_cb_t cb, void* user)
{
    lv_obj_t* b = box(parent);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(b, 64, 64);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_bg_color(b, HEX(2A2E33), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_t* l = label(b, &icons_40, color, icon);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

static lv_obj_t* panel(lv_obj_t* parent, const char* title, lv_obj_t** title_out)
{
    lv_obj_t* p = box(parent);
    lv_obj_set_size(p, (1280 - 2 * 24 - 2 * 22) / 3, lv_pct(100));  // a third of the tab; column_panel() sets other widths
    lv_obj_set_style_bg_color(p, PANEL, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, 24, 0);
    lv_obj_set_style_pad_all(p, 28, 0);
    column(p, 18);
    lv_obj_t* t = label(p, &inter_sb30, TEXT, title);
    if (title_out) *title_out = t;
    return p;
}

static void style_textarea(lv_obj_t* ta, const lv_font_t* font)
{
    lv_obj_set_style_text_font(ta, font, 0);
    lv_obj_set_style_text_color(ta, TEXT, 0);
    lv_obj_set_style_bg_color(ta, HEX(0E0F12), 0);
    lv_obj_set_style_border_color(ta, LINE, 0);
    lv_obj_set_style_border_color(ta, ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_radius(ta, 14, 0);
    lv_obj_set_style_pad_hor(ta, 18, 0);
    lv_obj_set_style_pad_ver(ta, 12, 0);
    lv_obj_set_style_text_color(ta, HEX(4A4F57), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(ta, ACCENT, LV_PART_CURSOR);
    lv_obj_set_style_border_color(ta, ACCENT, LV_PART_CURSOR);
}

/* -------------------------------- Keyboard -------------------------------- */

static void keyboard_show(lv_obj_t* ta, lv_keyboard_mode_t mode)
{
    lv_keyboard_set_mode(keyboard, mode);
    lv_keyboard_set_textarea(keyboard, ta);
    lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(keyboard);
}

static void keyboard_hide()
{
    lv_keyboard_set_textarea(keyboard, NULL);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------------------------- Wi-Fi --------------------------------- */

static void close_password()
{
    keyboard_hide();
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

static void join(const char* ssid, const char* pass)
{
    strlcpy(join_ssid, ssid, sizeof(join_ssid));
    join_watch  = true;
    join_failed = false;
    wifi_join(ssid, pass);
    lv_label_set_text_fmt(wifi_status, TR("Łączenie z %s...", "Connecting to %s..."), ssid);
    lv_obj_set_style_text_color(wifi_status, ACCENT, 0);
}

static void on_pass_connect(lv_event_t* e)
{
    LV_UNUSED(e);
    join(join_ssid, lv_textarea_get_text(pass_ta));
    close_password();
}

static void on_pass_cancel(lv_event_t* e)
{
    LV_UNUSED(e);
    close_password();
}

static void set_pass_visible(bool on)
{
    lv_textarea_set_password_mode(pass_ta, !on);
    lv_label_set_text(pass_show, on ? TR("Ukryj", "Hide") : TR("Pokaż", "Show"));
}

static void on_pass_show(lv_event_t* e)
{
    LV_UNUSED(e);
    set_pass_visible(lv_textarea_get_password_mode(pass_ta));
}

static void open_password(const char* ssid)
{
    strlcpy(join_ssid, ssid, sizeof(join_ssid));
    lv_label_set_text_fmt(pass_title, TR("Hasło do sieci %s", "Password for %s"), ssid);
    lv_textarea_set_text(pass_ta, "");
    set_pass_visible(false);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(overlay);
    lv_obj_add_state(pass_ta, LV_STATE_FOCUSED);
    keyboard_show(pass_ta, LV_KEYBOARD_MODE_TEXT_LOWER);
}

static void on_scan_row(lv_event_t* e)
{
    const WifiAp& ap = scan_aps[(intptr_t)lv_event_get_user_data(e)];
    if (ap.open) {
        join(ap.ssid, "");
    } else {
        open_password(ap.ssid);
    }
}

static void on_forget(lv_event_t* e)
{
    wifi_forget((intptr_t)lv_event_get_user_data(e));
    saved_cache_n = -1;  // redraw
}

static void on_rescan(lv_event_t* e)
{
    LV_UNUSED(e);
    if (wifi_scan_start()) scan_shown = -2;
}

static lv_obj_t* list_row(lv_obj_t* parent)
{
    lv_obj_t* r = box(parent);
    lv_obj_set_size(r, lv_pct(100), 64);
    row(r, 14);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(r, LINE, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    return r;
}

static lv_obj_t* ssid_label(lv_obj_t* parent, const char* ssid, lv_color_t color)
{
    lv_obj_t* l = label(parent, &inter_30, color, ssid);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(l, 1);
    return l;
}

static void refresh_saved()
{
    char saved[WIFI_MAX_SAVED][33], current[33];
    int n = wifi_saved(saved);
    wifi_current(current);
    if (n == saved_cache_n && memcmp(saved, saved_cache, sizeof(saved)) == 0 && strcmp(current, current_cache) == 0) return;
    saved_cache_n = n;
    memcpy(saved_cache, saved, sizeof(saved));
    strlcpy(current_cache, current, sizeof(current_cache));

    lv_label_set_text_fmt(saved_title, TR("Zapisane sieci %d/%d", "Saved networks %d/%d"), n, WIFI_MAX_SAVED);
    lv_obj_clean(saved_list);
    if (n == 0) label(saved_list, &inter_20, DIM, TR("Brak zapisanych sieci", "No saved networks"));
    for (int i = 0; i < n; i++) {
        bool active = strcmp(saved[i], current) == 0;
        lv_obj_t* r = list_row(saved_list);
        label(r, &icons_40, active ? GOOD : DIM, active ? ICON_CHECK : ICON_WIFI);
        ssid_label(r, saved[i], active ? TEXT : HEX(B5B0A8));
        icon_button(r, ICON_TRASH, HEX(8A8580), on_forget, (void*)(intptr_t)i);
    }
}

static void refresh_scan()
{
    int n = wifi_scan_results(scan_aps, 20);
    if (n == scan_shown) return;
    if (n < 0 && scan_shown == -1) return;
    scan_shown = n < 0 ? -1 : n;
    lv_obj_clean(scan_list);
    if (n < 0) {
        label(scan_list, &inter_20, DIM, TR("Szukam sieci...", "Searching for networks..."));
        return;
    }
    if (n == 0) label(scan_list, &inter_20, DIM, TR("Nie znaleziono sieci", "No networks found"));
    for (int i = 0; i < n; i++) {
        lv_obj_t* r = list_row(scan_list);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(r, HEX(22262B), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(r, on_scan_row, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        // Signal strength as icon brightness
        lv_color_t sig = scan_aps[i].rssi > -60 ? TEXT : scan_aps[i].rssi > -75 ? HEX(9A958D) : HEX(55524E);
        label(r, &icons_40, sig, ICON_WIFI);
        ssid_label(r, scan_aps[i].ssid, TEXT);
        if (!scan_aps[i].open) label(r, &icons_40, DIM, ICON_LOCK);
    }
}

static void refresh_wifi_status()
{
    if (join_watch) {
        WifiJoinState st = wifi_join_state();
        if (st == WIFI_JOIN_TRYING) return;  // keeps "Łączenie z ..."
        join_watch = false;
        if (st == WIFI_JOIN_FAILED) {
            lv_label_set_text_fmt(wifi_status, TR("Nie udało się połączyć z %s. Sprawdź hasło.", "Could not connect to %s. Check the password."), join_ssid);
            lv_obj_set_style_text_color(wifi_status, BAD, 0);
            join_failed = true;
            return;
        }
    }
    if (join_failed && !wifi_connected()) return;
    join_failed = false;
    char current[33];
    wifi_current(current);
    if (current[0]) {
        char ip[16];
        wifi_ip(ip);
        lv_label_set_text_fmt(wifi_status, TR("Połączono z %s\nStrona: tab5.local · %s", "Connected to %s\nPage: tab5.local · %s"), current, ip);
        lv_obj_set_style_text_color(wifi_status, GOOD, 0);
    } else {
        lv_label_set_text(wifi_status, TR("Brak połączenia", "Not connected"));
        lv_obj_set_style_text_color(wifi_status, DIM, 0);
    }
}

/* --------------------------------- Weather -------------------------------- */

static Place* found;  // [SEARCH_MAX] results shown as buttons, in PSRAM

static void on_place_pick(lv_event_t* e)
{
    const Place& p = found[(intptr_t)lv_event_get_user_data(e)];
    weather_set_place(p);
    lv_obj_clean(place_results);
    lv_label_set_text_fmt(postcode_msg, TR("Pogoda będzie dla: %s", "Weather for: %s"), p.name);
    lv_obj_set_style_text_color(postcode_msg, GOOD, 0);
    lv_textarea_set_text(postcode_ta, "");
    search_seen = SEARCH_IDLE;
    search_done = true;
    search_mine = false;
}

// One button per place: name, then region and country
static void show_results()
{
    lv_obj_clean(place_results);
    if (!found) found = (Place*)heap_caps_calloc(SEARCH_MAX, sizeof(Place), MALLOC_CAP_SPIRAM);
    int n = found ? weather_search_results(found, SEARCH_MAX) : 0;
    for (int i = 0; i < n; i++) {
        lv_obj_t* b = box(place_results);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(b, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(b, 14, 0);
        lv_obj_set_style_pad_hor(b, 20, 0);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_set_style_bg_color(b, HEX(22262B), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, HEX(2E333A), LV_STATE_PRESSED);
        column(b, 2);
        label(b, &inter_sb24, TEXT, found[i].name);
        char sub[112];
        snprintf(sub, sizeof(sub), "%s%s%s%s%s", found[i].postcode, found[i].postcode[0] ? " · " : "", found[i].region,
                 found[i].region[0] ? ", " : "", found[i].country);
        label(b, &inter_20, DIM, sub);
        lv_obj_add_event_cb(b, on_place_pick, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
}

static void refresh_location()
{
    char code[8], place[48];
    weather_location(code, place);
    lv_label_set_text(place_label, place);
    lv_label_set_text(postcode_label, code);

    bool online = wifi_connected();
    if (online) {
        lv_obj_remove_state(postcode_ta, LV_STATE_DISABLED);
        lv_obj_remove_state(postcode_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(postcode_ta, LV_STATE_DISABLED);
        lv_obj_add_state(postcode_btn, LV_STATE_DISABLED);
    }

    SearchState st = search_mine ? weather_search_state() : SEARCH_IDLE;
    if (st != search_seen) {
        search_seen = st;
        switch (st) {
            case SEARCH_LOOKING:
                lv_obj_clean(place_results);
                lv_label_set_text(postcode_msg, TR("Szukam miejscowości...", "Looking up the place..."));
                lv_obj_set_style_text_color(postcode_msg, ACCENT, 0);
                break;
            case SEARCH_OK:
                lv_label_set_text(postcode_msg, TR("Wybierz miejscowość:", "Choose the place:"));
                lv_obj_set_style_text_color(postcode_msg, DIM, 0);
                show_results();
                break;
            case SEARCH_NOT_FOUND:
                lv_label_set_text(postcode_msg, TR("Nic nie znaleziono — sprawdź nazwę", "Nothing found — check the name"));
                lv_obj_set_style_text_color(postcode_msg, BAD, 0);
                break;
            case SEARCH_ERROR:
                lv_label_set_text(postcode_msg, TR("Błąd połączenia, spróbuj ponownie", "Connection error, try again"));
                lv_obj_set_style_text_color(postcode_msg, BAD, 0);
                break;
            default:
                break;
        }
    }
    if (!online && st != SEARCH_LOOKING) {
        lv_label_set_text(postcode_msg, TR("Połącz się z Wi-Fi, aby zmienić lokalizację", "Connect to Wi-Fi to change the location"));
        lv_obj_set_style_text_color(postcode_msg, DIM, 0);
        search_seen = SEARCH_IDLE;
    } else if (online && st == SEARCH_IDLE && !search_done) {
        lv_label_set_text(postcode_msg, TR("Wpisz nazwę miejscowości albo polski kod pocztowy", "Enter a place name or a Polish postcode"));
        lv_obj_set_style_text_color(postcode_msg, DIM, 0);
    }
}

static void on_postcode_save(lv_event_t* e)
{
    LV_UNUSED(e);
    const char* q = lv_textarea_get_text(postcode_ta);
    while (*q == ' ') q++;
    if (strlen(q) < 2) {
        lv_label_set_text(postcode_msg, TR("Wpisz co najmniej 2 znaki", "Type at least 2 characters"));
        lv_obj_set_style_text_color(postcode_msg, BAD, 0);
        return;
    }
    keyboard_hide();
    lv_obj_remove_state(postcode_ta, LV_STATE_FOCUSED);
    search_seen = SEARCH_IDLE;
    search_done = false;
    search_mine = true;
    weather_search(q);
}

static void on_postcode_focus(lv_event_t* e)
{
    LV_UNUSED(e);
    keyboard_show(postcode_ta, LV_KEYBOARD_MODE_TEXT_LOWER);
}

static void on_keyboard(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* ta         = lv_keyboard_get_textarea(keyboard);
    if (code == LV_EVENT_READY) {
        if (ta == pass_ta) {
            on_pass_connect(e);
        } else if (ta == postcode_ta) {
            on_postcode_save(e);
        }
    } else if (code == LV_EVENT_CANCEL) {
        if (ta == pass_ta) {
            close_password();
        } else {
            keyboard_hide();
            if (ta) lv_obj_remove_state(ta, LV_STATE_FOCUSED);
        }
    }
}

/* ------------------------------ Panel settings ----------------------------- */
// The same settings as on the web page (config.h); both stay in sync because they read config() every tick.
// The menu keeps what is quick to set by touch; color presets, alarm names and volume live on the web page only.
// Every tab is 2 or 3 equal, full-height panels of "label ......... control" rows.

static const uint32_t COLORS[] = {0x5BE7E1, 0xF2F3F5, 0xFFC83D, 0xFF8A3D, 0xFF4B4B, 0xFF6FB5, 0xA98BFF, 0x4DA3FF, 0x5BE38A};
static const int NCOLORS       = sizeof(COLORS) / sizeof(COLORS[0]);
static const uint8_t CYCLES[]  = {0, 1, 2, 5, 10, 15, 30, 60};
static const int BODY_W        = 1280 - 2 * 24;  // page padding
static const int GAP           = 22;
static const int CONTROL_W     = 270;            // segments and dropdowns in the rows

static const int TABS = 6;
static lv_obj_t* tab_btn[TABS];
static lv_obj_t* tab_body[TABS];
static lv_obj_t* face_btn[3];
static lv_obj_t* mode_btn[2];
static lv_obj_t* digit_sw[NCOLORS];
static lv_obj_t* value_sw[NCOLORS + 1];  // [0] = "Jak cyfry"
static lv_obj_t* bright_slider;
static lv_obj_t* bright_value;
static lv_obj_t* night_roll[4];          // start hour, start minute, end hour, end minute
static lv_obj_t* off_roll[4];
static lv_obj_t* off_switch;
static lv_obj_t* cycle_dd;
static lv_obj_t* default_dd;
static lv_obj_t* confirm_overlay;
static lv_obj_t* confirm_text;
static lv_obj_t* confirm_ok;
static void (*confirm_action)(void);

// Settings changed from here: save them and repaint the clocks
static void changed()
{
    config_save();
    ui_config_changed();
}

static void select_button(lv_obj_t* b, bool on)
{
    lv_obj_set_style_bg_color(b, on ? ACCENT : HEX(22262B), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 0), on ? HEX(111111) : TEXT, 0);
}

// A panel taking 1/n of the tab width and its full height
static lv_obj_t* column_panel(lv_obj_t* body, const char* title, int n)
{
    lv_obj_t* p = panel(body, title, NULL);
    lv_obj_set_width(p, (BODY_W - (n - 1) * GAP) / n);
    return p;
}

// Segmented control: buttons in a rounded strip
static lv_obj_t* segment(lv_obj_t* parent, const char* const* names, int n, lv_obj_t** out, lv_event_cb_t cb)
{
    lv_obj_t* seg = box(parent);
    lv_obj_set_size(seg, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(seg, HEX(0E0F12), 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(seg, 18, 0);
    lv_obj_set_style_pad_all(seg, 5, 0);
    row(seg, 5);
    for (int i = 0; i < n; i++) {
        lv_obj_t* b = button(seg, names[i], HEX(22262B), TEXT, cb, (void*)(intptr_t)i);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_height(b, 54);
        lv_obj_set_style_pad_hor(b, 4, 0);
        out[i] = b;
    }
    return seg;
}

// Description text that wraps inside its panel
static lv_obj_t* note(lv_obj_t* parent, const char* text)
{
    lv_obj_t* l = label(parent, &inter_20, DIM, text);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

static lv_obj_t* caption(lv_obj_t* parent, const char* text)
{
    lv_obj_t* l = label(parent, &inter_20, DIM, text);
    lv_obj_set_style_margin_top(l, 6, 0);
    return l;
}

// Round color button; user data = the color
static lv_obj_t* swatch(lv_obj_t* parent, uint32_t color, lv_event_cb_t cb)
{
    lv_obj_t* s = box(parent);
    lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s, 4);
    lv_obj_set_size(s, 48, 48);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s, PANEL, 0);
    lv_obj_set_style_outline_color(s, TEXT, 0);
    lv_obj_set_style_outline_pad(s, 0, 0);
    lv_obj_add_event_cb(s, cb, LV_EVENT_CLICKED, (void*)(uintptr_t)color);
    return s;
}

static void mark_swatch(lv_obj_t* s, bool on)
{
    lv_obj_set_style_border_width(s, on ? 4 : 0, 0);
    lv_obj_set_style_outline_width(s, on ? 3 : 0, 0);
}

// A wrapping row of swatches
static lv_obj_t* swatch_row(lv_obj_t* parent)
{
    lv_obj_t* g = box(parent);
    lv_obj_set_size(g, lv_pct(100), LV_SIZE_CONTENT);
    row(g, 12);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(g, 12, 0);
    lv_obj_set_style_pad_all(g, 4, 0);  // room for the selection ring
    return g;
}

static void style_dropdown(lv_obj_t* dd)
{
    lv_obj_set_style_text_font(dd, &inter_30, 0);
    lv_obj_set_style_text_color(dd, TEXT, 0);
    lv_obj_set_style_bg_color(dd, HEX(0E0F12), 0);
    lv_obj_set_style_border_color(dd, LINE, 0);
    lv_obj_set_style_border_width(dd, 2, 0);
    lv_obj_set_style_radius(dd, 14, 0);
    lv_obj_set_style_pad_hor(dd, 18, 0);
    lv_obj_set_style_pad_ver(dd, 12, 0);
    lv_dropdown_set_symbol(dd, NULL);  // the arrow glyph is not in our fonts
    lv_obj_t* list = lv_dropdown_get_list(dd);
    lv_obj_set_style_text_font(list, &inter_30, 0);
    lv_obj_set_style_text_color(list, TEXT, 0);
    lv_obj_set_style_bg_color(list, PANEL, 0);
    lv_obj_set_style_border_color(list, LINE, 0);
    lv_obj_set_style_radius(list, 14, 0);
    lv_obj_set_style_bg_color(list, ACCENT, LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(list, HEX(111111), LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(list, HEX(2A2E33), LV_PART_SELECTED | LV_STATE_PRESSED);
}

static lv_obj_t* dropdown(lv_obj_t* parent, const char* options, int width, lv_event_cb_t cb)
{
    lv_obj_t* dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, options);
    lv_obj_set_width(dd, width);
    style_dropdown(dd);
    lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return dd;
}

static void style_switch(lv_obj_t* sw)
{
    lv_obj_set_size(sw, 84, 46);
    lv_obj_set_style_bg_color(sw, HEX(2A2E33), 0);
    lv_obj_set_style_bg_color(sw, ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, TEXT, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, HEX(111111), LV_PART_KNOB | LV_STATE_CHECKED);
}

// "Label ............ control" line; the control is added to the returned row
static lv_obj_t* setting_row(lv_obj_t* parent, const char* title, const char* sub)
{
    lv_obj_t* r = box(parent);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    row(r, 16);
    lv_obj_set_style_pad_ver(r, 6, 0);
    lv_obj_t* t = box(r);
    lv_obj_set_size(t, 1, LV_SIZE_CONTENT);  // takes the space left by the control (flex grow)
    lv_obj_set_flex_grow(t, 1);
    column(t, 2);
    lv_obj_t* l = label(t, &inter_30, TEXT, title);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    if (sub) note(t, sub);
    return r;
}

// Hour or minute roller
static lv_obj_t* time_roller(lv_obj_t* parent, bool minutes, int width, lv_event_cb_t cb, void* user)
{
    static char hours_opts[24 * 3 + 1], minutes_opts[60 * 3 + 1];
    if (!hours_opts[0]) {
        for (int i = 0; i < 24; i++) snprintf(hours_opts + i * 3, 4, "%02d\n", i);
        for (int i = 0; i < 60; i++) snprintf(minutes_opts + i * 3, 4, "%02d\n", i);
        hours_opts[24 * 3 - 1] = minutes_opts[60 * 3 - 1] = '\0';
    }
    lv_obj_t* ro = lv_roller_create(parent);
    lv_roller_set_options(ro, minutes ? minutes_opts : hours_opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(ro, 3);
    lv_obj_set_width(ro, width);
    lv_obj_set_style_text_font(ro, &inter_30, 0);
    lv_obj_set_style_text_color(ro, DIM, 0);
    lv_obj_set_style_text_align(ro, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(ro, HEX(0E0F12), 0);
    lv_obj_set_style_border_color(ro, LINE, 0);
    lv_obj_set_style_border_width(ro, 2, 0);
    lv_obj_set_style_radius(ro, 14, 0);
    lv_obj_set_style_bg_color(ro, HEX(2A2E33), LV_PART_SELECTED);
    lv_obj_set_style_text_color(ro, TEXT, LV_PART_SELECTED);
    lv_obj_set_style_text_font(ro, &inter_30, LV_PART_SELECTED);
    lv_obj_add_event_cb(ro, cb, LV_EVENT_VALUE_CHANGED, user);
    return ro;
}

static void ask(const char* text, const char* ok, bool danger, void (*action)(void))
{
    lv_label_set_text(confirm_text, text);
    lv_label_set_text(lv_obj_get_child(confirm_ok, 0), ok);
    lv_obj_set_style_bg_color(confirm_ok, danger ? BAD : ACCENT, 0);
    confirm_action = action;
    lv_obj_remove_flag(confirm_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(confirm_overlay);
}

static void on_confirm(lv_event_t* e)
{
    bool ok = lv_event_get_user_data(e) != NULL;
    lv_obj_add_flag(confirm_overlay, LV_OBJ_FLAG_HIDDEN);
    if (ok && confirm_action) confirm_action();
}

/* ---------------------------------- Clock --------------------------------- */

static void on_face(lv_event_t* e)
{
    ui_show_face((intptr_t)lv_event_get_user_data(e));
}

static void on_mode(lv_event_t* e)
{
    ui_set_night((intptr_t)lv_event_get_user_data(e) == 1);
}

static void on_digit_color(lv_event_t* e)
{
    config().digit_color = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    changed();
}

static void on_value_color(lv_event_t* e)
{
    uint32_t c = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    config().sensor_follow = c == 0xFFFFFFFF;
    if (c != 0xFFFFFFFF) config().sensor_color = c;
    changed();
}

static void on_cycle(lv_event_t* e)
{
    LV_UNUSED(e);
    config().cycle_min = CYCLES[lv_dropdown_get_selected(cycle_dd)];
    changed();
}

static void on_default_face(lv_event_t* e)
{
    LV_UNUSED(e);
    config().default_face = lv_dropdown_get_selected(default_dd);
    changed();
}

static void build_clock_tab(lv_obj_t* body)
{
    static const char* const faces[] = {"1", "2", "3"};
    static const char* const modes[] = {TR("Jasny", "Light"), TR("Ciemny", "Dark")};
    lv_obj_t* p = column_panel(body, TR("Wygląd", "Look"), 2);
    lv_obj_t* r = setting_row(p, TR("Wariant", "Theme"), NULL);
    lv_obj_set_width(segment(r, faces, 3, face_btn, on_face), CONTROL_W);
    r = setting_row(p, TR("Tryb", "Mode"), NULL);
    lv_obj_set_width(segment(r, modes, 2, mode_btn, on_mode), CONTROL_W);
    r        = setting_row(p, TR("Zmiana wariantu", "Theme rotation"), TR("Gdy nikt nie dotyka ekranu", "When nobody touches the screen"));
    cycle_dd = dropdown(r, TR("wyłączona\nco 1 min\nco 2 min\nco 5 min\nco 10 min\nco 15 min\nco 30 min\nco godzinę", "off\nevery 1 min\nevery 2 min\nevery 5 min\nevery 10 min\nevery 15 min\nevery 30 min\nevery hour"),
                        CONTROL_W, on_cycle);
    r          = setting_row(p, TR("Wariant po starcie", "Theme at start"), NULL);
    default_dd = dropdown(r, TR("Wariant 1\nWariant 2\nWariant 3", "Theme 1\nTheme 2\nTheme 3"), CONTROL_W, on_default_face);

    p = column_panel(body, TR("Kolory", "Colors"), 2);
    caption(p, TR("Kolor cyfr", "Digit color"));
    lv_obj_t* g = swatch_row(p);
    for (int i = 0; i < NCOLORS; i++) digit_sw[i] = swatch(g, COLORS[i], on_digit_color);
    caption(p, TR("Kolor wartości sensorów", "Sensor value color"));
    value_sw[0] = button(p, TR("Jak cyfry", "Like digits"), HEX(22262B), TEXT, on_value_color, (void*)(uintptr_t)0xFFFFFFFF);
    lv_obj_set_height(value_sw[0], 52);
    g = swatch_row(p);
    for (int i = 0; i < NCOLORS; i++) value_sw[i + 1] = swatch(g, COLORS[i], on_value_color);
    note(p, TR("Presety kolorów zapiszesz na stronie www Tab5.", "Color presets are saved on the Tab5 web page."));
}

/* --------------------------------- Screen --------------------------------- */

static void on_bright(lv_event_t* e)
{
    int v = lv_slider_get_value(bright_slider);
    lv_label_set_text_fmt(bright_value, "%d%%", v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        config().brightness = v;
        changed();
    }
}

static void on_screen_off(lv_event_t* e)
{
    LV_UNUSED(e);
    ui_set_screen(false);
}

static void on_intro(lv_event_t* e)
{
    LV_UNUSED(e);
    ui_play_intro();
}

static void on_time(lv_event_t* e)
{
    lv_obj_t** r   = (lv_obj_t**)lv_event_get_user_data(e);
    PanelConfig& c = config();
    uint16_t start = lv_roller_get_selected(r[0]) * 60 + lv_roller_get_selected(r[1]);
    uint16_t end   = lv_roller_get_selected(r[2]) * 60 + lv_roller_get_selected(r[3]);
    if (r == night_roll) c.night_start = start, c.night_end = end;
    else c.off_start = start, c.off_end = end;
    changed();
}

// "Od hh:mm" and "Do hh:mm" lines with four rollers; `r` receives them
static void time_range(lv_obj_t* parent, lv_obj_t** r)
{
    for (int line = 0; line < 2; line++) {
        lv_obj_t* l = box(parent);
        lv_obj_set_size(l, lv_pct(100), LV_SIZE_CONTENT);
        row(l, 10);
        lv_obj_t* name = label(l, &inter_30, DIM, line ? TR("Do", "To") : TR("Od", "From"));
        lv_obj_set_width(name, 64);
        r[line * 2] = time_roller(l, false, 96, on_time, r);
        label(l, &inter_sb30, DIM, ":");
        r[line * 2 + 1] = time_roller(l, true, 96, on_time, r);
    }
}

static void on_off_switch(lv_event_t* e)
{
    LV_UNUSED(e);
    config().off_enabled = lv_obj_has_state(off_switch, LV_STATE_CHECKED);
    changed();
}

static void build_screen_tab(lv_obj_t* body)
{
    lv_obj_t* p  = column_panel(body, TR("Ekran", "Screen"), 3);
    lv_obj_t* r  = setting_row(p, TR("Jasność", "Brightness"), TR("W trybie jasnym", "In light mode"));
    bright_value = label(r, &inter_sb30, TEXT, "");
    bright_slider = lv_slider_create(p);
    lv_obj_set_size(bright_slider, lv_pct(100), 16);
    lv_obj_set_style_margin_ver(bright_slider, 18, 0);
    lv_obj_set_style_margin_hor(bright_slider, 14, 0);
    lv_slider_set_range(bright_slider, 10, 100);
    lv_obj_set_style_bg_color(bright_slider, HEX(2A2E33), 0);
    lv_obj_set_style_bg_opa(bright_slider, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bright_slider, ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bright_slider, ACCENT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(bright_slider, 10, LV_PART_KNOB);
    lv_obj_add_event_cb(bright_slider, on_bright, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(bright_slider, on_bright, LV_EVENT_RELEASED, NULL);
    lv_obj_t* b = button(p, TR("Wyłącz ekran", "Screen off"), HEX(2A2E33), TEXT, on_screen_off, NULL);
    lv_obj_set_width(b, lv_pct(100));
    b = button(p, TR("Pokaż intro", "Show intro"), HEX(2A2E33), TEXT, on_intro, NULL);
    lv_obj_set_width(b, lv_pct(100));
    note(p, TR("Dotknięcie wygaszonego ekranu zapala go na minutę.", "Touching the dark screen lights it for a minute."));

    p = column_panel(body, TR("Tryb nocny", "Night mode"), 3);
    note(p, TR("W tych godzinach zegar jest ciemny.", "During these hours the clock is dark."));
    time_range(p, night_roll);

    p          = column_panel(body, TR("Wygaszanie", "Screen off"), 3);
    r          = setting_row(p, TR("Włączone", "Enabled"), NULL);
    off_switch = lv_switch_create(r);
    style_switch(off_switch);
    lv_obj_add_event_cb(off_switch, on_off_switch, LV_EVENT_VALUE_CHANGED, NULL);
    time_range(p, off_roll);
    note(p, TR("Ekran zgaszony całkiem; dotknięcie budzi go na minutę.", "The screen is fully off; a touch wakes it for a minute."));
}

/* --------------------------------- Alarms --------------------------------- */
// List on the left (tap = edit), the chosen alarm in the middle (time, days, on / off, delete), snooze on the right.
// Names and volume are set on the web page.

static void set_checked(lv_obj_t* o, bool on);
static void set_roller(lv_obj_t* r, int v);
static lv_obj_t* alarm_list;
static lv_obj_t* alarm_add_btn;
static lv_obj_t *snooze_dd, *timer_snooze_dd;
static lv_obj_t *edit_box, *edit_empty, *edit_title, *edit_roll[2], *edit_day[7], *edit_switch;
static int edit_idx = -1;  // alarm shown in the editor

// Snooze lengths: options and their minutes
static const char* const SNOOZE_OPTS       = "5 min\n10 min\n15 min\n20 min\n30 min";
static const int SNOOZE_MIN[]              = {5, 10, 15, 20, 30};
static const char* const TIMER_SNOOZE_OPTS = "1 min\n2 min\n3 min\n5 min\n10 min";
static const int TIMER_SNOOZE_MIN[]        = {1, 2, 3, 5, 10};

static void select_minutes(lv_obj_t* dd, const int* mins, int n, int value)
{
    for (int k = 0; k < n; k++)
        if (mins[k] == value) lv_dropdown_set_selected(dd, k);
}

static char alarms_key[ALARM_MAX * 8 + 8];  // what the list shows, to rebuild it only when it changes

static const char* day_short(int k)
{
    static const char* const pl[] = {"Pn", "Wt", "Śr", "Cz", "Pt", "So", "Nd"};
    static const char* const en[] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
    return (g_lang_en ? en : pl)[k];
}

static void days_text(uint8_t days, char* out, size_t n)
{
    out[0] = '\0';
    if (!days) strlcpy(out, TR("jednorazowo", "once"), n);
    else if (days == 0x7F) strlcpy(out, TR("codziennie", "every day"), n);
    else if (days == 0x1F) strlcpy(out, TR("w dni robocze", "on weekdays"), n);
    else if (days == 0x60) strlcpy(out, TR("w weekend", "at the weekend"), n);
    else
        for (int k = 0; k < 7; k++)
            if (days & (1 << k)) {
                if (out[0]) strlcat(out, " ", n);
                strlcat(out, day_short(k), n);
            }
}

// The editor shows alarm edit_idx (or the hint when there is none)
static void fill_editor()
{
    bool on = edit_idx >= 0 && edit_idx < alarm_count();
    if (on) lv_obj_remove_flag(edit_box, LV_OBJ_FLAG_HIDDEN), lv_obj_add_flag(edit_empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(edit_box, LV_OBJ_FLAG_HIDDEN), lv_obj_remove_flag(edit_empty, LV_OBJ_FLAG_HIDDEN);
    if (!on) return;
    Alarm a = alarm_get(edit_idx);
    if (a.label[0]) lv_label_set_text(edit_title, a.label);
    else lv_label_set_text_fmt(edit_title, TR("Alarm %d", "Alarm %d"), edit_idx + 1);
    set_roller(edit_roll[0], a.hour);
    set_roller(edit_roll[1], a.minute);
    for (int k = 0; k < 7; k++) select_button(edit_day[k], a.days & (1 << k));
    set_checked(edit_switch, a.enabled);
}

static void edit_alarm(int i)
{
    edit_idx      = i;
    alarms_key[0] = '\0';  // redraw the list with the new selection
}

static void on_alarm_row(lv_event_t* e)
{
    edit_alarm((intptr_t)lv_event_get_user_data(e));
}

static void on_alarm_switch(lv_event_t* e)
{
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    alarm_set_enabled((intptr_t)lv_event_get_user_data(e), lv_obj_has_state(sw, LV_STATE_CHECKED));
    alarms_key[0] = '\0';
}

static void on_edit_time(lv_event_t* e)
{
    LV_UNUSED(e);
    if (edit_idx < 0 || edit_idx >= alarm_count()) return;
    Alarm a  = alarm_get(edit_idx);
    a.hour   = lv_roller_get_selected(edit_roll[0]);
    a.minute = lv_roller_get_selected(edit_roll[1]);
    alarm_set(edit_idx, a);
    alarms_key[0] = '\0';
}

static void on_edit_day(lv_event_t* e)
{
    if (edit_idx < 0 || edit_idx >= alarm_count()) return;
    Alarm a = alarm_get(edit_idx);
    a.days ^= 1 << (intptr_t)lv_event_get_user_data(e);
    alarm_set(edit_idx, a);
    alarms_key[0] = '\0';
}

static void on_edit_switch(lv_event_t* e)
{
    LV_UNUSED(e);
    if (edit_idx < 0 || edit_idx >= alarm_count()) return;
    alarm_set_enabled(edit_idx, lv_obj_has_state(edit_switch, LV_STATE_CHECKED));
    alarms_key[0] = '\0';
}

static void delete_alarm()
{
    if (edit_idx < 0 || edit_idx >= alarm_count()) return;
    alarm_delete(edit_idx);
    edit_alarm(edit_idx < alarm_count() ? edit_idx : alarm_count() - 1);
}

static void on_edit_delete(lv_event_t* e)
{
    LV_UNUSED(e);
    ask(TR("Usunąć ten alarm?", "Delete this alarm?"), TR("Usuń", "Delete"), true, delete_alarm);
}

static void on_alarm_add(lv_event_t* e)
{
    LV_UNUSED(e);
    Alarm a   = {};
    a.enabled = 1;
    a.hour    = 7;
    a.days    = 0x1F;
    a.volume  = 80;
    int i     = alarm_add(a);
    if (i >= 0) edit_alarm(i);
}

static void refresh_alarms()
{
    if (!alarm_list) return;
    if (!lv_dropdown_is_open(snooze_dd)) select_minutes(snooze_dd, SNOOZE_MIN, 5, alarm_snooze_min(false));
    if (!lv_dropdown_is_open(timer_snooze_dd)) select_minutes(timer_snooze_dd, TIMER_SNOOZE_MIN, 5, alarm_snooze_min(true));
    int n = alarm_count();
    if (edit_idx >= n) edit_idx = n - 1;
    if (edit_idx < 0 && n) edit_idx = 0;
    char key[sizeof(alarms_key)];
    int len = snprintf(key, sizeof(key), "%d:%d|", n, edit_idx);
    for (int i = 0; i < n && len < (int)sizeof(key) - 8; i++) {
        Alarm a = alarm_get(i);
        len += snprintf(key + len, sizeof(key) - len, "%c%02x%02x%02x", a.enabled ? '1' : '0', a.hour, a.minute, a.days);
    }
    if (n >= ALARM_MAX) lv_obj_add_state(alarm_add_btn, LV_STATE_DISABLED);
    else lv_obj_remove_state(alarm_add_btn, LV_STATE_DISABLED);
    if (!strcmp(key, alarms_key)) return;
    strcpy(alarms_key, key);
    fill_editor();
    lv_obj_clean(alarm_list);
    if (!n) {
        note(alarm_list, TR("Brak alarmów. Dodaj pierwszy przyciskiem poniżej.", "No alarms. Add the first one with the button below."));
        return;
    }
    for (int i = 0; i < n; i++) {
        Alarm a = alarm_get(i);
        char title[40], sub[40];
        snprintf(title, sizeof(title), "%02d:%02d%s%s", a.hour, a.minute, a.label[0] ? "  " : "", a.label);
        days_text(a.days, sub, sizeof(sub));
        lv_obj_t* r = setting_row(alarm_list, title, sub);
        lv_obj_set_style_pad_hor(r, 14, 0);
        lv_obj_set_style_radius(r, 14, 0);
        lv_obj_set_style_bg_color(r, HEX(22262B), 0);
        lv_obj_set_style_bg_opa(r, i == edit_idx ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, on_alarm_row, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_t* sw = lv_switch_create(r);
        style_switch(sw);
        set_checked(sw, a.enabled);
        lv_obj_add_event_cb(sw, on_alarm_switch, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)i);
    }
}

static void on_snooze(lv_event_t* e)
{
    bool timer = (bool)(intptr_t)lv_event_get_user_data(e);
    int k      = lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e));
    alarm_set_snooze_min(timer, timer ? TIMER_SNOOZE_MIN[k] : SNOOZE_MIN[k]);
}

static void build_alarm_tab(lv_obj_t* body)
{
    lv_obj_t* p = column_panel(body, TR("Alarmy", "Alarms"), 3);
    alarm_list  = box(p);
    lv_obj_set_width(alarm_list, lv_pct(100));
    lv_obj_set_flex_grow(alarm_list, 1);
    lv_obj_add_flag(alarm_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(alarm_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(alarm_list, LV_SCROLLBAR_MODE_OFF);
    column(alarm_list, 4);
    alarm_add_btn = button(p, TR("+ Dodaj alarm", "+ Add alarm"), ACCENT, HEX(111111), on_alarm_add, NULL);
    lv_obj_set_width(alarm_add_btn, lv_pct(100));

    // Editor of the chosen alarm
    p          = column_panel(body, TR("Edycja", "Edit"), 3);
    edit_empty = note(p, TR("Wybierz alarm z listy albo dodaj nowy.", "Pick an alarm from the list or add one."));
    edit_box   = box(p);
    lv_obj_set_width(edit_box, lv_pct(100));
    lv_obj_set_flex_grow(edit_box, 1);
    column(edit_box, 16);
    edit_title = label(edit_box, &inter_30, TEXT, "");
    lv_label_set_long_mode(edit_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(edit_title, lv_pct(100));
    lv_obj_t* t = box(edit_box);
    lv_obj_set_size(t, lv_pct(100), LV_SIZE_CONTENT);
    row(t, 10);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    edit_roll[0] = time_roller(t, false, 120, on_edit_time, NULL);
    label(t, &inter_sb30, DIM, ":");
    edit_roll[1] = time_roller(t, true, 120, on_edit_time, NULL);
    note(edit_box, TR("Dni tygodnia; bez zaznaczonych dni alarm zadzwoni raz.", "Weekdays; with none selected the alarm rings once."));
    lv_obj_t* d = box(edit_box);
    lv_obj_set_size(d, lv_pct(100), LV_SIZE_CONTENT);
    row(d, 6);
    for (int k = 0; k < 7; k++) {
        edit_day[k] = button(d, day_short(k), HEX(22262B), TEXT, on_edit_day, (void*)(intptr_t)k);
        lv_obj_set_flex_grow(edit_day[k], 1);
        lv_obj_set_height(edit_day[k], 52);
        lv_obj_set_style_pad_hor(edit_day[k], 0, 0);
    }
    lv_obj_t* r = setting_row(edit_box, TR("Włączony", "On"), NULL);
    edit_switch = lv_switch_create(r);
    style_switch(edit_switch);
    lv_obj_add_event_cb(edit_switch, on_edit_switch, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t* del = button(edit_box, TR("Usuń alarm", "Delete alarm"), HEX(2A2E33), BAD, on_edit_delete, NULL);
    lv_obj_set_width(del, lv_pct(100));

    p = column_panel(body, TR("Drzemka", "Snooze"), 3);
    for (int t2 = 0; t2 < 2; t2++) {
        r            = setting_row(p, t2 ? TR("Minutnik", "Timer") : TR("Alarmy", "Alarms"), NULL);
        lv_obj_t* dd = lv_dropdown_create(r);
        lv_dropdown_set_options(dd, t2 ? TIMER_SNOOZE_OPTS : SNOOZE_OPTS);
        lv_obj_set_width(dd, 160);
        style_dropdown(dd);
        lv_obj_add_event_cb(dd, on_snooze, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)t2);
        (t2 ? timer_snooze_dd : snooze_dd) = dd;
    }
    // plain quotes only: the bitmap fonts have no typographic quotes or dashes
    note(p, TR("Po ilu minutach alarm albo minutnik zadzwoni znowu po drzemce. Nazwy i głośność alarmów zmienisz na stronie www Tab5.",
               "After how many minutes the alarm or the timer rings again after a snooze. Alarm names and volume are set on the Tab5 web page."));
}

/* ---------------------------------- Other --------------------------------- */

static void do_restart()
{
    esp_restart();
}

// Device language: saved, then a restart builds everything in it
static lv_obj_t* lang_dd;
static uint8_t lang_pending;

static void do_lang()
{
    config().lang = lang_pending;
    config_save();
    esp_restart();
}

static void on_lang(lv_event_t* e)
{
    lang_pending = lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e));
    if (lang_pending == config().lang) return;
    ask(TR("Tab5 uruchomi się ponownie w nowym języku, potrwa to około 10 sekund.", "The Tab5 will restart in the new language, this takes about 10 seconds."),
        TR("Zmień język", "Change language"), false, do_lang);
}

static void on_restart(lv_event_t* e)
{
    LV_UNUSED(e);
    ask(TR("Tab5 uruchomi się ponownie, potrwa to około 10 sekund.", "The Tab5 will restart, this takes about 10 seconds."), TR("Restartuj", "Restart"), false, do_restart);
}

static void build_other_tab(lv_obj_t* body)
{
    lv_obj_t* p = column_panel(body, TR("Urządzenie", "Device"), 2);
    lv_obj_t* r = setting_row(p, TR("Język", "Language"), TR("Menu, daty i przyciski; Tab5 uruchomi się ponownie", "Menu, dates and buttons; the Tab5 restarts"));
    lang_dd     = lv_dropdown_create(r);
    lv_dropdown_set_options(lang_dd, "Polski\nEnglish");
    lv_obj_set_width(lang_dd, 200);
    style_dropdown(lang_dd);
    lv_obj_add_event_cb(lang_dd, on_lang, LV_EVENT_VALUE_CHANGED, NULL);
    r           = setting_row(p, "Restart", TR("Ustawienia zostają", "Settings are kept"));
    lv_obj_t* b = button(r, "Restart", HEX(2A2E33), BAD, on_restart, NULL);
    lv_obj_set_width(b, 200);

    // Which program runs: the date and time of its Studio build, as in the Studio build list
    p            = column_panel(body, TR("Wersja programu", "Program version"), 2);
    lv_obj_t* ver = label(p, &inter_m64, ACCENT, STUDIO_BUILD_NAME);
    lv_obj_set_width(ver, lv_pct(100));
    lv_label_set_long_mode(ver, LV_LABEL_LONG_WRAP);
    if (STUDIO_BUILD_NOTE[0]) {
        lv_obj_t* what = label(p, &inter_30, TEXT, STUDIO_BUILD_NOTE);
        lv_obj_set_width(what, lv_pct(100));
        lv_label_set_long_mode(what, LV_LABEL_LONG_WRAP);
    }
    note(p, TR("Data i godzina kompilacji w Tab5 Studio, ta sama co na liście kompilacji.", "Date and time of the Tab5 Studio build, as in the build list."));
}

/* ------------------------------- Tabs & sync ------------------------------- */

static void show_tab(int k)
{
    for (int i = 0; i < TABS; i++) {
        select_button(tab_btn[i], i == k);
        if (i == k) lv_obj_remove_flag(tab_body[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(tab_body[i], LV_OBJ_FLAG_HIDDEN);
    }
    keyboard_hide();
}

static void on_tab(lv_event_t* e)
{
    show_tab((intptr_t)lv_event_get_user_data(e));
}

static void set_checked(lv_obj_t* o, bool on)
{
    if (on) lv_obj_add_state(o, LV_STATE_CHECKED);
    else lv_obj_remove_state(o, LV_STATE_CHECKED);
}

// Sets a roller unless it is being dragged
static void set_roller(lv_obj_t* r, int v)
{
    if (!lv_obj_has_state(r, LV_STATE_PRESSED) && (int)lv_roller_get_selected(r) != v) lv_roller_set_selected(r, v, LV_ANIM_OFF);
}

// Shows the current settings (they may also change from the web page)
static void refresh_panel()
{
    refresh_alarms();
    if (lang_dd && !lv_dropdown_is_open(lang_dd)) lv_dropdown_set_selected(lang_dd, config().lang);  // also undoes a cancelled change
    const PanelConfig& c = config();
    int face             = ui_face();
    for (int i = 0; i < 3; i++) select_button(face_btn[i], i == face);
    if (ui_face_count() == 3) lv_obj_remove_flag(face_btn[2], LV_OBJ_FLAG_HIDDEN);  // only with Wariant-3 from Studio
    else lv_obj_add_flag(face_btn[2], LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 2; i++) select_button(mode_btn[i], (i == 1) == ui_night());
    for (int i = 0; i < NCOLORS; i++) mark_swatch(digit_sw[i], COLORS[i] == c.digit_color);
    select_button(value_sw[0], c.sensor_follow);
    for (int i = 0; i < NCOLORS; i++) mark_swatch(value_sw[i + 1], !c.sensor_follow && COLORS[i] == c.sensor_color);

    if (!lv_obj_has_state(bright_slider, LV_STATE_PRESSED)) {
        lv_slider_set_value(bright_slider, c.brightness, LV_ANIM_OFF);
        lv_label_set_text_fmt(bright_value, "%d%%", c.brightness);
    }

    set_roller(night_roll[0], c.night_start / 60);
    set_roller(night_roll[1], c.night_start % 60);
    set_roller(night_roll[2], c.night_end / 60);
    set_roller(night_roll[3], c.night_end % 60);
    set_roller(off_roll[0], c.off_start / 60);
    set_roller(off_roll[1], c.off_start % 60);
    set_roller(off_roll[2], c.off_end / 60);
    set_roller(off_roll[3], c.off_end % 60);
    set_checked(off_switch, c.off_enabled);

    int cyc = 0;
    for (int i = 0; i < (int)sizeof(CYCLES); i++)
        if (CYCLES[i] == c.cycle_min) cyc = i;
    if (!lv_dropdown_is_open(cycle_dd) && (int)lv_dropdown_get_selected(cycle_dd) != cyc) lv_dropdown_set_selected(cycle_dd, cyc);
    if (!lv_dropdown_is_open(default_dd) && (int)lv_dropdown_get_selected(default_dd) != c.default_face)
        lv_dropdown_set_selected(default_dd, c.default_face);
}

// Yes / no dialog over everything
static void build_dialogs()
{
    lv_obj_t* ov = box(scr);
    lv_obj_set_size(ov, 1280, 720);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_80, 0);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* dlg = box(ov);
    lv_obj_set_size(dlg, 680, LV_SIZE_CONTENT);
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, PANEL, 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dlg, 24, 0);
    lv_obj_set_style_pad_all(dlg, 28, 0);
    column(dlg, 18);
    confirm_overlay = ov;
    confirm_text    = label(dlg, &inter_sb30, TEXT, "");
    lv_obj_set_width(confirm_text, lv_pct(100));
    lv_label_set_long_mode(confirm_text, LV_LABEL_LONG_WRAP);
    lv_obj_t* btns = box(dlg);
    lv_obj_set_size(btns, lv_pct(100), LV_SIZE_CONTENT);
    row(btns, 16);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    button(btns, TR("Anuluj", "Cancel"), HEX(2A2E33), TEXT, on_confirm, NULL);
    confirm_ok = button(btns, "OK", ACCENT, HEX(111111), on_confirm, (void*)1);
}

/* --------------------------------- Screen --------------------------------- */

static void tick(lv_timer_t* t)
{
    LV_UNUSED(t);
    if (lv_screen_active() != scr) return;
    refresh_panel();
    refresh_wifi_status();
    refresh_saved();
    refresh_scan();
    refresh_location();
}

static void on_back(lv_event_t* e)
{
    LV_UNUSED(e);
    close_password();
    lv_obj_add_flag(confirm_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_state(postcode_ta, LV_STATE_FOCUSED);
    if (s_on_back) s_on_back();
}

void settings_build(void (*back)(void))
{
    s_on_back = back;
    scr       = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* page = box(scr);
    lv_obj_set_size(page, 1280, 720);
    lv_obj_set_style_pad_all(page, 24, 0);
    column(page, 20);

    // Header: back, a gear icon (settings) and the tabs, all 72 px high like the timer header
    lv_obj_t* head = box(page);
    lv_obj_set_size(head, lv_pct(100), 72);
    row(head, 8);
    // Back: arrow and text in one big button (easier to hit than the arrow alone), like the timer screen
    lv_obj_t* back_btn = box(head);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(back_btn, 200, 72);
    lv_obj_set_style_radius(back_btn, 16, 0);
    lv_obj_set_style_bg_color(back_btn, HEX(1A1E23), 0);
    lv_obj_set_style_bg_color(back_btn, LINE, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(back_btn, LINE, 0);
    lv_obj_set_style_border_width(back_btn, 2, 0);
    lv_obj_set_style_pad_right(back_btn, 10, 0);
    row(back_btn, 6);
    lv_obj_set_flex_align(back_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(back_btn, &icons_40, TEXT, ICON_BACK);
    label(back_btn, &inter_sb30, TEXT, TR("Powrót", "Back"));
    lv_obj_add_event_cb(back_btn, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t* title = label(head, layout_font_fixed("icons", 40), DIM, ICON_SETTINGS);
    lv_obj_set_style_margin_left(title, 14, 0);
    lv_obj_set_flex_grow(title, 1);
    // Tabs
    static const char* const tabs[] = {"Wi-Fi", TR("Pogoda", "Weather"), TR("Zegar", "Clock"), TR("Ekran", "Screen"),
                                       TR("Budzik", "Alarms"), TR("Inne", "Other")};
    lv_obj_t* tabbar = segment(head, tabs, TABS, tab_btn, on_tab);
    lv_obj_set_width(tabbar, LV_SIZE_CONTENT);
    for (lv_obj_t* b : tab_btn) {
        lv_obj_set_flex_grow(b, 0);
        lv_obj_set_height(b, 62);
        lv_obj_set_style_pad_hor(b, 14, 0);
        lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &inter_sb28, 0);
    }
    // Outside the row layout, pinned to the right edge: LVGL re-aligns it whenever its width changes
    lv_obj_add_flag(tabbar, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(tabbar, LV_ALIGN_RIGHT_MID, 0, 0);
    for (int i = 0; i < TABS; i++) {
        lv_obj_t* b = tab_body[i] = box(page);
        lv_obj_set_width(b, lv_pct(100));
        lv_obj_set_flex_grow(b, 1);
        row(b, 22);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    }
    lv_obj_t* body = tab_body[0];
    build_clock_tab(tab_body[2]);
    build_screen_tab(tab_body[3]);
    build_alarm_tab(tab_body[4]);
    build_other_tab(tab_body[5]);

    // Wi-Fi: saved networks and available networks, half of the tab each
    lv_obj_t* p1 = panel(body, "", &saved_title);
    lv_obj_set_width(p1, (BODY_W - GAP) / 2);
    wifi_status  = label(p1, &inter_20, DIM, "");
    lv_obj_set_width(wifi_status, lv_pct(100));
    lv_label_set_long_mode(wifi_status, LV_LABEL_LONG_WRAP);
    saved_list = box(p1);
    lv_obj_set_width(saved_list, lv_pct(100));
    lv_obj_set_flex_grow(saved_list, 1);
    column(saved_list, 0);

    // Available networks
    lv_obj_t* p2 = panel(body, "", NULL);
    lv_obj_set_width(p2, (BODY_W - GAP) / 2);
    lv_obj_t* t2 = lv_obj_get_child(p2, 0);
    lv_obj_delete(t2);
    lv_obj_t* h2 = box(p2);
    lv_obj_set_size(h2, lv_pct(100), LV_SIZE_CONTENT);
    row(h2, 0);
    lv_obj_t* h2t = label(h2, &inter_sb30, TEXT, TR("Dostępne sieci", "Available networks"));
    lv_obj_set_flex_grow(h2t, 1);
    icon_button(h2, ICON_REFRESH, DIM, on_rescan, NULL);
    lv_obj_set_style_margin_ver(h2, -10, 0);
    scan_list = box(p2);
    lv_obj_set_width(scan_list, lv_pct(100));
    lv_obj_set_flex_grow(scan_list, 1);
    lv_obj_add_flag(scan_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(scan_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scan_list, LV_SCROLLBAR_MODE_OFF);
    column(scan_list, 0);

    // Weather tab: the place and the search on the left, the places found on the right
    lv_obj_t* p3 = column_panel(tab_body[1], TR("Miejscowość", "Place"), 2);
    lv_obj_t* loc = box(p3);
    lv_obj_set_size(loc, lv_pct(100), LV_SIZE_CONTENT);
    row(loc, 12);
    label(loc, &icons_40, ACCENT, ICON_PIN);
    lv_obj_t* names = box(loc);
    lv_obj_set_size(names, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(names, 1);
    column(names, 0);
    place_label    = label(names, &inter_30, TEXT, "");
    postcode_label = label(names, &inter_20, DIM, "");
    lv_label_set_long_mode(place_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(place_label, lv_pct(100));

    label(p3, &inter_20, DIM, TR("Miejscowość lub kod pocztowy", "Place or Polish postcode"));
    lv_obj_t* form = box(p3);
    lv_obj_set_size(form, lv_pct(100), LV_SIZE_CONTENT);
    row(form, 12);
    lv_obj_set_style_margin_top(form, -10, 0);
    postcode_ta = lv_textarea_create(form);
    lv_textarea_set_one_line(postcode_ta, true);
    lv_textarea_set_max_length(postcode_ta, 40);
    lv_textarea_set_placeholder_text(postcode_ta, TR("np. Warszawa", "e.g. Berlin"));
    lv_obj_set_flex_grow(postcode_ta, 1);
    style_textarea(postcode_ta, &inter_30);
    lv_obj_add_event_cb(postcode_ta, on_postcode_focus, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(postcode_ta, on_postcode_focus, LV_EVENT_CLICKED, NULL);
    postcode_btn = button(form, TR("Szukaj", "Search"), ACCENT, HEX(111111), on_postcode_save, NULL);
    postcode_msg = label(p3, &inter_20, DIM, "");
    lv_obj_set_width(postcode_msg, lv_pct(100));
    lv_label_set_long_mode(postcode_msg, LV_LABEL_LONG_WRAP);
    lv_obj_t* p4 = column_panel(tab_body[1], TR("Znalezione", "Found"), 2);
    note(p4, TR("Wpisz miejscowość albo kod pocztowy i dotknij Szukaj, potem wybierz miejsce z listy.",
                "Type a place or a Polish postcode, tap Search, then pick the place from the list."));
    place_results = box(p4);
    lv_obj_set_width(place_results, lv_pct(100));
    lv_obj_set_flex_grow(place_results, 1);
    lv_obj_add_flag(place_results, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(place_results, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(place_results, LV_SCROLLBAR_MODE_OFF);
    column(place_results, 8);

    build_dialogs();

    // Password dialog over everything, keyboard below it
    overlay = box(scr);
    lv_obj_set_size(overlay, 1280, 720);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);  // blocks taps on the screen behind
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_80, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* dlg = box(overlay);
    lv_obj_set_size(dlg, 760, LV_SIZE_CONTENT);
    lv_obj_align(dlg, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(dlg, PANEL, 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dlg, 24, 0);
    lv_obj_set_style_pad_all(dlg, 28, 0);
    column(dlg, 18);
    pass_title = label(dlg, &inter_sb30, TEXT, "");
    lv_label_set_long_mode(pass_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(pass_title, lv_pct(100));
    pass_ta = lv_textarea_create(dlg);
    lv_textarea_set_one_line(pass_ta, true);
    lv_textarea_set_password_mode(pass_ta, true);
    lv_textarea_set_password_bullet(pass_ta, "*");
    lv_textarea_set_password_show_time(pass_ta, 1200);
    lv_textarea_set_max_length(pass_ta, 64);
    lv_textarea_set_placeholder_text(pass_ta, TR("hasło", "password"));
    lv_obj_set_width(pass_ta, lv_pct(100));
    style_textarea(pass_ta, &kb_30);  // ASCII + keyboard symbols
    lv_obj_set_style_text_font(pass_ta, &inter_30, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_t* btns = box(dlg);
    lv_obj_set_size(btns, lv_pct(100), LV_SIZE_CONTENT);
    row(btns, 16);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* show = button(btns, TR("Pokaż", "Show"), HEX(2A2E33), TEXT, on_pass_show, NULL);
    pass_show = lv_obj_get_child(show, 0);
    lv_obj_t* gap = box(btns);  // pushes Cancel / Connect to the right
    lv_obj_set_flex_grow(gap, 1);
    button(btns, TR("Anuluj", "Cancel"), HEX(2A2E33), TEXT, on_pass_cancel, NULL);
    button(btns, TR("Połącz", "Connect"), ACCENT, HEX(111111), on_pass_connect, NULL);

    keyboard = lv_keyboard_create(scr);
    lv_obj_set_size(keyboard, 1280, 330);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(keyboard, HEX(101215), 0);
    lv_obj_set_style_border_width(keyboard, 0, 0);
    lv_obj_set_style_pad_all(keyboard, 10, 0);
    lv_obj_set_style_pad_gap(keyboard, 8, 0);
    lv_obj_set_style_text_font(keyboard, &kb_30, LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard, TEXT, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard, HEX(2A2E33), LV_PART_ITEMS);
    // Control keys (ABC / abc, 1#, backspace, enter, arrows, hide) are "checked" buttons: the theme gives them
    // dark text, so set a lighter key with light text of their own
    lv_obj_set_style_bg_color(keyboard, HEX(454B54), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(keyboard, TEXT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(keyboard, HEX(5E6670), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(keyboard, HEX(5E6670), LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(keyboard, TEXT, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(keyboard, 10, LV_PART_ITEMS);
    lv_obj_add_event_cb(keyboard, on_keyboard, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, on_keyboard, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);

    show_tab(0);
    lv_timer_create(tick, 500, NULL);
}

lv_obj_t* settings_screen(void)
{
    return scr;
}

void settings_on_open(void)
{
    saved_cache_n = -1;
    scan_shown    = -2;
    search_seen = SEARCH_IDLE;
    search_done = false;
    search_mine = false;
    lv_obj_clean(place_results);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    keyboard_hide();
    alarms_key[0] = '\0';  // rebuild the alarm list
    refresh_panel();
    wifi_scan_start();
    refresh_wifi_status();
    refresh_saved();
    refresh_scan();
    refresh_location();
}

void settings_show_tab(int tab)
{
    if (tab < 0 || tab >= TABS) return;
    show_tab(tab);  // the alarm list is redrawn only when it changed, so a screenshot shows it settled
    refresh_panel();
    refresh_wifi_status();
    refresh_saved();
    refresh_location();
    lv_obj_update_layout(scr);
}
