#pragma once

#include <stdint.h>
#include <esp_http_server.h>

// Alarm clock: up to ALARM_MAX alarms (time, days of the week or once, label, sound from the SD card, volume),
// snooze, automatic stop after ALARM_RING_S. The sound is a built-in beep.
static const int ALARM_MAX    = 10;
static const int ALARM_RING_S = 600;

struct Alarm {
    uint8_t enabled;
    uint8_t hour, minute;
    uint8_t days;    // bit 0 = Monday … bit 6 = Sunday; 0 = once (switches itself off after ringing)
    uint8_t volume;  // 0–100
    uint8_t pad[3];
    char label[24];
    char sound[64];  // unused (sounds from the SD card were dropped), kept for the stored layout
};

void alarm_init(void);
void alarm_tick(void);     // LVGL context, twice a second
void alarm_snooze(void);   // LVGL context (buttons of the ringing screen)
void alarm_dismiss(void);  // LVGL context
bool alarm_ringing(void);
void alarm_ring_timer(void);  // LVGL context: the timer reached zero
int alarm_count(void);
bool alarm_indicator(int* hour, int* minute);  // any alarm on (or ringing / snoozing): true and the time it rings next
Alarm alarm_get(int i);
void alarm_set_enabled(int i, bool on);  // LVGL context (settings)
// Editing on the Tab5 (LVGL context, settings): each call saves the list
void alarm_set(int i, const Alarm& a);
int alarm_add(const Alarm& a);  // index of the new alarm, -1 when the list is full
void alarm_delete(int i);
int alarm_snooze_min(bool timer);         // snooze length of the alarms or of the timer
void alarm_set_snooze_min(bool timer, int min);
void alarm_register(httpd_handle_t server);
