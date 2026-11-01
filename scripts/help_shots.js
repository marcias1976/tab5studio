// Screenshots for the in-app help of Tab5 Studio (PL and EN), with numbered markers on the overview.
// Needs a running Studio and Node with Playwright (Chromium) plus Python 3 with Pillow (to write WebP).
//   STUDIO_URL=http://localhost:8080/ STUDIO_PASSWORD=... node scripts/help_shots.js   ->  app/static/help/{pl,en}/*.webp
// The help shows the default project; uploaded images stay out (they may be other brands' logos).
const { chromium } = require("playwright");
const { execFileSync } = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

const URL = process.env.STUDIO_URL || "http://localhost:8080/";
const OUT = path.join(__dirname, "..", "app", "static", "help");
const W = 1600, H = 900, SCALE = 1.5;
const TMP = fs.mkdtempSync(path.join(os.tmpdir(), "help-"));
const wait = ms => new Promise(r => setTimeout(r, ms));

// PNG → WebP, at most `width` px wide
function save(png, name, lang, width = 1400) {
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

async function run(browser, lang) {
  const ctx = await browser.newContext({ viewport: { width: W, height: H }, deviceScaleFactor: SCALE });
  const p = await ctx.newPage();
  const js = (f, a) => p.evaluate(f, a);
  const rect = sel => p.$eval(sel, e => { const r = e.getBoundingClientRect(); return { x: r.x, y: r.y, width: r.width, height: r.height }; });
  const shot = async (name, clip, width) => { const f = path.join(TMP, `${lang}-${name}.png`); await p.screenshot({ path: f, clip }); save(f, name, lang, width); };
  const closeAll = () => js(() => { document.querySelectorAll(".modal").forEach(m => m.classList.remove("show")); $("previewPop").classList.remove("show"); S.sel = null; S.multi = []; renderAll(); });

  await p.goto(URL + "login");
  await js(pw => fetch("/api/login", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ password: pw }) }), process.env.STUDIO_PASSWORD || "");
  await p.addInitScript(l => { try { localStorage.clear(); localStorage.uiLang = l; localStorage.buildLang = l; } catch (e) {} }, lang);
  // The Tab5 as Studio sees it in the save dialog (no device needed); the library with two sample clocks
  // Sample data instead of the real Tab5 address and builds (the repo is public)
  await p.route("**/api/settings", r => r.request().method() === "GET" ? r.fulfill({ json: { tab5_host: "192.168.1.50", has_pin: true, login: true } }) : r.continue());
  await p.route("**/api/builds", r => r.fulfill({ json: { builds: [{ id: "20261101-0832-demo", project: lang === "pl" ? "Mój zegar" : "My clock", size: 12620487 },
    { id: "20261031-1940-demo", project: lang === "pl" ? "Mój zegar" : "My clock", size: 12610211 }] } }));
  await p.route("**/api/tab5/test", r => r.fulfill({ json: { ok: true, ip: "192.168.1.50", ssid: "MojaSiec", scene_live: false, can_save: true, room_kb: 3290, saved: null, saved_err: "" } }));
  await p.goto(URL);
  await p.waitForFunction(() => typeof S !== "undefined" && typeof $ === "function", null, { timeout: 60000 }); await wait(1500);
  await p.addStyleTag({ content: "#assetList .asset { display: none !important; }" });
  await closeAll(); await wait(400);

  // 1. The whole window with numbered areas
  const R = {};
  for (const [k, s] of Object.entries({ lang: "#uiLang", proj: "#projName", faces: "[data-face=basic]", lib: "#bLib", add: "#addBtns", layers: "#layers",
    stage: "#stage", props: "#pane-props", dock: ".dtabs", zoom: "#zoomVal", help: "#bHelp", prev: "#bPreview" })) R[k] = await rect(s);
  const at = (r, dx = 0.5, dy = 0.5, ox = 0, oy = 0) => [r.x + r.width * dx + ox, r.y + r.height * dy + oy];
  const pts = [at(R.lang, .5, .5, 0, 26), at(R.proj, .5, .5, 0, 26), at(R.faces, .5, .5, 0, 26), at(R.lib, .5, .5, 0, 26), at(R.add, .88, .08), at(R.layers, .6, .02),
    at(R.stage, .05, .08), at(R.props, .88, .03), at(R.dock, .3, .5), at(R.zoom, .5, .5, 0, 26), at(R.help, .5, .5, 0, 26), at(R.prev, .5, .5, 0, 26)];
  await js(pts => pts.forEach(([x, y], i) => {
    const m = document.createElement("div"); m.className = "shotmark";
    Object.assign(m.style, { position: "fixed", left: x - 15 + "px", top: y - 15 + "px", width: "30px", height: "30px", borderRadius: "50%", background: "#FFC83D",
      boxShadow: "0 0 0 3px #0B0D10", color: "#141008", font: "700 17px Inter, sans-serif", display: "grid", placeItems: "center", zIndex: 99999 });
    m.textContent = i + 1; document.body.appendChild(m);
  }), pts);
  await shot("overview");
  await js(() => document.querySelectorAll(".shotmark").forEach(m => m.remove()));

  // 2. A selected element and its properties
  await js(() => { S.sel = faceEls().find(e => e.type === "clock").id; renderAll(); }); await wait(500);
  const v = await rect("#viewport");
  await shot("element", { x: v.x, y: v.y, width: W - v.x, height: v.height });
  let pr = await rect("#pane-props");
  await shot("props_clock", { x: pr.x, y: pr.y, width: pr.width, height: Math.min(pr.height, H - pr.y) }, 600);

  // 3. Several selected, grouped: align and group tools (the group has its own color in the layers)
  await js(() => { S.multi = faceEls().filter(e => e.type === "sensor" || e.type === "icon").map(e => e.id); S.sel = S.multi[0]; renderAll(); }); await wait(500);
  await shot("multi", { x: v.x, y: v.y, width: W - v.x, height: v.height });
  await js(() => { S.sel = null; S.multi = []; renderAll(); });

  // 4. The other screens
  for (const face of ["intro", "alarm", "timer"]) {
    await js(f => document.querySelector(`[data-face=${f}]`).click(), face); await wait(700);
    await shot(face, await rect("#stage"), 1100);
  }
  // Alarm and timer with their own design (starter) and the touch actions; the project is restored afterwards
  for (const [face, starter] of [["alarm", "alarmStarter"], ["timer", "timerStarter"]]) {
    await js(f => document.querySelector(`[data-face=${f}]`).click(), face); await wait(500);
    await js(st => { window._keep = S.scene.faces[S.face]; S.scene.faces[S.face] = { bg: "#050505", builtin: false, elements: window[st]() };
      if (uiLang === "en") faceEls().forEach(e => { if (typeof e.text === "string") e.text = tr(e.text); });
      S.sel = (faceEls().find(e => e.action) || {}).id || null; renderAll(); }, starter); await wait(800);
    await shot(`${face}_custom`, { x: v.x, y: v.y, width: W - v.x, height: v.height });
    pr = await rect("#pane-props");
    await shot(`props_${face}`, { x: pr.x, y: pr.y, width: pr.width, height: Math.min(pr.height, H - pr.y) }, 600);
    await js(() => { S.scene.faces[S.face] = window._keep; S.sel = null; renderAll(); });
  }
  await js(() => document.querySelector("[data-face=basic]").click()); await wait(500);

  // 5. Library, with two sample clocks from the default project
  const faces = await js(() => { const d = defaultScene().faces; return [d.basic, d.full]; });
  const now = Math.floor(Date.now() / 1000), names = lang === "pl" ? ["Cyfry LED", "Duże cyfry"] : ["LED digits", "Big digits"];
  await p.route("**/api/library/clock", r => r.request().method() === "GET" ? r.fulfill({ json: { items: faces.map((face, i) => ({ name: names[i], mtime: now - i * 86400 * 3, face })) } }) : r.continue());
  await js(() => $("bLib").click()); await wait(1500);
  await shot("library", await rect("#mLib .dialog"), 1100);
  await closeAll();

  // 6. Build tab and assets tab
  await js(() => { $("dock").style.flexBasis = "380px"; dispatchEvent(new Event("resize")); document.querySelector("[data-tab=build]").click(); }); await wait(1000);
  await shot("build", await rect("#dock"), 1100);
  await js(() => document.querySelector("[data-tab=assets]").click()); await wait(400);
  await shot("assets", await rect("#dock"), 1100);
  await js(() => { $("dock").style.flexBasis = ""; dispatchEvent(new Event("resize")); }); await wait(300);

  // 7. Preview menu while a preview is on the Tab5 (yellow LIVE button, "Close the preview" at the top)
  await js(() => { setLive(true); $("previewPop").classList.add("show"); }); await wait(400);
  const pop = await rect("#previewPop");
  await shot("preview_menu", { x: pop.x - 20, y: 0, width: W - pop.x + 20, height: pop.y + pop.height + 16 }, 600);
  await js(() => { setLive(false); $("previewPop").classList.remove("show"); });

  // 8. "Save to the Tab5 for good"
  await js(() => { $("projName").value = uiLang === "en" ? "My clock" : "Mój zegar"; $("bSaveTab5").click(); }); await wait(1500);
  await shot("save_tab5", await rect("#mSave .dialog"), 700);
  await closeAll();
  await ctx.close();
}

(async () => {
  const browser = await chromium.launch();
  for (const lang of ["pl", "en"]) await run(browser, lang);
  await browser.close();
  fs.rmSync(TMP, { recursive: true, force: true });
})().catch(e => { console.error(e); process.exit(1); });
