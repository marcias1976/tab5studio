# Tab5 Studio — notes for Claude

Read this first, then the file in `docs/` that matches the task. Author and owner: **Jakub W.** (MARCIASMEDIA).

## On the server

Port **8080** (`STUDIO_PORT`), container `tab5studio`. The user flow: flash the program once over USB, then “Podgląd → Zapisz na Tab5 na stałe” saves the design on the Tab5 (no building); program updates go over Wi-Fi (“Na Tab5” / “Kompiluj i wyślij”).

## What this is

- `app/` — **Tab5 Studio**: a Flask server (`app/server.py`) plus a single-page WYSIWYG editor (`app/static/index.html`, vanilla JS, no build step). Users design screens for the M5Stack Tab5 and build its firmware.
- `firmware/` — the **Tab5 firmware** (ESP-IDF 5.4.2, LVGL 9.2.2, ESP32-P4, 1280×720). Studio copies it, adds the generated design (`main/studio/`) and runs `idf.py build`.
- `docs/SERVER-SETUP.md` — step-by-step server setup to walk the user through (Polish).
- `docs/` — details: [ARCHITECTURE](docs/ARCHITECTURE.md), [STUDIO](docs/STUDIO.md), [FIRMWARE](docs/FIRMWARE.md), [API](docs/API.md).

## Working with the user

- The user writes in **Polish**, often short and with typos. Answer in Polish, plainly. Keep code, comments and commit messages in English.
- Every user-facing text exists in **PL and EN**:
  - Studio: Polish in the markup, English in the `EN` dict / `EN_RE` rules in `index.html`, applied by `tr()`.
  - Tab5 web page: the same pattern in `firmware/main/web/index.html`.
  - Device: `TR("pl", "en")` in C++.
- **UI rules** (Studio and the web page):
  - no emoji as icons; use inline SVG (Lucide style);
  - no native browser dialogs (`alert`, `confirm`, `prompt`) and no unstyled native widgets; use the in-app dialogs (`ask()`, styled inputs);
  - dark theme, accent `#FFC83D`.
- **Help lives only in Studio.**
  - The Pomoc dialog has two tabs: `app/static/help/help.{pl,en}.html` (Tab5 Studio) and `web.{pl,en}.html` (Web Interface).
  - Never put guides into the firmware, its screen or its web page.
  - When a feature changes, update the help text in both languages. Regenerate the screenshots with `scripts/help_shots.js` (Studio) and `scripts/web_shots.js` (device web page); usage at their top (they need Node with Playwright and Python with Pillow).
- **Secrets**:
  - never print or commit Wi-Fi passwords or PINs;
  - `firmware/main/wifi_secrets.h` and `web_secrets.h` are git-ignored, and only the `*.example` files are in the repo;
  - builds take the secrets from `.env`.
- **Before you act**:
  - ask before pushing, deleting user data in `data/`, or flashing a device;
  - commit only when asked, and end the message with the attribution line the session gives.

## Run and build

```bash
docker compose up -d --build            # on the server, Studio at :8080 (STUDIO_PORT in .env)
docker compose logs -f tab5studio
docker compose exec tab5studio bash     # shell with ESP-IDF in PATH
```

Inside the container you can build by hand:

```bash
cd /data/work/src && idf.py build
```

This is the folder Studio builds in. It holds the latest `rsync` copy of `firmware/` plus `main/studio/`.

Without Docker, see `run-local.sh` (defaults: the repo firmware, `~/esp/esp-idf/export.sh`, `./data`; override `FIRMWARE_DIR`, `IDF_EXPORT`, `DATA_DIR`).

`app/` and `firmware/` are mounted into the container read-only. Files in `app/static/` are served live (the editor and help are sent with `no-cache`). Flask does not auto-reload: restart the container after editing `server.py`.

## Firmware gotchas (read before touching `firmware/main`)

1. **Internal RAM is tight** because esp_hosted Wi-Fi takes much of it. Keep `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024` and `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` in `sdkconfig.defaults`: with the defaults, small blocks and network buffers ate it down to a few tens of KB within minutes; with them about 180 KB stayed free. LVGL allocates in PSRAM (`CONFIG_LV_USE_CUSTOM_MALLOC=y`, `main/lv_mem_psram.c`): with the C library allocator its many small objects took internal RAM and each new design from Studio left 1–2 KB behind; now about 255 KB stays free and steady. Fonts for screens built once (menu, timer) come from `layout_font_fixed()`, never from the face font cache, which rebuilds free. "Stan techniczny" on the web page (`heap`, `boot_heap` in `/api/state`) shows the memory, and Studio warns when it runs low.
   - New tables or buffers of more than a few hundred bytes go in **PSRAM**: `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`, allocated lazily on first use.
   - Static arrays of a few KB in internal RAM cause a boot loop (`xPortcheckValidStackMem`).
