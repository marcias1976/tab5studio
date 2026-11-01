#pragma once

#include <stdint.h>

static const int WEATHER_DAYS = 7;

// Outdoor conditions from Open-Meteo (temporary source until Home Assistant sensors are wired in)
struct WeatherData {
    float temperature;  // °C
    float humidity;     // %
    float pressure;     // hPa, reduced to sea level
    uint32_t version;   // increments on every successful update, 0 = no data yet
    int16_t code;       // WMO weather code now (-1 = unknown)
    bool is_day;
    // Daily forecast: [0] today, [1] tomorrow … [6]
    int16_t code_day[WEATHER_DAYS];          // WMO code (-1 = unknown)
    float t_max[WEATHER_DAYS], t_min[WEATHER_DAYS];  // °C
    int8_t wday[WEATHER_DAYS];               // day of the week, 0 = Sunday (-1 = unknown)
    int16_t sunrise, sunset;   // minutes after midnight today (-1 = unknown)
};

// Starts a background task that refreshes the data every 10 minutes once Wi-Fi is up
void weather_start(void);

WeatherData weather_get(void);

// Location by Polish postal code ("00-950"), looked up online and kept in NVS
enum PostcodeState { POSTCODE_IDLE, POSTCODE_LOOKING, POSTCODE_OK, POSTCODE_NOT_FOUND, POSTCODE_ERROR };
void weather_set_postcode(const char* postcode);
PostcodeState weather_postcode_state(void);
void weather_location(char postcode[8], char place[48]);

// Place search: a Polish postcode ("00-950") or a place name anywhere ("Berlin"), in the program's language.
// Runs in the weather task; poll weather_search_state(), then read the results and pass the chosen one to weather_set_place().
struct Place {
    char name[48];
    char region[48];
    char country[48];
    char postcode[8];  // "" when found by name
    float lat, lon;
};
enum SearchState { SEARCH_IDLE, SEARCH_LOOKING, SEARCH_OK, SEARCH_NOT_FOUND, SEARCH_ERROR };
static const int SEARCH_MAX = 8;
void weather_search(const char* query);
SearchState weather_search_state(void);
int weather_search_results(Place* out, int max);
void weather_set_place(const Place& p);  // also refreshes the weather
