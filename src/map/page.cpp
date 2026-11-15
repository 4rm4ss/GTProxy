#include "page.hpp"

#include <array>
#include <string_view>

namespace map {
namespace {
// Split in pieces below 16 KB: MSVC rejects longer string literals.
constexpr std::string_view PAGE_HEAD{ R"GTPROXY(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>GTProxy map</title>
<style>
:root {
  --bg: #0e1116; --panel: #171b22; --panel-2: #1f2530; --line: #2b3240;
  --text: #e6e9ef; --muted: #8b93a3; --accent: #ffcf4a; --ok: #59d18c; --bad: #ff6b6b;
  --path: #4fc3ff;
}
* { box-sizing: border-box; }
html, body { margin: 0; height: 100%; background: var(--bg); color: var(--text);
  font: 13px/1.35 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; overflow: hidden; }
#bar { position: fixed; inset: 0 0 auto 0; height: 44px; display: flex; align-items: center; gap: 10px;
  padding: 0 10px; background: var(--panel); border-bottom: 1px solid var(--line); z-index: 2; }
#bar .title { font-weight: 600; letter-spacing: .2px; white-space: nowrap; }
#dot { width: 9px; height: 9px; border-radius: 50%; background: var(--bad); flex: none; }
#dot.on { background: var(--ok); }
#world { color: var(--accent); font-weight: 600; white-space: nowrap; }
#hover { flex: 1; min-width: 0; color: var(--muted); white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
  font-variant-numeric: tabular-nums; }
#hover b { color: var(--text); font-weight: 600; }
.chip { color: var(--muted); border: 1px solid var(--line); border-radius: 999px; padding: 2px 8px; white-space: nowrap; }
.chip.on { color: var(--ok); border-color: #2f6b4b; }
button { font: inherit; color: var(--text); background: var(--panel-2); border: 1px solid var(--line);
  border-radius: 6px; padding: 5px 10px; cursor: pointer; white-space: nowrap; }
button:hover { border-color: #46506a; }
button.active { border-color: var(--accent); color: var(--accent); }
#stop { border-color: #6b2f36; }
#map { position: fixed; inset: 44px 0 0 0; display: block; width: 100%; height: calc(100% - 44px);
  cursor: crosshair; touch-action: none; }
#map.panning { cursor: grabbing; }
#empty { position: fixed; inset: 44px 0 0 0; display: grid; place-items: center; text-align: center;
  color: var(--muted); pointer-events: none; }
#empty b { display: block; color: var(--text); font-size: 15px; margin-bottom: 4px; }
#toast { position: fixed; left: 12px; bottom: 12px; max-width: min(560px, calc(100% - 24px));
  background: var(--panel); border: 1px solid var(--line); border-left: 3px solid var(--ok);
  border-radius: 6px; padding: 8px 12px; opacity: 0; transform: translateY(6px);
  transition: opacity .15s, transform .15s; pointer-events: none; }
#toast.show { opacity: 1; transform: none; }
#toast.bad { border-left-color: var(--bad); }
#info { position: fixed; right: 12px; bottom: 12px; width: min(340px, calc(100% - 24px)); background: var(--panel);
  border: 1px solid var(--line); border-radius: 6px; padding: 10px 12px; display: none; }
#info.show { display: block; }
#info h3 { margin: 0 0 4px; font-size: 14px; }
#info.reach-ok { border-left: 3px solid var(--ok); }
#info.reach-ok h3 { color: var(--ok); }
#info.reach-bad { border-left: 3px solid var(--bad); }
#info.reach-bad h3 { color: var(--bad); }
#info.reach-none { border-left: 3px solid var(--muted); }
#info.reach-none h3 { color: var(--muted); }
#help { position: fixed; right: 12px; top: 56px; width: min(300px, calc(100% - 24px)); background: var(--panel);
  border: 1px solid var(--line); border-radius: 6px; padding: 10px 12px; display: none; color: var(--muted); }
