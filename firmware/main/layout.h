#pragma once

#include <stdint.h>
#include <lvgl.h>

// Clock face layout designed in the web editor: where each element is, how big, which font and icon.
// Coordinates are screen pixels (1280×720), top-left corner of the element.

// Folder for the panel's own files on the SD card (custom fonts in "Czcionki")
#define SYSTEM_DIR "/sdcard/Tab5_system"
#define FONTS_DIR SYSTEM_DIR "/Czcionki"

// Colors: 0 = automatic (palette / theme), otherwise LAYOUT_COLOR | 0xRRGGBB
#define LAYOUT_COLOR 0x01000000u

struct LayoutClock {
    int16_t x, y;
    uint16_t size;   // scale in percent (100 = 280 px high digits)
    uint8_t seconds; // small seconds after HH:MM
    uint8_t visible;
    char font[32];   // "" = LED segments, otherwise a font name as in LayoutText
};

struct LayoutText {
    int16_t x, y;
    uint16_t size;   // value font size in px
    uint8_t visible;
    char font[32];   // built-in font name, or "sd:<file>.ttf" from FONTS_DIR
    uint16_t icon;   // Lucide codepoint, 0 = no icon
    uint16_t icon_size;   // px, 0 = 87 % of size
    uint16_t unit_size;   // px, 0 = 47 % of size
    char unit[12];        // "°C", "%", "hPa" or anything else
    uint32_t icon_color;  // see LAYOUT_COLOR
    uint32_t value_color;
    uint32_t unit_color;
};

struct Layout {
    LayoutClock basic_clock;
    LayoutText basic_sensor[3];  // temperature, humidity, pressure
    LayoutText basic_date;
    LayoutClock full_clock;
    LayoutText full_temp;
    LayoutText full_date;
};

// Loads the saved layout (NVS); call once at boot
void layout_load(void);

// Layout on screen (may be a not yet saved "LIVE" preview) and the saved one
Layout& layout_current(void);
const Layout& layout_saved(void);
const Layout& layout_default(void);

// Makes `l` the layout on screen; with `save` it is also stored in NVS (display lock held, then ui_layout_changed)
void layout_set(const Layout& l, bool save);
bool layout_is_live(void);  // the screen shows an unsaved layout

// Named layouts saved from the web editor (NVS); all functions with the display lock held
static const int MAX_LAYOUT_PRESETS = 8;
int layout_presets_count(void);
const char* layout_preset_name(int index);
const Layout* layout_preset(int index);
// index -1 = new preset (false when full); otherwise overwrites it (name NULL or "" keeps the old name)
bool layout_preset_store(int index, const char* name, const Layout& l);
bool layout_preset_rename(int index, const char* name);
bool layout_preset_delete(int index);

// Built-in fonts (also served to the web editor)
struct BuiltinFont {
    const char* name;
    const char* title;
    const uint8_t* data;
    size_t size;
};
int layout_builtin_fonts(const BuiltinFont** out);

// Fonts sent with a live scene from Tab5 Studio (data must stay valid until layout_live_fonts_clear)
bool layout_live_font_add(const char* name, const uint8_t* data, size_t size);
void layout_live_fonts_retire(void);  // before the views are rebuilt: live fonts are no longer handed out
void layout_live_fonts_clear(void);   // after the live scene's labels are gone

// Font file contents by name (built-in, live from Studio or from the SD card); NULL when missing
const uint8_t* layout_font_data(const char* name, size_t* size);

// LVGL font for a name and pixel size, cached; falls back to the built-in Inter
lv_font_t* layout_font(const char* name, int px);
lv_font_t* layout_icon_font(int px);
// The same, made once and never freed: for the screens built once (menu, timer), which the rebuilds of the
// clock faces do not touch (layout_fonts_end() frees cached fonts the faces no longer use). name: a built-in font
lv_font_t* layout_font_fixed(const char* name, int px);
// Frees fonts not requested since the last layout_fonts_begin() (call around a rebuild)
void layout_fonts_begin(void);
void layout_fonts_end(void);
