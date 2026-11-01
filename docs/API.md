# Tab5 device API

Base URL: `http://tab5.local` or the device IP. Body and responses are JSON unless noted otherwise.

**PIN**: every `/api/*` call needs the PIN in one of these places:
- the `X-PIN` header;
- the `pin` cookie;
- the `?pin=` query.

A wrong PIN returns `401`. After 5 wrong tries within 5 minutes the device answers `429` for 30 s. The page `/` is public: it shows only the PIN screen.

## State and settings

| Method | Path | Notes |
|---|---|---|
| GET | `/api/state` | the whole state (below) |
| POST | `/api/set` | any subset of the fields below; invalid values are ignored |
| POST | `/api/intro` | plays the intro |
| POST | `/api/restart` | answers, then restarts after 0.5 s |
| POST | `/api/login` | 200 when the PIN is right |

### `/api/set` fields

| Field | Example | Meaning |
|---|---|---|
| `face` | `1` | theme now: 0 = Theme-1, 1 = Theme-2, 2 = Theme-3 (Wariant-3, when the design has it) |
| `night` | `true` | dark mode |
| `screen` | `false` | screen off / on |
| `brightness` | `60` | 10–100 (light mode); the web page offers Min = 10 and Max = 100 |
| `night_start`, `night_end` | `"23:00"` | night mode hours |
| `off_enabled`, `off_start`, `off_end` | `true`, `"01:00"` | screen-off schedule |
| `cycle_min` | `5` | theme rotation in minutes, 0 = off |
| `default_face` | `1` | theme after start (0–2) |
| `night_face` | `0` | theme while the night hours last (0–2), -1 = no change |
| `postcode` | `"00-950"` | Polish postcode lookup |
| `place` | `{"name":"Berlin","lat":52.52,"lon":13.41}` | a place from `/api/places` |
| `lang` | `"en"` | device language; saved, then the device restarts |
| `digit_color` | `"#FF8A3D"` | palette: digits |
| `sensor_color` | `"auto"` | palette: values (`auto` = like the digits) |
| `preset_save` / `preset_overwrite` / `preset_rename` / `preset_delete` / `preset_apply` | | colour presets (up to 8) |
| `day_preset`, `night_preset` | `0` / `-1` | preset applied with light / dark mode (-1 = none) |

### `/api/state` (example)

```json
{"face":0,"night":false,"screen":true,"brightness":100,"night_start":"23:00","night_end":"05:40",
 "off_enabled":false,"cycle_min":0,"default_face":0,"lang":"pl",
 "temperature":7.2,"humidity":89,"pressure":1029.4,"place":"Warszawa","ssid":"MojaSiec","ip":"192.168.1.50",
 "rssi":-67,"uptime":37,"reset_reason":"power","build":"20261101-100136-b977","build_note":"","scene_live":false,
 "...": "colours, presets, sd, scene, heap, app"}
```

- `face` = -1 while the settings screen is open. `faces` is 3 when the live or saved scene has Wariant-3 (then `face` and `default_face` may be 2).
- `build`: the Studio build ID of the running program, as in the Studio build list (`"dev"` for a build made outside Studio). `build_note`: its note from Studio.
- `intro`: the intro (built-in or from Studio) is on screen now; the web page lights "Pokaż intro" until it is false.
- `reset_reason`: why the device last restarted (`power`, `software`, `panic`, `watchdog`, `brownout`, …).
- Saved design: `scene_live` (a preview is on screen), `scene_saved` (`null` or `{name, kb, time, shown}`), `scene_saved_err` (`""`, `"safe_mode"`, `"load_failed"`), `scene_room_kb` (room for a design, `-1` = not available), `scene_busy` (a save is running).
- Technical state (the "Stan techniczny" card on the web page): `heap` (`int_free`, `int_min`, `int_block`, `psram_free`), `boot_heap` (free internal RAM after each start step), `app` (program slots: `running`, `boot`, `other` and their states), `last_crash` (`{step, uptime, int_free}` after a panic), `last_freeze` (`{count, task, uptime, time}` after the screen watchdog restarted a frozen Tab5). `heap_tasks` appears only when task tracking is built in (see `sdkconfig.defaults`).

