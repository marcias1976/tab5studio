/*
 * Web page for the panel: http://tab5.local (or the IP shown in the settings).
 * The page itself is web/index.html, embedded in the firmware; it talks to the JSON API below.
 */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <esp_partition.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_flash.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include <driver/jpeg_encode.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mdns.h>
#include <nvs.h>
#include <bsp/m5stack_tab5.h>
#include "config.h"
#include <dirent.h>
#include "layout.h"
#include "settings.h"
#include "files.h"
#include "lang.h"
#include "alarm.h"
#include "version.h"
#include "livescene.h"
#include "uiwatch.h"
#include "sdcard.h"
#include "ui.h"
#include "weather.h"
#include "web.h"
#include "web_secrets.h"
#include "wifi.h"

static const char* TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

/* -------------------------------- Helpers -------------------------------- */

static void add_time(cJSON* o, const char* key, int minutes)
{
    char buf[12];
    snprintf(buf, sizeof(buf), "%02d:%02d", minutes / 60 % 24, minutes % 60);
    cJSON_AddStringToObject(o, key, buf);
}

// "HH:MM" → minutes after midnight, -1 when invalid
static int parse_time(const cJSON* v)
{
    int h, m;
    if (!cJSON_IsString(v) || sscanf(v->valuestring, "%d:%d", &h, &m) != 2) return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

static esp_err_t send_json(httpd_req_t* req, cJSON* root)
{
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text ? text : "{}");
    cJSON_free(text);
    return err;
}

static esp_err_t send_ok(httpd_req_t* req)
{
    return send_json(req, cJSON_CreateObject());
}

// Why the Tab5 last started, as a short code; the page shows it in its own language
static const char* reset_code()
{
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "power";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "watchdog (interrupts)";
        case ESP_RST_TASK_WDT: return "watchdog (tasks)";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_DEEPSLEEP: return "wakeup";
        case ESP_RST_EXT: return "button";
        case ESP_RST_USB: return "usb";
        case ESP_RST_JTAG: return "flash";
        default: return "other";
    }
}

/* ----------------------------------- PIN ---------------------------------- */

// 5 wrong PINs within 5 minutes lock every request out for 30 s, so the 4 digits cannot be guessed quickly.
// Requests with the right PIN (e.g. an open page refreshing) do not reset the count.
static const int MAX_FAILS          = 5;
static const int64_t LOCKOUT_US     = 30 * 1000000LL;
static const int64_t FAIL_WINDOW_US = 5 * 60 * 1000000LL;
static int s_fails;
static int64_t s_first_fail;
static int64_t s_locked_until;

// The PIN sent with the request: X-PIN header, then the "pin" cookie, then ?pin=
static bool request_pin(httpd_req_t* req, char* out, size_t size)
{
    if (httpd_req_get_hdr_value_str(req, "X-PIN", out, size) == ESP_OK) return true;
    char cookie[256];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) == ESP_OK) {
        for (char* p = strstr(cookie, "pin="); p; p = strstr(p + 1, "pin=")) {
            if (p == cookie || p[-1] == ' ' || p[-1] == ';') {
                strlcpy(out, p + 4, size);
                out[strcspn(out, "; ")] = '\0';
                return true;
            }
        }
    }
    char query[128];
    return httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
           httpd_query_key_value(query, "pin", out, size) == ESP_OK;
}

// true when the PIN matches; otherwise answers 401 (or 429 while locked out)
static bool check_pin(httpd_req_t* req)
{
    char pin[16] = "";
    bool sent    = request_pin(req, pin, sizeof(pin));
    if (esp_timer_get_time() < s_locked_until) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_sendstr(req, TR("Za dużo prób, poczekaj 30 s", "Too many attempts, wait 30 s"));
        return false;
    }
    if (sent && strcmp(pin, WEB_PIN) == 0) return true;
    if (sent && pin[0]) {
        int64_t now = esp_timer_get_time();
        if (s_fails == 0 || now - s_first_fail > FAIL_WINDOW_US) {
            s_fails      = 0;
            s_first_fail = now;
        }
        if (++s_fails >= MAX_FAILS) {
            s_fails        = 0;
            s_locked_until = now + LOCKOUT_US;
            ESP_LOGW(TAG, "too many wrong PINs, locked for 30 s");
        }
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, TR("Zły PIN", "Wrong PIN"));
    return false;
}

static esp_err_t guarded(httpd_req_t* req)
{
    if (!check_pin(req)) return ESP_OK;
    return ((esp_err_t(*)(httpd_req_t*))req->user_ctx)(req);
}

void web_register_api(httpd_handle_t server, const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*))
{
    httpd_uri_t u = {.uri = uri, .method = method, .handler = guarded, .user_ctx = (void*)handler};
    httpd_register_uri_handler(server, &u);
}

// POST /api/login with the PIN: 200 when right (the page then keeps it in a cookie)
static esp_err_t login_post(httpd_req_t* req)
{
    return send_ok(req);
}

/* ---------------------------------- Pages --------------------------------- */

static esp_err_t page_get(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);  // without the trailing NUL
}