2. **Audio**:
   - the speaker opens **once** in `audio_start()` at boot (16 kHz, stereo, 16-bit) and is never closed or reconfigured;
   - reconfiguring I2S later fails a DMA allocation and crashes inside esp_codec_dev;
   - `audio.cpp` only plays the built-in beep. MP3 and WAV were dropped on purpose.
3. **LVGL from other tasks**: wrap UI calls in `bsp_display_lock()` / `bsp_display_unlock()`. Web and alarm code run outside the LVGL task.
4. **Live scenes** (`livescene.cpp`):
   - before freeing old element arrays, rebuild or close every view that uses them (`ui_scene_live_*`), and finish a playing intro first;
   - never read cJSON after `cJSON_Delete`;
   - live fonts share names with compiled ones (`f_...`, `studio_icons`): call `layout_live_fonts_retire()` before rebuilding any view, so the font cache never hands a live font to the compiled look;
   - the saved design is a live scene loaded at boot (`livescene_boot_early`, see FIRMWARE.md → Saved design), stored only in the `scene` partition (not on SD);
   - never do slow work (files, SD, flash) while holding `bsp_display_lock()`: touch and drawing freeze until it is released.
5. **Partitions and updates**: two OTA slots (`ota_0` 0x20000, `ota_1` 0x4A0000, 4.5 MB each) and `scene` LittleFS at 0x920000 (about 6.9 MB). Older devices keep the old table (`scene` at 0xCA0000) until a USB flash; the code finds `scene` by name. Keep `partitions.csv` and the `merge_bin` offsets in `server.py` in step. Updates go over Wi-Fi (`/api/ota`): rollback until `uiwatch.cpp` marks the program good after 30 s; the "Aktualizacja… N%" screen (`ui_show_update`) and the 800 ms interrupt watchdog keep flash erases from tripping watchdogs.
6. **NVS blobs** (`PanelConfig`, presets, layout, alarms, place) are saved as raw structs and restored from Studio backups (`BACKUP_ITEMS` in `web.cpp`): never remove or reorder fields, add new ones at the end.
7. **Language**: `TR()` reads the runtime flag `g_lang_en`, which `config_load()` sets from `config().lang`.
   - File-scope arrays of translated strings must be functions (see `day_name()`).
   - A language change restarts the device.
8. **HTTP handlers**: `max_uri_handlers` is 40 in `web.cpp` (35 used). Raise it when you add endpoints.
9. **Fonts** are a CMake glob: run `idf.py reconfigure` after adding a font file (Studio does this by itself for Studio fonts).

## Firmware source of truth

The firmware was developed in a separate ESP-IDF project and copied here with `scripts/sync-firmware.sh <project>`, which leaves the secrets out. On the server, edit `firmware/` in this repo directly; a separate project is not needed.

After firmware changes:
1. build in Studio, or with `idf.py build` in the container;
2. update `docs/FIRMWARE.md` / `docs/API.md` if behaviour or endpoints changed;
3. update the Web Interface help if the web page changed.

## Repo layout

```
app/server.py            Flask: projects, library, assets, fonts, build jobs, live preview proxy, OTA, Tab5 backups, place search
app/static/index.html    the editor (HTML + CSS + JS in one file)
app/static/help/         illustrated help: help.{pl,en}.html, web.{pl,en}.html, screenshots (webp)
app/static/lucide.json   icon list for the icon picker
firmware/                Tab5 ESP-IDF project (main/*.cpp, main/web/index.html = device web page, main/version.h = build ID)
scripts/                 sync-firmware.sh, help_shots.js (Studio help screenshots), web_shots.js (device web page screenshots, no Tab5 needed)
data/                    (git-ignored) projects, library, assets, fonts, builds, work dir, settings.json,
                         tab5_last.json, tab5_saved.json, tab5_backups/ (see docs/ARCHITECTURE.md → Data folder)
```
