#!/usr/bin/env python3
"""
Tab5 Studio MCP server — author: Jakub W. (MARCIASMEDIA)

Optional: lets Claude (Claude Code, Claude Desktop or any MCP client) design Tab5 screens through a running Tab5 Studio.
It talks only to Studio's own HTTP API: projects, live preview on the Tab5, a screenshot of the Tab5, saving on the Tab5.
Nothing runs on the Tab5 and nothing new is exposed to the network: the client starts this script over stdio.

Python 3.8+ standard library only (no pip install).

Environment:
  STUDIO_URL       Tab5 Studio address (default http://127.0.0.1:8080)
  STUDIO_PASSWORD  Studio password, when Studio has one (sent as HTTP Basic, as scripts do)
"""
import base64
import json
import os
import random
import re
import string
import sys
import urllib.error
import urllib.parse
import urllib.request

STUDIO_URL = os.environ.get("STUDIO_URL", "http://127.0.0.1:8080").rstrip("/")
PASSWORD = os.environ.get("STUDIO_PASSWORD", "")
VERSION = "1.0.0"

W, H = 1280, 720  # the Tab5 screen
FACES = ("basic", "full", "third", "intro", "alarm", "timer")
FACE_NAMES = {"basic": "Wariant-1", "full": "Wariant-2", "third": "Wariant-3", "intro": "Intro", "alarm": "Alarm", "timer": "Timer"}
TYPES = ("clock", "analog", "sensor", "date", "text", "icon", "image", "rect", "ellipse", "line", "place", "loader", "weather",
         "forecast", "alarm_badge", "timer_badge", "alarm_label", "timer_time", "timer_go")
ALARM_ACTIONS = ("snooze", "stop")
TIMER_ACTIONS = ("timer_go", "timer_reset", "timer_plus", "timer_minus", "timer_plus_s", "timer_minus_s", "timer_set", "timer_close")

