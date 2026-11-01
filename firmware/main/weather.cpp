#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <cJSON.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <esp_heap_caps.h>
#include "lang.h"
#include "weather.h"
#include "wifi.h"

static const char* TAG = "weather";

// Location, default Warszawa 00-950; changed with a postal code in the settings
struct Location {
    char postcode[8];
    char place[48];
    float lat, lon;
};
static Location s_loc = {"00-950", "Warszawa", 52.2297f, 21.0122f};
static char s_pending[8];  // postal code waiting to be looked up
static volatile PostcodeState s_pc_state = POSTCODE_IDLE;
static TaskHandle_t s_task;
static char s_query[64];  // place search waiting to run
static volatile SearchState s_search = SEARCH_IDLE;
static Place* s_results;  // [SEARCH_MAX] in PSRAM (internal RAM is short at boot)
static int s_nresults;

static const uint32_t REFRESH_MS = 10 * 60 * 1000;
static const uint32_t RETRY_MS   = 30 * 1000;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static WeatherData s_data;

// GET into body (NUL-terminated); returns the HTTP status, or -1 when the request failed
static int http_get(const char* url, char* body, int size)
{
    esp_http_client_config_t cfg = {};
    cfg.url                      = url;
    cfg.timeout_ms               = 10000;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) return -1;
    int status = -1;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        status  = esp_http_client_get_status_code(client);
        int len = esp_http_client_read_response(client, body, size - 1);
        body[len > 0 ? len : 0] = '\0';
    }
    esp_http_client_cleanup(client);
    return status;
}

static void load_location()
{
    nvs_handle_t h;
    if (nvs_open("meteo", NVS_READONLY, &h) != ESP_OK) return;
    Location loc;
    size_t len = sizeof(loc);
    if (nvs_get_blob(h, "loc", &loc, &len) == ESP_OK && len == sizeof(loc)) s_loc = loc;
    nvs_close(h);
}

static void save_location()
{
    nvs_handle_t h;
    if (nvs_open("meteo", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "loc", &s_loc, sizeof(s_loc));
    nvs_commit(h);
    nvs_close(h);
}

// Postal code → place and coordinates (zippopotam.us)
static void lookup_postcode(const char* code)
{
    char url[64], body[2048];
    snprintf(url, sizeof(url), "http://api.zippopotam.us/pl/%s", code);
    int status = http_get(url, body, sizeof(body));
    if (status == 404) {
        s_pc_state = POSTCODE_NOT_FOUND;
        return;
    }
    cJSON* root   = status == 200 ? cJSON_Parse(body) : NULL;
    cJSON* place  = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "places"), 0);
    cJSON* name   = cJSON_GetObjectItem(place, "place name");
    cJSON* lat    = cJSON_GetObjectItem(place, "latitude");
    cJSON* lon    = cJSON_GetObjectItem(place, "longitude");
    if (cJSON_IsString(name) && cJSON_IsString(lat) && cJSON_IsString(lon)) {
        Location loc;
        strlcpy(loc.postcode, code, sizeof(loc.postcode));
        strlcpy(loc.place, name->valuestring, sizeof(loc.place));
        loc.lat = strtof(lat->valuestring, NULL);
        loc.lon = strtof(lon->valuestring, NULL);
        taskENTER_CRITICAL(&s_lock);
        s_loc = loc;
        taskEXIT_CRITICAL(&s_lock);
        save_location();
        ESP_LOGI(TAG, "location: %s %s (%.4f, %.4f)", loc.postcode, loc.place, loc.lat, loc.lon);
        s_pc_state = POSTCODE_OK;
    } else {
        ESP_LOGW(TAG, "postcode lookup failed (HTTP %d)", status);
        s_pc_state = status == 200 ? POSTCODE_NOT_FOUND : POSTCODE_ERROR;
    }
    cJSON_Delete(root);
}

static bool is_postcode(const char* q, char code[8])
{
    char d[6];
    int n = 0;
    for (const char* p = q; *p; p++) {
        if (isdigit((unsigned char)*p)) {
            if (n == 5) return false;
            d[n++] = *p;
        } else if (*p != '-' && *p != ' ') {
            return false;
        }
    }
    if (n != 5) return false;
    snprintf(code, 8, "%c%c-%c%c%c", d[0], d[1], d[2], d[3], d[4]);
    return true;
}

static void url_encode(const char* in, char* out, size_t size)
{
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p && o + 4 < size; p++) {
        if (isalnum(*p) || *p == '-' || *p == '.' || *p == '_') out[o++] = *p;
        else o += snprintf(out + o, size - o, "%%%02X", *p);
    }
    out[o] = '\0';
}

