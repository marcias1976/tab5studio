# Tab5 firmware (firmware/)

ESP-IDF 5.4.2, LVGL 9.2.2, M5Stack Tab5 (ESP32-P4, 16 MB flash, PSRAM). Wi-Fi goes through the ESP32-C6 (`esp_hosted`). The display is 1280×720: the panel is physically portrait and rotated 90°.

## Build and flash by hand

```bash
idf.py build
idf.py -p <port> flash          # bootloader, partition table, otadata and app (ota_0); NVS and the saved design stay
idf.py -p <port> app-flash      # app only, into ota_0
```

`app-flash` does not touch otadata: after an update over Wi-Fi the Tab5 may boot from ota_1 and ignore it, so prefer `flash`.

The secrets are `main/wifi_secrets.h` (`WIFI_SSID`, `WIFI_PASSWORD`) and `main/web_secrets.h` (`WEB_PIN`). Copy them from `*.example`.

## Files (main/)

| File | Role |
|---|---|
| `main.cpp` | Startup order: config, layout and alarms (NVS) → RTC time → display + LVGL → saved design (`livescene_boot_early`) → UI → Wi-Fi → weather → web → SD card → sound (`audio_start()`). `uiwatch_mark()` after each step records the free internal RAM (`boot_heap` in `/api/state`). `app_main` never returns, because cleanup after it crashed from PSRAM. |
| `ui.cpp` / `ui.h` | Everything on screen. |
| `settings.cpp` | The on-device settings screen with a big "Back" button, a gear icon and 6 tabs of equal full-height panels (`column_panel`) and "label … control" rows (`setting_row`): Wi-Fi (saved networks, scan and join), Weather (place search and results), Clock (theme, mode, rotation, colors), Screen (brightness, off, intro, night mode, screen-off hours), Alarms (list, editor via `alarm_set` / `alarm_add` / `alarm_delete`, snooze), Other (language, restart, program version). Color presets and alarm names / volume are web-page only. The bitmap fonts have no typographic quotes or dashes: use plain ones in menu texts. |
| `config.cpp` / `config.h` | The `PanelConfig` struct in NVS (`panel/cfg`): brightness, night and off hours, colours and presets, theme cycling, default and night face, `lang`. It is stored as a raw blob and restored from Studio backups: never remove or reorder fields, add new ones at the end (a retired field stays as `unused_voice`). `config_load()` sets `g_lang_en`. |
| `web.cpp` | HTTP server and JSON API (see API.md). PIN check with lockout (5 wrong tries in 5 min → 30 s of 429). OTA (`/api/ota`), program backup (`/api/backup`), settings backup and restore (`/api/settings/backup` / `restore`, NVS blobs from the `BACKUP_ITEMS` whitelist). Registers the `files`, `livescene` and `alarm` handlers. `max_uri_handlers` = 40 (35 in use). |
| `web/index.html` | The device web page, embedded with `EMBED_TXTFILES`: five tabs (`.tab[data-tab]`, remembered in `localStorage` and the address hash), two columns from 900 px. Vanilla JS with PL/EN via the `EN` dict and `EN_RE` rules. Screenshots for the help: `scripts/web_shots.js`. |
| `livescene.cpp` | Receives the live preview from Studio: blobs in PSRAM, up to 6 faces (2 themes, intro, alarm, timer, Wariant-3). Saves the scene for good (see “Saved design”) and loads it at every start. |
| `version.h` | The program version is the Studio build: `STUDIO_BUILD` (build ID, `build` in `/api/state`), `STUDIO_BUILD_NAME` (its date and time, "01.11.2026 10:01") and `STUDIO_BUILD_NOTE` (the note typed in Studio, `build_note`), from `studio/build.h` written by `server.py` (`"dev"` without it). Shown in Settings → Other → Program version, on the web page and in Studio. |
| `uiwatch.cpp` | Screen watchdog, crash breadcrumbs, boot memory marks and the OTA "good program" mark (see below). Adds `heap`, `boot_heap`, `app`, `last_crash` and `last_freeze` to `/api/state`. |
| `scene.h` | `SceneType`, `SceneAction`, flags, `SceneEl`, `SceneFace`. This is the contract with `server.py`. |
| `alarm.cpp` / `alarm.h` | Up to 10 alarms, stored in NVS (namespace `alarms`, key `list`) as an `AlarmStore` in PSRAM. |
| `audio.cpp` / `audio.h` | The built-in beep only, with a fade-in. `audio_start()` opens the speaker and makes the sound task at boot. |
| `weather.cpp` / `weather.h` | Open-Meteo forecast every 10 minutes: now (temperature, humidity, pressure, WMO code, day / night), 7 days (`WEATHER_DAYS`: code, max / min, weekday) and today's sunrise / sunset. Place search and the `Place` struct. `SC_WEATHER` shows the icon (`weather_glyph()`) or the sun times; `SC_FORECAST` shows one day (0 today … 6); sensors have sources 3–6 for max / min today and tomorrow. |
| `wifi.cpp` / `wifi.h` | Up to 5 saved networks with auto-connect, mDNS `tab5.local`, SNTP. The built-in network (`wifi_secrets.h`) joins the saved list on first boot and again whenever a program brings a different one (a hash in NVS key `wifi/seed`); a network already saved under that name keeps the password typed on the device; a full list gives up its last slot. |
| `sdcard.cpp`, `files.cpp` | microSD mount and format, file manager API (list, download, upload, mkdir, delete). |
| `layout.cpp` / `layout.h` | The older layout system for the built-in clocks, live font registration for the Studio preview, and the font cache: built-in TTFs in `main/ttf/` (Inter, Inter Light, Orbitron, Roboto Mono, Bebas Neue, Oswald, Rajdhani Bold subset to Latin, and `icons.ttf`, a lucide subset with the glyphs the firmware draws). `layout_font()` serves the faces and may free fonts after a rebuild; `layout_font_fixed()` makes fonts that are never freed, for screens built once. |
| `lv_mem_psram.c` | LVGL's allocator (`CONFIG_LV_USE_CUSTOM_MALLOC`): LVGL objects and caches live in PSRAM. |
| `lang.h` | `TR(pl, en)`, `LANG_CODE`, `LANG_DEFAULT_EN` from `studio/lang.h`. |
| `rx8130.cpp` | RTC chip driver (from M5Stack). |
| `studio/` | Generated by Studio at build time and git-ignored: `scene.h`, `layout.h`, `colors.h`, `lang.h`, images, fonts. |