REFERENCE = """
# Tab5 Studio scene reference

Screen: 1280 x 720 px, landscape. Coordinates: x, y = top-left corner of the element, in px.

## Project
{"colors": {"digit": "#RRGGBB", "sensor": "auto" | "#RRGGBB"}, "faces": {basic, full, third, intro, alarm, timer}}
- colors.digit: palette color for clocks, icons, weather, forecast, badges whose color is "" (auto).
- colors.sensor: palette color for sensors with color ""; "auto" = same as digit.
  After "save on Tab5" the Tab5 gets this palette; its owner can still change it on the Tab5 web page.

## Faces (screens)
- basic = Wariant-1, full = Wariant-2: the two clock faces (the user swipes between them).
- third = Wariant-3: a third clock face, used only when "enabled": true.
- intro: start-up screen; "builtin": true = the built-in MARCIASMEDIA intro. Set "builtin": false to use its
  elements; "ms" = how long it shows (1000-60000).
- alarm / timer: the ringing alarm and the timer screen; "builtin": true = the built-in look. Set "builtin": false
  to use designed elements, which then need tap actions (below).
Each face: {"bg": "#RRGGBB", "elements": [...]} (+ enabled / builtin / ms as above). Max 64 elements per face.

## Common element fields
id (any unique string; filled in for you), type, name (label in Studio), x, y, visible (true), opacity (0-100),
color ("" = automatic palette color; give text and shapes an explicit color), group (same string = moved together in Studio).
Fonts ("font"): the names returned by get_reference / meta, e.g. inter, inter_light, orbitron, roboto_mono, bebas,
oswald, rajdhani (Rajdhani Bold, great for big numbers). Studio fonts start with f_.
Text height is about 1.25 x size. Text width with "w": 0 is its natural width (align "left" only).

## Element types
- clock: time. style "led" (7-segment LED digits; size = digit height; width = 716*size/280, 858*size/280 with
  seconds; "unlit": true shows dim unlit segments) or style "font" (text in "font"; size = font px). seconds: bool.
- analog: analog clock; size = diameter (40-720); seconds, marks (0 none, 1 hours, 2 hours+minutes), rim (0-60),
  color (hands), second_color, mark_color, fill (dial).
- sensor: a number. source: 0 temperature, 1 humidity, 2 pressure, 3 max today, 4 min today, 5 max tomorrow,
  6 min tomorrow (weather from the internet for the Tab5's place). decimals (0-3), font, size, unit (e.g. "°C"),
  unit_size (0 = smaller automatic), unit_color.
- date: format 0 "Sunday, 1 November", 1 "01.11.2026", 2 "Sunday", 3 "1 November"; upper (uppercase), font, size.
  Language follows the Tab5.
- place: the Tab5's town name; font, size, upper, align ("left" | "center" | "right" with width "w").
- text: fixed text; text, font, size, align, w (width for center / right; 0 with left).
- icon: a Lucide icon; cp = code point (use search_icons), size (px), color.
- weather: source 0 icon of the weather now, 1 icon for tomorrow (size = icon px), 2 sunrise time, 3 sunset time
  (font, size).
- forecast: one day of the 7-day forecast in a column: day name, icon, max / min temperature. day 0 (today, shows
  "Today") ... 6; w = column width (text centered), size = text px, icon_size (0 = 1.6 x size), color, icon_color,
  min_color, long (full day names), upper. Height is about 2.6 x size + icon size.
- alarm_badge / timer_badge: shown only while an alarm is set / the timer runs. show "both" | "icon" | "time",
  cp (icon), icon_size, icon_color, font, size. A face with its own timer_badge hides the Tab5's corner timer pill.
- rect: w, h, fill, radius, border (px), border_color.  ellipse: w, h, fill, border, border_color.
- line: w, h (end point relative to x, y; h 0 = horizontal), width (thickness), color, round (round caps).
- image: asset (a PNG already uploaded to Studio, see get_reference), w, h.
- loader: progress animation (mostly for intro): style bar | dots | spinner | ring | wave, w, h, color, track, ms, loop.
- alarm_label (alarm face): the alarm's name; font, size, align, w.
- timer_time (timer face): remaining time; font, size, align, w.  timer_go (timer face): "Start" / "Pause" text.

## Tap actions (only on alarm and timer faces with builtin false)
Any element can get "action": its box becomes a touch button (put the action on a rect, and the text on top).
- alarm: snooze, stop.
- timer: timer_go (start / pause), timer_reset, timer_plus / timer_minus (minutes = action_val),
  timer_plus_s / timer_minus_s (seconds = action_val), timer_set (set to action_val minutes), timer_close (back).
A designed alarm face must have a stop action; a designed timer face must have timer_close.

## Layout rules that look good on the Tab5
- Keep 40-60 px margins from the screen edges; align columns; use a few sizes only.
- On clock faces keep the top-left corner (about x < 300, y < 84) free: while the timer runs the Tab5 shows a
  timer pill there (unless the face has its own timer_badge).
- The top-right corner (x > 1176, y < 96) holds the Tab5's menu button (gray hamburger) on every clock face.
- Big time: LED clock size 280-420, or a font clock (rajdhani / bebas / oswald) size 110-260.
- Dark backgrounds (#050505 - #0E1014) with one accent color look best. Muted secondary text e.g. #8A929C.
- Built-in look of the alarm / timer screens: background #0B0D10, buttons #1A1E23 with 2 px border #262B31 and radius
  16, main button #FFC83D with dark text #1A1408, light text #E9E4DA.

## Workflow
1. get_reference (fonts, uploaded images), list_projects / get_project.
2. create_project (a new project for your design; do not change the user's projects unless asked),
   then set_face / update_elements.
3. check_layout, then preview_on_tab5 + tab5_screenshot to see the real result, fix, repeat.
4. save_to_tab5 only when the user asks for it.
The user sees the project in Studio after opening it (Open project); a project already open in Studio must be
opened again to show the changes, and saving it in Studio would overwrite them.
"""


# ------------------------------------------------------------------ Studio ---

class StudioError(Exception):
    pass