static const char* jstr(const cJSON* o, const char* k)
{
    const cJSON* v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

// Postcode → zippopotam.us (one place); name → Open-Meteo geocoding (up to SEARCH_MAX places)
static void run_search(const char* query)
{
    static const int BODY = 16384;
    char* body = (char*)heap_caps_malloc(BODY, MALLOC_CAP_SPIRAM);
    if (!body) {
        s_search = SEARCH_ERROR;
        return;
    }
    Place found[SEARCH_MAX];
    int n = 0, status;
    char code[8], url[256];
    bool pc = is_postcode(query, code);
    if (pc) {
        snprintf(url, sizeof(url), "http://api.zippopotam.us/pl/%s", code);
    } else {
        char q[160];
        url_encode(query, q, sizeof(q));
        snprintf(url, sizeof(url), "http://geocoding-api.open-meteo.com/v1/search?name=%s&count=%d&language=%s&format=json", q, SEARCH_MAX,
                 LANG_CODE);
    }
    status       = http_get(url, body, BODY);
    cJSON* root  = status == 200 ? cJSON_Parse(body) : NULL;
    cJSON* items = cJSON_GetObjectItem(root, pc ? "places" : "results");
    cJSON* it;
    cJSON_ArrayForEach(it, items)
    {
        if (n == SEARCH_MAX) break;
        Place& p = found[n];
        memset(&p, 0, sizeof(p));
        if (pc) {
            strlcpy(p.name, jstr(it, "place name"), sizeof(p.name));
            strlcpy(p.region, jstr(it, "state"), sizeof(p.region));
            strlcpy(p.country, TR("Polska", "Poland"), sizeof(p.country));
            strlcpy(p.postcode, code, sizeof(p.postcode));
            p.lat = strtof(jstr(it, "latitude"), NULL);
            p.lon = strtof(jstr(it, "longitude"), NULL);
        } else {
            strlcpy(p.name, jstr(it, "name"), sizeof(p.name));
            strlcpy(p.region, jstr(it, "admin1"), sizeof(p.region));
            strlcpy(p.country, jstr(it, "country"), sizeof(p.country));
            p.lat = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "latitude"));
            p.lon = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "longitude"));
        }
        if (p.name[0]) n++;
    }
    cJSON_Delete(root);
    free(body);
    if (!s_results) n = 0;
    taskENTER_CRITICAL(&s_lock);
    if (n) memcpy(s_results, found, sizeof(Place) * n);
    s_nresults = n;
    taskEXIT_CRITICAL(&s_lock);
    s_search = n ? SEARCH_OK : (status == 200 || status == 404) ? SEARCH_NOT_FOUND : SEARCH_ERROR;
    ESP_LOGI(TAG, "search \"%s\": %d places (HTTP %d)", query, n, status);
}

// "2026-11-01T06:42" → minutes after midnight, -1 when it is not a time
static int16_t iso_minutes(const cJSON* v)
{
    const char* s = cJSON_IsString(v) ? v->valuestring : "";
    if (strlen(s) < 16 || s[10] != 'T') return -1;
    return (int16_t)(atoi(s + 11) * 60 + atoi(s + 14));
}

static float day_value(const cJSON* daily, const char* key, int i, float fallback)
{
    const cJSON* v = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, key), i);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : fallback;
}

static bool fetch()
{
    char url[320];
    static const int BODY_SIZE = 8192;  // the forecast for a week (PSRAM: internal RAM is short)
    static char* body;
    if (!body) body = (char*)heap_caps_malloc(BODY_SIZE, MALLOC_CAP_SPIRAM);
    if (!body) return false;
    taskENTER_CRITICAL(&s_lock);
    float lat = s_loc.lat, lon = s_loc.lon;
    taskEXIT_CRITICAL(&s_lock);
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,pressure_msl,weather_code,is_day"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset&forecast_days=7&timezone=auto",
             lat, lon);

    bool ok    = false;
    int status = http_get(url, body, BODY_SIZE);
    if (status >= 0) {
        if (status == 200 && body[0]) {
            cJSON* root   = cJSON_Parse(body);
            cJSON* cur    = cJSON_GetObjectItem(root, "current");
            cJSON* temp   = cJSON_GetObjectItem(cur, "temperature_2m");
            cJSON* hum    = cJSON_GetObjectItem(cur, "relative_humidity_2m");
            cJSON* pres   = cJSON_GetObjectItem(cur, "pressure_msl");
            if (cJSON_IsNumber(temp) && cJSON_IsNumber(hum) && cJSON_IsNumber(pres)) {
                const cJSON* code  = cJSON_GetObjectItem(cur, "weather_code");
                const cJSON* day   = cJSON_GetObjectItem(cur, "is_day");
                const cJSON* daily = cJSON_GetObjectItem(root, "daily");
                WeatherData d      = {};
                d.temperature      = (float)temp->valuedouble;
                d.humidity         = (float)hum->valuedouble;
                d.pressure         = (float)pres->valuedouble;
                d.code             = cJSON_IsNumber(code) ? (int16_t)code->valueint : -1;
                d.is_day           = !cJSON_IsNumber(day) || day->valueint;
                const cJSON* dates = cJSON_GetObjectItem(daily, "time");  // "2026-11-01" each
                for (int i = 0; i < WEATHER_DAYS; i++) {
                    d.code_day[i] = (int16_t)day_value(daily, "weather_code", i, -1);
                    d.t_max[i]    = day_value(daily, "temperature_2m_max", i, d.temperature);
                    d.t_min[i]    = day_value(daily, "temperature_2m_min", i, d.temperature);
                    const char* ds = cJSON_GetStringValue(cJSON_GetArrayItem(dates, i));
                    int y, m, dd;
                    struct tm t = {};
                    if (ds && sscanf(ds, "%d-%d-%d", &y, &m, &dd) == 3) {
                        t.tm_year = y - 1900, t.tm_mon = m - 1, t.tm_mday = dd, t.tm_hour = 12, t.tm_isdst = -1;
                        mktime(&t);  // fills tm_wday
                        d.wday[i] = (int8_t)t.tm_wday;
                    } else {
                        d.wday[i] = -1;
                    }
                }
                d.sunrise = iso_minutes(cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "sunrise"), 0));
                d.sunset  = iso_minutes(cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "sunset"), 0));
                taskENTER_CRITICAL(&s_lock);
                d.version = s_data.version + 1;
                s_data    = d;
                taskEXIT_CRITICAL(&s_lock);
                ESP_LOGI(TAG, "%.1f °C, %.0f %%, %.0f hPa", temp->valuedouble, hum->valuedouble, pres->valuedouble);
                ok = true;
            } else {
                ESP_LOGW(TAG, "unexpected response");
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "HTTP %d", status);
        }
    } else {
        ESP_LOGW(TAG, "request failed");
    }
    return ok;
}