## Alarms and timer

| Method | Path | Body / notes |
|---|---|---|
| GET | `/api/alarms` | `{alarms:[{enabled,hour,minute,days,volume,label}], snooze, timer_snooze, ringing}` |
| POST | `/api/alarms` | the whole list plus `snooze` (alarms) and `timer_snooze` (timer) in minutes |
| POST | `/api/alarm/test` | `{"volume":60}` plays the beep for 8 s |
| POST | `/api/alarm/snooze` | snoozes the ringing alarm (the timer restarts for `timer_snooze`) |
| POST | `/api/alarm/stop` | stops the ringing alarm or the test sound |
| GET | `/api/timer` | `{running, left}` |
| POST | `/api/timer` | `{"start":300}`, `{"cancel":true}`, `{"open":true|false}` |

`days`: Mon=1, Tue=2, Wed=4, Thu=8, Fri=16, Sat=32, Sun=64; 0 = once.

## Places, screen, backup, update

| Method | Path | Notes |
|---|---|---|
| GET | `/api/places?q=Berlin` | place search (name or Polish postcode); waits up to 15 s |
| GET | `/api/preview` | JPEG of the screen; `?settings=N` = settings tab N |
| GET | `/api/screenshot` | BMP 1280×720 |
| GET | `/api/backup?kind=app` | the running program (its slot, ota_0 or ota_1) |
| GET | `/api/backup?kind=full` | a flash image from 0x0 to the end of the running program (with the old partition table: to the end of the old `model` data), NVS and unused space blanked; flash at 0x0 |
| GET | `/api/settings/backup` | `{v, build, items:[{ns, key, hex}]}`: settings, colour presets, the layout and its presets, alarms and the weather place as raw NVS blobs (no Wi-Fi passwords) |
| POST | `/api/settings/restore` | the JSON from `/api/settings/backup` → `{restored, skipped}`; an item whose size does not fit this program is skipped; restarts when anything was written |
| POST | `/api/ota` | `tab5panel-app.bin` as the body: written to the other program slot, then a restart (rollback when the new program does not start); see FIRMWARE.md → Updates over Wi-Fi |
| GET / POST | `/api/layout` | the older layout system of the built-in clocks; no page uses it now |
| GET | `/api/font?name=inter` | a built-in TTF font, for the same older layout editor |

## SD card

| Method | Path | Notes |
|---|---|---|
| GET | `/api/files?path=/` | list a folder |
| GET | `/api/file?path=/a.jpg` | download |
| POST | `/api/upload?path=/a.jpg` | raw file as the body; overwrites |
| POST | `/api/mkdir?path=/dir` | new folder |
| POST | `/api/delete?path=/x` | file, or folder with its contents |
| POST | `/api/sd/format` | formats in the background; progress in `/api/state` → `sd` |

## Live scene (used by Studio)

| Method | Path | Notes |
|---|---|---|
| POST | `/api/scene/begin` | clears the previous live scene |
| POST | `/api/scene/blob?name=img0&kind=img&w=&h=&alpha=1` | raw RGB565 (+A8) image |
| POST | `/api/scene/blob?name=f_x&kind=font` | TTF |
| POST | `/api/scene/show` | `{faces:[basic,full], third, intro, alarm, timer, play_intro, show_screen}` (`third` = Wariant-3); element keys `t o f s x y w h z z2 c c2 c3 c4 act av d a i font text img` |
| POST | `/api/scene/stop` | back to the saved design, or to the compiled look when none is saved |
| POST | `/api/scene/save` | `{"name":"..."}` saves the scene on screen for good → `{kb}`; errors `400 {"error": "no_space"\|"storage"\|"write"\|"nothing"}` |
| POST | `/api/scene/forget` | deletes the saved design, back to the compiled look |

## curl

```bash
curl -H 'X-PIN: 1234' http://tab5.local/api/state
curl -H 'X-PIN: 1234' -X POST -d '{"face":1}' http://tab5.local/api/set
```
