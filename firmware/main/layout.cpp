#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <nvs.h>
#include "layout.h"

static const char* TAG = "layout";

#define TTF(sym)                                                                                                                  \
    extern const uint8_t sym##_start[] asm("_binary_" #sym "_ttf_start");                                                         \
    extern const uint8_t sym##_end[] asm("_binary_" #sym "_ttf_end");
TTF(inter)
TTF(inter_light)
TTF(orbitron)
TTF(roboto_mono)
TTF(bebas)
TTF(oswald)
TTF(rajdhani)
TTF(icons)

// Tab5 Studio (layout editor that compiles the firmware) can generate studio/layout.h: the default layout and extra
// fonts (studio/<name>.ttf) built into this firmware. Without it nothing changes.
#if __has_include("studio/layout.h")
#include "studio/layout.h"
#endif
#ifdef STUDIO_FONTS
#define STUDIO_TTF_DECL(sym, title) TTF(sym)
STUDIO_FONTS(STUDIO_TTF_DECL)
#endif

static const BuiltinFont BUILTIN[] = {
    {"inter", "Inter", inter_start, 0},
    {"inter_light", "Inter Light", inter_light_start, 0},
    {"orbitron", "Orbitron", orbitron_start, 0},
    {"roboto_mono", "Roboto Mono", roboto_mono_start, 0},
    {"bebas", "Bebas Neue", bebas_start, 0},
    {"oswald", "Oswald", oswald_start, 0},
    {"rajdhani", "Rajdhani Bold", rajdhani_start, 0},
#ifdef STUDIO_FONTS
#define STUDIO_FONT_ENTRY(sym, title) {#sym, title, sym##_start, 0},
    STUDIO_FONTS(STUDIO_FONT_ENTRY)
#endif
};
static const uint8_t* const BUILTIN_END[] = {inter_end, inter_light_end, orbitron_end, roboto_mono_end, bebas_end, oswald_end, rajdhani_end,
#ifdef STUDIO_FONTS
#define STUDIO_FONT_END(sym, title) sym##_end,
                                             STUDIO_FONTS(STUDIO_FONT_END)
#endif
};
static BuiltinFont s_builtin[sizeof(BUILTIN) / sizeof(BUILTIN[0])];
static const int NBUILTIN = sizeof(BUILTIN) / sizeof(BUILTIN[0]);

static LayoutText text(int x, int y, int size, bool visible, uint16_t icon, const char* unit)
{
    LayoutText t;
    memset(&t, 0, sizeof(t));
    t.x = x, t.y = y, t.size = size, t.visible = visible, t.icon = icon;
    strlcpy(t.font, "inter", sizeof(t.font));
    strlcpy(t.unit, unit, sizeof(t.unit));
    return t;
}

// Same look as the hand-made layout before the editor existed
static Layout make_default()
{
    Layout l;
    memset(&l, 0, sizeof(l));
    l.basic_clock     = {82, 112, 130, 1, 1, ""};
    l.basic_sensor[0] = text(90, 590, 64, true, 0xE186, "°C");   // thermometer
    l.basic_sensor[1] = text(480, 590, 64, true, 0xE0B8, "%");   // droplet
    l.basic_sensor[2] = text(870, 590, 64, true, 0xE1BF, "hPa");  // gauge
    l.basic_date      = text(64, 40, 30, false, 0, "");
    l.full_clock      = {103, 180, 150, 0, 1, ""};
    l.full_temp       = text(64, 42, 64, true, 0xE186, "°C");
    l.full_date       = text(64, 640, 30, false, 0, "");
#ifdef STUDIO_LAYOUT
    studio_layout(l);  // the layout designed in Tab5 Studio
#endif
    return l;
}

struct LayoutPreset {
    char name[32];
    Layout layout;
};

// All layouts live in PSRAM (allocated in layout_load): internal RAM is needed by the Wi-Fi driver at boot
struct LayoutStore {
    Layout def, saved, current;
    LayoutPreset presets[MAX_LAYOUT_PRESETS];
};
static LayoutStore* st;
#define s_default (st->def)
#define s_saved (st->saved)
#define s_current (st->current)
#define s_presets (st->presets)
static int s_preset_count;