static void weather_task(void* arg)
{
    for (;;) {
        if (!wifi_connected()) {
            if (s_pc_state == POSTCODE_LOOKING) s_pc_state = POSTCODE_ERROR;
            if (s_search == SEARCH_LOOKING) s_search = SEARCH_ERROR;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
            continue;
        }
        if (s_pc_state == POSTCODE_LOOKING) {
            char code[8];
            taskENTER_CRITICAL(&s_lock);
            strlcpy(code, s_pending, sizeof(code));
            taskEXIT_CRITICAL(&s_lock);
            lookup_postcode(code);
        }
        if (s_search == SEARCH_LOOKING) {
            char q[64];
            taskENTER_CRITICAL(&s_lock);
            strlcpy(q, s_query, sizeof(q));
            taskEXIT_CRITICAL(&s_lock);
            run_search(q);
            if (s_search != SEARCH_LOOKING) {  // a new search may have come in meanwhile
                ulTaskNotifyTake(pdTRUE, 0);
                continue;
            }
        }
        // Sleeps until the next refresh, or until a new postal code wakes it up
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(fetch() ? REFRESH_MS : RETRY_MS));
    }
}

void weather_start(void)
{
    load_location();
    s_results = (Place*)heap_caps_calloc(SEARCH_MAX, sizeof(Place), MALLOC_CAP_SPIRAM);
    xTaskCreate(weather_task, "weather", 8192, NULL, 3, &s_task);
}

void weather_set_postcode(const char* postcode)
{
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_pending, postcode, sizeof(s_pending));
    taskEXIT_CRITICAL(&s_lock);
    s_pc_state = POSTCODE_LOOKING;
    if (s_task) xTaskNotifyGive(s_task);
}

void weather_search(const char* query)
{
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_query, query, sizeof(s_query));
    taskEXIT_CRITICAL(&s_lock);
    s_search = SEARCH_LOOKING;
    if (s_task) xTaskNotifyGive(s_task);
}

SearchState weather_search_state(void)
{
    return s_search;
}

int weather_search_results(Place* out, int max)
{
    taskENTER_CRITICAL(&s_lock);
    int n = s_nresults < max ? s_nresults : max;
    if (n) memcpy(out, s_results, sizeof(Place) * n);
    taskEXIT_CRITICAL(&s_lock);
    return n;
}

void weather_set_place(const Place& p)
{
    Location loc;
    strlcpy(loc.postcode, p.postcode, sizeof(loc.postcode));
    strlcpy(loc.place, p.name, sizeof(loc.place));
    loc.lat = p.lat, loc.lon = p.lon;
    taskENTER_CRITICAL(&s_lock);
    s_loc = loc;
    taskEXIT_CRITICAL(&s_lock);
    save_location();
    ESP_LOGI(TAG, "location: %s (%.4f, %.4f)", loc.place, loc.lat, loc.lon);
    s_search = SEARCH_IDLE;
    if (s_task) xTaskNotifyGive(s_task);  // weather for the new place right away
}

PostcodeState weather_postcode_state(void)
{
    return s_pc_state;
}

void weather_location(char postcode[8], char place[48])
{
    taskENTER_CRITICAL(&s_lock);
    strlcpy(postcode, s_loc.postcode, 8);
    strlcpy(place, s_loc.place, 48);
    taskEXIT_CRITICAL(&s_lock);
}

WeatherData weather_get(void)
{
    taskENTER_CRITICAL(&s_lock);
    WeatherData d = s_data;
    taskEXIT_CRITICAL(&s_lock);
    return d;
}