def studio(path, body=None, method=None, raw=False, timeout=60):
    """Request to Tab5 Studio; returns parsed JSON (or bytes with raw)"""
    data = json.dumps(body, ensure_ascii=False).encode("utf-8") if body is not None else None
    req = urllib.request.Request(STUDIO_URL + path, data=data, method=method or ("POST" if data is not None else "GET"))
    if data is not None:
        req.add_header("Content-Type", "application/json")
    if PASSWORD:
        req.add_header("Authorization", "Basic " + base64.b64encode(b"mcp:" + PASSWORD.encode()).decode())
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            out = r.read()
            if raw:
                return out, r.headers.get("Content-Type", "")
            return json.loads(out or b"{}")
    except urllib.error.HTTPError as e:
        msg = e.read().decode("utf-8", "replace")[:300]
        if e.code == 401:
            raise StudioError("Studio refused the login: set STUDIO_PASSWORD for the MCP server to Studio's password")
        if e.code == 404:
            raise StudioError(f"Not found: {path}")
        try:
            msg = json.loads(msg).get("error") or msg
        except ValueError:
            pass
        raise StudioError(f"Studio: HTTP {e.code} {msg}")
    except urllib.error.URLError as e:
        raise StudioError(f"No connection to Tab5 Studio at {STUDIO_URL} ({e.reason}); is it running? Set STUDIO_URL.")


def quote(name):
    return urllib.parse.quote(str(name), safe="")


def get_project(name):
    p = studio(f"/api/projects/{quote(name)}")
    if not isinstance(p, dict) or not isinstance(p.get("scene"), dict):
        raise StudioError(f"Project {name!r} has no scene")
    return p["scene"]


def put_project(name, scene):
    studio(f"/api/projects/{quote(name)}", {"name": name, "scene": scene}, method="PUT")


def safe_name(name):
    """The name Studio stores the project under (the same rule as safe_name() in app/server.py)"""
    return re.sub(r"[^\w \-ąćęłńóśźżĄĆĘŁŃÓŚŹŻ]", "", str(name or "").strip())[:40].strip()


def project_names():
    return [p["name"] for p in studio("/api/projects").get("projects", [])]


def nid():
    return "m" + "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(8))


def empty_scene():
    return {"colors": {"digit": "#5BE7E1", "sensor": "auto"}, "faces": {
        "basic": {"bg": "#050505", "elements": []}, "full": {"bg": "#050505", "elements": []},
        "third": {"bg": "#050505", "enabled": False, "elements": []},
        "intro": {"bg": "#000000", "builtin": True, "ms": 4000, "elements": []},
        "alarm": {"bg": "#0B0D10", "builtin": True, "elements": []},
        "timer": {"bg": "#0B0D10", "builtin": True, "elements": []}}}


def norm_element(el, face):
    """Defaults Studio expects; raises StudioError for what Studio could not show"""
    if not isinstance(el, dict):
        raise StudioError("Each element must be an object")
    t = el.get("type")
    if t not in TYPES:
        raise StudioError(f"Unknown element type {t!r}; types: {', '.join(TYPES)}")
    for k in ("x", "y"):
        if not isinstance(el.get(k), (int, float)):
            raise StudioError(f"{t}: {k} (number) is required")
    out = {"id": str(el.get("id") or nid()), "name": el.get("name") or t}
    out.update({k: v for k, v in el.items() if k not in ("id", "name")})
    for k, v in out.items():  # whole numbers, as Studio stores them (and as its sceneHash sees them)
        if isinstance(v, float):
            out[k] = round(v)
    out.setdefault("visible", True)
    out.setdefault("locked", False)
    out.setdefault("opacity", 100)
    if t == "clock":
        out.setdefault("style", "led")
        out.setdefault("size", 280)
        out.setdefault("font", "inter")
        out.setdefault("color", "")
    act = out.get("action")
    if act:
        ok = ALARM_ACTIONS if face == "alarm" else TIMER_ACTIONS if face == "timer" else ()
        if act not in ok:
            raise StudioError(f"Action {act!r} is not available on the {face} face" + (f" (use: {', '.join(ok)})" if ok else ""))
    return out


