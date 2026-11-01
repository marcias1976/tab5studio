#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <nvs.h>
#include <nvs_flash.h>
#include "wifi.h"
#include "wifi_secrets.h"

static const char* TAG = "wifi";

static void (*s_on_time_sync)(void);
static volatile bool s_connected;

// Everything below is shared between the event task and the UI, guarded by s_lock
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_ssid[WIFI_MAX_SAVED][33];
static char s_pass[WIFI_MAX_SAVED][65];
static int s_count;
static int s_try;                // saved network being tried / used
static bool s_joining;           // trying a new network instead of the saved ones
static int s_join_attempts;
static char s_join_ssid[33];
static char s_join_pass[65];
static WifiJoinState s_join_state = WIFI_JOIN_IDLE;
static char s_current[33];
static char s_ip[16];
static bool s_scanning;
static bool s_scan_ready;
static WifiAp s_scan[20];
static int s_scan_count;

static esp_timer_handle_t s_retry_timer;
static const int RETRY_DELAY_MS = 5000;  // pause after every saved network failed

/* ---------------------------------- NVS ---------------------------------- */

static void save_list()
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    char key[12];
    nvs_set_u8(h, "n", (uint8_t)s_count);
    for (int i = 0; i < s_count; i++) {
        snprintf(key, sizeof(key), "s%d", i);
        nvs_set_str(h, key, s_ssid[i]);
        snprintf(key, sizeof(key), "p%d", i);
        nvs_set_str(h, key, s_pass[i]);
    }
    nvs_commit(h);
    nvs_close(h);
}

// The network built into the program (wifi_secrets.h) joins the saved list once per program that brings
// a new one, so an update that keeps the settings still adds it. A network already saved under that name
// is left alone: a password typed on the device always wins. NVS keeps a hash of the last built-in
// network, so a network the user removed does not come back on every boot.
static void add_built_in()
{
    if (!WIFI_SSID[0]) return;
    uint32_t hash = 2166136261u;  // FNV-1a of "ssid\0password"
    for (const char* p = WIFI_SSID; ; p++) { hash = (hash ^ (uint8_t)*p) * 16777619u; if (!*p) break; }
    for (const char* p = WIFI_PASSWORD; *p; p++) hash = (hash ^ (uint8_t)*p) * 16777619u;

    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t seen = 0;
    if (nvs_get_u32(h, "seed", &seen) != ESP_OK || seen != hash) {
        bool saved = false;
        for (int i = 0; i < s_count; i++)
            if (strcmp(s_ssid[i], WIFI_SSID) == 0) saved = true;
        if (!saved) {
            int at = s_count < WIFI_MAX_SAVED ? s_count++ : WIFI_MAX_SAVED - 1;  // full: replaces the last one
            strlcpy(s_ssid[at], WIFI_SSID, sizeof(s_ssid[0]));
            strlcpy(s_pass[at], WIFI_PASSWORD, sizeof(s_pass[0]));
            save_list();
            ESP_LOGI(TAG, "built-in network \"%s\" added to the saved list", WIFI_SSID);
        }
        nvs_set_u32(h, "seed", hash);
        nvs_commit(h);
    }
    nvs_close(h);
}

static void load_list()
{
    nvs_handle_t h;
    uint8_t n = 0;
    bool found = false;
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
        found = nvs_get_u8(h, "n", &n) == ESP_OK;
        char key[12];
        for (int i = 0; i < n && i < WIFI_MAX_SAVED; i++) {
            size_t ls = sizeof(s_ssid[0]), lp = sizeof(s_pass[0]);
            snprintf(key, sizeof(key), "s%d", i);
            if (nvs_get_str(h, key, s_ssid[s_count], &ls) != ESP_OK) continue;
            snprintf(key, sizeof(key), "p%d", i);
            if (nvs_get_str(h, key, s_pass[s_count], &lp) != ESP_OK) s_pass[s_count][0] = '\0';
            s_count++;
        }
        nvs_close(h);
    }
    if (!found) {  // first boot: start with the network from wifi_secrets.h
        strlcpy(s_ssid[0], WIFI_SSID, sizeof(s_ssid[0]));
        strlcpy(s_pass[0], WIFI_PASSWORD, sizeof(s_pass[0]));
        s_count = 1;
        save_list();
    }
    add_built_in();
}