static esp_err_t state_get(httpd_req_t* req)
{
    cJSON* root = cJSON_CreateObject();
    if (bsp_display_lock(2000)) {
        cJSON_AddNumberToObject(root, "face", ui_face());
        cJSON_AddNumberToObject(root, "faces", ui_face_count());  // 3 with Wariant-3 from Studio
        cJSON_AddBoolToObject(root, "night", ui_night());
        cJSON_AddBoolToObject(root, "screen", ui_screen_on());
        cJSON_AddBoolToObject(root, "intro", ui_intro_playing());
        cJSON_AddStringToObject(root, "lang", LANG_CODE);  // device language (menu, dates); the page follows it by default
        const PanelConfig& c = config();
        add_time(root, "night_start", c.night_start);
        add_time(root, "night_end", c.night_end);
        cJSON_AddBoolToObject(root, "off_enabled", c.off_enabled);
        add_time(root, "off_start", c.off_start);
        add_time(root, "off_end", c.off_end);
        cJSON_AddNumberToObject(root, "brightness", c.brightness);
        cJSON_AddNumberToObject(root, "cycle_min", c.cycle_min);
        cJSON_AddNumberToObject(root, "default_face", c.default_face);
        char color[8];
        snprintf(color, sizeof(color), "#%06lX", (unsigned long)(c.digit_color & 0xFFFFFF));
        cJSON_AddStringToObject(root, "digit_color", color);
        snprintf(color, sizeof(color), "#%06lX", (unsigned long)(c.sensor_color & 0xFFFFFF));
        cJSON_AddStringToObject(root, "sensor_color", c.sensor_follow ? "auto" : color);
        cJSON_AddNumberToObject(root, "day_preset", c.day_preset);
        cJSON_AddNumberToObject(root, "night_preset", c.night_preset);
        cJSON_AddNumberToObject(root, "night_face", c.night_face);
        cJSON* list = cJSON_AddArrayToObject(root, "presets");
        for (int i = 0; i < presets_count(); i++) {
            const ColorPreset* p = preset(i);
            cJSON* o             = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", p->name);
            snprintf(color, sizeof(color), "#%06lX", (unsigned long)(p->digit_color & 0xFFFFFF));
            cJSON_AddStringToObject(o, "digit_color", color);
            snprintf(color, sizeof(color), "#%06lX", (unsigned long)(p->sensor_color & 0xFFFFFF));
            cJSON_AddStringToObject(o, "sensor_color", p->sensor_follow ? "auto" : color);
            cJSON_AddItemToArray(list, o);
        }
        bsp_display_unlock();
    }

    WeatherData w = weather_get();
    if (w.version) {
        cJSON_AddNumberToObject(root, "temperature", w.temperature);
        cJSON_AddNumberToObject(root, "humidity", w.humidity);
        cJSON_AddNumberToObject(root, "pressure", w.pressure);
    }
    char postcode[8], place[48];
    weather_location(postcode, place);
    cJSON_AddStringToObject(root, "postcode", postcode);
    cJSON_AddStringToObject(root, "place", place);
    cJSON_AddNumberToObject(root, "postcode_state", weather_postcode_state());

    char ssid[33], ip[16];
    wifi_current(ssid);
    wifi_ip(ip);
    cJSON_AddStringToObject(root, "ssid", ssid);
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddNumberToObject(root, "rssi", wifi_rssi());
    cJSON_AddNumberToObject(root, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(root, "build", STUDIO_BUILD);  // Studio build ID of this program
    cJSON_AddStringToObject(root, "build_note", STUDIO_BUILD_NOTE);
    cJSON_AddStringToObject(root, "reset_reason", reset_code());  // power, software, panic, watchdog, brownout, ...

    livescene_state(root);  // outside the display lock: it may wait for a save
    uiwatch_state(root);
    SdInfo sd  = sdcard_info();
    cJSON* sdj = cJSON_AddObjectToObject(root, "sd");
    cJSON_AddBoolToObject(sdj, "mounted", sd.mounted);
    cJSON_AddBoolToObject(sdj, "formatting", sdcard_formatting());
    cJSON_AddNumberToObject(sdj, "format_result", sdcard_format_result());
    if (sd.mounted) {
        cJSON_AddStringToObject(sdj, "name", sd.name);
        cJSON_AddNumberToObject(sdj, "total_mb", (double)(sd.total / (1024 * 1024)));
        cJSON_AddNumberToObject(sdj, "free_mb", (double)(sd.free / (1024 * 1024)));
    }
    return send_json(req, root);
}

static void restart_task(void* arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));  // let the response go out
    esp_restart();
}

