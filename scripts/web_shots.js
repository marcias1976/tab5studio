// Screenshots of the Tab5 web page (firmware/main/web/index.html) for the Studio help, PL and EN.
// No Tab5 needed: the page is loaded from the repo and its API answers with the sample data below
// (the repo is public: sample Wi-Fi name, place and address only).
//   node scripts/web_shots.js [screen.jpg]  ->  app/static/help/web/{pl,en}/*.webp
// Needs Node with Playwright (Chromium) and Python 3 with Pillow, like help_shots.js. screen.jpg = the clock picture
// in the preview (optional).
const { chromium } = require("playwright");
const { execFileSync } = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

const ROOT = path.join(__dirname, "..");
const PAGE = fs.readFileSync(path.join(ROOT, "firmware", "main", "web", "index.html"));
const OUT = path.join(ROOT, "app", "static", "help", "web");
const SCREEN = process.argv[2] && fs.existsSync(process.argv[2]) ? fs.readFileSync(process.argv[2]) : null;
const TMP = fs.mkdtempSync(path.join(os.tmpdir(), "web-"));
const wait = ms => new Promise(r => setTimeout(r, ms));

const STATE = {
  face: 0, faces: 3, night: false, screen: true, lang: "pl", night_start: "23:00", night_end: "05:40", off_enabled: false,
  off_start: "01:00", off_end: "05:00", brightness: 80, cycle_min: 0, default_face: 0, digit_color: "#5BE7E1", sensor_color: "auto",
  day_preset: -1, night_preset: 1, night_face: -1,
  presets: [{ name: "Turkus", digit_color: "#5BE7E1", sensor_follow: true, sensor_color: "#5BE7E1" },
            { name: "Noc", digit_color: "#FF8A3D", sensor_follow: false, sensor_color: "#8A929C" }],
  temperature: 8.4, humidity: 85, pressure: 1032, postcode: "00-950", place: "Warszawa", postcode_state: 0,
  ssid: "MojaSiec", ip: "192.168.1.50", rssi: -48, uptime: 7380, build: "20261101-112406-a55e", build_note: "większy zegar",
  reset_reason: "software", scene_live: false, scene_saved: { name: "Mój zegar", kb: 207 }, scene_saved_err: "", scene_room_kb: 6947,
  heap: { int_free: 179951, int_min: 176240, int_block: 122880, psram_free: 21879536 },
  app: { running: "ota_1", boot: "ota_1", other: "ota_0", running_state: 2 },
  boot_heap: [["start", 350], ["settings, alarms", 348], ["display, LVGL", 332], ["saved design", 330], ["screens", 255], ["Wi-Fi", 240],
    ["weather", 232], ["web page", 214], ["SD card", 214], ["sound", 190], ["30 s after start", 179]].map(([step, kb]) => ({ step, free: kb * 1024, block: Math.min(kb, 248) * 700 })),
  sd: { mounted: true, formatting: false, format_result: 0, name: "SC32G", total_mb: 30429, free_mb: 30101 },
};
const ALARMS = { snooze: 10, timer_snooze: 1, ringing: false, sd: true, alarms: [
  { enabled: true, hour: 6, minute: 30, days: 31, volume: 80, label: "Praca" },
  { enabled: false, hour: 9, minute: 0, days: 96, volume: 60, label: "Weekend" }] };
const FILES = { items: [{ name: "Tab5_system", dir: true, size: 0, mtime: 1793433834 }, { name: "zdjecie.jpg", dir: false, size: 482113, mtime: 1793520000 }] };

function save(png, name, lang, width) {
  fs.mkdirSync(path.join(OUT, lang), { recursive: true });
  execFileSync("python3", ["-c", `
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
w = int(sys.argv[3])
if im.width > w: im = im.resize((w, round(im.height * w / im.width)), Image.LANCZOS)
im.save(sys.argv[2], "WEBP", quality=82, method=6)`, png, path.join(OUT, lang, name + ".webp"), String(width)]);
  console.log(lang, name);
}

async function run(lang) {
  // the language of the browser itself decides the time fields (24 h, not AM / PM)
  const browser = await chromium.launch({ args: [`--lang=${lang === "pl" ? "pl-PL" : "en-GB"}`] });
  const ctx = await browser.newContext({ viewport: { width: 1180, height: 900 }, deviceScaleFactor: 1.5, locale: lang === "pl" ? "pl-PL" : "en-GB" });
  const p = await ctx.newPage();
  await p.addInitScript(l => { try { localStorage.clear(); localStorage.lang = l; } catch (e) {} }, lang);
  await p.route("http://tab5.test/**", r => {
    const u = new URL(r.request().url()), json = o => r.fulfill({ body: JSON.stringify(o), contentType: "application/json" });
    if (u.pathname === "/") return r.fulfill({ body: PAGE, contentType: "text/html" });
    if (u.pathname === "/api/state") return json(STATE);
    if (u.pathname === "/api/alarms") return json(ALARMS);
    if (u.pathname === "/api/files") return json(FILES);
    if (u.pathname === "/api/preview") return SCREEN ? r.fulfill({ body: SCREEN, contentType: "image/jpeg" }) : r.fulfill({ status: 404 });
    return json({});
  });
  const shot = async (name, sel, width = 1400) => {
    const f = path.join(TMP, `${lang}-${name}.png`);
    await p.screenshot({ path: f, fullPage: true, clip: await (await p.$(sel)).boundingBox() });
    save(f, name, lang, width);
  };
  await p.goto("http://tab5.test/#home"); await wait(1500);
  await shot("header", "header", 1200);
  for (const t of ["home", "look", "alarm", "plan", "sys"]) {
    await p.evaluate(t => document.querySelector(`#tabs [data-tab=${t}]`).click(), t); await wait(600);
    if (t === "sys") await p.evaluate(() => document.querySelector("details.boot").open = true);
    // headless Chromium has no regional data and shows AM / PM; a phone in Polish or British English shows 24 h like this
    await p.evaluate(() => document.querySelectorAll("input[type=time]").forEach(i => { i.type = "text"; i.style.width = "auto"; i.size = 5; }));
    await shot("tab_" + t, `.tab[data-tab=${t}]`, t === "alarm" || t === "plan" ? 1100 : 1400);
  }
  // The PIN screen
  await p.evaluate(() => { document.getElementById("lock").classList.add("show"); });
  await p.setViewportSize({ width: 420, height: 640 }); await wait(400);
  await shot("pin", "#lock", 600);
  await ctx.close();
  await browser.close();
}

(async () => {
  for (const lang of ["pl", "en"]) await run(lang);
  fs.rmSync(TMP, { recursive: true, force: true });
})().catch(e => { console.error(e); process.exit(1); });