static void presets_store()
{
    nvs_handle_t h;
    if (nvs_open("panel", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "lpresets2", st->presets, sizeof(LayoutPreset) * s_preset_count);
    nvs_commit(h);
    nvs_close(h);
}

int layout_presets_count(void) { return s_preset_count; }
const char* layout_preset_name(int i) { return i >= 0 && i < s_preset_count ? s_presets[i].name : NULL; }
const Layout* layout_preset(int i) { return i >= 0 && i < s_preset_count ? &s_presets[i].layout : NULL; }

bool layout_preset_store(int index, const char* name, const Layout& l)
{
    if (index < 0) {
        if (s_preset_count >= MAX_LAYOUT_PRESETS || !name || !name[0]) return false;
        index = s_preset_count++;
    } else if (index >= s_preset_count) {
        return false;
    }
    if (name && name[0]) strlcpy(s_presets[index].name, name, sizeof(s_presets[index].name));
    memcpy(&s_presets[index].layout, &l, sizeof(Layout));
    presets_store();
    return true;
}

bool layout_preset_rename(int i, const char* name)
{
    if (!layout_preset(i) || !name[0]) return false;
    strlcpy(s_presets[i].name, name, sizeof(s_presets[i].name));
    presets_store();
    return true;
}

bool layout_preset_delete(int i)
{
    if (!layout_preset(i)) return false;
    memmove(&s_presets[i], &s_presets[i + 1], sizeof(LayoutPreset) * (s_preset_count - i - 1));
    s_preset_count--;
    presets_store();
    return true;
}

void layout_load(void)
{
    st = (LayoutStore*)heap_caps_calloc(1, sizeof(LayoutStore), MALLOC_CAP_SPIRAM);
    for (int i = 0; i < NBUILTIN; i++) {
        s_builtin[i]      = BUILTIN[i];
        s_builtin[i].size = BUILTIN_END[i] - BUILTIN[i].data;
    }
    s_default = make_default();
    s_saved   = s_default;
    nvs_handle_t h;
    if (nvs_open("panel", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(Layout);
        Layout tmp;
        if (nvs_get_blob(h, "layout2", &tmp, &len) == ESP_OK && len == sizeof(Layout)) memcpy(&s_saved, &tmp, sizeof(Layout));
        len = sizeof(st->presets);
        if (nvs_get_blob(h, "lpresets2", st->presets, &len) == ESP_OK && len % sizeof(LayoutPreset) == 0)
            s_preset_count = len / sizeof(LayoutPreset);
        nvs_close(h);
    }
    s_current = s_saved;
}

Layout& layout_current(void) { return s_current; }
const Layout& layout_saved(void) { return s_saved; }
const Layout& layout_default(void) { return s_default; }
// Field by field: a byte compare would also see the padding between fields, which copies do not keep
static bool same(const LayoutClock& a, const LayoutClock& b)
{
    return a.x == b.x && a.y == b.y && a.size == b.size && a.seconds == b.seconds && a.visible == b.visible &&
           strcmp(a.font, b.font) == 0;
}

static bool same(const LayoutText& a, const LayoutText& b)
{
    return a.x == b.x && a.y == b.y && a.size == b.size && a.visible == b.visible && a.icon == b.icon &&
           strcmp(a.font, b.font) == 0 && a.icon_size == b.icon_size && a.unit_size == b.unit_size &&
           strcmp(a.unit, b.unit) == 0 && a.icon_color == b.icon_color && a.value_color == b.value_color &&
           a.unit_color == b.unit_color;
}

bool layout_is_live(void)
{
    const Layout &a = s_current, &b = s_saved;
    bool eq = same(a.basic_clock, b.basic_clock) && same(a.basic_date, b.basic_date) && same(a.full_clock, b.full_clock) &&
              same(a.full_temp, b.full_temp) && same(a.full_date, b.full_date);
    for (int k = 0; k < 3; k++) eq = eq && same(a.basic_sensor[k], b.basic_sensor[k]);
    return !eq;
}

void layout_set(const Layout& l, bool save)
{
    s_current = l;
    if (!save) return;
    s_saved = l;
    nvs_handle_t h;
    if (nvs_open("panel", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "layout2", &s_saved, sizeof(s_saved));
    nvs_commit(h);
    nvs_close(h);
}

int layout_builtin_fonts(const BuiltinFont** out)
{
    *out = s_builtin;
    return NBUILTIN;
}

/* ---------------------------------- Fonts ---------------------------------- */

// Font files from the SD card are read once into PSRAM
struct FileData {
    char name[32];
    uint8_t* data;
    size_t size;
};
static FileData s_files[6];

struct LiveFont {
    char name[32];
    const uint8_t* data;
    size_t size;
};
static LiveFont s_live[12];

bool layout_live_font_add(const char* name, const uint8_t* data, size_t size)
{
    for (LiveFont& f : s_live) {
        if (!f.data) {
            strlcpy(f.name, name, sizeof(f.name));
            f.data = data, f.size = size;
            return true;
        }
    }
    return false;
}

const uint8_t* layout_font_data(const char* name, size_t* size)
{
    for (const LiveFont& f : s_live) {
        if (f.data && strcmp(f.name, name) == 0) {
            *size = f.size;
            return f.data;
        }
    }
    for (int i = 0; i < NBUILTIN; i++) {
        if (strcmp(name, s_builtin[i].name) == 0) {
            *size = s_builtin[i].size;
            return s_builtin[i].data;
        }
    }
    if (strcmp(name, "icons") == 0) {
        *size = icons_end - icons_start;
        return icons_start;
    }
    if (strncmp(name, "sd:", 3) != 0 || strchr(name, '/') || strstr(name, "..")) return NULL;
    for (FileData& f : s_files)
        if (f.data && strcmp(f.name, name) == 0) {
            *size = f.size;
            return f.data;
        }
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", FONTS_DIR, name + 3);
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t* buf = len > 0 && len < 4 * 1024 * 1024 ? (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM) : NULL;
    if (buf && fread(buf, 1, len, fp) != (size_t)len) {
        free(buf);
        buf = NULL;
    }
    fclose(fp);
    if (!buf) return NULL;
    // Keep it; replace the oldest slot when all are taken
    static int next = 0;
    FileData& slot = s_files[next];
    next           = (next + 1) % 6;
    free(slot.data);
    strlcpy(slot.name, name, sizeof(slot.name));
    slot.data = buf;
    slot.size = len;
    ESP_LOGI(TAG, "font %s loaded (%ld bytes)", name, len);
    *size = len;
    return buf;
}

// LVGL fonts made by TinyTTF for each (name, size) in use
struct FontEntry {
    char name[32];
    int px;
    lv_font_t* font;
    uint32_t used;  // generation of the last request
    bool live;      // made from live scene data (freed by layout_live_fonts_clear)
    bool retired;   // live font still on screen, but no longer handed out (layout_live_fonts_retire)
};
static FontEntry s_fonts[24];
static uint32_t s_gen = 1;

lv_font_t* layout_font(const char* name, int px)
{
    if (px < 8) px = 8;
    if (px > 800) px = 800;
    for (FontEntry& e : s_fonts) {
        if (e.font && !e.retired && e.px == px && strcmp(e.name, name) == 0) {
            e.used = s_gen;
            return e.font;
        }
    }
    size_t size;
    const uint8_t* data = layout_font_data(name, &size);
    if (!data && strcmp(name, "inter") != 0) return layout_font("inter", px);
    FontEntry* slot = NULL;
    for (FontEntry& e : s_fonts)
        if (!e.font) slot = &e;
    if (!slot) {
        // All taken: reuse one not used by the current layout
        for (FontEntry& e : s_fonts)
            if (e.used != s_gen) slot = &e;
        if (!slot) return s_fonts[0].font;
        lv_tiny_ttf_destroy(slot->font);
        slot->font = NULL;
    }
    slot->font = lv_tiny_ttf_create_data_ex(data, size, px, LV_FONT_KERNING_NORMAL, 128);
    if (!slot->font) return NULL;
    strlcpy(slot->name, name, sizeof(slot->name));
    slot->px      = px;
    slot->used    = s_gen;
    slot->retired = false;
    slot->live    = false;
    for (const LiveFont& f : s_live)
        if (f.data && f.data == data) slot->live = true;
    return slot->font;
}

lv_font_t* layout_icon_font(int px)
{
    return layout_font("icons", px);
}

lv_font_t* layout_font_fixed(const char* name, int px)
{
    struct Fixed {
        const char* name;
        int px;
        lv_font_t* font;
    };
    static Fixed fixed[6];
    for (Fixed& f : fixed)
        if (f.font && f.px == px && strcmp(f.name, name) == 0) return f.font;
    size_t size;
    const uint8_t* data = layout_font_data(name, &size);
    lv_font_t* font     = data ? lv_tiny_ttf_create_data_ex(data, size, px, LV_FONT_KERNING_NORMAL, 32) : NULL;
    for (Fixed& f : fixed)
        if (!f.font && font) {
            f = {name, px, font};
            return font;
        }
    return font ? font : (lv_font_t*)LV_FONT_DEFAULT;
}

void layout_fonts_begin(void)
{
    s_gen++;
}

void layout_fonts_end(void)
{
    for (FontEntry& e : s_fonts) {
        if (e.font && e.used != s_gen) {
            lv_tiny_ttf_destroy(e.font);
            e.font = NULL;
        }
    }
}

void layout_live_fonts_retire(void)
{
    // The compiled look uses the same font names (f_..., studio_icons): without this it would get the live fonts
    // from the cache, and layout_live_fonts_clear would then free fonts that are on screen
    for (FontEntry& e : s_fonts)
        if (e.font && e.live) e.retired = true;
    for (LiveFont& f : s_live) f = {};
}

void layout_live_fonts_clear(void)
{
    // LVGL fonts made from live data must go before the data does
    for (FontEntry& e : s_fonts) {
        if (!e.font || !e.live) continue;
        lv_tiny_ttf_destroy(e.font);
        e = {};
    }
    for (LiveFont& f : s_live) f = {};
}
