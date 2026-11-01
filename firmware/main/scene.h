#pragma once

#include <stdint.h>
#include <lvgl.h>

// Clock faces designed in Tab5 Studio, compiled into the firmware as data (studio/scene.h).
// When a build has a scene, the Basic and Full faces show it instead of the layout from the web editor;
// everything else (web page, Wi-Fi, settings, palette, dark mode) works as usual.

enum SceneType : uint8_t {
    SC_CLOCK_LED,   // seven-segment digits drawn like the built-in clock
    SC_CLOCK_TEXT,  // HH:MM in a font
    SC_SENSOR,      // weather value with optional unit
    SC_DATE,
    SC_TEXT,
    SC_ICON,        // glyph from the studio icon font
    SC_IMAGE,
    SC_RECT,
    SC_ELLIPSE,
    SC_LINE,
    SC_ANALOG,      // dial with hour, minute and (optional) second hands
    SC_PLACE,       // name of the weather place (set on the Tab5 by postcode)
    SC_LOADER,      // animation: source = style (0 bar filling over size2×100 ms, 1 dots, 2 spinner, 3 ring, 4 wave)
    SC_ALARM_LABEL, // alarm screen: name of the ringing alarm (text style like SC_TEXT)
    SC_TIMER_TIME,  // timer screen: time left, MM:SS
    SC_TIMER_GO,    // timer screen: "Start" / "Pauza"
    SC_ALARM_BADGE, // clock faces: icon and / or time of the next alarm, shown while any alarm is on (see below)
    SC_TIMER_BADGE, // clock faces: icon and / or time left, shown while the timer runs (hides the corner pill there)
    SC_WEATHER,     // weather icon (source 0 now, 1 tomorrow) or sunrise / sunset time (source 2 / 3)
    SC_FORECAST,    // one day of the forecast: day name, icon, max / min (source = day, 0 today … 6), see below
};
// Forecast: font + size + color = day name and max, size2 = icon px (0 = 1.6 × size), color2 = icon, color3 = min;
// w = width (contents centered); SC_UPPER = capital letters, SC_LONG = full day names
// Badges: source = what shows (0 icon and time, 1 icon, 2 time); icon, size2 = icon px (0 = like the text),
// font + size + color = the time, color2 = the icon

// What a tap on an element does (alarm and timer screens); action_val = minutes for SA_TIMER_SET
enum SceneAction : uint8_t {
    SA_NONE,
    SA_SNOOZE,
    SA_STOP,
    SA_TIMER_GO,     // start / pause
    SA_TIMER_RESET,
    SA_TIMER_PLUS,   // +1 min
    SA_TIMER_MINUS,  // −1 min
    SA_TIMER_PLUS_S, // +5 s
    SA_TIMER_MINUS_S,
    SA_TIMER_SET,    // action_val minutes
    SA_TIMER_CLOSE,
};

// flags
#define SC_SECONDS 0x01   // clocks: show seconds (analog: second hand)
#define SC_UNLIT 0x02     // LED clock: draw the unlit segments faintly
#define SC_UPPER 0x04     // date, place: capital letters
#define SC_ROUND 0x08     // line: rounded ends
#define SC_LOOP 0x10      // loader bar: start again when full
#define SC_LONG 0x20      // forecast: full day names ("Poniedziałek" instead of "Pn")

// Colors: 0 = automatic (palette: digit color for clocks, value color for sensors, gray for the rest),
// otherwise SC_COLOR | 0xRRGGBB
#define SC_COLOR 0x01000000u

struct SceneEl {
    uint8_t type;
    uint8_t opa;      // 0–255
    uint8_t flags;
    uint8_t source;   // sensor: 0 temperature, 1 humidity, 2 pressure, 3 / 4 max / min today, 5 / 6 max / min tomorrow;
                      // date: format 0–3; analog: marks 0 none, 1 hours, 2 hours + minutes
    int16_t x, y;
    int16_t w, h;     // box size; line: end point relative to x, y
    uint16_t size;    // font px, LED digit height, line or border width
    uint16_t size2;   // unit font px, corner radius; analog: rim width
    uint32_t color;   // main / fill color
    uint32_t color2;  // unit / border color
    uint8_t decimals; // sensor
    uint8_t align;    // text: 0 left, 1 center, 2 right (inside w)
    uint16_t icon;    // codepoint in the studio icon font
    const char* font;
    const char* text; // text, or the unit of a sensor
    const lv_image_dsc_t* img;
    uint32_t color3;  // analog: marks and rim
    uint32_t color4;  // analog: dial fill (0 = none)
    uint8_t action;   // SceneAction on tap
    uint8_t action_val;
};

struct SceneFace {
    uint32_t bg;      // background 0xRRGGBB
    const SceneEl* els;
    int count;
    int ms;           // intro: how long it shows (0 = 4 s)
};
