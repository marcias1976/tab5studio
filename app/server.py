"""
Tab5 Studio — author: Jakub W. (MARCIASMEDIA)

Tab5 Studio: WYSIWYG editor for the Tab5 clock faces that compiles the firmware with the designed layout.

Environment:
  FIRMWARE_DIR    Tab5 firmware source (default: ../firmware next to app/)
  DATA_DIR        projects, uploaded fonts, build workspace and results (default: ../data)
  IDF_EXPORT      path to esp-idf export.sh (not needed in the Docker image, where ESP-IDF is already set up)
  STUDIO_PASSWORD when set, the app asks for it on /login (scripts may use HTTP Basic, any user name)
  WIFI_SSID, WIFI_PASSWORD, WEB_PIN   optional: written into the firmware for each build
                                      (otherwise the secrets files in FIRMWARE_DIR are used)
  PORT            default 8080
"""
import hashlib
import hmac
import io
import ipaddress
import json
import os
import re
import secrets
import shutil
import subprocess
import threading
import time
import uuid
import zipfile
from datetime import timedelta
from functools import wraps
from pathlib import Path
from urllib.parse import urlsplit

from flask import Flask, Response, abort, jsonify, redirect, request, send_file, send_from_directory, session

APP_DIR = Path(__file__).resolve().parent
FIRMWARE_DIR = Path(os.environ.get("FIRMWARE_DIR", APP_DIR.parent / "firmware")).resolve()
DATA_DIR = Path(os.environ.get("DATA_DIR", APP_DIR.parent / "data")).resolve()
IDF_EXPORT = os.environ.get("IDF_EXPORT", "")
PASSWORD = os.environ.get("STUDIO_PASSWORD", "")

PROJECTS = DATA_DIR / "projects"
LIBRARY = DATA_DIR / "library"  # saved designs of single screens: library/<kind>/<name>.json
LIBRARY_KINDS = ("clock", "intro", "alarm", "timer")
FONTS = DATA_DIR / "fonts"
ASSETS = DATA_DIR / "assets"
BUILDS = DATA_DIR / "builds"
WORK = DATA_DIR / "work" / "src"
for d in (PROJECTS, FONTS, ASSETS, BUILDS, WORK.parent):
    d.mkdir(parents=True, exist_ok=True)

BUILTIN_TITLES = {"inter": "Inter", "inter_light": "Inter Light", "orbitron": "Orbitron", "roboto_mono": "Roboto Mono",
                  "bebas": "Bebas Neue", "oswald": "Oswald", "rajdhani": "Rajdhani Bold"}

app = Flask(__name__, static_folder=None)
app.config["MAX_CONTENT_LENGTH"] = 32 * 1024 * 1024  # a project import with its images and fonts


# ---------------------------------------------------------------- password ---

def session_key():
    """Random key that signs the login cookie, kept in DATA_DIR so logins survive a restart"""
    path = DATA_DIR / ".session_key"
    if not path.exists():
        path.write_bytes(secrets.token_bytes(32))
        path.chmod(0o600)
    return path.read_bytes()


app.secret_key = session_key()
app.config.update(SESSION_COOKIE_HTTPONLY=True, SESSION_COOKIE_SAMESITE="Lax",
                  PERMANENT_SESSION_LIFETIME=timedelta(days=30))


def password_tag():
    """Stored in the session: changing STUDIO_PASSWORD logs everybody out"""
    return hmac.new(app.secret_key, PASSWORD.encode(), hashlib.sha256).hexdigest()[:32]


def logged_in():
    if not PASSWORD or session.get("pw") == password_tag():
        return True
    # scripts can still call the API with HTTP Basic (curl -u x:<password>); pages ignore it, because a browser
    # that once saw the old Basic dialog keeps sending it and would never be signed out
    au = request.authorization if request.path.startswith("/api/") else None
    if not au:
        return False
    ip = request.remote_addr or ""
    fails, last = login_fails.get(ip, (0, 0))
    if fails >= 5 and time.time() - last < 30:  # the same lockout as the login page
        return False
    if hmac.compare_digest((au.password or "").encode(), PASSWORD.encode()):
        login_fails.pop(ip, None)
        return True
    login_fails[ip] = (fails + 1, time.time())
    return False


def auth(f):
    @wraps(f)
    def wrapper(*a, **kw):
        if not logged_in():
            if request.method == "GET" and request.path == "/":
                return redirect("/login")
            return Response("Zaloguj się do Tab5 Studio", 401)  # no WWW-Authenticate: no browser dialog
        return f(*a, **kw)
    return wrapper


login_fails = {}  # ip -> (failed attempts, time of the last one)


@app.get("/login")
def login_page():
    if logged_in():
        return redirect("/")
    return send_from_directory(APP_DIR / "static", "login.html", max_age=0)


@app.post("/api/login")
def login():
    ip = request.remote_addr or ""
    fails, last = login_fails.get(ip, (0, 0))
    if fails >= 5 and time.time() - last < 30:
        return jsonify(ok=False), 429
    pw = str((request.get_json(silent=True) or {}).get("password", ""))
    if PASSWORD and not hmac.compare_digest(pw.encode(), PASSWORD.encode()):
        login_fails[ip] = (fails + 1, time.time())
        time.sleep(0.5)
        return jsonify(ok=False), 401
    login_fails.pop(ip, None)
    session.permanent = True
    session["pw"] = password_tag()
    return jsonify(ok=True)


@app.get("/logout")
def logout():
    session.clear()
    return redirect("/login")


def safe_name(name, maxlen=40):
    """File-system safe name: letters (also Polish), digits, space, - and _"""
    name = re.sub(r"[^\w \-ąćęłńóśźżĄĆĘŁŃÓŚŹŻ]", "", (name or "").strip())[:maxlen].strip()
    if not name:
        abort(400, "Zła nazwa")
    return name


def json_body():
    """JSON request body; only real application/json, which other sites cannot send without asking first"""
    if not request.is_json:
        abort(415, "Oczekiwano JSON")
    return request.get_json()


ALLOWED_HOSTS = {h.strip().lower() for h in os.environ.get("ALLOWED_HOSTS", "").split(",") if h.strip()}


def host_allowed(host):
    """Without a password Studio answers only to an IP address, localhost or a name from ALLOWED_HOSTS: a web page
    on another domain that points its name at this server (DNS rebinding) gets nothing"""
    if host.startswith("["):  # [IPv6]:port
        name = host[1:host.find("]")]
    else:
        name = host.rsplit(":", 1)[0] if host.count(":") == 1 else host
    name = name.lower()
    if name in ("localhost",) or name in ALLOWED_HOSTS:
        return True
    try:
        ipaddress.ip_address(name)
        return True
    except ValueError:
        return False


@app.before_request
def same_origin():
    """Changes only from Studio's own page: a form or script on another site is refused"""
    if not PASSWORD and not host_allowed(request.host or ""):
        return Response("Ustaw STUDIO_PASSWORD albo dopisz tę nazwę do ALLOWED_HOSTS w .env", 403)
    origin = request.headers.get("Origin")
    if request.method in ("POST", "PUT", "DELETE") and origin and urlsplit(origin).netloc != request.host:
        abort(403)


# ------------------------------------------------------------------- pages ---

@app.get("/")
@auth
def index():
    return send_from_directory(APP_DIR / "static", "index.html")


@app.get("/static/<path:name>")
@auth
def static_file(name):
    # the editor and its help change with updates: always check for a newer version
    fresh = name == "index.html" or name.startswith("help/")
    return send_from_directory(APP_DIR / "static", name, max_age=0 if fresh else 3600)


@app.get("/fonts/<name>")
@auth
def font_file(name):
    if not re.fullmatch(r"[a-z0-9_]+", name):
        abort(404)
    path = FIRMWARE_DIR / "main" / "ttf" / f"{name}.ttf"
    if not path.exists():
        path = FONTS / f"{name}.ttf"
    if not path.exists():
        abort(404)
    return send_file(path, mimetype="font/ttf", max_age=3600)