// Body: any subset of the state keys; postcode starts a lookup
static esp_err_t set_post(httpd_req_t* req)
{
    char body[512];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty");
    body[len]   = '\0';
    cJSON* root = cJSON_Parse(body);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");

    const cJSON* v;
    bool save = false;
    if (!bsp_display_lock(2000)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
    }
    PanelConfig& c = config();
    int t;
    if ((t = parse_time(cJSON_GetObjectItem(root, "night_start"))) >= 0) c.night_start = t, save = true;
    if ((t = parse_time(cJSON_GetObjectItem(root, "night_end"))) >= 0) c.night_end = t, save = true;
    if ((t = parse_time(cJSON_GetObjectItem(root, "off_start"))) >= 0) c.off_start = t, save = true;
    if ((t = parse_time(cJSON_GetObjectItem(root, "off_end"))) >= 0) c.off_end = t, save = true;
    if (cJSON_IsBool(v = cJSON_GetObjectItem(root, "off_enabled"))) c.off_enabled = cJSON_IsTrue(v), save = true;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(root, "brightness")) && v->valueint >= 10 && v->valueint <= 100)
        c.brightness = v->valueint, save = true;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(root, "cycle_min")) && v->valueint >= 0 && v->valueint <= 60)
        c.cycle_min = v->valueint, save = true;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(root, "default_face")) && v->valueint >= 0 && v->valueint <= 2)
        c.default_face = v->valueint, save = true;
    if (cJSON_IsString(v = cJSON_GetObjectItem(root, "digit_color")) && v->valuestring[0] == '#' &&
        strlen(v->valuestring) == 7)
        c.digit_color = strtoul(v->valuestring + 1, NULL, 16), save = true;
    // "auto" = same as the digits
    if (cJSON_IsString(v = cJSON_GetObjectItem(root, "sensor_color"))) {
        if (strcmp(v->valuestring, "auto") == 0) {
            c.sensor_follow = true, save = true;
        } else if (v->valuestring[0] == '#' && strlen(v->valuestring) == 7) {
            c.sensor_follow = false, c.sensor_color = strtoul(v->valuestring + 1, NULL, 16), save = true;
        }
    }
    // Color presets
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(root, "day_preset")) && v->valueint >= -1 && v->valueint < presets_count())
        c.day_preset = v->valueint, save = true;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(root, "night_preset")) && v->valueint >= -1 && v->valueint < presets_count())
        c.night_preset = v->valueint, save = true;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(root, "night_face")) && v->valueint >= -1 && v->valueint <= 2)
        c.night_face = v->valueint, save = true;
    bool preset_ok = true;
    if (cJSON_IsString(v = cJSON_GetObjectItem(root, "preset_save"))) preset_ok &= preset_save(v->valuestring);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(root, "preset_overwrite"))) preset_ok &= preset_overwrite(v->valueint);
    if (cJSON_IsObject(v = cJSON_GetObjectItem(root, "preset_rename"))) {
        const cJSON* i = cJSON_GetObjectItem(v, "index");
        const cJSON* n = cJSON_GetObjectItem(v, "name");
        preset_ok &= cJSON_IsNumber(i) && cJSON_IsString(n) && preset_rename(i->valueint, n->valuestring);
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(root, "preset_delete"))) {
        preset_ok &= preset_delete(v->valueint);
        save = true;  // day / night references may have moved
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(root, "preset_apply"))) {
        preset_ok &= preset_apply(v->valueint);
        save = true;
    }
    if (save) ui_config_changed();
    // Immediate actions come after the schedules, so they are not undone by them
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(root, "face")) && v->valueint >= 0 && v->valueint < ui_face_count()) ui_show_face(v->valueint);
    if (cJSON_IsBool(v = cJSON_GetObjectItem(root, "night"))) ui_set_night(cJSON_IsTrue(v));
    if (cJSON_IsBool(v = cJSON_GetObjectItem(root, "screen"))) ui_set_screen(cJSON_IsTrue(v));
    bsp_display_unlock();
    if (save) config_save();

    bool restart = false;
    if (cJSON_IsString(v = cJSON_GetObjectItem(root, "lang"))) {  // takes effect after a restart
        uint8_t l = strcmp(v->valuestring, "en") == 0;
        if (l != config().lang) {
            config().lang = l;
            config_save();
            restart = true;
        }
    }
    if (cJSON_IsString(v = cJSON_GetObjectItem(root, "postcode"))) weather_set_postcode(v->valuestring);
    if (cJSON_IsObject(v = cJSON_GetObjectItem(root, "place"))) {  // {name, lat, lon, postcode?} from /api/places
        const cJSON *n = cJSON_GetObjectItem(v, "name"), *la = cJSON_GetObjectItem(v, "lat"), *lo = cJSON_GetObjectItem(v, "lon"),
                    *pc = cJSON_GetObjectItem(v, "postcode");
        if (cJSON_IsString(n) && cJSON_IsNumber(la) && cJSON_IsNumber(lo)) {
            Place pl = {};
            strlcpy(pl.name, n->valuestring, sizeof(pl.name));
            if (cJSON_IsString(pc)) strlcpy(pl.postcode, pc->valuestring, sizeof(pl.postcode));
            pl.lat = (float)la->valuedouble, pl.lon = (float)lo->valuedouble;
            weather_set_place(pl);
        }
    }
    cJSON_Delete(root);
    if (!preset_ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "preset");
    esp_err_t r = send_ok(req);
    if (restart) xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return r;
}

// GET /api/timer: {running, left}; POST /api/timer {open: true|false, start: seconds, cancel: true}
static esp_err_t timer_handler(httpd_req_t* req)
{
    if (req->method == HTTP_POST) {
        char body[128] = "";
        int n = req->content_len < sizeof(body) - 1 ? httpd_req_recv(req, body, req->content_len) : -1;
        if (n < 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        body[n]     = '\0';
        cJSON* root = cJSON_Parse(body);
        const cJSON *o = cJSON_GetObjectItem(root, "open"), *st = cJSON_GetObjectItem(root, "start"), *c = cJSON_GetObjectItem(root, "cancel");
        if (bsp_display_lock(3000)) {
            ui_timer(cJSON_IsBool(o) ? (cJSON_IsTrue(o) ? 1 : -1) : 0, cJSON_IsNumber(st) ? st->valueint : 0, cJSON_IsTrue(c));
            bsp_display_unlock();
        }
        cJSON_Delete(root);
    }
    bool running = false;
    int left     = 0;
    if (bsp_display_lock(3000)) {
        left = ui_timer_left(&running);
        bsp_display_unlock();
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "running", running);
    cJSON_AddNumberToObject(root, "left", left);
    return send_json(req, root);
}

// GET /api/places?q=Berlin (or a Polish postcode): places to choose from, waits for the search (up to 15 s)
static esp_err_t places_get(httpd_req_t* req)
{
    char q[160] = "", query[64] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) httpd_query_key_value(q, "q", query, sizeof(query));
    // decode %XX and + from the query string
    char* o = query;
    for (const char* p = query; *p; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = {p[1], p[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            *o++ = *p == '+' ? ' ' : *p;
        }
    }
    *o = '\0';
    if (!query[0]) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "q");
    weather_search(query);
    for (int i = 0; i < 150 && weather_search_state() == SEARCH_LOOKING; i++) vTaskDelay(pdMS_TO_TICKS(100));
    SearchState st = weather_search_state();
    cJSON* root    = cJSON_CreateObject();
    cJSON* list    = cJSON_AddArrayToObject(root, "results");
    if (st == SEARCH_OK) {
        Place* found = (Place*)heap_caps_malloc(sizeof(Place) * SEARCH_MAX, MALLOC_CAP_SPIRAM);
        int n        = found ? weather_search_results(found, SEARCH_MAX) : 0;
        for (int i = 0; i < n; i++) {
            cJSON* it = cJSON_CreateObject();
            cJSON_AddStringToObject(it, "name", found[i].name);
            cJSON_AddStringToObject(it, "region", found[i].region);
            cJSON_AddStringToObject(it, "country", found[i].country);
            cJSON_AddStringToObject(it, "postcode", found[i].postcode);
            cJSON_AddNumberToObject(it, "lat", found[i].lat);
            cJSON_AddNumberToObject(it, "lon", found[i].lon);
            cJSON_AddItemToArray(list, it);
        }
        free(found);
    }
    cJSON_AddStringToObject(root, "status", st == SEARCH_OK ? "ok" : st == SEARCH_NOT_FOUND ? "not_found" : st == SEARCH_LOOKING ? "timeout" : "error");
    return send_json(req, root);
}

