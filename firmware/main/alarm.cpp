#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>
#include <bsp/m5stack_tab5.h>
#include "alarm.h"
#include "uiwatch.h"
#include "audio.h"
#include "lang.h"
#include "layout.h"
#include "sdcard.h"
#include "ui.h"
#include "web.h"

static const char* TAG = "alarm";

struct AlarmStore {
    uint8_t count;
    uint8_t snooze_min;        // alarms
    uint8_t timer_snooze_min;  // the timer, on its own
    uint8_t pad;
    Alarm a[ALARM_MAX];
};
static AlarmStore* s;  // in PSRAM (internal RAM is short at boot)

static bool s_ringing;
static int s_ring_idx = -1;  // alarm that rings (or snoozes), -2 = the timer, -1 = none
static char s_ring_label[24];
static int s_ring_volume;
static time_t s_ring_start, s_snooze_until;
static esp_timer_handle_t s_test_timer;  // ends the "listen" preview

static void save()
{
    nvs_handle_t h;
    if (nvs_open("alarms", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "list", s, sizeof(*s));
    nvs_commit(h);
    nvs_close(h);
}

void alarm_init(void)
{
    s = (AlarmStore*)heap_caps_calloc(1, sizeof(AlarmStore), MALLOC_CAP_SPIRAM);
    if (!s) return;
    s->snooze_min       = 10;
    s->timer_snooze_min = 1;
    nvs_handle_t h;
    if (nvs_open("alarms", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(*s);
        if (nvs_get_blob(h, "list", s, &len) != ESP_OK || len != sizeof(*s)) {
            memset(s, 0, sizeof(*s));
            s->snooze_min = 10;
        }
        nvs_close(h);
    }
    if (s->count > ALARM_MAX) s->count = 0;
    if (!s->snooze_min || s->snooze_min == 9) s->snooze_min = 10;  // 9 min is no longer offered
    if (!s->timer_snooze_min) s->timer_snooze_min = 1;
}


static void ring_now(int idx, const char* label, int volume)
{
    uiwatch_crumb("alarm: start");
    s_ringing      = true;
    s_ring_idx     = idx;
    s_ring_start   = time(NULL);
    s_snooze_until = 0;
    strlcpy(s_ring_label, label, sizeof(s_ring_label));
    s_ring_volume = volume;
    uiwatch_crumb("alarm: sound");
    audio_play("", volume, true, 30);  // built-in beep
    uiwatch_crumb("alarm: screen");
    ui_alarm_show(label, idx == -2 ? (s ? s->timer_snooze_min : 1) : 0);  // the timer shows its snooze length
    uiwatch_crumb("");
    ESP_LOGI(TAG, "ringing: %s", label);
}

static void ring(int i)
{
    ring_now(i, s->a[i].label, s->a[i].volume);
}

void alarm_ring_timer(void)
{
    ring_now(-2, TR("Minutnik", "Timer"), 80);
}

static void stop_ringing()
{
    audio_stop();
    ui_alarm_hide();
    s_ringing = false;
}

void alarm_snooze(void)
{
    if (!s_ringing) return;
    stop_ringing();
    if (s_ring_idx == -2) {  // the timer: it counts the snooze itself, with the pill in the corner
        s_ring_idx     = -1;
        s_snooze_until = 0;
        ui_timer_restart((s ? s->timer_snooze_min : 1) * 60);
        return;
    }
    s_snooze_until = time(NULL) + (s ? s->snooze_min : 10) * 60;
}

void alarm_dismiss(void)
{
    stop_ringing();
    s_snooze_until = 0;
    s_ring_idx     = -1;
}

void alarm_tick(void)
{
    if (!s) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year < 124) return;  // no valid time yet
    if (s_ringing && now - s_ring_start >= ALARM_RING_S) alarm_dismiss();
    if (!s_ringing && s_snooze_until && now >= s_snooze_until && s_ring_idx != -1) ring_now(s_ring_idx, s_ring_label, s_ring_volume);

    static int last = -1;
    int key = tm.tm_yday * 1440 + tm.tm_hour * 60 + tm.tm_min;
    if (key == last) return;
    last    = key;
    int day = (tm.tm_wday + 6) % 7;  // Monday = 0
    for (int i = 0; i < s->count; i++) {
        Alarm& a = s->a[i];
        if (!a.enabled || a.hour != tm.tm_hour || a.minute != tm.tm_min) continue;
        if (a.days && !(a.days & (1 << day))) continue;
        if (!a.days) {
            a.enabled = 0;  // once
            save();
        }
        ring(i);
        break;
    }
}

bool alarm_ringing(void) { return s_ringing; }

bool alarm_indicator(int* hour, int* minute)
{
    if (!s) return false;
    time_t now = time(NULL), at = 0;
    if (s_ring_idx >= 0 && (s_ringing || s_snooze_until)) at = s_ringing ? s_ring_start : s_snooze_until;  // ringing or snoozing
    if (at) {
        struct tm tm;
        localtime_r(&at, &tm);
        *hour = tm.tm_hour, *minute = tm.tm_min;
        return true;
    }
    struct tm tm;
    localtime_r(&now, &tm);
    int today = (tm.tm_wday + 6) % 7, now_min = tm.tm_hour * 60 + tm.tm_min, best = -1, best_in = 8 * 1440;
    for (int i = 0; i < s->count; i++) {
        const Alarm& a = s->a[i];
        if (!a.enabled) continue;
        for (int d = 0; d < 8; d++) {
            if (a.days && !(a.days & (1 << ((today + d) % 7)))) continue;
            int in = d * 1440 + a.hour * 60 + a.minute - now_min;
            if (in <= 0) continue;
            if (in < best_in) best_in = in, best = i;
            break;
        }
    }
    if (best < 0) return false;
    *hour = s->a[best].hour, *minute = s->a[best].minute;
    return true;
}
int alarm_count(void) { return s ? s->count : 0; }
Alarm alarm_get(int i) { return s->a[i]; }

int alarm_snooze_min(bool timer)
{
    return !s ? (timer ? 1 : 10) : timer ? s->timer_snooze_min : s->snooze_min;
}

void alarm_set_snooze_min(bool timer, int min)
{
    if (!s || min < 1 || min > 60) return;
    if (timer) s->timer_snooze_min = min;
    else s->snooze_min = min;
    save();
}

void alarm_set_enabled(int i, bool on)
{
    if (!s || i < 0 || i >= s->count) return;
    s->a[i].enabled = on;
    save();
}

void alarm_set(int i, const Alarm& a)
{
    if (!s || i < 0 || i >= s->count) return;
    s->a[i] = a;
    s->a[i].label[sizeof(a.label) - 1] = '\0';
    save();
}

int alarm_add(const Alarm& a)
{
    if (!s || s->count >= ALARM_MAX) return -1;
    s->a[s->count] = a;
    s->count++;
    save();
    return s->count - 1;
}

void alarm_delete(int i)
{
    if (!s || i < 0 || i >= s->count) return;
    if (s_ring_idx == i) alarm_dismiss();  // the one ringing or snoozing goes quiet
    else if (s_ring_idx > i) s_ring_idx--;
    memmove(&s->a[i], &s->a[i + 1], sizeof(Alarm) * (s->count - i - 1));
    s->count--;
    save();
}

/* ----------------------------------- API ---------------------------------- */

static esp_err_t send_json(httpd_req_t* req, cJSON* root)
{
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text ? text : "{}");
    free(text);
    return err;
}