@app.get("/api/meta")
@auth
def meta():
    fonts = []
    for f in sorted((FIRMWARE_DIR / "main" / "ttf").glob("*.ttf")):
        if f.stem != "icons":
            fonts.append({"name": f.stem, "title": BUILTIN_TITLES.get(f.stem, f.stem), "builtin": True})
    for f in sorted(FONTS.glob("*.ttf")):
        title = (FONTS / f"{f.stem}.title").read_text("utf-8") if (FONTS / f"{f.stem}.title").exists() else f.stem
        fonts.append({"name": f.stem, "title": title, "builtin": False})
    return jsonify(fonts=fonts, firmware=str(FIRMWARE_DIR), firmware_ok=(FIRMWARE_DIR / "main" / "layout.cpp").exists())


# ------------------------------------------------------------------- fonts ---

@app.post("/api/fonts")
@auth
def font_upload():
    f = request.files.get("file")
    if not f or not f.filename.lower().endswith((".ttf", ".otf")):
        abort(400, "Wybierz plik .ttf lub .otf")
    title = Path(f.filename).stem[:40]
    # C identifier for the embedded file symbol (_binary_<name>_ttf_start), unique, max 28 chars
    base = "f_" + (re.sub(r"[^a-z0-9]+", "_", title.lower()).strip("_") or "font")[:20]
    name, n = base, 2
    while (FONTS / f"{name}.ttf").exists():
        name, n = f"{base}_{n}", n + 1
    data = f.read()
    if data[:4] not in (b"\x00\x01\x00\x00", b"OTTO", b"true"):
        abort(400, "To nie jest plik czcionki")
    (FONTS / f"{name}.ttf").write_bytes(data)
    (FONTS / f"{name}.title").write_text(title, "utf-8")
    return jsonify(name=name, title=title)


@app.delete("/api/fonts/<name>")
@auth
def font_delete(name):
    if not re.fullmatch(r"f_[a-z0-9_]+", name):
        abort(400)
    for ext in (".ttf", ".title"):
        (FONTS / f"{name}{ext}").unlink(missing_ok=True)
    return jsonify(ok=True)


# ------------------------------------------------------------------ images ---

@app.get("/api/assets")
@auth
def assets():
    from PIL import Image
    items = []
    for f in sorted(ASSETS.glob("*.png"), key=lambda p: p.stat().st_mtime, reverse=True):
        with Image.open(f) as im:
            items.append({"name": f.name, "w": im.width, "h": im.height})
    return jsonify(assets=items)


@app.get("/assets/<name>")
@auth
def asset_file(name):
    if not re.fullmatch(r"[\w\-]+\.png", name):
        abort(404)
    return send_from_directory(ASSETS, name, max_age=3600)


@app.post("/api/assets")
@auth
def asset_upload():
    """Images (PNG, JPG, WebP; SVG is turned into PNG by the browser) are stored as PNG, max 1280×720"""
    from PIL import Image
    f = request.files.get("file")
    if not f:
        abort(400, "Brak pliku")
    try:
        im = Image.open(f.stream)
        im.load()
    except Exception:
        abort(400, "Nieobsługiwany obraz")
    im = im.convert("RGBA")
    im.thumbnail((1280, 720), Image.LANCZOS)
    base = re.sub(r"[^a-z0-9]+", "-", Path(f.filename or "obraz").stem.lower()).strip("-")[:30] or "obraz"
    name, n = f"{base}.png", 2
    while (ASSETS / name).exists():
        name, n = f"{base}-{n}.png", n + 1
    im.save(ASSETS / name, optimize=True)
    return jsonify(name=name, w=im.width, h=im.height)


@app.delete("/api/assets/<name>")
@auth
def asset_delete(name):
    if not re.fullmatch(r"[\w\-]+\.png", name):
        abort(400)
    (ASSETS / name).unlink(missing_ok=True)
    return jsonify(ok=True)


# ---------------------------------------------------------------- projects ---

# ------------------------------------------------------------------ library --
# One screen design (a clock face, an intro, …) saved under a name; a project copies the ones it uses.
def lib_dir(kind):
    if kind not in LIBRARY_KINDS:
        abort(404)
    d = LIBRARY / kind
    d.mkdir(parents=True, exist_ok=True)
    return d


@app.get("/api/library/<kind>")
@auth
def library_list(kind):
    items = []
    for f in sorted(lib_dir(kind).glob("*.json"), key=lambda p: p.stat().st_mtime, reverse=True):
        try:
            data = json.loads(f.read_text("utf-8"))
        except ValueError:
            continue
        items.append({"name": data.get("name", f.stem), "mtime": int(f.stat().st_mtime), "face": data.get("face")})
    return jsonify(items=items)


@app.put("/api/library/<kind>/<name>")
@auth
def library_put(kind, name):
    body = json_body() or {}
    if not isinstance(body.get("face"), dict):
        abort(400, "face")
    nm = str(body.get("name") or name).strip()[:40] or "Bez nazwy"
    (lib_dir(kind) / f"{safe_name(nm)}.json").write_text(json.dumps({"name": nm, "face": body["face"]}, ensure_ascii=False), "utf-8")
    return jsonify(ok=True, name=nm)


@app.post("/api/library/<kind>/<name>/rename")
@auth
def library_rename(kind, name):
    new = str((json_body() or {}).get("name", "")).strip()[:40]
    src, dst = lib_dir(kind) / f"{safe_name(name)}.json", lib_dir(kind) / f"{safe_name(new)}.json"
    if not new or not src.exists():
        abort(400)
    if dst.exists() and dst != src:
        abort(409, "Taka nazwa już jest")
    data = json.loads(src.read_text("utf-8"))
    data["name"] = new
    dst.write_text(json.dumps(data, ensure_ascii=False), "utf-8")
    if dst != src:
        src.unlink()
    return jsonify(ok=True)


@app.delete("/api/library/<kind>/<name>")
@auth
def library_delete(kind, name):
    f = lib_dir(kind) / f"{safe_name(name)}.json"
    if f.exists():
        f.unlink()
    return jsonify(ok=True)


@app.get("/api/projects")
@auth
def projects():
    items = []
    for f in sorted(PROJECTS.glob("*.json"), key=lambda p: p.stat().st_mtime, reverse=True):
        items.append({"name": f.stem, "mtime": int(f.stat().st_mtime)})
    return jsonify(projects=items)


@app.get("/api/projects/<name>")
@auth
def project_get(name):
    path = PROJECTS / f"{safe_name(name)}.json"
    if not path.exists():
        abort(404)
    return send_file(path, mimetype="application/json", max_age=0)


@app.put("/api/projects/<name>")
@auth
def project_put(name):
    body = json_body()
    if not isinstance(body, dict) or "scene" not in body:
        abort(400)
    (PROJECTS / f"{safe_name(name)}.json").write_text(json.dumps(body, ensure_ascii=False, indent=1), "utf-8")
    return jsonify(ok=True)


def project_files(body):
    """Images and Studio fonts a project uses"""
    assets, fonts = set(), set()
    for face in ((body.get("scene") or {}).get("faces") or {}).values():
        for el in face.get("elements", []) if isinstance(face, dict) else []:
            if not isinstance(el, dict):
                continue
            if el.get("type") == "image" and re.fullmatch(r"[\w\-]+\.png", str(el.get("asset", ""))):
                assets.add(el["asset"])
            if re.fullmatch(r"f_[a-z0-9_]+", str(el.get("font", ""))):
                fonts.add(el["font"])
    return assets, fonts