/* --------------------------- Settings backup ---------------------------- */
// The settings kept in NVS as raw blobs, for a copy in Studio (Wi-Fi passwords are left out on purpose).
// The structs keep their layout between versions (new fields only at the end), so a copy restores into a newer program.
struct BackupItem {
    const char* ns;
    const char* key;
    bool any_size;  // read with a length check of its own (config, presets); the rest must match the stored size
};
static const BackupItem BACKUP_ITEMS[] = {
    {"panel", "cfg", true},       {"panel", "presets", true}, {"panel", "layout2", false},
    {"panel", "lpresets2", true}, {"alarms", "list", false},  {"meteo", "loc", false},
};

// GET /api/settings/backup → {"v":1,"build":"…","items":[{"ns","key","hex"}]}
static esp_err_t settings_backup_get(httpd_req_t* req)
{
    cJSON* root  = cJSON_CreateObject();
    cJSON* items = cJSON_AddArrayToObject(root, "items");
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "build", STUDIO_BUILD);
    for (const BackupItem& it : BACKUP_ITEMS) {
        nvs_handle_t h;
        if (nvs_open(it.ns, NVS_READONLY, &h) != ESP_OK) continue;
        size_t len = 0;
        if (nvs_get_blob(h, it.key, NULL, &len) == ESP_OK && len > 0 && len <= 16384) {
            uint8_t* data = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
            char* hex     = (char*)heap_caps_malloc(len * 2 + 1, MALLOC_CAP_SPIRAM);
            if (data && hex && nvs_get_blob(h, it.key, data, &len) == ESP_OK) {
                for (size_t i = 0; i < len; i++) sprintf(hex + i * 2, "%02x", data[i]);
                cJSON* o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "ns", it.ns);
                cJSON_AddStringToObject(o, "key", it.key);
                cJSON_AddStringToObject(o, "hex", hex);
                cJSON_AddItemToArray(items, o);
            }
            free(data);
            free(hex);
        }
        nvs_close(h);
    }
    return send_json(req, root);
}

// POST /api/settings/restore with the JSON from /api/settings/backup: writes the known items back, then restarts.
// → {"restored":N,"skipped":[…]}; an item whose size does not fit this program is skipped.
static esp_err_t settings_restore_post(httpd_req_t* req)
{
    size_t len = req->content_len;
    if (len < 2 || len > 128 * 1024) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "size");
    char* body = (char*)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memory");
    size_t got = 0;
    while (got < len) {
        int n = httpd_req_recv(req, body + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) break;
        got += n;
    }
    body[got]   = '\0';
    cJSON* root = got == len ? cJSON_Parse(body) : NULL;
    free(body);
    const cJSON* items = root ? cJSON_GetObjectItem(root, "items") : NULL;
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    cJSON* out     = cJSON_CreateObject();
    cJSON* skipped = cJSON_AddArrayToObject(out, "skipped");
    int restored   = 0;
    const cJSON* o;
    cJSON_ArrayForEach(o, items) {
        const char* ns  = cJSON_GetStringValue(cJSON_GetObjectItem(o, "ns"));
        const char* key = cJSON_GetStringValue(cJSON_GetObjectItem(o, "key"));
        const char* hex = cJSON_GetStringValue(cJSON_GetObjectItem(o, "hex"));
        const BackupItem* it = NULL;
        for (const BackupItem& b : BACKUP_ITEMS)
            if (ns && key && !strcmp(b.ns, ns) && !strcmp(b.key, key)) it = &b;
        size_t n = hex ? strlen(hex) / 2 : 0;
        bool ok  = it && n > 0 && n <= 16384 && strlen(hex) == n * 2;
        nvs_handle_t h;
        if (ok && nvs_open(it->ns, NVS_READWRITE, &h) == ESP_OK) {
            size_t cur = 0;
            if (!it->any_size && nvs_get_blob(h, it->key, NULL, &cur) == ESP_OK && cur != n) ok = false;  // another struct size
            uint8_t* data = ok ? (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM) : NULL;
            for (size_t i = 0; data && i < n; i++) {
                unsigned v;
                if (sscanf(hex + i * 2, "%2x", &v) != 1) ok = false;
                data[i] = (uint8_t)v;
            }
            ok = ok && data && nvs_set_blob(h, it->key, data, n) == ESP_OK && nvs_commit(h) == ESP_OK;
            free(data);
            nvs_close(h);
        } else {
            ok = false;
        }
        if (ok) restored++;
        else if (ns && key) {
            char name[48];
            snprintf(name, sizeof(name), "%s/%s", ns, key);
            cJSON_AddItemToArray(skipped, cJSON_CreateString(name));
        }
    }
    cJSON_Delete(root);
    cJSON_AddNumberToObject(out, "restored", restored);
    esp_err_t r = send_json(req, out);
    if (restored) xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);  // the settings load at the next start
    return r;
}