def norm_face(face_key, face):
    if len(face.get("elements", [])) > 64:
        raise StudioError("Too many elements on one face (max 64)")
    ids = set()
    for el in face["elements"]:
        while el["id"] in ids:
            el["id"] = nid()
        ids.add(el["id"])


# ----------------------------------------------------------- layout check ---

def text_w(chars, size):
    return chars * size * 0.56


def bbox(el):
    """Approximate box (x, y, w, h) of an element as the Tab5 draws it"""
    t, x, y = el.get("type"), el.get("x", 0), el.get("y", 0)
    size = el.get("size") or 40
    w, h = el.get("w") or 0, el.get("h") or 0
    if t == "clock":
        n = 8 if el.get("seconds") else 5
        if el.get("style") == "font":
            return x, y, n * size * 0.42, size * 1.25  # digits and colons of a font clock are narrower than text
        return x, y, (858 if el.get("seconds") else 716) * size / 280, size
    if t == "analog":
        return x, y, size, size
    if t in ("rect", "ellipse", "image", "loader"):
        return x, y, w, h
    if t == "line":
        th = el.get("width", 4)
        return min(x, x + w), min(y, y + h), abs(w) + th, abs(h) + th
    if t == "icon":
        return x, y, size, size
    if t == "weather":
        if el.get("source", 0) <= 1:
            return x, y, size, size
        return x, y, text_w(5, size), size * 1.25
    if t == "forecast":
        icon = el.get("icon_size") or size * 1.6
        return x, y, w or 170, size * 2.6 + icon
    if t in ("alarm_badge", "timer_badge"):
        icon = el.get("icon_size") or size
        show = el.get("show", "both")
        bw = (icon if show != "time" else 0) + (text_w(5, size) if show != "icon" else 0) + (12 if show == "both" else 0)
        return x, y, bw, max(icon, size * 1.25)
    if t == "sensor":
        unit = str(el.get("unit", ""))
        us = el.get("unit_size") or size * 0.5
        dec = el.get("decimals", 1) or 0
        digits = 2 + dec + (1 if dec else 0)
        return x, y, text_w(digits, size) + text_w(len(unit), us), size * 1.25
    if t == "date":
        n = {0: 24, 1: 10, 2: 12, 3: 14}.get(el.get("format", 0), 20)
        return x, y, text_w(n, size), size * 1.25
    chars = {"text": len(str(el.get("text", ""))), "place": 12, "alarm_label": 12, "timer_time": 5, "timer_go": 5}.get(t, 8)
    if w and el.get("align", "left") != "left":
        return x, y, w, size * 1.25
    return x, y, w or text_w(chars, size), size * 1.25