@app.get("/api/projects/<name>/export")
@auth
def project_export(name):
    """The project as a .zip with the images and Studio fonts it uses (a backup, or to move it to another Studio)"""
    path = PROJECTS / f"{safe_name(name)}.json"
    if not path.exists():
        abort(404)
    body = json.loads(path.read_text("utf-8"))
    assets, fonts = project_files(body)
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(body, ensure_ascii=False, indent=1))
        for a in sorted(assets):
            if (ASSETS / a).exists():
                z.write(ASSETS / a, f"assets/{a}")
        for f in sorted(fonts):
            for ext in (".ttf", ".title"):
                if (FONTS / f"{f}{ext}").exists():
                    z.write(FONTS / f"{f}{ext}", f"fonts/{f}{ext}")
    buf.seek(0)
    return send_file(buf, mimetype="application/zip", as_attachment=True, download_name=f"{safe_name(name)}.tab5.zip")


@app.post("/api/projects/import")
@auth
def project_import():
    """A .zip from the export: the project under a free name, plus its images and fonts. A file with the same name
    but other content is kept, and the imported one gets a new name (the project is changed to use it)."""
    from PIL import Image
    f = request.files.get("file")
    if not f:
        abort(400, "Brak pliku")
    try:
        z = zipfile.ZipFile(f.stream)
        if "project.json" not in z.namelist() or sum(i.file_size for i in z.infolist()) > 64 * 1024 * 1024:
            raise ValueError
        body = json.loads(z.read("project.json"))
        if not isinstance(body, dict) or not isinstance((body.get("scene") or {}).get("faces"), dict):
            raise ValueError
    except (zipfile.BadZipFile, ValueError, KeyError):
        abort(400, "To nie jest plik eksportu projektu Tab5 Studio")
    renamed = {}

    def free(folder, base, ext, sep):
        n = 2
        while (folder / f"{base}{sep}{n}{ext}").exists():
            n += 1
        return f"{base}{sep}{n}"

    for n in z.namelist():
        if m := re.fullmatch(r"assets/([\w\-]+)\.png", n):
            data = z.read(n)
            try:
                Image.open(io.BytesIO(data)).verify()
            except Exception:
                continue
            name = m.group(1)
            if (ASSETS / f"{name}.png").exists() and (ASSETS / f"{name}.png").read_bytes() != data:
                new = free(ASSETS, name[:30], ".png", "-")
                renamed[f"{name}.png"] = f"{new}.png"
                name = new
            (ASSETS / f"{name}.png").write_bytes(data)
        elif m := re.fullmatch(r"fonts/(f_[a-z0-9_]+)\.ttf", n):
            data = z.read(n)
            if data[:4] not in (b"\x00\x01\x00\x00", b"OTTO", b"true"):
                continue
            name = m.group(1)
            title = z.read(f"fonts/{name}.title").decode("utf-8", "replace")[:40] if f"fonts/{name}.title" in z.namelist() else name
            if (FONTS / f"{name}.ttf").exists() and (FONTS / f"{name}.ttf").read_bytes() != data:
                new = free(FONTS, name[:24], ".ttf", "_")
                renamed[name] = new
                name = new
            (FONTS / f"{name}.ttf").write_bytes(data)
            (FONTS / f"{name}.title").write_text(title, "utf-8")
    for face in body["scene"]["faces"].values():
        for el in face.get("elements", []) if isinstance(face, dict) else []:
            if isinstance(el, dict):
                for k in ("asset", "font"):
                    if el.get(k) in renamed:
                        el[k] = renamed[el[k]]
    base = name = safe_name(body.get("name") or "Import")
    n = 2
    while (PROJECTS / f"{name}.json").exists():
        name, n = safe_name(f"{base[:30]} ({n})"), n + 1
    body["name"] = name
    (PROJECTS / f"{name}.json").write_text(json.dumps(body, ensure_ascii=False, indent=1), "utf-8")
    return jsonify(ok=True, name=name, renamed=len(renamed))


@app.delete("/api/projects/<name>")
@auth
def project_delete(name):
    (PROJECTS / f"{safe_name(name)}.json").unlink(missing_ok=True)
    return jsonify(ok=True)


# ------------------------------------------------------------------- build ---

def c_str(s):
    """C string literal; control characters become octal escapes, so a text cannot end the literal early"""
    out = []
    for ch in str(s):
        if ch in '"\\':
            out.append("\\" + ch)
        elif ord(ch) < 0x20 or ord(ch) == 0x7F:
            out.append(f"\\{ord(ch):03o}")
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def hex6(v, default):
    """#RRGGBB from the scene as RRGGBB for C code; anything else gives the default"""
    return v[1:].upper() if isinstance(v, str) and re.fullmatch(r"#[0-9A-Fa-f]{6}", v) else default


def font_name(v):
    return v if isinstance(v, str) and re.fullmatch(r"[a-z0-9_]{1,40}", v) else "inter"


def c_color(v):
    return f"0x01{v[1:].upper()}u" if isinstance(v, str) and re.fullmatch(r"#[0-9A-Fa-f]{6}", v) else "0"


def num(v, lo, hi, d=0):
    try:
        return max(lo, min(hi, int(v)))
    except (TypeError, ValueError):
        return d


SC = {"clock_led": 0, "clock_font": 1, "sensor": 2, "date": 3, "text": 4, "icon": 5, "image": 6, "rect": 7, "ellipse": 8, "line": 9, "analog": 10, "place": 11, "loader": 12, "alarm_label": 13, "timer_time": 14, "timer_go": 15,
      "alarm_badge": 16, "timer_badge": 17,  # badges: icon and / or time while an alarm is on / the timer runs
      "weather": 18,  # weather icon (now / tomorrow) or sunrise / sunset
      "forecast": 19}  # one day of the forecast: day name, icon, max / min
# Lucide glyphs the Tab5 picks for the weather icon at run time (see weather_glyph in ui.cpp): all go into the icon font
WEATHER_ICONS = (0xE17C, 0xE122, 0xE216, 0xE215, 0xE08C, 0xE214, 0xE08E, 0xE092, 0xE094, 0xE090)
ACTIONS = {"snooze": 1, "stop": 2, "timer_go": 3, "timer_reset": 4, "timer_plus": 5, "timer_minus": 6, "timer_plus_s": 7,
           "timer_minus_s": 8, "timer_set": 9, "timer_close": 10}
EXTRA_SCREENS = ("intro", "alarm", "timer")  # designed screens next to the two clock faces
LOADERS = ["bar", "dots", "spinner", "ring", "wave"]


def color_val(v):
    return 0x01000000 | int(v[1:], 16) if isinstance(v, str) and re.fullmatch(r"#[0-9A-Fa-f]{6}", v) else 0


def image_bin(asset, w, h):
    """RGB565 (+ alpha plane when the picture has transparency), little endian, as LVGL expects"""
    from PIL import Image
    src = ASSETS / str(asset)
    if Path(str(asset)).name != asset or str(asset).startswith(".") or not src.is_file():
        raise RuntimeError(f"Brak obrazka {asset}")
    im = Image.open(src).convert("RGBA").resize((max(1, w), max(1, h)), Image.LANCZOS)
    px = im.tobytes()
    rgb = bytearray()
    alpha = bytearray()
    for i in range(0, len(px), 4):
        r, g, b, a = px[i], px[i + 1], px[i + 2], px[i + 3]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        rgb += bytes((v & 0xFF, v >> 8))
        alpha.append(a)
    has_alpha = any(a < 255 for a in alpha)
    return bytes(rgb + alpha) if has_alpha else bytes(rgb), has_alpha


