# Tab5 Studio (app/)

## Server — `app/server.py`

Flask, one file. Every route except the login uses `@auth`. With `STUDIO_PASSWORD` set, the browser signs in on `/login` (a session cookie for 30 days; 5 wrong tries → 30 s of 429) and scripts may use HTTP Basic on `/api/*` (`curl -u x:<password>`). An empty password leaves Studio open. POST, PUT and DELETE from another origin are refused (403). HTTP Basic shares the same 5-tries lockout. Without a password Studio answers only to IP addresses, `localhost` and the names in `ALLOWED_HOSTS` (DNS rebinding). Scenes loaded from files or the server go through `cleanScene()` in the editor: numbers, colors, font names and known words only, nested data only under faces → elements.

### Environment

| Variable | Default | Meaning |
|---|---|---|
| `FIRMWARE_DIR` | `../firmware` | firmware source copied into each build |
| `DATA_DIR` | `../data` | all user data (Docker: `/data`) |
| `IDF_EXPORT` | empty | path to `export.sh` when ESP-IDF is not already in `PATH` (empty in Docker) |
| `PORT` | 8080 | HTTP port inside the container |
| `STUDIO_PORT` | 8080 | port on the server; `docker-compose.yml` maps it to port 8080 in the container |
| `STUDIO_PASSWORD` | empty | Studio password (see above) |
| `WIFI_SSID`, `WIFI_PASSWORD` | — | written into every build |
| `WEB_PIN` | — | device web page PIN when the build gives none |
| `TAB5_HOST`, `TAB5_PIN` | — | live preview target; `data/settings.json` overrides them, as set in the editor |

### Routes

| Method | Path | Purpose |
|---|---|---|
| GET | `/login`, `/logout` | sign-in page, sign out |
| POST | `/api/login` | `{password}` |
| GET | `/` | editor (`static/index.html`, no-cache); redirects to `/login` when signed out |
| GET | `/static/<path>` | static files; `help/*` and `index.html` are sent no-cache |
| GET | `/fonts/<name>` | TTF for the editor (built-in and user fonts) |
| GET | `/api/icons` | Lucide icons `[[name, code point], …]` (the same as `/static/lucide.json`, but reachable with HTTP Basic: for scripts and the MCP server) |
| GET | `/api/meta` | fonts, icon set, constants for the editor |
| POST / DELETE | `/api/fonts`, `/api/fonts/<name>` | upload or delete a user font (TTF/OTF → `fonts/f_<id>.ttf` + `.title`) |
| GET / POST / DELETE | `/api/assets`, `/assets/<name>` | list, serve, upload or delete images |
| GET | `/api/library/<kind>` | library entries of a kind (`clock`, `intro`, `alarm`, `timer`) |
| PUT | `/api/library/<kind>/<name>` | save `{face, thumb}` |
| POST | `/api/library/<kind>/<name>/rename` | `{to}` |
| DELETE | `/api/library/<kind>/<name>` | delete |
| GET / PUT / DELETE | `/api/projects`, `/api/projects/<name>` | projects |
| GET | `/api/projects/<name>/export` | the project as a `.zip` with its images and Studio fonts |
| POST | `/api/projects/import` | a `.zip` from the export (multipart `file`): added under a free name, clashing files renamed |
| POST | `/api/build` | `{name, scene, lang: "pl"|"en", pin?, note?}` → `{id}`; the PIN must be 4 digits and is never stored; one build runs and one more may wait (else 429) |
| GET | `/api/build/<id>?offset=` | `{status, log, offset, files, seconds, busy}` |
| GET | `/api/builds` | the newest builds (up to 20) with project name, note and size. After each build only the newest `KEEP_BUILDS` (10) and the one on the Tab5 (`data/tab5_last.json`, updated by `/api/tab5/test`) stay |
| DELETE | `/api/build/<id>` | delete a build |
| GET | `/api/build/<id>/<file>` | download a `.bin` |
| GET | `/api/places?q=` | place search: Open-Meteo geocoding, plus zippopotam.us for Polish postcodes |
| GET / PUT | `/api/settings` | Tab5 host and PIN for the live preview; the PIN is never returned |
| POST | `/api/tab5/test` | reads the Tab5 `/api/state`: ip, ssid, place, preview and saved design, build and note, heap, last crash / freeze, plus `studio_saved` from `data/tab5_saved.json` |
| POST | `/api/live` | sends the scene to the Tab5 (see ARCHITECTURE → Live preview) |
| POST | `/api/live` with `save: {name, project, hash}` | also saves the design on the Tab5 for good (`/api/scene/save`), writes `data/tab5_saved.json` and sets the Tab5 palette (`digit_color`, `sensor_color`) to the project's, because a saved design keeps "auto" colors |
| POST | `/api/live/stop` | ends the live preview |
| POST | `/api/live/size` | `{scene}` → `{kb}`: size of the design as saved on the Tab5 |
| POST | `/api/live/forget` | deletes the design saved on the Tab5 |
| POST | `/api/tab5/ota` | `{id}`: backs up the Tab5 settings first (`make_backup`, skipped on programs without it), then sends that build's `tab5panel-app.bin` over Wi-Fi |
| POST | `/api/tab5/backup` | copies the Tab5 settings and alarms (`/api/settings/backup`, no Wi-Fi passwords) and `tab5_saved.json` to `data/tab5_backups/` (the newest 20 stay) |
| GET | `/api/tab5/backups` | the copies |
| POST / GET | `/api/tab5/restore` | `{name}`: writes a copy back (`/api/settings/restore`, the Tab5 restarts), then saves the project from the copy again; GET = progress |
| GET | `/api/tab5/screen` | what the Tab5 shows now (JPEG, from its `/api/preview`) |