def check_face(scene, face_key):
    face = scene["faces"].get(face_key) or {}
    notes = []
    if face_key in ("intro", "alarm", "timer") and face.get("builtin", True):
        notes.append(f"{face_key}: builtin is true, so the Tab5 shows its built-in screen and these elements are not used")
    if face_key == "third" and not face.get("enabled"):
        notes.append("third: enabled is false, so the Tab5 does not show Wariant-3")
    els = [e for e in face.get("elements", []) if e.get("visible", True)]
    boxes = []
    for e in els:
        x, y, w, h = bbox(e)
        label = f"{e.get('name') or e['type']} [{e.get('id')}]"
        boxes.append((e, label, (x, y, w, h)))
        if x < 0 or y < 0 or x + w > W + 1 or y + h > H + 1:
            notes.append(f"{label}: about {round(x)},{round(y)} {round(w)}x{round(h)} goes off the 1280x720 screen")
        elif face_key in ("basic", "full", "third") and x < 300 and y < 84 and e["type"] not in ("rect", "ellipse", "line", "image"):
            if not any(b.get("type") == "timer_badge" for b in els):
                notes.append(f"{label}: in the top-left corner where the timer pill appears while the timer runs")
        if face_key in ("basic", "full", "third") and x + w > 1176 and y < 96 and e["type"] not in ("rect", "ellipse", "line", "image"):
            notes.append(f"{label}: under the menu button in the top-right corner (about x 1186-1270, y 10-94)")
    shapes = ("rect", "ellipse", "line", "image", "loader")
    for i in range(len(boxes)):
        for j in range(i + 1, len(boxes)):
            (a, la, (ax, ay, aw, ah)), (b, lb, (bx, by, bw, bh)) = boxes[i], boxes[j]
            if a["type"] in shapes or b["type"] in shapes:
                continue  # backgrounds and lines under content are expected
            ox = min(ax + aw, bx + bw) - max(ax, bx)
            oy = min(ay + ah, by + bh) - max(ay, by)
            if ox > 6 and oy > 6:
                notes.append(f"{la} and {lb} overlap (about {round(ox)}x{round(oy)} px)")
    if face_key in ("alarm", "timer") and not face.get("builtin", True):
        acts = {e.get("action") for e in els}
        need = "stop" if face_key == "alarm" else "timer_close"
        if need not in acts:
            notes.append(f"{face_key}: no element with action {need!r}; the user could not leave this screen")
    if len(els) > 64:
        notes.append(f"{len(els)} elements: the Tab5 takes max 64 per face")
    lines = [f"{FACE_NAMES[face_key]} ({face_key}): {len(els)} visible elements, bg {face.get('bg')}"]
    for e, label, (x, y, w, h) in boxes:
        lines.append(f"  {label}: {e['type']} at {round(x)},{round(y)} size ~{round(w)}x{round(h)}")
    lines.append("Problems:" if notes else "No problems found (boxes are estimates; check the Tab5 screenshot for the real look).")
    lines += ["  - " + n for n in notes]
    return "\n".join(lines)


# ------------------------------------------------------------------ hash ---

def scene_hash(sc):
    """Studio's sceneHash (so Studio shows the saved project as up to date)"""
    t = json.dumps(sc, ensure_ascii=False, separators=(",", ":")).encode("utf-16-le")
    m = 0xFFFFFFFF
    h1, h2 = 0xDEADBEEF, 0x41C6CE57
    for i in range(0, len(t), 2):
        c = t[i] | (t[i + 1] << 8)
        h1 = ((h1 ^ c) * 2654435761) & m
        h2 = ((h2 ^ c) * 1597334677) & m
    h1 = (((h1 ^ (h1 >> 16)) * 2246822507) & m) ^ (((h2 ^ (h2 >> 13)) * 3266489909) & m)
    h2 = (((h2 ^ (h2 >> 16)) * 2246822507) & m) ^ (((h1 ^ (h1 >> 13)) * 3266489909) & m)
    return format(h2, "x") + format(h1, "x")


# ------------------------------------------------------------------ tools ---

ICONS = None


def icons():
    global ICONS
    if ICONS is None:
        local = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "app", "static", "lucide.json")
        if os.path.exists(local):  # next to Studio in the repo
            with open(local, encoding="utf-8") as f:
                ICONS = json.load(f)
        else:
            ICONS = studio("/api/icons")
    return ICONS


def t_reference(a):
    meta = studio("/api/meta")
    fonts = ", ".join(f"{f['name']} ({f['title']})" for f in meta.get("fonts", []))
    try:
        assets = studio("/api/assets").get("assets", [])
        imgs = ", ".join(f"{i['name']} {i['w']}x{i['h']}" for i in assets) or "none"
    except StudioError:
        imgs = "unknown"
    return REFERENCE + f"\n## This Studio\nFonts: {fonts}\nUploaded images: {imgs}\n"


def t_list(a):
    items = studio("/api/projects").get("projects", [])
    if not items:
        return "No projects yet."
    return "Projects (newest first):\n" + "\n".join(f"- {p['name']}" for p in items)


def t_get(a):
    sc = get_project(a["name"])
    face = a.get("face")
    if face:
        if face not in FACES:
            raise StudioError(f"face must be one of {', '.join(FACES)}")
        return json.dumps(sc["faces"].get(face), ensure_ascii=False, indent=1)
    return json.dumps(sc, ensure_ascii=False, indent=1)