#help.show { display: block; }
#help b { color: var(--text); }
#help .sw { display: inline-block; width: 12px; height: 12px; border-radius: 2px; vertical-align: -2px; margin-right: 6px; }
@media (max-width: 720px) { .wide { display: none; } }
</style>
</head>
<body>
<div id="bar">
  <span id="dot" title="Proxy connection"></span>
  <span class="title wide">GTProxy map</span>
  <span id="world">-</span>
  <span id="hover"></span>
  <span id="mode" class="chip wide" title="How paths are found (config.json: map.gravity)"></span>
  <span id="cw" class="chip wide" title="/clickwalk in the game: clicking a tile walks there">click-walk off</span>
  <button id="center" title="Center on you (C)">Me</button>
  <button id="follow" title="Keep you in view (F)">Follow</button>
  <button id="stop" title="Stop walking (Esc)">Stop</button>
  <button id="helpbtn" title="Help">?</button>
</div>
<canvas id="map"></canvas>
<div id="empty"><div><b>No world yet</b>Enter a world in the game - it appears here.</div></div>
<div id="toast"></div>
<div id="info"><h3 id="info-title"></h3><div id="info-text"></div></div>
<div id="help">
  <p><b>Left click</b> a tile: walk there (on a block: next to it).<br>
  <b>Right click</b>: can you get there from where you stand
  (Цель доступна / Цель недоступна / Нет данных; 4 directions, no squeezing between corners).<br>
  <b>Drag</b>: move the map, <b>wheel</b>: zoom.<br>
  <b>C</b> center on you, <b>F</b> follow, <b>Esc</b> stop.</p>
  <p><span class="sw" style="background:#ffcf4a"></span>you &nbsp;
  <span class="sw" style="background:#e6e9ef"></span>players &nbsp;
  <span class="sw" style="background:#4fc3ff"></span>path<br>
  <span class="sw" style="background:#ff4d4d"></span>dangerous &nbsp;
  <span class="sw" style="border:2px solid #7aa7ff"></span>door</p>
  <p>Walking sends the server the same moves the game sends while you run, at normal speed.</p>
</div>
)GTPROXY" };

constexpr std::string_view PAGE_SCRIPT_1{ R"GTPROXY(<script>
"use strict";
const TILE = 32, MIN_ZOOM = 0.05, MAX_ZOOM = 8;
const canvas = document.getElementById("map");
const ctx = canvas.getContext("2d");
const $ = id => document.getElementById(id);

let W = 0, H = 0, dpr = 1;
let cam = { ox: 0, oy: 0, zoom: 0.75 };
let tiles = { rev: -1, width: 0, height: 0, fg: [], bg: [], cell: [], items: {} };
let state = { players: [], objects: [], path: [], object_names: {} };
let online = false, follow = false, centered = false;
let hover = null, marker = null, dirty = true;
let drag = null;
const colorCache = new Map();

function resize() {
  dpr = window.devicePixelRatio || 1;
  W = canvas.clientWidth; H = canvas.clientHeight;
  canvas.width = Math.round(W * dpr); canvas.height = Math.round(H * dpr);
  dirty = true;
}
window.addEventListener("resize", resize);

// Cursor -> tile, the same conversion the proxy does (map::screen_to_tile).
function screenToWorld(sx, sy) { return { x: cam.ox + sx / cam.zoom, y: cam.oy + sy / cam.zoom }; }
function worldToScreen(wx, wy) { return { x: (wx - cam.ox) * cam.zoom, y: (wy - cam.oy) * cam.zoom }; }
function screenToTile(sx, sy) {
  if (sx < 0 || sy < 0 || sx >= W || sy >= H) return null;
  const w = screenToWorld(sx, sy);
  const tx = Math.floor(w.x / TILE), ty = Math.floor(w.y / TILE);
  if (tx < 0 || ty < 0 || tx >= tiles.width || ty >= tiles.height) return null;
  return { x: tx, y: ty };
}
function playerTile(p) { return { x: Math.floor((p.x + 10) / TILE), y: Math.floor((p.y + 15) / TILE) }; }
function me() { return (state.players || []).find(p => p.me); }

function centerOn(wx, wy) { cam.ox = wx - W / 2 / cam.zoom; cam.oy = wy - H / 2 / cam.zoom; dirty = true; }
function centerOnMe() {
  const p = me();
  if (p) centerOn(p.x + 10, p.y + 15);
  else if (tiles.width) centerOn(tiles.width * TILE / 2, tiles.height * TILE / 2);
}
function zoomAt(sx, sy, factor) {
  const w = screenToWorld(sx, sy);
  cam.zoom = Math.min(MAX_ZOOM, Math.max(MIN_ZOOM, cam.zoom * factor));
  cam.ox = w.x - sx / cam.zoom; cam.oy = w.y - sy / cam.zoom;
  dirty = true;
}

function itemColor(id, shade) {
  const key = id * 4 + shade;
  let c = colorCache.get(key);
  if (c) return c;
  const it = tiles.items[id];
  const hex = it ? it.color : "#888888";
  let r = parseInt(hex.slice(1, 3), 16), g = parseInt(hex.slice(3, 5), 16), b = parseInt(hex.slice(5, 7), 16);
  const k = shade === 1 ? 0.38 : 1;
  c = `rgb(${Math.round(r * k)},${Math.round(g * k)},${Math.round(b * k)})`;
  colorCache.set(key, c);
  return c;
}
function itemName(id) { const it = tiles.items[id]; return it ? it.name : "Item " + id; }
const CELL_NAMES = ["free", "solid", "platform", "dangerous", "door"];
</script>
)GTPROXY" };