def compile_scene(scene, fixed_palette=False, third=False, max_img=3_500_000):
    """The editor's scene as firmware values: faces with element rows, images (RGB565/A8), fonts and icons used.
    fixed_palette: "auto" colors become the Studio palette (live preview shows what the editor shows;
    a compiled program keeps "auto" so the Tab5 palette and color presets still apply).
    third: also Wariant-3 when it is switched on (live and saved scenes only; a compiled program has two faces)"""
    pal = scene.get("colors") or {}
    digit = pal.get("digit") or "#5BE7E1"
    value = pal.get("sensor") if pal.get("sensor") not in (None, "", "auto") else digit
    fonts, icons, images, faces = set(), set(), [], {}
    total_img = 0
    keys = ["basic", "full"]
    if third and (scene["faces"].get("third") or {}).get("enabled"):
        keys.append("third")
    for k in EXTRA_SCREENS:  # designed screens instead of the built-in ones
        f = scene["faces"].get(k)
        if f and not f.get("builtin", True):
            keys.append(k)
    for face_key in keys:
        face = scene["faces"][face_key]
        rows = []
        for el in face.get("elements", []):
            if not el.get("visible", True):
                continue
            t = el.get("type")
            typ = ("clock_font" if el.get("style") == "font" else "clock_led") if t == "clock" else t
            if typ not in SC:
                continue
            r = {"t": SC[typ], "x": num(el.get("x"), -2000, 4000), "y": num(el.get("y"), -2000, 4000),
                 "w": num(el.get("w"), -4000, 4000), "h": num(el.get("h"), -4000, 4000),
                 "o": round(num(el.get("opacity", 100), 0, 100) * 2.55),
                 "f": (1 if el.get("seconds") else 0) | (2 if el.get("unlit") else 0) | (4 if el.get("upper") else 0) | (8 if el.get("round") else 0),
                 "s": num(el.get("source", el.get("format", 0)), 0, 6), "z": num(el.get("size", 0), 0, 1000), "z2": 0,
                 "c": color_val(el.get("color")), "c2": color_val(el.get("color2", el.get("unit_color"))),
                 "d": num(el.get("decimals", 1), 0, 3), "a": {"left": 0, "center": 1, "right": 2}.get(el.get("align"), 0),
                 "i": 0, "font": "inter", "text": "", "img": "", "c3": 0, "c4": 0,
                 "act": ACTIONS.get(el.get("action"), 0), "av": num(el.get("action_val", 5), 0, 99)}
            if typ in ("clock_font", "sensor", "date", "text", "place", "alarm_label", "timer_time", "timer_go", "alarm_badge", "timer_badge", "forecast"):
                r["font"] = font_name(el.get("font"))
                fonts.add(r["font"])
            if typ == "clock_led":
                r["w"], r["h"] = round((858 if el.get("seconds") else 716) * r["z"] / 280), r["z"]
            if typ == "sensor":
                r["text"], r["z2"] = el.get("unit", ""), num(el.get("unit_size", 0), 0, 1000)
            if typ in ("text", "place", "alarm_label", "timer_time", "timer_go"):
                r["text"] = el.get("text", "") if typ == "text" else ""
                if el.get("align", "left") == "left":
                    r["w"] = 0
            if typ == "icon":
                r["i"] = num(el.get("cp"), 0, 0xFFFF)
                icons.add(r["i"])
            if typ == "weather":
                r["s"] = num(el.get("source", 0), 0, 3)
                if r["s"] <= 1:
                    icons.update(WEATHER_ICONS)
                else:
                    r["font"] = font_name(el.get("font"))
                    fonts.add(r["font"])
            if typ == "forecast":
                # day 0 today … 6; size = day name and temperatures, icon_size = icon (0 = 1.6 × size); w = column width
                r["s"] = num(el.get("day", 0), 0, 6)
                r["z2"] = num(el.get("icon_size", 0), 0, 1000)
                r["c2"], r["c3"] = color_val(el.get("icon_color")), color_val(el.get("min_color"))
                r["f"] |= 0x20 if el.get("long") else 0
                icons.update(WEATHER_ICONS)
            if typ in ("alarm_badge", "timer_badge"):
                # show: "both" (icon and time), "icon", "time"; size = the time, icon_size = the icon (0 = like the time)
                r["s"] = {"both": 0, "icon": 1, "time": 2}.get(el.get("show"), 0)
                r["i"] = num(el.get("cp"), 0, 0xFFFF) if r["s"] != 2 else 0
                if r["i"]:
                    icons.add(r["i"])
                r["z2"] = num(el.get("icon_size", 0), 0, 1000)
                r["c2"] = color_val(el.get("icon_color"))
            if typ in ("rect", "ellipse"):
                r["z"], r["z2"] = num(el.get("border", 0), 0, 200), num(el.get("radius", 0), 0, 1000)
                r["c"], r["c2"] = color_val(el.get("fill")), color_val(el.get("border_color"))
            if typ == "line":
                r["z"], r["c"] = num(el.get("width", 4), 1, 200), color_val(el.get("color") or "#FFFFFF")
            if typ == "analog":
                r["w"] = r["h"] = num(el.get("size", 300), 40, 720)
                r["s"] = num(el.get("marks", 1), 0, 2)
                r["z2"] = num(el.get("rim", 0), 0, 60)
                r["c2"] = color_val(el.get("second_color"))
                r["c3"], r["c4"] = color_val(el.get("mark_color")), color_val(el.get("fill"))
            if typ == "loader":
                r["s"] = LOADERS.index(el.get("style")) if el.get("style") in LOADERS else 0
                r["c"], r["c2"] = color_val(el.get("color")), color_val(el.get("track"))
                r["z2"] = num(round(num(el.get("ms", 1200), 100, 60000) / 100), 1, 600)
                r["f"] = 0x10 if el.get("loop") else 0
            if typ == "image":
                data, alpha = image_bin(el.get("asset", ""), r["w"], r["h"])
                total_img += len(data)
                r["img"] = f"img{len(images)}"
                images.append({"name": r["img"], "data": data, "w": r["w"], "h": r["h"], "alpha": alpha})
            if fixed_palette and not r["c"] and typ in ("clock_led", "clock_font", "icon", "sensor", "analog", "loader", "alarm_badge", "timer_badge", "weather", "forecast"):
                r["c"] = color_val(value if typ == "sensor" else digit)
            rows.append(r)
        if len(rows) > 64:
            raise RuntimeError("Za dużo elementów na jednym zegarze (max 64)")
        faces[face_key] = {"bg": hex6(face.get("bg"), "050505"), "els": rows,
                           "ms": num(face.get("ms", 4000 if face_key == "intro" else 5000), 1000, 60000) if face_key == "intro" else 0}
    if total_img > max_img:  # a compiled program: room in its slot; live / saved: the Tab5 checks its own free space
        raise RuntimeError(f"Obrazki zajmują {total_img // 1024} KB — za dużo (max ok. {max_img // 1024} KB). Zmniejsz je albo usuń część.")
    return faces, images, fonts, icons, total_img


def gen_scene(scene, studio):
    """studio/scene.h (+ image files) from the editor's scene; returns (fonts used, icon codepoints, warnings)"""
    faces, images, fonts, icons, total_img = compile_scene(scene)
    lines = ["// Generated by Tab5 Studio — do not edit", "#pragma once", "#define STUDIO_SCENE 1", ""]
    for im in images:
        (studio / f"{im['name']}.bin").write_bytes(im["data"])
        cf = "LV_COLOR_FORMAT_RGB565A8" if im["alpha"] else "LV_COLOR_FORMAT_RGB565"
        n = im["name"]
        lines += [f'extern const uint8_t {n}_start[] asm("_binary_{n}_bin_start");',
                  f"static const lv_image_dsc_t studio_{n} = {{.header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = {cf}, .flags = 0, "
                  f".w = {im['w']}, .h = {im['h']}, .stride = {im['w'] * 2}}}, .data_size = {len(im['data'])}, .data = {n}_start, .reserved = NULL}};"]
    for key, face in faces.items():
        rows = [f"    {{{r['t']}, {r['o']}, {r['f']}, {r['s']}, {r['x']}, {r['y']}, {r['w']}, {r['h']}, {r['z']}, {r['z2']}, "
                f"{r['c']:#x}u, {r['c2']:#x}u, {r['d']}, {r['a']}, {r['i']:#x}, {c_str(r['font'])}, {c_str(r['text'])}, "
                f"{'&studio_' + r['img'] if r['img'] else 'NULL'}, {r['c3']:#x}u, {r['c4']:#x}u, {r['act']}, {r['av']}}}," for r in face["els"]]
        lines += [f"static const SceneEl studio_{key}[] = {{"] + (rows or ["    {}"]) + ["};", ""]
    lines.append("static const SceneFace studio_faces[2] = {" +
                 ", ".join(f"{{0x{faces[k]['bg']}, studio_{k}, {len(faces[k]['els'])}, 0}}" for k in ("basic", "full")) + "};")
    for k in EXTRA_SCREENS:
        if k in faces:
            f = faces[k]
            lines += [f"#define STUDIO_{k.upper()} 1", f"static const SceneFace studio_{k}_face = {{0x{f['bg']}, studio_{k}, {len(f['els'])}, {f['ms']}}};"]
    (studio / "scene.h").write_text("\n".join(lines) + "\n", "utf-8")
    return fonts, icons, [f"obrazki: {total_img // 1024} KB"] if total_img else []