def t_create(a):
    asked = str(a["name"]).strip()
    name = safe_name(asked)
    if not name or len(asked) > 40:
        raise StudioError("name: 1-40 characters (letters, digits, spaces, - and _)")
    if name in project_names() and not a.get("overwrite"):
        raise StudioError(f"Project {name!r} exists; pick another name or pass overwrite true (only if the user agreed)")
    base = a.get("copy_from")
    sc = get_project(base) if base else empty_scene()
    if a.get("colors"):
        sc["colors"] = {**sc.get("colors", {}), **a["colors"]}
    put_project(name, sc)
    note = f" (asked for {asked!r}; Studio keeps only letters, digits, spaces, - and _)" if name != asked else ""
    return f"Created project {name!r}{note}" + (f" as a copy of {base!r}" if base else " (empty clock faces, built-in intro / alarm / timer)")


def face_arg(a):
    f = a.get("face")
    if f not in FACES:
        raise StudioError(f"face must be one of {', '.join(FACES)}")
    return f


def t_set_face(a):
    name, face_key = a["project"], face_arg(a)
    sc = get_project(name)
    face = dict(sc["faces"].get(face_key) or {})
    face["elements"] = [norm_element(e, face_key) for e in a.get("elements", [])]
    for k in ("bg", "enabled", "builtin", "ms"):
        if k in a:
            face[k] = a[k]
    if face_key in ("intro", "alarm", "timer") and a.get("elements") and "builtin" not in a:
        face["builtin"] = False  # designed elements: use them instead of the built-in screen
    if face_key == "third" and a.get("elements") and "enabled" not in a:
        face["enabled"] = True
    norm_face(face_key, face)
    sc["faces"][face_key] = face
    if a.get("colors"):
        sc["colors"] = {**sc.get("colors", {}), **a["colors"]}
    put_project(name, sc)
    return f"Saved {len(face['elements'])} elements on {face_key} of {name!r}.\n\n" + check_face(sc, face_key)


def t_update(a):
    name, face_key = a["project"], face_arg(a)
    sc = get_project(name)
    face = sc["faces"].get(face_key)
    if not isinstance(face, dict):
        raise StudioError(f"Project {name!r} has no {face_key} face yet; create it with set_face")
    by_id = {e.get("id"): e for e in face.get("elements", [])}
    done = []
    for ch in a.get("changes", []):
        el = by_id.get(ch.get("id"))
        if not el:
            raise StudioError(f"No element with id {ch.get('id')!r} on {face_key}")
        for k, v in ch.items():
            if k == "id":
                continue
            if v is None:
                el.pop(k, None)
            else:
                el[k] = round(v) if isinstance(v, float) else v
        norm_element(el, face_key)
        done.append(ch["id"])
    rm = set(a.get("remove", []))
    if rm:
        face["elements"] = [e for e in face["elements"] if e.get("id") not in rm]
    for e in a.get("add", []):
        face["elements"].append(norm_element(e, face_key))
    for k in ("bg", "enabled", "builtin", "ms"):
        if k in a:
            face[k] = a[k]
    norm_face(face_key, face)
    if a.get("colors"):
        sc["colors"] = {**sc.get("colors", {}), **a["colors"]}
    put_project(name, sc)
    return (f"Updated {len(done)}, removed {len(rm)}, added {len(a.get('add', []))} on {face_key} of {name!r}.\n\n"
            + check_face(sc, face_key))


def t_check(a):
    sc = get_project(a["project"])
    return check_face(sc, face_arg(a))


def t_icons(a):
    q = str(a.get("query", "")).lower().strip()
    words = [w for w in q.replace(",", " ").split() if w]
    hits = [(n, cp) for n, cp in icons() if all(w in n for w in words)] if words else icons()
    lim = int(a.get("limit") or 40)
    if not hits:
        return "No icon matches; try a shorter English word (Lucide names, e.g. sun, cloud, droplet, thermometer, wind)."
    return f"{len(hits)} icons" + (f" (first {lim})" if len(hits) > lim else "") + ":\n" + \
        "\n".join(f"- {n}: cp {cp} (0x{cp:X})" for n, cp in hits[:lim])