constexpr std::string_view PAGE_SCRIPT_2{ R"GTPROXY(<script>
"use strict";
function draw() {
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.fillStyle = "#0e1116";
  ctx.fillRect(0, 0, W, H);
  $("empty").style.display = tiles.width ? "none" : "grid";
  if (!tiles.width) return;

  const ts = TILE * cam.zoom;
  const x0 = Math.max(0, Math.floor(cam.ox / TILE)), y0 = Math.max(0, Math.floor(cam.oy / TILE));
  const x1 = Math.min(tiles.width - 1, Math.floor((cam.ox + W / cam.zoom) / TILE));
  const y1 = Math.min(tiles.height - 1, Math.floor((cam.oy + H / cam.zoom) / TILE));
  const origin = worldToScreen(0, 0);
  ctx.fillStyle = "#151a22";
  ctx.fillRect(origin.x, origin.y, tiles.width * ts, tiles.height * ts);

  const pad = ts > 6 ? 0.5 : 0;
  for (let y = y0; y <= y1; y++) {
    for (let x = x0; x <= x1; x++) {
      const i = y * tiles.width + x;
      const sx = origin.x + x * ts, sy = origin.y + y * ts;
      const bg = tiles.bg[i], fg = tiles.fg[i], cell = tiles.cell[i];
      if (bg) { ctx.fillStyle = itemColor(bg, 1); ctx.fillRect(sx, sy, ts + 0.5, ts + 0.5); }
      if (fg) {
        ctx.fillStyle = itemColor(fg, 0);
        if (cell === 2) ctx.fillRect(sx + pad, sy + pad, ts - pad * 2, Math.max(2, ts * 0.3));
        else if (cell === 4) { ctx.globalAlpha = 0.55; ctx.fillRect(sx + pad, sy + pad, ts - pad * 2, ts - pad * 2); ctx.globalAlpha = 1; }
        else ctx.fillRect(sx + pad, sy + pad, ts - pad * 2, ts - pad * 2);
        if (cell === 3) { ctx.fillStyle = "rgba(255,60,60,0.45)"; ctx.fillRect(sx, sy, ts, ts); }
        if (cell === 4 && ts > 5) { ctx.strokeStyle = "#7aa7ff"; ctx.lineWidth = Math.max(1, ts / 14); ctx.strokeRect(sx + 1, sy + 1, ts - 2, ts - 2); }
      }
    }
  }

  if (ts >= 14) {
    ctx.strokeStyle = "rgba(255,255,255,0.05)";
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (let x = x0; x <= x1 + 1; x++) { const sx = Math.round(origin.x + x * ts) + 0.5; ctx.moveTo(sx, origin.y + y0 * ts); ctx.lineTo(sx, origin.y + (y1 + 1) * ts); }
    for (let y = y0; y <= y1 + 1; y++) { const sy = Math.round(origin.y + y * ts) + 0.5; ctx.moveTo(origin.x + x0 * ts, sy); ctx.lineTo(origin.x + (x1 + 1) * ts, sy); }
    ctx.stroke();
  }
  ctx.strokeStyle = "#3a4252"; ctx.lineWidth = 1;
  ctx.strokeRect(origin.x - 0.5, origin.y - 0.5, tiles.width * ts + 1, tiles.height * ts + 1);

  for (const o of state.objects || []) {
    const s = worldToScreen(o.x + 8, o.y + 8);
    const r = Math.max(2, 5 * cam.zoom);
    ctx.fillStyle = itemColorForObject(o.item);
    ctx.beginPath(); ctx.arc(s.x, s.y, r, 0, Math.PI * 2); ctx.fill();
    ctx.strokeStyle = "rgba(0,0,0,0.6)"; ctx.lineWidth = 1; ctx.stroke();
  }

  const path = state.path || [];
  if (path.length > 1) {
    ctx.strokeStyle = "#4fc3ff"; ctx.lineWidth = Math.max(2, ts / 10); ctx.setLineDash([Math.max(4, ts / 4), Math.max(3, ts / 6)]);
    ctx.beginPath();
    const p = me();
    const first = p ? worldToScreen(p.x + 10, p.y + 15) : worldToScreen(path[0].x * TILE + 16, path[0].y * TILE + 16);
    ctx.moveTo(first.x, first.y);
    for (let k = 1; k < path.length; k++) { const s = worldToScreen(path[k].x * TILE + 16, path[k].y * TILE + 16); ctx.lineTo(s.x, s.y); }
    ctx.stroke(); ctx.setLineDash([]);
  }
  const target = state.has_target ? state.target : null;
  if (target) ring(target, state.walking ? "#4fc3ff" : "rgba(79,195,255,0.45)");
  if (marker && performance.now() - marker.at < marker.hold) {
    const color = marker.kind === "none" ? "#8b93a3" : (marker.ok ? "#59d18c" : "#ff6b6b");
    ring(marker, color);
    const s = worldToScreen(marker.x * TILE, marker.y * TILE);
    ctx.strokeStyle = color; ctx.lineWidth = 2; ctx.strokeRect(s.x + 1, s.y + 1, TILE * cam.zoom - 2, TILE * cam.zoom - 2);
    dirty = true;
  }

  for (const p of state.players || []) {
    const s = worldToScreen(p.x, p.y);
    const w = 20 * cam.zoom, h = 30 * cam.zoom;
    ctx.fillStyle = p.me ? "#ffcf4a" : "#e6e9ef";
    ctx.strokeStyle = "rgba(0,0,0,0.7)"; ctx.lineWidth = 1.5;
    ctx.fillRect(s.x, s.y, Math.max(3, w), Math.max(4, h));
    ctx.strokeRect(s.x, s.y, Math.max(3, w), Math.max(4, h));
    if (ts >= 10 || p.me) {
      ctx.font = `${p.me ? 600 : 500} 12px system-ui, sans-serif`;
      ctx.textAlign = "center"; ctx.textBaseline = "bottom";
      const label = p.name || ("#" + p.id);
      ctx.lineWidth = 3; ctx.strokeStyle = "rgba(0,0,0,0.75)";
      ctx.strokeText(label, s.x + w / 2, s.y - 3);
      ctx.fillStyle = p.me ? "#ffcf4a" : "#e6e9ef";
      ctx.fillText(label, s.x + w / 2, s.y - 3);
    }
  }

  if (hover) {
    const s = worldToScreen(hover.x * TILE, hover.y * TILE);
    ctx.strokeStyle = "#ffffff"; ctx.lineWidth = 2;
    ctx.strokeRect(s.x + 1, s.y + 1, ts - 2, ts - 2);
  }
}
function ring(t, color) {
  const s = worldToScreen(t.x * TILE + 16, t.y * TILE + 16);
  ctx.strokeStyle = color; ctx.lineWidth = 2.5;
  ctx.beginPath(); ctx.arc(s.x, s.y, Math.max(6, 13 * cam.zoom), 0, Math.PI * 2); ctx.stroke();
}
function itemColorForObject(id) { return tiles.items[id] ? itemColor(id, 0) : "#ffd76a"; }
</script>
)GTPROXY" };

