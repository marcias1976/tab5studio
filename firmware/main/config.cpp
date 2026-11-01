#include <nvs.h>
#include <nvs_flash.h>
#include <initializer_list>
#include <string.h>
#include "config.h"
#include "lang.h"

// Default colors chosen in Tab5 Studio (studio/colors.h, generated for a build)
#if __has_include("studio/colors.h")
#include "studio/colors.h"
#endif
#ifndef STUDIO_DIGIT_COLOR
#define STUDIO_DIGIT_COLOR 0x5BE7E1
#define STUDIO_SENSOR_FOLLOW true
#define STUDIO_SENSOR_COLOR 0xF2F3F5
#endif

static PanelConfig s_cfg = {
    .night_start  = 23 * 60,
    .night_end    = 5 * 60 + 40,
    .off_enabled  = false,
    .off_start    = 1 * 60,
    .off_end      = 5 * 60,
    .brightness   = 100,
    .cycle_min    = 0,
    .default_face = 0,
    .unused_voice = false,
    .digit_color  = STUDIO_DIGIT_COLOR,
    .sensor_follow = STUDIO_SENSOR_FOLLOW,
    .sensor_color = STUDIO_SENSOR_COLOR,
    .day_preset   = -1,
    .night_preset = -1,
    .lang         = LANG_DEFAULT_EN,
    .night_face   = -1,
};

bool g_lang_en = LANG_DEFAULT_EN;

static ColorPreset s_presets[MAX_PRESETS];
static int s_preset_count;

static void presets_store()
{
    nvs_handle_t h;
    if (nvs_open("panel", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "presets", s_presets, sizeof(ColorPreset) * s_preset_count);
    nvs_commit(h);
    nvs_close(h);
}

void config_load(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    nvs_handle_t h;
    g_lang_en = s_cfg.lang == 1;
    if (nvs_open("panel", NVS_READONLY, &h) != ESP_OK) return;
    // Saved settings may be shorter (older firmware: newer fields keep their defaults) or longer (fields that were
    // removed at the end): read into a roomy buffer and take the common start
    uint8_t raw[256];
    size_t len = sizeof(raw);
    if (nvs_get_blob(h, "cfg", raw, &len) == ESP_OK) memcpy(&s_cfg, raw, len < sizeof(s_cfg) ? len : sizeof(s_cfg));
    if (s_cfg.default_face > 2) s_cfg.default_face = 0;  // 2 = Wariant-3 (falls back to the first face without it)
    if (s_cfg.night_face < -1 || s_cfg.night_face > 2) s_cfg.night_face = -1;
    if (s_cfg.lang > 1) s_cfg.lang = LANG_DEFAULT_EN;
    len = sizeof(s_presets);
    if (nvs_get_blob(h, "presets", s_presets, &len) == ESP_OK) s_preset_count = len / sizeof(ColorPreset);
    nvs_close(h);
    g_lang_en = s_cfg.lang == 1;
}

void config_save(void)
{
    nvs_handle_t h;
    if (nvs_open("panel", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "cfg", &s_cfg, sizeof(s_cfg));
    nvs_commit(h);
    nvs_close(h);
}

PanelConfig& config(void)
{
    return s_cfg;
}

int presets_count(void)
{
    return s_preset_count;
}

const ColorPreset* preset(int index)
{
    return index >= 0 && index < s_preset_count ? &s_presets[index] : NULL;
}

bool preset_save(const char* name)
{
    if (s_preset_count >= MAX_PRESETS || !name[0]) return false;
    ColorPreset& p = s_presets[s_preset_count++];
    strlcpy(p.name, name, sizeof(p.name));
    p.digit_color   = s_cfg.digit_color;
    p.sensor_follow = s_cfg.sensor_follow;
    p.sensor_color  = s_cfg.sensor_color;
    presets_store();
    return true;
}

bool preset_rename(int index, const char* name)
{
    if (!preset(index) || !name[0]) return false;
    strlcpy(s_presets[index].name, name, sizeof(s_presets[index].name));
    presets_store();
    return true;
}

bool preset_delete(int index)
{
    if (!preset(index)) return false;
    memmove(&s_presets[index], &s_presets[index + 1], sizeof(ColorPreset) * (s_preset_count - index - 1));
    s_preset_count--;
    for (int8_t* ref : {&s_cfg.day_preset, &s_cfg.night_preset}) {
        if (*ref == index) *ref = -1;
        else if (*ref > index) (*ref)--;
    }
    presets_store();
    return true;
}

bool preset_apply(int index)
{
    const ColorPreset* p = preset(index);
    if (!p) return false;
    s_cfg.digit_color   = p->digit_color;
    s_cfg.sensor_follow = p->sensor_follow;
    s_cfg.sensor_color  = p->sensor_color;
    return true;
}

bool preset_overwrite(int index)
{
    if (!preset(index)) return false;
    ColorPreset& p  = s_presets[index];
    p.digit_color   = s_cfg.digit_color;
    p.sensor_follow = s_cfg.sensor_follow;
    p.sensor_color  = s_cfg.sensor_color;
    presets_store();
    return true;
}