def subset_icons(cps, dst):
    from fontTools import subset
    from fontTools.ttLib import TTFont
    f = TTFont(APP_DIR / "static" / "lucide.ttf")
    opts = subset.Options()
    opts.hinting = False
    opts.notdef_outline = True
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=sorted(cps))
    sub.subset(f)
    f.save(dst)


def gen_colors_h(colors):
    digit = hex6(colors.get("digit"), "5BE7E1")
    sensor = colors.get("sensor") or "auto"
    follow = sensor == "auto"
    sc = "F2F3F5" if follow else hex6(sensor, "F2F3F5")
    return ("// Generated by Tab5 Studio — do not edit\n#pragma once\n"
            f"#define STUDIO_DIGIT_COLOR 0x{digit.upper()}\n#define STUDIO_SENSOR_FOLLOW {'true' if follow else 'false'}\n"
            f"#define STUDIO_SENSOR_COLOR 0x{sc.upper()}\n")


def build_name(build_id):
    """A build ID ("20261101-100136-b977") as people read it: "01.11.2026 10:01" """
    m = re.match(r"(\d{4})(\d\d)(\d\d)-(\d\d)(\d\d)", build_id)
    return f"{m[3]}.{m[2]}.{m[1]} {m[4]}:{m[5]}" if m else build_id


def build_note(body):
    """What a build is about, typed in Studio: one line, at most 60 characters"""
    return re.sub(r"\s+", " ", str(body.get("note") or "")).strip()[:60]


KEEP_BUILDS = 10  # older builds are deleted after each new one, except the one running on the Tab5
TAB5_LAST = DATA_DIR / "tab5_last.json"  # what the Tab5 said last: {"build": id}
TAB5_SAVED = DATA_DIR / "tab5_saved.json"  # the project saved on the Tab5 from Studio: {project, name, hash, time, scene}
TAB5_BACKUPS = DATA_DIR / "tab5_backups"  # copies of the Tab5 settings and alarms
KEEP_BACKUPS = 20


def read_json(path, default=None):
    try:
        return json.loads(path.read_text("utf-8"))
    except (OSError, ValueError):
        return default


def prune_builds():
    """Keeps the KEEP_BUILDS newest finished builds and the one on the Tab5"""
    on_tab5 = (read_json(TAB5_LAST, {}) or {}).get("build")
    done = [d for d in sorted(BUILDS.iterdir(), reverse=True) if (d / "tab5panel-app.bin").exists()]
    for d in done[KEEP_BUILDS:]:
        job = jobs.get(d.name)
        if d.name != on_tab5 and not (job and job.status in ("queued", "running")):
            shutil.rmtree(d, ignore_errors=True)
            jobs.pop(d.name, None)


class Job:
    def __init__(self, project):
        self.id = time.strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:4]
        self.project = project
        self.status = "queued"  # queued, running, done, error
        self.started = time.time()
        self.dir = BUILDS / self.id
        self.dir.mkdir(parents=True)
        self.log_path = self.dir / "build.log"
        self.files = []


jobs = {}
build_lock = threading.Lock()


def run_build(job, body):
    with build_lock:
        job.status = "running"
        log = open(job.log_path, "w", encoding="utf-8")

        def say(msg):
            log.write(msg + "\n")
            log.flush()

        try:
            say(f"▶ Projekt: {job.project}")
            say("▶ Kopiowanie kodu Tab5…")
            # managed_components/ stays: the downloaded ESP-IDF components are a cache (and --delete would leave them half-removed)
            excl = ["build/", "managed_components/", "backup/", "fonts_src/", "tools/", "main/studio/", "*.bin", ".DS_Store"]
            cmd = ["rsync", "-a", "--delete"] + sum([["--exclude", e] for e in excl], []) + \
                  [f"{FIRMWARE_DIR}/", f"{WORK}/"]
            subprocess.run(cmd, check=True)

            scene = body["scene"]
            studio = WORK / "main" / "studio"
            shutil.rmtree(studio, ignore_errors=True)
            studio.mkdir()
            used_fonts, icons, warn = gen_scene(scene, studio)
            fonts = []
            for name in sorted(n for n in used_fonts if n.startswith("f_")):
                src = FONTS / f"{name}.ttf"
                if not src.exists():
                    raise RuntimeError(f"Brak czcionki {name} — wgraj ją ponownie albo wybierz inną")
                shutil.copy(src, studio / f"{name}.ttf")
                title = (FONTS / f"{name}.title").read_text("utf-8") if (FONTS / f"{name}.title").exists() else name
                fonts.append((name, title))
            if icons:
                subset_icons(icons, studio / "studio_icons.ttf")
                fonts.append(("studio_icons", "Ikony"))
            layout_h = ["// Generated by Tab5 Studio — do not edit", "#pragma once"]
            if fonts:
                layout_h.append("#define STUDIO_FONTS(X) " + " ".join(f"X({n}, {c_str(t)})" for n, t in fonts))
            (studio / "layout.h").write_text("\n".join(layout_h) + "\n", "utf-8")
            (studio / "colors.h").write_text(gen_colors_h(scene.get("colors", {})), "utf-8")
            en = body.get("lang") == "en"
            (studio / "lang.h").write_text("// Generated by Tab5 Studio — language of the device texts\n#pragma once\n"
                                           f"#define LANG_EN {1 if en else 0}\n", "utf-8")
            (studio / "build.h").write_text("// Generated by Tab5 Studio — build ID, shown on the Tab5 (settings, web page, /api/state)\n"
                                            f"#pragma once\n#define STUDIO_BUILD {c_str(job.id)}\n#define STUDIO_BUILD_NAME {c_str(build_name(job.id))}\n"
                                            f"#define STUDIO_BUILD_NOTE {c_str(build_note(body))}\n", "utf-8")
            say(f"▶ Język programu: {'English' if en else 'polski'}")
            used = {n for n, _ in fonts} | {p.name for p in studio.glob("*.bin")}
            say(f"▶ Scena gotowa: {sum(len(scene['faces'][k].get('elements', [])) for k in ('basic', 'full'))} elementów, "
                f"czcionki: {', '.join(t for _, t in fonts) or 'wbudowane'}" + (f", {', '.join(warn)}" if warn else ""))

            # Secrets from the environment (server), else the files copied from the firmware folder
            if os.environ.get("WIFI_SSID"):
                (WORK / "main" / "wifi_secrets.h").write_text(
                    f'#pragma once\n\n#define WIFI_SSID {c_str(os.environ["WIFI_SSID"])}\n'
                    f'#define WIFI_PASSWORD {c_str(os.environ.get("WIFI_PASSWORD", ""))}\n', "utf-8")
            pin = body.get("pin") or os.environ.get("WEB_PIN")  # typed for this build, else from .env, else the one in the code
            if pin:
                (WORK / "main" / "web_secrets.h").write_text(f'#pragma once\n\n#define WEB_PIN {c_str(pin)}\n', "utf-8")
                say("▶ PIN strony: " + ("wpisany przy kompilacji" if body.get("pin") else "z pliku .env"))
            for f in ("wifi_secrets.h", "web_secrets.h"):
                if not (WORK / "main" / f).exists():
                    raise RuntimeError(f"Brak pliku main/{f} — ustaw WIFI_SSID / WIFI_PASSWORD / WEB_PIN w pliku .env")

            # The font list is a CMake glob: reconfigure when it changed
            stamp = WORK / ".studio_fonts"
            reconf = not stamp.exists() or stamp.read_text() != ",".join(sorted(used))
            stamp.write_text(",".join(sorted(used)))
            steps = (["idf.py reconfigure"] if reconf else []) + [
                "idf.py build",
                "python -m esptool --chip esp32p4 merge_bin -o build/tab5panel-web-install.bin --flash_mode dio "
                "--flash_size 16MB --flash_freq 80m 0x2000 build/bootloader/bootloader.bin "
                "0x8000 build/partition_table/partition-table.bin 0xf000 build/ota_data_initial.bin 0x20000 build/tab5panel.bin"]  # offsets as in firmware/partitions.csv
            script = (f". {IDF_EXPORT} >/dev/null 2>&1 && " if IDF_EXPORT else "") + f"cd {WORK} && " + " && ".join(steps)
            say("▶ Kompilacja (pierwsza trwa kilka minut, kolejne ok. minuty)…")
            p = subprocess.Popen(["bash", "-c", script], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                 errors="replace")
            for line in p.stdout:
                log.write(line)
                log.flush()
            if p.wait() != 0:
                raise RuntimeError("Kompilacja nie powiodła się — szczegóły wyżej w dzienniku")
            shutil.copy(WORK / "build" / "tab5panel-web-install.bin", job.dir / "tab5panel-web-install.bin")
            shutil.copy(WORK / "build" / "tab5panel.bin", job.dir / "tab5panel-app.bin")
            job.files = ["tab5panel-web-install.bin", "tab5panel-app.bin"]
            (job.dir / "project.json").write_text(json.dumps({k: v for k, v in body.items() if k != "pin"}, ensure_ascii=False), "utf-8")  # the PIN is not kept
            say(f"✔ Gotowe w {int(time.time() - job.started)} s")
            job.status = "done"
            prune_builds()
        except Exception as e:  # noqa: BLE001 — every failure is reported in the log
            say(f"✖ {e}")
            job.status = "error"
        finally:
            log.close()