`data/tab5_saved.json` keeps the project name, scene hash and scene last saved on the Tab5. Studio compares the hash with the open project ("taki jak otwarty" / "otwarty ma zmiany"), and a restore saves that scene again.

### Generator constants

- `SC`: element type → `SceneType` code. It must match `firmware/main/scene.h`.
- `ACTIONS`: tap actions → `SceneAction`.
- `LOADERS`: loader style index.
- `EXTRA_SCREENS = (intro, alarm, timer)`.
- `compile_scene(scene)` returns a dict of faces, one entry per designed screen.
- `gen_scene()` writes the C header and the resources. When you add an element type, change all three places together:
  1. `SC` + `compile_scene` in `server.py`;
  2. `SceneType` and the `SceneEl` fields in `scene.h`;
  3. `scene_make_el()` (and `scene_paint()` / `scene_time()` if it animates) in `ui.cpp`.

  Then add it to the editor too: `renderEl`, the properties panel and the add button. The live preview parser in `livescene.cpp` needs any new JSON keys.

## Editor — `app/static/index.html`

A single file of about 2500 lines: CSS, then markup, then JS. There is no framework and no build step.

### Main pieces of the JS

- `S`: the state.
  - `S.scene` (project), `S.face` (current screen key), `S.sel` / `S.multi` (selection), `S.zoom`, `S.preview`;
  - `S.assets`, `S.fonts`, `S.icons`;
  - `S.dirty`.
- `faceEls()`: the elements of the current screen.
- `commit(label)`: pushes an undo step. `undo()` / `redo()` undo and redo.
- `renderAll()` calls `renderStage()`, `renderLayers()` and `renderProps()`. `renderEl(el)` builds one element on the stage; LED and analog clocks are drawn on a canvas (`drawLed`, `drawAnalog`).
- `select(id, add, only)` selects; `groupSel()` / `ungroupSel()` group; `groupOf(el)` returns an element's group.
- Defaults and starters:
  - `defaultScene()`, `defaultIntro()`, `defaultScreen()`; `weekThird()` is the example Wariant-3 (the weather for a week) in the default and new projects and behind "Wstaw prognozę na tydzień";
  - on the first start (no project yet) Studio saves `defaultScene()` as "Original Project" and opens it;
  - `introStarter()`, `alarmStarter()`, `timerStarter()`: the built-in look of those screens made of ordinary elements.
- Stage hints on the clock faces: the MENU frame (top right) and the MINUTNIK frame (top left, the Tab5 timer pill while the timer runs in the background; hidden when the face has its own `timer_badge`). In the preview the pill shows only with the timer simulation on.
- Built-in mocks shown when `builtin` is set: `builtinIntro()`, `builtinScreen(kind)`.
- `ask(title, text, okLabel, danger)`: the in-app confirm dialog (a Promise). Never use `confirm()`.
- Translation:
  - `uiLang` (`localStorage.uiLang`);
  - the `EN` dict and `EN_RE` regex rules;
  - `tr(text)` and the DOM translator, which skips `.notr` elements.

  Add the English for every new Polish string.
- Help: `showHelp(doc)` loads `static/help/<doc>.<lang>.html`, where `doc` is `help` or `web`. The section menu is built from the `<section id><h2>` elements in that file.

### Keyboard

| Keys | Action |
|---|---|
| Ctrl/Cmd+S | save |
| Ctrl/Cmd+Z, Shift+Z / Y | undo, redo |
| Ctrl/Cmd+D | duplicate |
| Ctrl/Cmd+C, V | copy, paste (also onto another screen or project) |
| Ctrl/Cmd+A | select all |
| Ctrl/Cmd+G, Shift+G | group, ungroup |
| Delete | delete |
| arrows (Shift) | move by 1 px (10 px) |
| Shift + drag | no snapping |
| P | preview |
| Esc | close preview or dialog, deselect |
| ? / F1 | help |

## Help and screenshots

- Texts: `app/static/help/help.{pl,en}.html` (Studio) and `web.{pl,en}.html` (device web page). Plain HTML sections; the styles live in `index.html` under `.help-body`.
- Studio screenshots:
  - generated by `scripts/help_shots.js` (Node + Playwright with Chromium, Python with Pillow, a running Studio; usage in the comment at its top);
  - written to `help/{pl,en}/*.webp`, with numbered markers on `overview`;
  - uploaded images are hidden in the shots (they may be other brands' logos), and the Tab5 address is shown as 192.168.1.50.
- Web page screenshots (`help/web/{pl,en}`: header, one per tab, PIN screen) come from `scripts/web_shots.js`. It loads `firmware/main/web/index.html` straight from the repo with sample API answers (MojaSiec, Warszawa 00-950, 192.168.1.50), so no Tab5 is needed; an optional argument is a JPEG of the Tab5 screen for the preview. Same requirements as help_shots.js, without Studio.
- Device screenshots (`help/web/dev`) were made from a real Tab5. Keep sample data in every screenshot: the repo is public.