// GET /api/backup?kind=app|full: the program on the Tab5 as a file.
//   app  — the program only (the running slot, ota_0 at 0x20000 or ota_1)
//   full — the whole flash from 0x0 to the end of the old voice model data, if any (ESP Web Tool at 0x0); the settings area (NVS, with
//          Wi-Fi passwords) and unused space are written as empty (0xFF), so no private data leaves the device
static esp_err_t backup_get(httpd_req_t* req)
{
    char q[32], kind[8] = "app";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) httpd_query_key_value(q, "kind", kind, sizeof(kind));
    bool full                  = strcmp(kind, "full") == 0;
    const esp_partition_t* app = esp_ota_get_running_partition();
    esp_partition_pos_t pos    = {app->address, app->size};
    esp_image_metadata_t md;
    if (!app || esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &md) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "image");
    uint32_t start = full ? 0 : app->address, app_end = app->address + md.image_len, end = app_end;
    const esp_partition_t* model = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
    const esp_partition_t* nvs   = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
    uint8_t* buf                 = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!buf) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memory");
    if (full && model) {
        // the model data ends where the partition is empty to its end
        end = model->address + model->size;
        while (end > model->address) {
            esp_flash_read(NULL, buf, end - 4096, 4096);
            bool blank = true;
            for (int i = 0; i < 4096 && blank; i++) blank = buf[i] == 0xFF;
            if (!blank) break;
            end -= 4096;
        }
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", full ? "attachment; filename=\"tab5panel-web-install-backup.bin\""
                                                        : "attachment; filename=\"tab5panel-app-backup.bin\"");
    esp_err_t err = ESP_OK;
    for (uint32_t a = start; a < end && err == ESP_OK; a += 4096) {
        size_t n = end - a < 4096 ? end - a : 4096;
        bool skip = (nvs && a >= nvs->address && a < nvs->address + nvs->size) || (model && a >= app_end && a < model->address);
        if (skip) memset(buf, 0xFF, n);
        else esp_flash_read(NULL, buf, a, n);
        err = httpd_resp_send_chunk(req, (const char*)buf, n);
    }
    free(buf);
    if (err == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

// POST /api/ota: a new program (tab5panel-app.bin) as the body, from Studio over Wi-Fi. It goes into the other
// program slot; the Tab5 restarts into it, and goes back to this one if the new one does not run for 30 s
// (rollback, see uiwatch.cpp).
static void ota_restart(void* arg)
{
    (void)arg;
    esp_restart();
}

static esp_err_t ota_post(httpd_req_t* req)
{
    const esp_partition_t* part = esp_ota_get_next_update_partition(NULL);
    size_t len                  = req->content_len;
    if (!part || len < 1024 || len > part->size) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "size");
    uint8_t* buf = (uint8_t*)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
    if (!buf) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memory");
    esp_ota_handle_t ota = 0;
    // Erase sector by sector while writing: erasing the whole slot first blocks the CPUs for over 5 s and the task
    // watchdog restarts the Tab5 before the update is done
    uiwatch_crumb("update: begin");
    int shown = 0;  // percent on the "Updating…" screen
    if (bsp_display_lock(2000)) {
        ui_show_update(0);
        bsp_display_unlock();
    }
    esp_err_t err        = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    size_t got           = 0, noted = 0;
    while (err == ESP_OK && got < len) {
        int n = httpd_req_recv(req, (char*)buf, len - got < 16384 ? len - got : 16384);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota, buf, n);
        got += n;
        if (got - noted >= 256 * 1024) {
            char crumb[40];
            snprintf(crumb, sizeof(crumb), "update: %u KB written", (unsigned)(got / 1024));
            uiwatch_crumb(crumb);
            noted = got;
        }
        int pct = (int)(got * 100 / len);
        if (pct / 10 != shown / 10 && bsp_display_lock(50)) {  // a small label, every 10 %
            ui_show_update(pct);
            bsp_display_unlock();
            shown = pct;
        }
        vTaskDelay(1);  // let the idle tasks run (task watchdog)
    }
    free(buf);
    uiwatch_crumb("update: check image");
    if (err == ESP_OK) err = esp_ota_end(ota);  // checks the image
    else if (ota) esp_ota_abort(ota);
    uiwatch_crumb("update: switch slot");
    if (err == ESP_OK) err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        uiwatch_crumb("");
        if (bsp_display_lock(2000)) {
            ui_hide_update();
            bsp_display_unlock();
        }
        ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err == ESP_ERR_OTA_VALIDATE_FAILED ? "image" : "write");
    }
    uiwatch_crumb("update: restart");
    ESP_LOGI(TAG, "update written to %s (%u bytes), restarting", part->label, (unsigned)len);
    send_ok(req);
    static esp_timer_handle_t t;
    esp_timer_create_args_t args = {};
    args.callback                = ota_restart;
    args.name                    = "ota_restart";
    if (esp_timer_create(&args, &t) == ESP_OK) esp_timer_start_once(t, 1500000);  // after the answer went out
    return ESP_OK;
}

static esp_err_t intro_post(httpd_req_t* req)
{
    if (bsp_display_lock(2000)) {
        ui_play_intro();
        bsp_display_unlock();
    }
    return send_ok(req);
}

// The current screen as a 24-bit BMP (top-down rows), rendered off-screen
static esp_err_t screenshot_get(httpd_req_t* req)
{
    if (!bsp_display_lock(3000)) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
    lv_draw_buf_t* buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    bsp_display_unlock();
    if (!buf) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");

    int32_t w = buf->header.w, h = buf->header.h;
    uint32_t row = w * 3, size = 54 + row * h;  // w is a multiple of 4, so rows need no padding
    uint8_t hdr[54] = {'B', 'M'};
    auto put32 = [&](int at, uint32_t v) { memcpy(hdr + at, &v, 4); };
    put32(2, size);
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, (uint32_t)-h);  // negative height = rows top to bottom
    hdr[26] = 1;
    hdr[28] = 24;
    put32(34, row * h);
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_send_chunk(req, (const char*)hdr, sizeof(hdr));
    // LVGL keeps RGB888 as B, G, R in memory, the same order as BMP
    for (int32_t y = 0; y < h; y++) {
        if (httpd_resp_send_chunk(req, (const char*)buf->data + y * buf->header.stride, row) != ESP_OK) break;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    lv_draw_buf_destroy(buf);
    return ESP_OK;
}