constexpr std::string_view PAGE_SCRIPT_3{ R"GTPROXY(<script>
"use strict";
let toastTimer = 0, infoTimer = 0;
function toast(text, ok) {
  const t = $("toast");
  t.textContent = text;
  t.classList.toggle("bad", !ok);
  t.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.remove("show"), 4000);
}
function showInfo(title, text, kind) {
  $("info-title").textContent = title;
  $("info-text").textContent = text;
  $("info").className = "show" + (kind ? " reach-" + kind : "");
  clearTimeout(infoTimer);
  infoTimer = setTimeout(() => $("info").classList.remove("show"), 8000);
}

async function post(path, params) {
  const query = params ? "?" + new URLSearchParams(params) : "";
  const response = await fetch(path + query, { method: "POST", headers: { "X-GTProxy-Map": "1" } });
  if (!response.ok) throw new Error((await response.text()) || ("HTTP " + response.status));
  return response.json();
}

// The click handler: the proxy turns the cursor into a tile with this view.
async function sendClick(sx, sy, button) {
  try {
    const res = await post("/api/click", { x: sx, y: sy, button, ox: cam.ox, oy: cam.oy, zoom: cam.zoom, w: W, h: H });
    if (res.action === "reach") {
      // The reachability check: shown on the tile and in the card.
      const kind = res.reach === "reachable" ? "ok" : (res.reach === "no_data" ? "none" : "bad");
      if (res.has_tile) { marker = { x: res.tile.x, y: res.tile.y, ok: res.ok, kind, at: performance.now(), hold: 4000 }; dirty = true; }
      showInfo(res.message, res.detail, kind);
    } else {
      if (res.has_tile) { marker = { x: res.tile.x, y: res.tile.y, ok: res.ok, at: performance.now(), hold: 900 }; dirty = true; }
      toast(res.message, res.ok);
    }
    poll(true);
  } catch (e) { toast("The proxy did not answer: " + e.message, false); }
}
async function stopWalk() {
  try { const res = await post("/api/stop"); toast(res.message, true); poll(true); }
  catch (e) { toast("The proxy did not answer: " + e.message, false); }
}

