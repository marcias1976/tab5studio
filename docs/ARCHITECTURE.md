# Architecture

```
 Browser ──HTTP──► Tab5 Studio (Flask, :8080) ──rsync + idf.py──► firmware .bin ──USB (first time) / Wi-Fi OTA──► Tab5
    │                     │                                                                                 ▲
    │                     └──HTTP (X-PIN): live preview, saved design, OTA, settings backups ───────────────┤
    └──────────────── Tab5 web page (http://tab5.local, served by the device) ──────────────────────────────┘
```

## Parts

| Part | Where | Role |
|---|---|---|
| Editor | `app/static/index.html` | Design screens, library, projects, preview, builds. State in the JS object `S`; the project is `S.scene`. |
| Studio server | `app/server.py` | Stores projects, library, assets and fonts in `data/`. Turns a scene into C (`compile_scene`, `gen_scene`), runs builds, talks to the Tab5 (live preview, saved design, OTA updates, settings backups). See [STUDIO.md](STUDIO.md). |
| Firmware | `firmware/` | Clock UI (LVGL), settings screens, web page + JSON API, Wi-Fi, weather, alarms, timer, live scene receiver and saved design, OTA updates. See [FIRMWARE.md](FIRMWARE.md). |
| Device web page | `firmware/main/web/index.html` | Embedded in the firmware and served at `/`. PIN login, then five tabs: Pulpit (preview, theme, mode, screen on/off, intro, brightness Min/Max, weather and facts), Wygląd (colors, presets, themes), Budzik, Harmonogram (Plan on a phone), System (place, language, restart, technical state, program backup, SD files). |

## The scene (project) format

`S.scene` is saved as `data/projects/<name>.json` (`{name, scene}`):

```js
{
  colors: {digit: "#5BE7E1", sensor: "auto"},          // palette: "auto" elements use it; the Tab5 web page can change it
  faces: {
    basic: {bg: "#050505", elements: [...]},            // Theme-1 / Wariant-1
    full:  {bg: "#050505", elements: [...]},            // Theme-2 / Wariant-2
    third: {bg, enabled: false, elements: []},          // Wariant-3: live preview and saved design only, never compiled
    intro: {bg, builtin: true, ms: 4000, elements: []}, // builtin = use the firmware's own intro
    alarm: {bg, builtin: true, elements: []},
    timer: {bg, builtin: true, elements: []},
  }
}
```

### Elements

Every element is `{id, type, name, x, y, w, h, visible, locked, opacity, group?, action?, action_val?, ...type fields}`.

Types:
- `clock`, with `style` set to `led` or `font`;
- `analog`, `sensor`, `date`, `place`, `loader`, `weather`, `forecast`;
- `alarm_badge`, `timer_badge` (clock faces only);
- `text`, `icon`, `image`;
- `rect`, `ellipse`, `line`;
- `alarm_label`, `timer_time`, `timer_go` (alarm and timer screens only).

Elements with the same `group` id move and scale together.

### Library

`data/library/<kind>/<name>.json` holds single screens. The kinds are `clock` (fits both themes), `intro`, `alarm` and `timer`, each with a PNG thumbnail.

## Scene → firmware

`server.py`:
1. `compile_scene()` maps every element to a `SceneEl` initializer. The type codes are in `SC`, the actions in `ACTIONS`, the loaders in `LOADERS`.
2. `gen_scene()` writes `main/studio/scene.h`:
   - `STUDIO_SCENE`, `studio_faces[2]` (the two clock faces);
   - `studio_intro_face`, `studio_alarm_face`, `studio_timer_face` with `#define STUDIO_INTRO` / `STUDIO_ALARM` / `STUDIO_TIMER`, when the screen is designed.
3. It also writes the images as RGB565(+A8) `.bin` files, the used fonts as `.ttf` files, and a subset icon font. CMake embeds them all through `file(GLOB studio/*)`.
4. `run_build` writes `main/studio/layout.h` (font list), `colors.h` (palette), `lang.h` (`LANG_EN`, the start language) and `build.h` (`STUDIO_BUILD`, `STUDIO_BUILD_NAME`, `STUDIO_BUILD_NOTE`, read by `version.h`).