// Starts erasing the microSD card (new FAT file system)
static esp_err_t sd_format_post(httpd_req_t* req)
{
    if (!sdcard_format_start()) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
    return send_ok(req);  // runs in the background; /api/state shows sd.formatting
}

// GET /api/preview: the current screen as a small JPEG for the page, made by the hardware JPEG encoder.
// The buffers (about 3 MB of PSRAM) are allocated on the first request and kept.
static const int PREVIEW_W = 1280, PREVIEW_H = 720;
static const size_t PREVIEW_OUT = 512 * 1024;

static esp_err_t preview_get(httpd_req_t* req)
{
    static SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    static jpeg_encoder_handle_t enc;
    static uint8_t *in, *out;
    static size_t in_size, out_size;
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(5000)) != pdTRUE) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");

    esp_err_t err = ESP_OK;
    if (!enc) {
        jpeg_encode_engine_cfg_t ecfg = {.intr_priority = 0, .timeout_ms = 1000};
        jpeg_encode_memory_alloc_cfg_t icfg = {.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER};
        jpeg_encode_memory_alloc_cfg_t ocfg = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
        in  = (uint8_t*)jpeg_alloc_encoder_mem(PREVIEW_W * PREVIEW_H * 3, &icfg, &in_size);
        out = (uint8_t*)jpeg_alloc_encoder_mem(PREVIEW_OUT, &ocfg, &out_size);
        if (!in || !out || jpeg_new_encoder_engine(&ecfg, &enc) != ESP_OK) err = ESP_ERR_NO_MEM;
    }

    uint32_t jpeg_size = 0;
    if (err == ESP_OK) {
        lv_draw_buf_t buf;
        lv_draw_buf_init(&buf, PREVIEW_W, PREVIEW_H, LV_COLOR_FORMAT_RGB888, PREVIEW_W * 3, in, in_size);
        // ?settings=N: the settings screen with tab N (it does not have to be on screen)
        char query[32], tab[4];
        int settings_tab = -1;
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
            httpd_query_key_value(query, "settings", tab, sizeof(tab)) == ESP_OK)
            settings_tab = atoi(tab);
        if (!bsp_display_lock(3000)) {
            err = ESP_ERR_TIMEOUT;
        } else {
            lv_obj_t* target = lv_screen_active();
            if (settings_tab >= 0) {
                settings_show_tab(settings_tab);
                target = settings_screen();
            } else if (ui_alarm_view()) {
                target = ui_alarm_view();  // a ringing alarm covers the screen
            }
            if (lv_snapshot_take_to_draw_buf(target, LV_COLOR_FORMAT_RGB888, &buf) != LV_RESULT_OK) err = ESP_FAIL;
            bsp_display_unlock();
        }
    }
    if (err == ESP_OK) {
        // LVGL's RGB888 (B, G, R in memory) is the byte order the encoder expects
        jpeg_encode_cfg_t cfg = {.height        = PREVIEW_H,
                                 .width         = PREVIEW_W,
                                 .src_type      = JPEG_ENCODE_IN_FORMAT_RGB888,
                                 .sub_sample    = JPEG_DOWN_SAMPLING_YUV420,
                                 .image_quality = 70};
        err = jpeg_encoder_process(enc, &cfg, in, PREVIEW_W * PREVIEW_H * 3, out, out_size, &jpeg_size);
    }
    if (err == ESP_OK) {
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        err = httpd_resp_send(req, (const char*)out, jpeg_size);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }
    xSemaphoreGive(lock);
    return ESP_OK;
}

/* --------------------------------- Layout --------------------------------- */

// Layout colors as "#RRGGBB", "" = automatic
static void color_to(cJSON* o, const char* key, uint32_t c)
{
    char buf[8] = "";
    if (c & LAYOUT_COLOR) snprintf(buf, sizeof(buf), "#%06lX", (unsigned long)(c & 0xFFFFFF));
    cJSON_AddStringToObject(o, key, buf);
}

static void color_from(const cJSON* o, const char* key, uint32_t& c)
{
    const cJSON* v = cJSON_GetObjectItem(o, key);
    if (!cJSON_IsString(v)) return;
    c = v->valuestring[0] == '#' && strlen(v->valuestring) == 7 ? LAYOUT_COLOR | strtoul(v->valuestring + 1, NULL, 16) : 0;
}

static cJSON* clock_json(const LayoutClock& c)
{
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "x", c.x);
    cJSON_AddNumberToObject(o, "y", c.y);
    cJSON_AddNumberToObject(o, "size", c.size);
    cJSON_AddBoolToObject(o, "seconds", c.seconds);
    cJSON_AddBoolToObject(o, "visible", c.visible);
    cJSON_AddStringToObject(o, "font", c.font);
    return o;
}

static cJSON* text_json(const LayoutText& t)
{
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "x", t.x);
    cJSON_AddNumberToObject(o, "y", t.y);
    cJSON_AddNumberToObject(o, "size", t.size);
    cJSON_AddBoolToObject(o, "visible", t.visible);
    cJSON_AddStringToObject(o, "font", t.font);
    cJSON_AddNumberToObject(o, "icon", t.icon);
    cJSON_AddNumberToObject(o, "icon_size", t.icon_size);
    cJSON_AddNumberToObject(o, "unit_size", t.unit_size);
    cJSON_AddStringToObject(o, "unit", t.unit);
    color_to(o, "icon_color", t.icon_color);
    color_to(o, "value_color", t.value_color);
    color_to(o, "unit_color", t.unit_color);
    return o;
}