def t_preview(a):
    sc = get_project(a["project"])
    face = face_arg(a) if a.get("face") else "basic"
    r = studio("/api/live", {"scene": sc, "face": face}, timeout=180)
    if not r.get("ok"):
        raise StudioError(r.get("error") or "Preview failed")
    return (f"The Tab5 shows {a['project']!r} ({face}) as a live preview ({r.get('kb')} KB of images, {r.get('seconds')} s). "
            "It lasts until the Tab5 restarts or the preview is stopped. Use tab5_screenshot to see it.")


def t_screenshot(a):
    data, ctype = studio("/api/tab5/screen", raw=True, timeout=40)
    if not ctype.startswith("image/"):
        try:
            raise StudioError(json.loads(data).get("error") or "No screenshot")
        except ValueError:
            raise StudioError("No screenshot")
    return [{"type": "image", "data": base64.b64encode(data).decode(), "mimeType": ctype.split(";")[0]},
            {"type": "text", "text": "What the Tab5 shows now (1280x720)."}]


def t_stop(a):
    r = studio("/api/live/stop", {})
    if not r.get("ok"):
        raise StudioError(r.get("error") or "Could not stop the preview")
    return "Live preview stopped: the Tab5 shows its saved design again."


def t_save_tab5(a):
    name = a["project"]
    sc = get_project(name)
    shown = str(a.get("name") or name).strip()[:40]
    r = studio("/api/live", {"scene": sc, "face": a.get("face") or "basic",
                             "save": {"name": shown, "project": name, "hash": scene_hash(sc)}}, timeout=400)
    if not r.get("ok"):
        raise StudioError(r.get("error") or "Saving failed")
    return f"Saved {name!r} on the Tab5 as {shown!r} ({r.get('saved_kb')} KB); it shows after every start. The Tab5 palette was set from the project."


OBJ = {"type": "object"}
FACE = {"type": "string", "enum": list(FACES), "description": "basic = Wariant-1, full = Wariant-2, third = Wariant-3, intro, alarm, timer"}
COLORS = {"type": "object", "description": "Palette: {digit: '#RRGGBB', sensor: 'auto' or '#RRGGBB'}",
          "properties": {"digit": {"type": "string"}, "sensor": {"type": "string"}}}
FACE_OPTS = {"bg": {"type": "string", "description": "Background #RRGGBB"},
             "enabled": {"type": "boolean", "description": "third only: show Wariant-3"},
             "builtin": {"type": "boolean", "description": "intro / alarm / timer: true = built-in screen, false = designed elements"},
             "ms": {"type": "integer", "description": "intro only: duration in ms"}}