### What `ui.cpp` contains

- **Clock faces**: `scr_clock` (theme 1) and `scr_big` (theme 2).
  - Built-in LED clocks, or a Studio scene through `scene_view()` when `studio/scene.h` exists.
  - The live scene goes through `ui_scene_live()`.
- **Gestures**:
  - double tap: dark / light (`on_clock_tap`, 400 ms);
  - swipe left/right: change theme;
  - long press: timer;
  - menu icon (52 px font `icon_menu52`): settings.
- **Dark mode and brightness**: night dimming (backlight duty plus a black overlay), screen-off schedule, wake on touch for a minute.
- **Intro**: built-in (credit line in Inter Light from the built-in TTF, its own font copy freed when the intro ends), or a designed one (`show_scene_intro`, `ms` = duration). Also `ui_scene_live_intro`.
- **Alarm and timer views** share the look of the settings and Studio: `screen_button()` (16 px corners; primary = yellow fill, secondary = dark fill with a thin border), background `#0B0D10`. Both views are deleted and rebuilt with each new design (`ui_scene_live_screens`), so their fonts come from `layout_font_fixed()`.
- **Alarm view**: built-in (the time in LED digits 270 px high, drawn by `scene_led_cb` from a static `SceneEl`) or scene based (`SC_ALARM_LABEL`, actions `SA_SNOOZE` / `SA_STOP`).
- **Timer**:
  - built-in view: "‹ Powrót" button and the timer icon with the title, the time in Rajdhani Bold 250 px, field selection, −/+ with long-press repeat, 5/10/15 min in a strip, Start/Reset; a new timer starts at 00:00, a paused one keeps its time (`timer_paused`);
  - a pill with the lucide timer icon in the top left corner of the clock while it runs (hidden on a face with its own `SC_TIMER_BADGE`);
  - API: `ui_timer()`, `ui_timer_left()`, `ui_timer_restart(seconds)` (used by the timer snooze).
- **Scene engine**: `scene_make` / `scene_make_el` (build the LVGL objects), `scene_paint` (colours from the palette), `scene_time` (clocks, sensors, loaders each tick), `scene_view_refresh`, `scene_action_cb` (taps → actions).
- **Previews for the web page and Studio**: `ui_preview_screen(n)`; `ui_alarm_view()` returns the open alarm or timer view for `/api/preview`.