static cJSON* layout_json(const Layout& l)
{
    cJSON* o     = cJSON_CreateObject();
    cJSON* basic = cJSON_AddObjectToObject(o, "basic");
    cJSON_AddItemToObject(basic, "clock", clock_json(l.basic_clock));
    cJSON* s = cJSON_AddArrayToObject(basic, "sensors");
    for (const LayoutText& t : l.basic_sensor) cJSON_AddItemToArray(s, text_json(t));
    cJSON_AddItemToObject(basic, "date", text_json(l.basic_date));
    cJSON* full = cJSON_AddObjectToObject(o, "full");
    cJSON_AddItemToObject(full, "clock", clock_json(l.full_clock));
    cJSON_AddItemToObject(full, "temp", text_json(l.full_temp));
    cJSON_AddItemToObject(full, "date", text_json(l.full_date));
    return o;
}

static int clampi(const cJSON* v, int lo, int hi, int def)
{
    if (!cJSON_IsNumber(v)) return def;
    return v->valueint < lo ? lo : v->valueint > hi ? hi : v->valueint;
}

static void clock_from(const cJSON* o, LayoutClock& c)
{
    if (!cJSON_IsObject(o)) return;
    c.x    = clampi(cJSON_GetObjectItem(o, "x"), -1280, 2560, c.x);
    c.y    = clampi(cJSON_GetObjectItem(o, "y"), -720, 1440, c.y);
    c.size = clampi(cJSON_GetObjectItem(o, "size"), 10, 280, c.size);
    const cJSON* v;
    if (cJSON_IsBool(v = cJSON_GetObjectItem(o, "seconds"))) c.seconds = cJSON_IsTrue(v);
    if (cJSON_IsBool(v = cJSON_GetObjectItem(o, "visible"))) c.visible = cJSON_IsTrue(v);
    if (cJSON_IsString(v = cJSON_GetObjectItem(o, "font"))) strlcpy(c.font, v->valuestring, sizeof(c.font));  // "" = LED
}

static void text_from(const cJSON* o, LayoutText& t)
{
    if (!cJSON_IsObject(o)) return;
    t.x    = clampi(cJSON_GetObjectItem(o, "x"), -1280, 2560, t.x);
    t.y    = clampi(cJSON_GetObjectItem(o, "y"), -720, 1440, t.y);
    t.size = clampi(cJSON_GetObjectItem(o, "size"), 8, 400, t.size);
    t.icon = clampi(cJSON_GetObjectItem(o, "icon"), 0, 0xFFFF, t.icon);
    const cJSON* v;
    if (cJSON_IsBool(v = cJSON_GetObjectItem(o, "visible"))) t.visible = cJSON_IsTrue(v);
    if (cJSON_IsString(v = cJSON_GetObjectItem(o, "font")) && v->valuestring[0]) strlcpy(t.font, v->valuestring, sizeof(t.font));
    t.icon_size = clampi(cJSON_GetObjectItem(o, "icon_size"), 0, 400, t.icon_size);
    t.unit_size = clampi(cJSON_GetObjectItem(o, "unit_size"), 0, 400, t.unit_size);
    if (cJSON_IsString(v = cJSON_GetObjectItem(o, "unit"))) strlcpy(t.unit, v->valuestring, sizeof(t.unit));
    color_from(o, "icon_color", t.icon_color);
    color_from(o, "value_color", t.value_color);
    color_from(o, "unit_color", t.unit_color);
}

static void layout_from(const cJSON* lj, Layout& l)
{
    const cJSON* basic = cJSON_GetObjectItem(lj, "basic");
    const cJSON* full  = cJSON_GetObjectItem(lj, "full");
    clock_from(cJSON_GetObjectItem(basic, "clock"), l.basic_clock);
    const cJSON* sensors = cJSON_GetObjectItem(basic, "sensors");
    for (int k = 0; k < 3; k++) text_from(cJSON_GetArrayItem(sensors, k), l.basic_sensor[k]);
    text_from(cJSON_GetObjectItem(basic, "date"), l.basic_date);
    clock_from(cJSON_GetObjectItem(full, "clock"), l.full_clock);
    text_from(cJSON_GetObjectItem(full, "temp"), l.full_temp);
    text_from(cJSON_GetObjectItem(full, "date"), l.full_date);
}

// GET /api/layout: the layout on screen, the saved and default ones, and the fonts to choose from
static esp_err_t layout_get(httpd_req_t* req)
{
    cJSON* root = cJSON_CreateObject();
    if (bsp_display_lock(2000)) {
        cJSON_AddItemToObject(root, "current", layout_json(layout_current()));
        cJSON_AddItemToObject(root, "saved", layout_json(layout_saved()));
        cJSON_AddBoolToObject(root, "live", layout_is_live());
        cJSON* presets = cJSON_AddArrayToObject(root, "presets");
        for (int i = 0; i < layout_presets_count(); i++) {
            cJSON* p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "name", layout_preset_name(i));
            cJSON_AddItemToObject(p, "layout", layout_json(*layout_preset(i)));
            cJSON_AddItemToArray(presets, p);
        }
        bsp_display_unlock();
    }
    cJSON_AddItemToObject(root, "default", layout_json(layout_default()));
    cJSON* fonts = cJSON_AddArrayToObject(root, "fonts");
    const BuiltinFont* b;
    int n = layout_builtin_fonts(&b);
    for (int i = 0; i < n; i++) {
        cJSON* f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "name", b[i].name);
        cJSON_AddStringToObject(f, "title", b[i].title);
        cJSON_AddItemToArray(fonts, f);
    }
    // Custom fonts from the SD card
    if (sdcard_ready()) {
        DIR* dir = opendir(FONTS_DIR);
        struct dirent* e;
        while (dir && (e = readdir(dir)) != NULL) {
            const char* dot = strrchr(e->d_name, '.');
            if (e->d_name[0] == '.' || !dot || (strcasecmp(dot, ".ttf") != 0 && strcasecmp(dot, ".otf") != 0)) continue;
            if (strlen(e->d_name) > 28) continue;  // must fit LayoutText.font with the "sd:" prefix
            char name[40];
            snprintf(name, sizeof(name), "sd:%s", e->d_name);
            cJSON* f = cJSON_CreateObject();
            cJSON_AddStringToObject(f, "name", name);
            cJSON_AddStringToObject(f, "title", e->d_name);
            cJSON_AddItemToArray(fonts, f);
        }
        if (dir) closedir(dir);
    }
    return send_json(req, root);
}