TOOLS = [
    ("get_reference", t_reference, "Read first. The Tab5 Studio scene format: screen size, faces, every element type and its fields, "
     "tap actions, layout rules, the fonts and uploaded images of this Studio.", {}, []),
    ("list_projects", t_list, "List the Tab5 Studio projects.", {}, []),
    ("get_project", t_get, "Read a project's scene as JSON (or one face of it).",
     {"name": {"type": "string"}, "face": FACE}, ["name"]),
    ("create_project", t_create, "Create a new project: empty clock faces, or a copy of an existing project. "
     "Design in your own new project rather than changing the user's projects.",
     {"name": {"type": "string", "description": "Up to 40 characters"}, "copy_from": {"type": "string", "description": "Existing project to copy"},
      "colors": COLORS, "overwrite": {"type": "boolean", "description": "Replace an existing project (only when the user agreed)"}}, ["name"]),
    ("set_face", t_set_face, "Replace all elements of one face (screen) of a project, optionally its background and palette. "
     "Returns a layout check. Elements: see get_reference.",
     {"project": {"type": "string"}, "face": FACE, "elements": {"type": "array", "items": OBJ}, "colors": COLORS, **FACE_OPTS},
     ["project", "face", "elements"]),
    ("update_elements", t_update, "Change some elements of a face by id (only the given fields; null removes a field), remove "
     "elements, add new ones. Returns a layout check.",
     {"project": {"type": "string"}, "face": FACE,
      "changes": {"type": "array", "items": OBJ, "description": "[{id, field: value, ...}]"},
      "remove": {"type": "array", "items": {"type": "string"}, "description": "ids to remove"},
      "add": {"type": "array", "items": OBJ, "description": "new elements"}, "colors": COLORS, **FACE_OPTS},
     ["project", "face"]),
    ("check_layout", t_check, "Estimate element boxes on a face and report elements off screen, overlapping, or under the timer pill.",
     {"project": {"type": "string"}, "face": FACE}, ["project", "face"]),
    ("search_icons", t_icons, "Find Lucide icons by English name words; returns code points for icon / badge elements.",
     {"query": {"type": "string"}, "limit": {"type": "integer"}}, ["query"]),
    ("preview_on_tab5", t_preview, "Show a project on the Tab5 as a temporary live preview (nothing saved on the Tab5); "
     "face picks the screen to open. Needs the Tab5 address and PIN in Studio's settings.",
     {"project": {"type": "string"}, "face": FACE}, ["project"]),
    ("tab5_screenshot", t_screenshot, "Screenshot of what the Tab5 shows now (JPEG). Use after preview_on_tab5 to check the real look.", {}, []),
    ("stop_preview", t_stop, "End the live preview on the Tab5.", {}, []),
    ("save_to_tab5", t_save_tab5, "Save a project on the Tab5 for good (it shows after every start) and set the Tab5 palette "
     "from the project. Only when the user asks for it.",
     {"project": {"type": "string"}, "name": {"type": "string", "description": "Name shown on the Tab5 (default: project name)"}, "face": FACE},
     ["project"]),
]
HANDLERS = {n: f for n, f, *_ in TOOLS}


def tool_list():
    return [{"name": n, "description": d, "inputSchema": {"type": "object", "properties": p, "required": r}}
            for n, _f, d, p, r in TOOLS]


# ------------------------------------------------------------------- MCP ---

def reply(id_, result=None, error=None):
    msg = {"jsonrpc": "2.0", "id": id_}
    if error:
        msg["error"] = error
    else:
        msg["result"] = result
    sys.stdout.write(json.dumps(msg, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def handle(msg):
    method, id_ = msg.get("method"), msg.get("id")
    if id_ is None:
        return  # notifications (initialized, cancelled ...)
    if method == "initialize":
        ver = (msg.get("params") or {}).get("protocolVersion") or "2025-06-18"
        return reply(id_, {"protocolVersion": ver, "capabilities": {"tools": {}},
                           "serverInfo": {"name": "tab5studio", "version": VERSION},
                           "instructions": "Designs Tab5 screens in Tab5 Studio. Call get_reference before designing; "
                                           "save on the Tab5 only when the user asks."})
    if method == "ping":
        return reply(id_, {})
    if method == "tools/list":
        return reply(id_, {"tools": tool_list()})
    if method == "tools/call":
        p = msg.get("params") or {}
        f = HANDLERS.get(p.get("name"))
        if not f:
            return reply(id_, error={"code": -32602, "message": f"Unknown tool {p.get('name')}"})
        try:
            out = f(p.get("arguments") or {})
            content = out if isinstance(out, list) else [{"type": "text", "text": out}]
            return reply(id_, {"content": content, "isError": False})
        except (StudioError, KeyError, TypeError, ValueError) as e:
            text = f"Missing argument {e}" if isinstance(e, KeyError) else str(e)
            return reply(id_, {"content": [{"type": "text", "text": text}], "isError": True})
    reply(id_, error={"code": -32601, "message": f"Method not found: {method}"})


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            reply(None, error={"code": -32700, "message": "Parse error"})
            continue
        for m in (msg if isinstance(msg, list) else [msg]):
            if not isinstance(m, dict):
                reply(None, error={"code": -32600, "message": "Invalid Request"})
                continue
            try:
                handle(m)
            except Exception as e:  # noqa: BLE001 — keep serving
                if m.get("id") is not None:
                    reply(m["id"], error={"code": -32603, "message": str(e)})


if __name__ == "__main__":
    main()