The firmware side is `firmware/main/scene.h` (struct definitions) and the `scene_*` functions in `ui.cpp`: `scene_make`, `scene_make_el`, `scene_paint`, `scene_time` and `scene_view`.

## Build pipeline (`run_build`)

1. `rsync` copies `FIRMWARE_DIR` to `data/work/src` (it excludes `build/`, `managed_components/` and `main/studio/`, so the build cache stays between builds).
2. Generate `main/studio/*` from the scene and the build ID.
3. Write the secrets: `wifi_secrets.h` from `WIFI_SSID` / `WIFI_PASSWORD`, and `web_secrets.h` from the PIN typed for this build or from `WEB_PIN`.
4. `idf.py reconfigure`, only when the set of fonts changed.
5. `idf.py build`.
6. `esptool merge_bin` (bootloader 0x2000, partition table 0x8000, `ota_data_initial.bin` 0xF000, app 0x20000), which produces:
   - `tab5panel-web-install.bin`: the full flash image, written at 0x0 over USB;
   - `tab5panel-app.bin`: the app only, sent over Wi-Fi with “Na Tab5” (`POST /api/tab5/ota` → Tab5 `/api/ota`).

The results go to `data/builds/<id>/` together with `project.json` (with the note, never the PIN). The build ID is `YYYYMMDD-HHMMSS-xxxx`. Only one build runs at a time (`build_lock`). The log is streamed through `GET /api/build/<id>?offset=`. After a build, `prune_builds()` keeps the 10 newest and the one on the Tab5.

### Flash layout (16 MB)

| Address | Contents |
|---|---|
| 0x2000 | bootloader |
| 0x8000 | partition table |
| 0x9000 | nvs |
| 0xF000 | otadata (which program slot boots) |
| 0x11000 | phy_init |
| 0x20000 | ota_0 program slot (4.5 MB) |
| 0x4A0000 | ota_1 program slot (4.5 MB) |
| 0x920000 | scene: the saved Studio design (LittleFS, about 6.9 MB) |

The source is `firmware/partitions.csv`; keep the `merge_bin` offsets in `server.py` in step with it. Devices flashed before 2026-11 keep the old table (a 3.5 MB `model` partition at 0x920000, `scene` at 0xCA0000, about 3.4 MB) until a USB flash; see FIRMWARE.md → Saved design.

## Live preview

1. The editor posts the scene to `POST /api/live`.
2. The server converts the images and fonts and sends them to the Tab5 configured in `data/settings.json` (host + PIN):
   - `POST /api/scene/begin`;
   - one `POST /api/scene/blob?name=&kind=img|font&w=&h=&alpha=` per image or font;
   - `POST /api/scene/show` with JSON: `{faces:[basic, full], intro, alarm, timer, third, play_intro, show_screen}`.
3. `POST /api/live/stop` maps to `/api/scene/stop`.

A preview is not stored. A restart or `/api/live/stop` returns to the saved design, or to the compiled look when none is saved. `POST /api/live` with `save` also saves it for good (FIRMWARE.md → Saved design).

## Data folder (`DATA_DIR`, Docker: `/data`)

```
projects/<name>.json      projects
library/<kind>/<name>.json + .png
assets/                   uploaded images (PNG/JPG/WebP/SVG)
fonts/f_<id>.ttf + .title user fonts
builds/<id>/              built .bin files, log, project.json
work/src/                 build working copy of the firmware (+ ESP-IDF build cache)
settings.json             Tab5 host/PIN for live preview
tab5_last.json            the build the Tab5 reported last (kept when builds are pruned)
tab5_saved.json           the project last saved on the Tab5 (name, scene hash, scene)
tab5_backups/<time>.json  Tab5 settings backups (the newest 20)
.session_key              signs the Studio login cookie
```