// POST /api/layout: {"layout": {...}, "save": false} shows it (LIVE), "save": true also stores it;
// {"revert": true} goes back to the saved layout
static esp_err_t layout_post(httpd_req_t* req)
{
    int len = req->content_len;
    if (len <= 0 || len > 16384) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "size");
    char* body = (char*)malloc(len + 1);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    int got = 0;
    while (got < len) {
        int n = httpd_req_recv(req, body + got, len - got);
        if (n <= 0) break;
        got += n;
    }
    body[got]   = '\0';
    cJSON* root = cJSON_Parse(body);
    free(body);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    if (!bsp_display_lock(3000)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
    }
    const cJSON* v;
    bool ok = true, redraw = true;
    if (cJSON_IsTrue(cJSON_GetObjectItem(root, "revert"))) {
        layout_set(layout_saved(), false);
    } else if (cJSON_IsObject(v = cJSON_GetObjectItem(root, "preset_save"))) {
        // {"index": -1 (new) or i (overwrite), "name": "...", "layout": {...}}
        Layout l = layout_current();
        layout_from(cJSON_GetObjectItem(v, "layout"), l);
        const cJSON* n = cJSON_GetObjectItem(v, "name");
        ok     = layout_preset_store(clampi(cJSON_GetObjectItem(v, "index"), -1, MAX_LAYOUT_PRESETS, -1),
                                     cJSON_IsString(n) ? n->valuestring : NULL, l);
        redraw = false;
    } else if (cJSON_IsObject(v = cJSON_GetObjectItem(root, "preset_rename"))) {
        const cJSON* n = cJSON_GetObjectItem(v, "name");
        ok     = cJSON_IsString(n) && layout_preset_rename(clampi(cJSON_GetObjectItem(v, "index"), -1, 99, -1), n->valuestring);
        redraw = false;
    } else if (cJSON_IsNumber(v = cJSON_GetObjectItem(root, "preset_delete"))) {
        ok     = layout_preset_delete(v->valueint);
        redraw = false;
    } else if (cJSON_IsNumber(v = cJSON_GetObjectItem(root, "preset_apply"))) {
        const Layout* p = layout_preset(v->valueint);
        if (p) layout_set(*p, true);
        ok = p != NULL;
    } else {
        Layout l = layout_current();
        layout_from(cJSON_GetObjectItem(root, "layout"), l);
        layout_set(l, cJSON_IsTrue(cJSON_GetObjectItem(root, "save")));
    }
    if (redraw) ui_layout_changed();
    bsp_display_unlock();
    cJSON_Delete(root);
    if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "preset");
    return send_ok(req);
}

// GET /api/font?name=inter: the TTF file, for the editor to draw text exactly like the Tab5
static esp_err_t font_get(httpd_req_t* req)
{
    char query[96], name[48];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name");
    for (char* p = name; *p; p++)
        if (*p == '+') *p = ' ';
    size_t size         = 0;
    const uint8_t* data = NULL;
    if (bsp_display_lock(2000)) {  // the SD font cache is shared with the LVGL task
        data = layout_font_data(name, &size);
        bsp_display_unlock();
    }
    if (!data) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no font");
    httpd_resp_set_type(req, "font/ttf");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, (const char*)data, size);
}


static esp_err_t restart_post(httpd_req_t* req)
{
    send_ok(req);
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* ---------------------------------- Start --------------------------------- */

void web_start(void)
{
    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size       = 8192;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 40;
    httpd_handle_t server;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "server start failed");
        return;
    }
    // The page itself is public (it only shows the PIN screen until the PIN is right); every API call needs the PIN
    httpd_uri_t page = {.uri = "/", .method = HTTP_GET, .handler = page_get, .user_ctx = NULL};
    httpd_register_uri_handler(server, &page);
    web_register_api(server, "/api/login", HTTP_POST, login_post);
    web_register_api(server, "/api/state", HTTP_GET, state_get);
    web_register_api(server, "/api/places", HTTP_GET, places_get);
    web_register_api(server, "/api/timer", HTTP_GET, timer_handler);
    web_register_api(server, "/api/timer", HTTP_POST, timer_handler);
    web_register_api(server, "/api/backup", HTTP_GET, backup_get);
    web_register_api(server, "/api/settings/backup", HTTP_GET, settings_backup_get);
    web_register_api(server, "/api/settings/restore", HTTP_POST, settings_restore_post);
    web_register_api(server, "/api/ota", HTTP_POST, ota_post);
    web_register_api(server, "/api/set", HTTP_POST, set_post);
    web_register_api(server, "/api/intro", HTTP_POST, intro_post);
    web_register_api(server, "/api/restart", HTTP_POST, restart_post);
    web_register_api(server, "/api/sd/format", HTTP_POST, sd_format_post);
    web_register_api(server, "/api/screenshot", HTTP_GET, screenshot_get);
    web_register_api(server, "/api/preview", HTTP_GET, preview_get);
    web_register_api(server, "/api/layout", HTTP_GET, layout_get);
    web_register_api(server, "/api/layout", HTTP_POST, layout_post);
    web_register_api(server, "/api/font", HTTP_GET, font_get);
    files_register(server);
    livescene_register(server);
    alarm_register(server);

    // http://tab5.local
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("tab5");
        mdns_instance_name_set("Tab5 panel");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    ESP_LOGI(TAG, "web page ready");
}