static char* read_body(httpd_req_t* req, size_t max)
{
    size_t len = req->content_len;
    if (!len || len > max) return NULL;
    char* body = (char*)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (!body) return NULL;
    size_t got = 0;
    while (got < len) {
        int n = httpd_req_recv(req, body + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) break;
        got += n;
    }
    body[got] = '\0';
    return body;
}

// GET /api/alarms: the alarms, snooze minutes and whether one is ringing
static esp_err_t alarms_get(httpd_req_t* req)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "snooze", s ? s->snooze_min : 10);
    cJSON_AddNumberToObject(root, "timer_snooze", s ? s->timer_snooze_min : 1);
    cJSON_AddBoolToObject(root, "ringing", s_ringing);
    cJSON_AddBoolToObject(root, "sd", sdcard_ready());
    cJSON* list = cJSON_AddArrayToObject(root, "alarms");
    for (int i = 0; s && i < s->count; i++) {
        const Alarm& a = s->a[i];
        cJSON* o       = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "enabled", a.enabled);
        cJSON_AddNumberToObject(o, "hour", a.hour);
        cJSON_AddNumberToObject(o, "minute", a.minute);
        cJSON_AddNumberToObject(o, "days", a.days);
        cJSON_AddNumberToObject(o, "volume", a.volume);
        cJSON_AddStringToObject(o, "label", a.label);
        cJSON_AddStringToObject(o, "sound", a.sound);
        cJSON_AddItemToArray(list, o);
    }
    return send_json(req, root);
}