@app.post("/api/build")
@auth
def build_start():
    body = json_body()
    if not isinstance(body, dict) or "scene" not in body:
        abort(400)
    if isinstance(body, dict) and body.get("pin") and not re.fullmatch(r"\d{4}", str(body["pin"])):
        abort(400, "PIN musi mieć 4 cyfry")
    if sum(j.status in ("queued", "running") for j in jobs.values()) >= 2:
        abort(429, "Kompilacja już trwa i jedna czeka w kolejce — poczekaj")
    job = Job(body.get("name") or "bez nazwy")
    jobs[job.id] = job
    threading.Thread(target=run_build, args=(job, body), daemon=True).start()
    return jsonify(id=job.id)


@app.get("/api/build/<job_id>")
@auth
def build_status(job_id):
    job = jobs.get(job_id)
    if not job:
        abort(404)
    offset = int(request.args.get("offset", 0))
    text = job.log_path.read_text("utf-8", errors="replace") if job.log_path.exists() else ""
    return jsonify(status=job.status, log=text[offset:], offset=len(text), files=job.files,
                   seconds=int(time.time() - job.started), busy=build_lock.locked())


@app.get("/api/builds")
@auth
def builds():
    items = []
    for d in sorted(BUILDS.iterdir(), reverse=True)[:20]:
        if (d / "tab5panel-web-install.bin").exists():
            pj = read_json(d / "project.json", {}) or {}
            items.append({"id": d.name, "project": pj.get("name", ""), "note": build_note(pj),
                          "size": (d / "tab5panel-web-install.bin").stat().st_size})
    return jsonify(builds=items)


@app.delete("/api/build/<job_id>")
@auth
def build_delete(job_id):
    if not re.fullmatch(r"[\w\-]+", job_id) or not (BUILDS / job_id).is_dir():
        abort(404)
    job = jobs.get(job_id)
    if job and job.status in ("queued", "running"):
        abort(409, "Ta kompilacja jeszcze trwa")
    shutil.rmtree(BUILDS / job_id)
    jobs.pop(job_id, None)
    return jsonify(ok=True)


@app.get("/api/build/<job_id>/<fname>")
@auth
def build_file(job_id, fname):
    if not re.fullmatch(r"[\w\-]+", job_id) or fname not in ("tab5panel-web-install.bin", "tab5panel-app.bin", "build.log"):
        abort(404)
    path = BUILDS / job_id / fname
    if not path.exists():
        abort(404)
    return send_file(path, as_attachment=True, download_name=fname)


# ------------------------------------------------------------------ postcode --
@app.get("/api/places")
@auth
def places():
    """Place search like on the Tab5: a Polish postcode (zippopotam.us) or a name anywhere (Open-Meteo geocoding)"""
    import urllib.error
    import urllib.parse
    import urllib.request
    q, lang = request.args.get("q", "").strip(), "en" if request.args.get("lang") == "en" else "pl"
    if len(q) < 2:
        return jsonify(ok=False, error="Wpisz co najmniej 2 znaki")
    digits = re.sub(r"[\s-]", "", q)
    try:
        if re.fullmatch(r"\d{5}", digits):
            code = digits[:2] + "-" + digits[2:]
            try:
                with urllib.request.urlopen(f"http://api.zippopotam.us/pl/{code}", timeout=10) as r:
                    items = json.loads(r.read()).get("places") or []
            except urllib.error.HTTPError as e:
                if e.code != 404:
                    raise
                items = []
            res = [{"name": p["place name"], "region": p.get("state", ""), "country": "Polska" if lang == "pl" else "Poland", "postcode": code} for p in items]
        else:
            url = "http://geocoding-api.open-meteo.com/v1/search?" + urllib.parse.urlencode({"name": q, "count": 8, "language": lang, "format": "json"})
            with urllib.request.urlopen(url, timeout=10) as r:
                items = json.loads(r.read()).get("results") or []
            res = [{"name": p.get("name", ""), "region": p.get("admin1", ""), "country": p.get("country", ""), "postcode": ""} for p in items]
        if not res:
            return jsonify(ok=False, error="Nic nie znaleziono — sprawdź nazwę")
        return jsonify(ok=True, results=res)
    except Exception:  # noqa: BLE001
        return jsonify(ok=False, error="Brak połączenia z usługą wyszukiwania")


# ------------------------------------------------------------- live preview --
SETTINGS = DATA_DIR / "settings.json"


def settings():
    env_host = os.environ.get("TAB5_HOST", "")
    saved = json.loads(SETTINGS.read_text("utf-8")) if SETTINGS.exists() else {}
    host = saved.get("tab5_host") or env_host
    # TAB5_PIN from .env goes only to TAB5_HOST: a PIN is never sent to an address set later
    pin = saved.get("tab5_pin") or (os.environ.get("TAB5_PIN", "") if host == env_host else "")
    return {"tab5_host": host, "tab5_pin": pin}


@app.get("/api/settings")
@auth
def settings_get():
    st = settings()
    return jsonify(tab5_host=st["tab5_host"], has_pin=bool(st["tab5_pin"]),  # the PIN never goes back to the browser
                   login=bool(PASSWORD))  # Studio has a password: show the sign-out button


