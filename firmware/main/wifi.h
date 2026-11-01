#pragma once

#include <stdint.h>

// Wi-Fi station on the ESP32-C6 (SDIO via esp_hosted). Up to WIFI_MAX_SAVED networks are kept
// in NVS (most recently used first) and tried in turn; the one from wifi_secrets.h seeds the
// list on first boot. Reconnects automatically and keeps the clock in sync over NTP.
// on_time_sync runs (in the SNTP task) after every successful sync.
void wifi_start(void (*on_time_sync)(void));

bool wifi_connected(void);

// Signal strength of the current connection in dBm (0 when not connected) and its IP address ("" when none)
int wifi_rssi(void);
void wifi_ip(char out[16]);

static const int WIFI_MAX_SAVED = 5;

struct WifiAp {
    char ssid[33];
    int8_t rssi;
    bool open;  // no password needed
};

// Saved networks, most recently used first. Returns the count.
int wifi_saved(char out[WIFI_MAX_SAVED][33]);
void wifi_forget(int index);

// SSID of the current connection ("" when not connected)
void wifi_current(char out[33]);

// Tries a network now; it is saved (at the top of the list) once it connects.
// If it fails, the saved networks are tried again.
void wifi_join(const char* ssid, const char* password);

enum WifiJoinState { WIFI_JOIN_IDLE, WIFI_JOIN_TRYING, WIFI_JOIN_OK, WIFI_JOIN_FAILED };
WifiJoinState wifi_join_state(void);

// Asynchronous scan; wifi_scan_results returns -1 while it is running, else the number of
// networks (strongest first, duplicates merged)
bool wifi_scan_start(void);
int wifi_scan_results(WifiAp* out, int max);