canvas.addEventListener("mousedown", e => {
  if (e.button !== 0) return;
  drag = { x: e.offsetX, y: e.offsetY, ox: cam.ox, oy: cam.oy, moved: false };
});
window.addEventListener("mousemove", e => {
  const r = canvas.getBoundingClientRect();
  const sx = e.clientX - r.left, sy = e.clientY - r.top;
  if (drag) {
    const dx = sx - drag.x, dy = sy - drag.y;
    if (!drag.moved && Math.hypot(dx, dy) > 4) { drag.moved = true; canvas.classList.add("panning"); follow = false; updateButtons(); }
    if (drag.moved) { cam.ox = drag.ox - dx / cam.zoom; cam.oy = drag.oy - dy / cam.zoom; dirty = true; }
  }
  const t = screenToTile(sx, sy);
  if ((t && (!hover || t.x !== hover.x || t.y !== hover.y)) || (!t && hover)) { hover = t; dirty = true; showHover(); }
});
window.addEventListener("mouseup", e => {
  if (e.button !== 0 || !drag) return;
  const wasDrag = drag.moved;
  drag = null; canvas.classList.remove("panning");
  if (!wasDrag && e.target === canvas) sendClick(e.offsetX, e.offsetY, 0);
});
canvas.addEventListener("contextmenu", e => { e.preventDefault(); sendClick(e.offsetX, e.offsetY, 2); });
canvas.addEventListener("mouseleave", () => { if (hover) { hover = null; dirty = true; showHover(); } });
canvas.addEventListener("wheel", e => { e.preventDefault(); zoomAt(e.offsetX, e.offsetY, Math.exp(-e.deltaY * 0.0015)); }, { passive: false });
window.addEventListener("keydown", e => {
  if (e.key === "c" || e.key === "C") centerOnMe();
  else if (e.key === "f" || e.key === "F") { follow = !follow; updateButtons(); }
  else if (e.key === "Escape") stopWalk();
  else if (e.key === "+" || e.key === "=") zoomAt(W / 2, H / 2, 1.25);
  else if (e.key === "-") zoomAt(W / 2, H / 2, 0.8);
});
$("center").onclick = () => centerOnMe();
$("follow").onclick = () => { follow = !follow; updateButtons(); if (follow) centerOnMe(); };
$("stop").onclick = () => stopWalk();
$("helpbtn").onclick = () => $("help").classList.toggle("show");