@app.put("/api/settings")
@auth
def settings_put():
    body = json_body() or {}
    st = json.loads(SETTINGS.read_text("utf-8")) if SETTINGS.exists() else {}
    host = str(body.get("tab5_host", "")).strip()
    host = re.sub(r"^https?://", "", host).strip("/")
    if host and not re.fullmatch(r"[A-Za-z0-9.\-]+(:\d+)?", host):
        abort(400, "Zły adres Tab5")
    if host != st.get("tab5_host", os.environ.get("TAB5_HOST", "")):
        st.pop("tab5_pin", None)  # a new address does not get the old PIN
    st["tab5_host"] = host
    if body.get("tab5_pin"):
        st["tab5_pin"] = str(body["tab5_pin"]).strip()
    SETTINGS.write_text(json.dumps(st), "utf-8")
    os.chmod(SETTINGS, 0o600)
    return jsonify(ok=True)


# Error codes of /api/scene/save on the Tab5
SAVE_ERRORS = {"no_space": "Projekt nie mieści się w pamięci Tab5",
               "storage": "Pamięć Tab5 niedostępna", "write": "Zapis na Tab5 nie powiódł się", "nothing": "Na Tab5 nie ma czego zapisać"}


def tab5(path, data=None, ctype="application/json", timeout=30):
    """Request to the Tab5 with the PIN; raises RuntimeError with a readable message"""
    import urllib.error
    import urllib.request
    st = settings()
    if not st["tab5_host"] or not st["tab5_pin"]:
        raise RuntimeError("Podaj adres i PIN Tab5 w ustawieniach Studio")
    req = urllib.request.Request(f"http://{st['tab5_host']}{path}", data=data, method="POST" if data is not None else "GET",
                                 headers={"X-PIN": st["tab5_pin"], "Content-Type": ctype})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        try:
            code = json.loads(e.read() or b"{}").get("error", "")
        except ValueError:
            code = ""
        if code in SAVE_ERRORS:
            raise RuntimeError(SAVE_ERRORS[code])
        msg = {401: "zły PIN", 429: "za dużo prób PIN-u — poczekaj 30 s", 404: "ten program Tab5 nie ma tej funkcji — wgraj nowszy program"}.get(e.code, f"HTTP {e.code}")
        raise RuntimeError(f"Tab5: {msg}")
    except Exception as e:  # noqa: BLE001
        raise RuntimeError(f"Brak połączenia z Tab5 ({st['tab5_host']}): {e}")


@app.post("/api/tab5/test")
@auth
def tab5_test():
    try:
        st = json.loads(tab5("/api/state", timeout=8))
        if st.get("build") and (read_json(TAB5_LAST, {}) or {}).get("build") != st["build"]:
            TAB5_LAST.write_text(json.dumps({"build": st["build"]}), "utf-8")
        rec = read_json(TAB5_SAVED)
        studio_saved = {k: rec.get(k) for k in ("project", "name", "hash", "time")} if rec else None
        return jsonify(ok=True, ip=st.get("ip"), ssid=st.get("ssid"), place=st.get("place"), scene_live=bool(st.get("scene_live")),
                       saved=st.get("scene_saved"), saved_err=st.get("scene_saved_err", ""), room_kb=st.get("scene_room_kb", -1),
                       can_save="scene_room_kb" in st, last_freeze=st.get("last_freeze"), build=st.get("build"),
                       build_note=st.get("build_note", ""), heap=st.get("heap"), last_crash=st.get("last_crash"),
                       reset_reason=st.get("reset_reason"), uptime=st.get("uptime"), studio_saved=studio_saved)
    except RuntimeError as e:
        return jsonify(ok=False, error=str(e))


live_lock = threading.Lock()


def scene_size(images, fonts, icons):
    """Bytes the Tab5 keeps for a scene: images, Studio fonts and the icon font (estimated)"""
    size = sum(len(im["data"]) for im in images)
    size += sum((FONTS / f"{n}.ttf").stat().st_size for n in fonts if n.startswith("f_") and (FONTS / f"{n}.ttf").exists())
    return size + (len(icons) * 600 + 8000 if icons else 0) + 16 * 1024  # icon subset and the scene JSON