// Puts a network at the top of the list (adding it, or moving it up), dropping the oldest beyond 5.
// Call with s_lock held; the caller saves.
static void put_first(const char* ssid, const char* pass)
{
    int at = s_count;
    for (int i = 0; i < s_count; i++)
        if (strcmp(s_ssid[i], ssid) == 0) at = i;
    if (at == s_count) at = s_count < WIFI_MAX_SAVED ? s_count++ : WIFI_MAX_SAVED - 1;
    for (int i = at; i > 0; i--) {
        memcpy(s_ssid[i], s_ssid[i - 1], sizeof(s_ssid[0]));
        memcpy(s_pass[i], s_pass[i - 1], sizeof(s_pass[0]));
    }
    strlcpy(s_ssid[0], ssid, sizeof(s_ssid[0]));
    strlcpy(s_pass[0], pass, sizeof(s_pass[0]));
}

/* ------------------------------- Connecting ------------------------------ */

// Configures the station for the current target (new network or saved s_try) and connects
static void connect_target()
{
    wifi_config_t cfg = {};
    taskENTER_CRITICAL(&s_lock);
    bool have = s_joining || s_count > 0;
    if (s_joining) {
        strlcpy((char*)cfg.sta.ssid, s_join_ssid, sizeof(cfg.sta.ssid));
        strlcpy((char*)cfg.sta.password, s_join_pass, sizeof(cfg.sta.password));
    } else if (s_count > 0) {
        if (s_try >= s_count) s_try = 0;
        strlcpy((char*)cfg.sta.ssid, s_ssid[s_try], sizeof(cfg.sta.ssid));
        strlcpy((char*)cfg.sta.password, s_pass[s_try], sizeof(cfg.sta.password));
    }
    bool scanning = s_scanning;
    taskEXIT_CRITICAL(&s_lock);
    if (!have || scanning) return;

    cfg.sta.threshold.authmode = cfg.sta.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable    = true;
    ESP_LOGI(TAG, "connecting to \"%s\"", (char*)cfg.sta.ssid);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();
}

static void on_retry_timer(void* arg)
{
    if (!s_connected) connect_target();
}

static void on_disconnected(int reason)
{
    s_connected = false;
    bool now = true;
    taskENTER_CRITICAL(&s_lock);
    s_current[0] = '\0';
    s_ip[0]      = '\0';
    if (s_scanning) {
        now = false;  // reconnects after the scan
    } else if (s_joining) {
        if (++s_join_attempts >= 3) {  // the new network did not work: back to the saved ones
            s_joining    = false;
            s_join_state = WIFI_JOIN_FAILED;
            s_try        = 0;
        }
    } else if (s_count > 0) {
        s_try = (s_try + 1) % s_count;
        now   = s_try != 0;  // after a full round wait a moment
    }
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGW(TAG, "disconnected (reason %d)", reason);
    if (now) {
        connect_target();
    } else if (!s_scanning) {
        esp_timer_start_once(s_retry_timer, RETRY_DELAY_MS * 1000ULL);
    }
}

static void on_got_ip()
{
    wifi_config_t cfg = {};
    esp_wifi_get_config(WIFI_IF_STA, &cfg);
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_current, (char*)cfg.sta.ssid, sizeof(s_current));
    if (s_joining) {
        s_joining    = false;
        s_join_state = WIFI_JOIN_OK;
    }
    put_first((char*)cfg.sta.ssid, (char*)cfg.sta.password);  // most recently used first
    s_try = 0;
    taskEXIT_CRITICAL(&s_lock);
    save_list();
}

static void on_scan_done()
{
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    static wifi_ap_record_t recs[30];
    uint16_t got = n < 30 ? n : 30;
    if (esp_wifi_scan_get_ap_records(&got, recs) != ESP_OK) got = 0;
    taskENTER_CRITICAL(&s_lock);
    s_scan_count = 0;
    for (int i = 0; i < got && s_scan_count < 20; i++) {  // records come sorted by signal
        const char* ssid = (const char*)recs[i].ssid;
        if (!ssid[0]) continue;  // hidden network
        bool dup = false;
        for (int k = 0; k < s_scan_count; k++)
            if (strcmp(s_scan[k].ssid, ssid) == 0) dup = true;
        if (dup) continue;
        WifiAp& ap = s_scan[s_scan_count++];
        strlcpy(ap.ssid, ssid, sizeof(ap.ssid));
        ap.rssi = recs[i].rssi;
        ap.open = recs[i].authmode == WIFI_AUTH_OPEN;
    }
    s_scanning   = false;
    s_scan_ready = true;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "scan: %d networks", s_scan_count);
    if (!s_connected) connect_target();
}