## Saved design

The scene on screen can be saved for good, so the look changes without building and flashing.

- **Partitions** (`partitions.csv`, see ARCHITECTURE.md → Flash layout): two program slots `ota_0` (0x20000) and `ota_1` (0x4A0000) of 4.5 MB, `scene` (LittleFS) at 0x920000, about 6.9 MB. The partition table changes only with a USB flash: devices flashed before 2026-11 keep the old one (a 3.5 MB `model` partition at 0x920000, `scene` at 0xCA0000, about 3.4 MB). The program finds `scene` by name, so both work. The SD card is not used for designs.
- **Where**: `/scene` (the `scene` partition, mounted in `livescene_boot_early()`, formatted when it does not mount). NVS namespace `livescene`: `loc` (1 = a design is saved) and `crash`.
- **Files**: `cur/scene.json` (`name`, `time`, `blobs` [{n, k, w, h, a, f}], `show`) and the blobs `b0`, `b1`… A save writes `new/`, with `scene.json` last, then swaps `cur` → `old`, `new` → `cur`; a cut-off swap is read from `old/`. When there is no room for both, the old design goes first.
- **Start**: `livescene_boot_early()` runs before `ui_create()` (display lock held): the faces are preset with `ui_scene_live_preset()`, so the first frame and the start intro already use the design.
- **Safe mode**: a start after a panic or watchdog reset counts in `crash`; 30 s of running clears it. At 3 the design is skipped (`scene_saved_err` = `safe_mode`) until it is saved again or deleted.
- `/api/scene/stop` reloads the saved design.
- **Locks**: `livescene_state()` runs outside the display lock and waits at most 200 ms for the livescene mutex; the free space is cached (`s_int_free`), because reading it walks the file system. Never do slow work (files, SD, flash) while holding `bsp_display_lock()`: touch and drawing stop until it is released.

## Updates over Wi-Fi

`POST /api/ota` (web.cpp) takes `tab5panel-app.bin` as the body, writes it into the other slot sector by sector (`OTA_WITH_SEQUENTIAL_WRITES`), sets it to boot and restarts after the answer.