@app.post("/api/live/size")
@auth
def live_size():
    """Size of the scene as it would be saved on the Tab5"""
    _faces, images, fonts, icons, _total = compile_scene(json_body()["scene"], third=True, max_img=8_000_000)
    return jsonify(kb=scene_size(images, fonts, icons) // 1024)


@app.post("/api/live")
@auth
def live_show():
    """Sends the scene to the Tab5 as a temporary preview (images, fonts, elements); with "save": {name, project, hash}
    the Tab5 also saves it for good in its memory and shows it after every start"""
    body = json_body()
    if not live_lock.acquire(blocking=False):
        return jsonify(ok=False, error="Wysyłanie już trwa")
    try:
        return jsonify(send_scene(body))
    except (RuntimeError, KeyError) as e:
        return jsonify(ok=False, error=str(e))
    finally:
        live_lock.release()


def send_scene(body):
    """The work of /api/live (live_lock held); raises RuntimeError"""
    save = body.get("save") if isinstance(body.get("save"), dict) else None
    t0 = time.time()
    # A saved design keeps "auto" colors: the palette and presets from the Tab5 web page still apply
    faces, images, fonts, icons, total = compile_scene(body["scene"], fixed_palette=not save, third=True, max_img=8_000_000)
    if save:
        st = json.loads(tab5("/api/state", timeout=8))
        if "scene_room_kb" not in st:
            raise RuntimeError("Ten program Tab5 nie umie zapisywać projektów — wgraj nowszy program (web-install.bin)")
        room = st["scene_room_kb"]
        if room < 0:
            raise RuntimeError("Pamięć Tab5 niedostępna")
        if scene_size(images, fonts, icons) // 1024 > room:
            raise RuntimeError(f"Projekt nie mieści się: wolne {room} KB")
    tab5("/api/scene/begin", b"{}")
    for im in images:
        tab5(f"/api/scene/blob?name={im['name']}&kind=img&w={im['w']}&h={im['h']}&alpha={1 if im['alpha'] else 0}",
             im["data"], "application/octet-stream", timeout=60)
    for name in sorted(n for n in fonts if n.startswith("f_")):
        if (FONTS / f"{name}.ttf").exists():
            tab5(f"/api/scene/blob?name={name}&kind=font", (FONTS / f"{name}.ttf").read_bytes(), "application/octet-stream", timeout=60)
    if icons:
        tmp = DATA_DIR / "live_icons.ttf"
        subset_icons(icons, tmp)
        tab5("/api/scene/blob?name=studio_icons&kind=font", tmp.read_bytes(), "application/octet-stream")
    show = {"faces": [faces["basic"], faces["full"]]}
    if "third" in faces:
        show["third"] = faces["third"]  # Wariant-3
    for k in EXTRA_SCREENS:
        if k in faces:
            show[k] = faces[k]
    if not save:
        show["play_intro"] = body.get("face") == "intro"  # editing the intro: play it on the Tab5 now
        if body.get("face") in ("alarm", "timer"):
            show["show_screen"] = body["face"]  # and the alarm / timer screen: open it
    tab5("/api/scene/show", json.dumps(show, ensure_ascii=False).encode("utf-8"))
    saved_kb = None
    if save:
        name = str(save.get("name") or "Studio").strip()[:40] or "Studio"
        r = json.loads(tab5("/api/scene/save", json.dumps({"name": name}, ensure_ascii=False).encode("utf-8"), timeout=300))
        saved_kb = r.get("kb")
        # Studio remembers what it saved: shown next to the open project, and sent again after a restore
        TAB5_SAVED.write_text(json.dumps({"project": str(save.get("project") or name)[:80], "name": name,
                                          "hash": str(save.get("hash") or "")[:40], "time": int(time.time()),
                                          "scene": body["scene"]}, ensure_ascii=False), "utf-8")
        # The saved design keeps "auto" colors, which follow the Tab5 palette: give the Tab5 the project's palette,
        # so it looks as in Studio (still changeable later on the Tab5 web page or in its menu)
        pal = body["scene"].get("colors") or {}
        colors = {}
        if re.fullmatch(r"#[0-9A-Fa-f]{6}", str(pal.get("digit") or "")):
            colors["digit_color"] = pal["digit"].upper()
        sensor = str(pal.get("sensor") or "auto")
        if sensor == "auto" or re.fullmatch(r"#[0-9A-Fa-f]{6}", sensor):
            colors["sensor_color"] = sensor.upper() if sensor != "auto" else "auto"
        if colors:
            tab5("/api/set", json.dumps(colors).encode())
    if body.get("face") in ("basic", "full") or (body.get("face") == "third" and "third" in faces):
        tab5("/api/set", json.dumps({"face": {"basic": 0, "full": 1, "third": 2}[body["face"]]}).encode())
    return dict(ok=True, seconds=round(time.time() - t0, 1), kb=total // 1024, saved_kb=saved_kb)


ota_lock = threading.Lock()


@app.post("/api/tab5/ota")
@auth
def tab5_ota():
    """Sends a built program (tab5panel-app.bin of build {id}) to the Tab5 over Wi-Fi; it restarts into it and goes
    back to the previous program by itself when the new one does not start properly"""
    job_id = str((json_body() or {}).get("id", ""))
    path = BUILDS / job_id / "tab5panel-app.bin"
    if not re.fullmatch(r"[\w\-]+", job_id) or not path.exists():
        return jsonify(ok=False, error="Nie ma takiej kompilacji")
    if not ota_lock.acquire(blocking=False):
        return jsonify(ok=False, error="Wysyłanie programu już trwa")
    try:
        t0 = time.time()
        backup = None
        try:
            backup = make_backup("przed aktualizacją")  # in case the new program loses the settings
        except RuntimeError:
            pass  # an older program without /api/settings/backup: update anyway
        try:
            tab5("/api/ota", path.read_bytes(), "application/octet-stream", timeout=300)
        except RuntimeError as e:
            if "HTTP 404" in str(e):
                raise RuntimeError("Ten program Tab5 nie przyjmuje aktualizacji przez Wi-Fi — wgraj raz web-install.bin")
            if "HTTP 400" in str(e):
                raise RuntimeError("Tab5 odrzucił plik programu")
            raise
        return jsonify(ok=True, seconds=round(time.time() - t0, 1), backup=backup)
    except RuntimeError as e:
        return jsonify(ok=False, error=str(e))
    finally:
        ota_lock.release()


# ---------------------------------------------------------- Tab5 backups --
def make_backup(reason):
    """Copies the Tab5 settings and alarms (and the record of the project saved from Studio) to TAB5_BACKUPS;
    returns its name. Wi-Fi passwords are not part of it (the Tab5 does not give them out)."""
    try:
        data = json.loads(tab5("/api/settings/backup", timeout=15))
    except RuntimeError as e:
        if "nie ma tej funkcji" in str(e):
            raise RuntimeError("Ten program Tab5 nie umie robić kopii — wgraj nowszy program")
        raise
    TAB5_BACKUPS.mkdir(exist_ok=True)
    name = time.strftime("%Y%m%d-%H%M%S")
    data.update(time=int(time.time()), reason=reason, saved=read_json(TAB5_SAVED))
    (TAB5_BACKUPS / f"{name}.json").write_text(json.dumps(data, ensure_ascii=False), "utf-8")
    for old in sorted(TAB5_BACKUPS.glob("*.json"), reverse=True)[KEEP_BACKUPS:]:
        old.unlink(missing_ok=True)
    return name


@app.post("/api/tab5/backup")
@auth
def tab5_backup():
    try:
        return jsonify(ok=True, name=make_backup("ręczna"))
    except RuntimeError as e:
        return jsonify(ok=False, error=str(e))


@app.get("/api/tab5/backups")
@auth
def tab5_backups():
    items = []
    for f in sorted(TAB5_BACKUPS.glob("*.json"), reverse=True) if TAB5_BACKUPS.exists() else []:
        d = read_json(f, {}) or {}
        saved = d.get("saved") or {}
        items.append({"name": f.stem, "time": d.get("time"), "reason": d.get("reason", ""), "build": d.get("build", ""),
                      "items": len(d.get("items") or []), "project": saved.get("project")})
    return jsonify(items=items)


restore_state = {"status": "idle", "msg": ""}


@app.post("/api/tab5/restore")
@auth
def tab5_restore():
    """Writes a backup back to the Tab5 (it restarts), then, in the background, saves the project it had again"""
    name = str((json_body() or {}).get("name", ""))
    path = TAB5_BACKUPS / f"{name}.json"
    if not re.fullmatch(r"\d{8}-\d{6}", name) or not path.exists():
        return jsonify(ok=False, error="Nie ma takiej kopii")
    d = read_json(path, {}) or {}
    try:
        r = json.loads(tab5("/api/settings/restore", json.dumps({"v": 1, "items": d.get("items") or []}).encode(), timeout=20))
    except RuntimeError as e:
        return jsonify(ok=False, error=str(e))
    saved = d.get("saved")
    if saved and saved.get("scene"):
        restore_state.update(status="waiting", msg="Tab5 uruchamia się ponownie…")
        threading.Thread(target=restore_scene, args=(saved,), daemon=True).start()
    else:
        restore_state.update(status="done", msg="")
    return jsonify(ok=True, restored=r.get("restored", 0), skipped=r.get("skipped", []), scene=bool(saved and saved.get("scene")))


def restore_scene(saved):
    """After a restore: waits for the Tab5 to come back, then saves the project it had"""
    time.sleep(8)
    for _ in range(30):
        try:
            tab5("/api/state", timeout=4)
            break
        except RuntimeError:
            time.sleep(3)
    else:
        restore_state.update(status="error", msg="Tab5 nie wrócił po restarcie — zapisz projekt ręcznie")
        return
    restore_state.update(status="saving", msg=f"Zapisuję projekt „{saved.get('project')}” na Tab5…")
    with live_lock:
        try:
            send_scene({"scene": saved["scene"], "save": {"name": saved.get("name"), "project": saved.get("project"), "hash": saved.get("hash")}})
            restore_state.update(status="done", msg=f"Przywrócono projekt „{saved.get('project')}”")
        except (RuntimeError, KeyError) as e:
            restore_state.update(status="error", msg=f"Nie udało się zapisać projektu: {e}")


@app.get("/api/tab5/restore")
@auth
def tab5_restore_state():
    return jsonify(restore_state)


@app.get("/api/tab5/screen")
@auth
def tab5_screen():
    """What the Tab5 shows now, as a JPEG"""
    try:
        return Response(tab5("/api/preview", timeout=20), mimetype="image/jpeg", headers={"Cache-Control": "no-store"})
    except RuntimeError as e:
        return jsonify(ok=False, error=str(e)), 502


@app.post("/api/live/forget")
@auth
def live_forget():
    """Deletes the design saved on the Tab5: back to the look of its program"""
    try:
        tab5("/api/scene/forget", b"{}")
        TAB5_SAVED.unlink(missing_ok=True)
        return jsonify(ok=True)
    except RuntimeError as e:
        return jsonify(ok=False, error=str(e))


@app.post("/api/live/stop")
@auth
def live_stop():
    try:
        tab5("/api/scene/stop", b"{}")
        return jsonify(ok=True)
    except RuntimeError as e:
        return jsonify(ok=False, error=str(e))


if __name__ == "__main__":
    if not PASSWORD:
        print("WARNING: STUDIO_PASSWORD is empty, anyone who can reach this port can use Studio", flush=True)
    app.run(host="0.0.0.0", port=int(os.environ.get("PORT", 8080)), threaded=True)