function showHover() {
  const el = $("hover");
  if (!hover) { el.textContent = ""; return; }
  const i = hover.y * tiles.width + hover.x;
  const fg = tiles.fg[i], bg = tiles.bg[i];
  el.textContent = "";
  const b = document.createElement("b");
  b.textContent = `${hover.x}, ${hover.y}`;
  el.append(b, "  " + (fg ? `${itemName(fg)} (${CELL_NAMES[tiles.cell[i]] || "?"})` : "empty") + (bg ? ` · ${itemName(bg)}` : ""));
}
function updateButtons() { $("follow").classList.toggle("active", follow); }
</script>
)GTPROXY" };

constexpr std::string_view PAGE_SCRIPT_4{ R"GTPROXY(<script>
"use strict";
let polling = false, pollTimer = 0;
async function loadTiles() {
  const t = await (await fetch("/api/tiles", { cache: "no-store" })).json();
  const worldChanged = t.world !== tiles.world || t.width !== tiles.width || t.height !== tiles.height;
  tiles = { rev: t.rev ?? 0, world: t.world || "", width: t.width || 0, height: t.height || 0,
    fg: t.fg || [], bg: t.bg || [], cell: t.cell || [], items: t.items || {} };
  colorCache.clear();
  if (worldChanged) centered = false;
  showHover();
}
async function poll(now) {
  if (polling && !now) return;
  polling = true;
  clearTimeout(pollTimer);
  try {
    const s = await (await fetch("/api/state", { cache: "no-store" })).json();
    online = true;
    state = s;
    if (s.tiles_rev !== tiles.rev) await loadTiles();
    if (!centered && tiles.width && W) { centerOnMe(); centered = !!me(); }
    if (follow) centerOnMe();
  } catch (e) { online = false; }
  $("dot").classList.toggle("on", online && !!state.connected);
  $("dot").title = !online ? "Proxy not running" : (state.connected ? "Connected to the game server" : "Proxy running, game not connected");
  $("world").textContent = !online ? "proxy offline" : (state.in_world ? (state.world || "?") : "not in a world");
  $("mode").textContent = state.gravity ? "paths: on foot" : "paths: free";
  $("cw").textContent = state.click_walk ? "click-walk on" : "click-walk off";
  $("cw").classList.toggle("on", !!state.click_walk);
  dirty = true;
  polling = false;
  pollTimer = setTimeout(() => poll(false), online ? 200 : 1000);
}
function frame() {
  if (dirty) { dirty = false; draw(); }
  requestAnimationFrame(frame);
}
resize();
updateButtons();
poll(false);
requestAnimationFrame(frame);
</script>
</body>
</html>
)GTPROXY" };
}

const std::string& page_html()
{
    static const std::string page{ [] {
        std::string html;
        for (const auto part : std::array{ PAGE_HEAD, PAGE_SCRIPT_1, PAGE_SCRIPT_2, PAGE_SCRIPT_3, PAGE_SCRIPT_4 }) {
            html += part;
        }
        return html;
    }() };
    return page;
}
}