- Meanwhile `ui_show_update()` shows a still "Aktualizacja… N%" screen: every flash erase stalls both cores, and the ticking clock faces would starve the idle tasks into a watchdog restart. The interrupt watchdog is 800 ms (`CONFIG_ESP_INT_WDT_TIMEOUT_MS`) for the same reason.
- Rollback is on (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`): the new program counts as good when `uiwatch.cpp` sees a working screen 30 s after the start (`esp_ota_mark_app_valid_cancel_rollback`); a crash or freeze before that boots the previous slot again.
- Studio sends it with `POST /api/tab5/ota {id}` (“Na Tab5” next to a build, or “Kompiluj i wyślij”) and backs up the settings first (`/api/settings/backup`).
- A device whose partition table has no OTA slots needs one USB flash of `web-install.bin`.

## Screen watchdog

`uiwatch.cpp`: an LVGL timer beats every second; an esp_timer checks every 5 s. After 15 s without a beat the Tab5 writes NVS `uiwatch` (`n`, `who` = the task holding the LVGL mutex from `lvgl_port_lock_holder()`, `up`, `time`, `pending`) and restarts. `/api/state` shows `last_freeze`; the next start counts as a crash for the saved design's safe mode. LVGL keeps running while the screen is off, so a dark screen does not trip it.

Breadcrumbs (`uiwatch_crumb()`) name the step that runs now and survive a panic in RTC memory: after a crash `/api/state` shows `last_crash` (`step`, `uptime`, free internal RAM). Studio warns about a crash and about low internal RAM (`heap.int_min` under 30 KB).

## Touch and analog dials

- Touch is polled in LVGL timer mode (`esp_lvgl_port_touch.c`), with the interrupt only waking the LVGL task. In event mode one lost interrupt stopped touch for good while the screen kept drawing.
- Analog dials (`scene_analog_cb` in `ui.cpp`) keep their still part in a `DialCache` picture (ARGB8888 in PSRAM, drawn once via a hidden canvas from `lv_async_call`); each second `dial_tick()` invalidates only the boxes of the moving hands. Redrawing the whole dial every second made taps and swipes get lost.

## Wariant-3

A third clock face from the live or saved scene (`"third"` in the show JSON, `out[5]` in `livescene.cpp`). `ui_scene_live_third()` keeps the face; `scr_third` is made on first use (`third_create`), and its elements exist only while it is shown: `show_screen()` builds them (`third_build`) and deletes them when another screen loads (`third_unbuild`). While built, `scene_build()` remakes them inside its font generation, so `layout_fonts_end()` never frees their fonts. `face_count()` is 2 or 3 for swipes, the theme cycle, `/api/set face` and the settings (the Wariant-3 button is hidden without it). A compiled program has two faces.

## Alarms and timer

Badges on the clock faces (`SC_ALARM_BADGE`, `SC_TIMER_BADGE`): a row with the icon and / or the time (`source` 0 both, 1 icon, 2 time; `size2` = icon px, `color2` = icon color). `scene_badge()` runs every second: the alarm badge shows while `alarm_indicator()` is true (any alarm on, or ringing / snoozing) with the next ring time; the timer badge while the timer runs, with the time left. `face_has_timer_badge()` keeps the corner pill away on such a face.

- `alarm_tick()` runs every 0.5 s. A matching alarm calls `ring_now()`: the beep rises over 30 s and the ringing screen shows. `ALARM_RING_S` = 600: it stops by itself after 10 minutes.
- `days` is a bitmask (Mon=1 … Sun=64); 0 = once, after which the alarm disables itself.
- Snooze:
  - alarms ring again after `snooze_min` (default 10);
  - the timer ringing (`s_ring_idx == -2`) instead restarts the timer for `timer_snooze_min` (default 1) with `ui_timer_restart()`, so the stopwatch pill counts it down.
- The web page has a ringing bar with Snooze / Stop (`/api/alarm/snooze`, `/api/alarm/stop`).

## Weather and places

- The forecast comes from Open-Meteo (`timezone=auto`), with pressure reduced to sea level.
- Place search: Open-Meteo geocoding for names; zippopotam.us for Polish postcodes (`NN-NNN`). The results are in PSRAM.
- The chosen place is stored in config as name + lat/lon.

## Memory rules (important)

- `sdkconfig.defaults`: only allocations under 1 KB go to internal RAM (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024`), lwIP buffers live in PSRAM (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`) and mbedTLS follows malloc. With the ESP-IDF default (16 KB) small blocks and network buffers filled internal RAM within minutes (183 KB after boot → 36 KB). With these settings about 180 KB stayed free. LVGL itself allocates in PSRAM (`CONFIG_LV_USE_CUSTOM_MALLOC=y`, `main/lv_mem_psram.c`, pulled in with `-u lv_mem_init` in `main/CMakeLists.txt`): with the C library allocator every LVGL object and font cache entry was a small internal block, and each new design left 1–2 KB behind. Now about 260 KB is free after boot and stays at about 252 KB after repeated previews. Screens built once (menu, timer, intro credit) take their fonts from `layout_font_fixed()`: the face font cache frees fonts the faces stop using, which crashed the Tab5 when the menu or timer still held one.
- To see who holds internal RAM, turn on the task tracking described in `sdkconfig.defaults` (`heap_tasks` in `/api/state`).
- esp_hosted and the Wi-Fi buffers take much of internal RAM. Put any new table, string pool or buffer in PSRAM, allocated lazily: `heap_caps_malloc/calloc(..., MALLOC_CAP_SPIRAM)`.
- These caused boot loops before (`xPortcheckValidStackMem` / stack canary) until they were moved to PSRAM:
  - live scene tables;
  - place search results;
  - the alarm store.
- Big task stacks: the web server stack is 8192.

## Audio rules (important)

- The speaker opens once in `audio_start()` at boot (16 kHz, stereo, 16-bit). Never reconfigure or close it later. Changing the sample rate later fails a DMA allocation in `esp_codec_dev` `set_drv_fs` and the device restarts.
- MP3 and WAV playback from the SD card was tried and dropped. The beep is the only sound.

## Debugging

- `/api/state` → `reset_reason` (power, software, panic, watchdog, brownout, …) shows why the device last restarted; `last_crash`, `last_freeze`, `heap` and `boot_heap` add the details (the "Stan techniczny" card on the web page).
- The serial log is on USB at 115200. Opening the port with DTR/RTS can reset the device, so keep serial sessions short.
- `/api/preview` returns a JPEG of the screen (`?settings=N` gives settings tab N). `/api/screenshot` returns a BMP.
