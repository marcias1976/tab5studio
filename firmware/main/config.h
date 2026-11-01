#pragma once

#include <stdint.h>

// Panel settings changed from the web page, kept in NVS. Times are minutes after midnight.
struct PanelConfig {
    uint16_t night_start;   // dark mode window
    uint16_t night_end;
    bool off_enabled;       // screen-off window
    uint16_t off_start;
    uint16_t off_end;
    uint8_t brightness;     // day backlight, 10–100 %
    uint8_t cycle_min;      // change the clock face every N minutes, 0 = off
    uint8_t default_face;   // face shown after boot (0, 1, 2 = Wariant-3 when the design has one)
    bool unused_voice;      // was "Hi ESP" voice commands; kept so the settings saved in NVS keep their layout
    uint32_t digit_color;   // digital clock digits, 0xRRGGBB (new fields go at the end, see config_load)
    bool sensor_follow;     // sensor values in the digit color
    uint32_t sensor_color;  // otherwise this color, 0xRRGGBB
    int8_t day_preset;      // color preset switched on with light mode, -1 = none
    int8_t night_preset;    // and with dark mode
    uint8_t lang;           // device language: 0 Polish, 1 English (applied at the next start)
    int8_t night_face;      // clock face shown while the night window lasts (0–2), -1 = no change
};

// Named color sets saved from the web page
static const int MAX_PRESETS = 8;
struct ColorPreset {
    char name[32];          // UTF-8
    uint32_t digit_color;
    bool sensor_follow;
    uint32_t sensor_color;
};

// Loads the settings (initializing NVS); call once at boot before anything reads them
void config_load(void);
void config_save(void);

// Shared settings; written only with the display lock held
PanelConfig& config(void);

// Color presets (display lock held). Saving/renaming/deleting stores them in NVS; deleting keeps
// day_preset / night_preset pointing at the same presets (call config_save after it).
int presets_count(void);
const ColorPreset* preset(int index);  // NULL when out of range
bool preset_save(const char* name);    // current colors as a new preset; false when full
bool preset_overwrite(int index);      // current colors into an existing preset (keeps its name)
bool preset_rename(int index, const char* name);
bool preset_delete(int index);
bool preset_apply(int index);          // copies its colors into config(); the caller saves and repaints