static int geti(const cJSON* o, const char* k, int def)
{
    const cJSON* v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valueint : cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

// POST /api/alarms {alarms: [...], snooze}: replaces the whole list
static esp_err_t alarms_post(httpd_req_t* req)
{
    char* body  = read_body(req, 16384);
    cJSON* root = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!root || !s) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
    }
    AlarmStore* n = (AlarmStore*)heap_caps_calloc(1, sizeof(AlarmStore), MALLOC_CAP_SPIRAM);
    if (!n) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memory");
    }
    *n             = *s;
    int snooze     = geti(root, "snooze", s->snooze_min);
    n->snooze_min  = snooze < 1 ? 1 : snooze > 60 ? 60 : snooze;
    int tsnooze         = geti(root, "timer_snooze", s->timer_snooze_min);
    n->timer_snooze_min = tsnooze < 1 ? 1 : tsnooze > 60 ? 60 : tsnooze;
    cJSON* list    = cJSON_GetObjectItem(root, "alarms");
    if (cJSON_IsArray(list)) {
        n->count = 0;
        cJSON* o;
        cJSON_ArrayForEach(o, list)
        {
            if (n->count == ALARM_MAX) break;
            Alarm& a  = n->a[n->count++];
            memset(&a, 0, sizeof(a));
            a.enabled = geti(o, "enabled", 1) != 0;
            a.hour    = geti(o, "hour", 7) % 24;
            a.minute  = geti(o, "minute", 0) % 60;
            a.days    = geti(o, "days", 0) & 0x7F;
            int vol   = geti(o, "volume", 80);
            a.volume  = vol < 0 ? 0 : vol > 100 ? 100 : vol;
            const cJSON* v = cJSON_GetObjectItem(o, "label");
            if (cJSON_IsString(v)) strlcpy(a.label, v->valuestring, sizeof(a.label));
            v = cJSON_GetObjectItem(o, "sound");
            if (cJSON_IsString(v) && !strchr(v->valuestring, '/')) strlcpy(a.sound, v->valuestring, sizeof(a.sound));
        }
    }
    cJSON_Delete(root);
    if (bsp_display_lock(3000)) {  // alarm_tick reads the list in the LVGL task
        *s = *n;
        bsp_display_unlock();
    }
    free(n);
    save();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

static void test_end(void* arg)
{
    if (!s_ringing) audio_stop();
}

// POST /api/alarm/test {volume}: plays the alarm beep for 8 seconds
static esp_err_t test_post(httpd_req_t* req)
{
    char* body  = read_body(req, 1024);
    cJSON* root = body ? cJSON_Parse(body) : NULL;
    free(body);
    int vol = geti(root, "volume", 80);
    cJSON_Delete(root);
    if (s_ringing) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ringing");
    audio_play("", vol, true, 0);
    if (!s_test_timer) {
        esp_timer_create_args_t a = {};
        a.callback                = test_end;
        a.name                    = "alarm_test";
        esp_timer_create(&a, &s_test_timer);
    }
    esp_timer_stop(s_test_timer);
    esp_timer_start_once(s_test_timer, 8 * 1000000);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

// POST /api/alarm/snooze: like the Drzemka button on the Tab5
static esp_err_t snooze_post(httpd_req_t* req)
{
    if (bsp_display_lock(3000)) {
        alarm_snooze();
        bsp_display_unlock();
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

// POST /api/alarm/stop: stops the preview or the ringing alarm
static esp_err_t stop_post(httpd_req_t* req)
{
    if (bsp_display_lock(3000)) {
        if (s_ringing) alarm_dismiss();
        else audio_stop();
        bsp_display_unlock();
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

void alarm_register(httpd_handle_t server)
{
    web_register_api(server, "/api/alarms", HTTP_GET, alarms_get);
    web_register_api(server, "/api/alarms", HTTP_POST, alarms_post);
    web_register_api(server, "/api/alarm/test", HTTP_POST, test_post);
    web_register_api(server, "/api/alarm/stop", HTTP_POST, stop_post);
    web_register_api(server, "/api/alarm/snooze", HTTP_POST, snooze_post);
}