static void on_event(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        connect_target();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        on_disconnected(static_cast<wifi_event_sta_disconnected_t*>(data)->reason);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        on_scan_done();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* ev = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(TAG, "connected, ip " IPSTR, IP2STR(&ev->ip_info.ip));
        taskENTER_CRITICAL(&s_lock);
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        taskEXIT_CRITICAL(&s_lock);
        on_got_ip();
        s_connected = true;
        esp_netif_sntp_start();
    }
}

static void on_sntp_sync(struct timeval* tv)
{
    ESP_LOGI(TAG, "time synced over NTP");
    if (s_on_time_sync) s_on_time_sync();
}

void wifi_start(void (*on_time_sync)(void))
{
    s_on_time_sync = on_time_sync;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    load_list();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    esp_timer_create_args_t timer = {};
    timer.callback                = on_retry_timer;
    timer.name                    = "wifi_retry";
    ESP_ERROR_CHECK(esp_timer_create(&timer, &s_retry_timer));

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // SNTP is started on every IP acquisition (see on_event) and then re-syncs periodically
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp.start             = false;
    sntp.sync_cb           = on_sntp_sync;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp));

    ESP_ERROR_CHECK(esp_wifi_start());
}

bool wifi_connected(void)
{
    return s_connected;
}

int wifi_saved(char out[WIFI_MAX_SAVED][33])
{
    taskENTER_CRITICAL(&s_lock);
    int n = s_count;
    memcpy(out, s_ssid, sizeof(s_ssid));
    taskEXIT_CRITICAL(&s_lock);
    return n;
}

void wifi_forget(int index)
{
    taskENTER_CRITICAL(&s_lock);
    if (index >= 0 && index < s_count) {
        for (int i = index; i < s_count - 1; i++) {
            memcpy(s_ssid[i], s_ssid[i + 1], sizeof(s_ssid[0]));
            memcpy(s_pass[i], s_pass[i + 1], sizeof(s_pass[0]));
        }
        s_count--;
        if (s_try >= s_count) s_try = 0;
    }
    taskEXIT_CRITICAL(&s_lock);
    save_list();
}

void wifi_current(char out[33])
{
    taskENTER_CRITICAL(&s_lock);
    strlcpy(out, s_current, 33);
    taskEXIT_CRITICAL(&s_lock);
}

void wifi_join(const char* ssid, const char* password)
{
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_join_ssid, ssid, sizeof(s_join_ssid));
    strlcpy(s_join_pass, password, sizeof(s_join_pass));
    s_joining       = true;
    s_join_attempts = s_connected ? -1 : 0;  // leaving the current network does not count as a failure
    s_join_state    = WIFI_JOIN_TRYING;
    taskEXIT_CRITICAL(&s_lock);
    esp_timer_stop(s_retry_timer);
    bool was_connected = s_connected;
    esp_wifi_disconnect();  // when connected, the disconnect event connects to the new network
    if (!was_connected) connect_target();
}

WifiJoinState wifi_join_state(void)
{
    return s_join_state;
}

bool wifi_scan_start(void)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_scanning) {
        taskEXIT_CRITICAL(&s_lock);
        return true;
    }
    s_scanning   = true;
    s_scan_ready = false;
    taskEXIT_CRITICAL(&s_lock);
    esp_timer_stop(s_retry_timer);
    if (!s_connected) esp_wifi_disconnect();  // a connection attempt in progress would block the scan
    wifi_scan_config_t cfg = {};
    cfg.show_hidden        = false;
    esp_err_t err          = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        taskENTER_CRITICAL(&s_lock);
        s_scanning = false;
        taskEXIT_CRITICAL(&s_lock);
        if (!s_connected) connect_target();
        return false;
    }
    return true;
}

int wifi_scan_results(WifiAp* out, int max)
{
    taskENTER_CRITICAL(&s_lock);
    int n = -1;
    if (s_scan_ready) {
        n = s_scan_count < max ? s_scan_count : max;
        memcpy(out, s_scan, n * sizeof(WifiAp));
    }
    taskEXIT_CRITICAL(&s_lock);
    return n;
}

int wifi_rssi(void)
{
    wifi_ap_record_t ap;
    if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

void wifi_ip(char out[16])
{
    taskENTER_CRITICAL(&s_lock);
    strlcpy(out, s_ip, 16);
    taskEXIT_CRITICAL(&s_lock);
}
