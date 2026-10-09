// airmidi web UI: plain JS, no framework, no build step besides gzip. Polls /api/status once a second; every
// control is drawn from that state, so the page is always what the box reports, never what we last sent.
"use strict";

const $ = (s) => document.querySelector(s);
const HUES = { trs: "fir", ble: "slate", net: "heather", rtp: "heather", web: "salmon", usb: "fir", espnow: "salmon" };
let status = null;
let last = null;  // previous status, for message rates
let lastAt = 0;

async function api(path, opts = {}) {
  const r = await fetch(path, opts);
  const body = r.headers.get("content-type")?.includes("json") ? await r.json() : await r.text();
  if (!r.ok) throw new Error(body.error || body || r.statusText);
  return body;
}
const post = (path, obj) =>
  api(path, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify(obj) });

// ---- pages -----------------------------------------------------------------------------------------------------

function show(page) {
  document.querySelectorAll(".page").forEach((p) => p.classList.toggle("on", p.id === page));
  document.querySelectorAll("#tabs a").forEach((a) => a.classList.toggle("on", a.dataset.page === page));
  if (page === "wifi") loadNetworks();
  if (page === "play") wsConnect(), loadDevice();
  if (page === "settings") loadSettings();
}
document.querySelectorAll("#tabs a").forEach((a) => (a.onclick = () => (location.hash = a.dataset.page)));
window.onhashchange = () => show(location.hash.slice(1) || "status");

// ---- controls ----------------------------------------------------------------------------------------------------

// Text twice: once in the neutral, once in the background colour clipped to the fill, so it inverts where the fill
// crosses it (the tile's signature).
function textLayers(html, clip) {
  return `<div class="t">${html}</div><div class="t inv" style="clip-path:${clip}">${html}</div>`;
}

function tile(el, name, value, frac) {
  frac = Math.max(0, Math.min(1, frac));
  const h = `max(calc(3 * var(--u)), ${frac * 100}%)`;  // never thinner than the stroke
  el.innerHTML =
    `<div class="fill" style="height:${h}"></div>` +
    textLayers(`<span class="v value">${value}</span><span class="nm label">${name}</span>`,
      `inset(calc(100% - ${h}) 0 0 0)`);
}

function wide(el, name, value, frac) {
  if (el.querySelector("input")) return;  // being edited: leave it alone
  frac = Math.max(0, Math.min(1, frac));
  const w = frac > 0 ? `max(calc(3 * var(--u)), ${frac * 100}%)` : "0%";
  el.innerHTML =
    `<div class="fill" style="width:${w}"></div>` +
    textLayers(`<span class="label">${name}</span><span class="label">${value}</span>`,
      `inset(0 calc(100% - ${w}) 0 0)`);
}

// Click a wide tile to type into it: the value turns accent with an accent caret; enter commits, escape reverts.
function editable(el, name, get, commit, secret = false) {
  el.onclick = () => {
    if (el.querySelector("input")) return;
    const input = document.createElement("input");
    input.type = secret ? "password" : "text";
    input.value = get();
    el.querySelector(".t:not(.inv) span:last-child")?.replaceChildren();
    el.querySelector(".t.inv")?.remove();
    el.appendChild(input);
    input.focus();
    input.select();
    const done = async (ok) => {
      const v = input.value;
      input.remove();
      if (ok) await commit(v);
      render();
    };
    input.onkeydown = (e) => {
      if (e.key === "Enter") done(true);
      if (e.key === "Escape") done(false);
    };
    input.onblur = () => done(false);
  };
  el.dataset.name = name;
}

// ---- status page -------------------------------------------------------------------------------------------------

function portHue(name) {
  return HUES[name.replace(/[0-9]/g, "")] || "salmon";
}

function renderMeters() {
  const dt = last ? (status.uptime_ms - last.uptime_ms) / 1000 : 0;
  const level = (n) => Math.log10(1 + n) / 3;  // messages per second, 1000/s = full
  $("#meters").innerHTML = status.ports
    .map((p, i) => {
      const prev = last?.ports[i];
      const rin = prev && dt > 0 ? (p.in - prev.in) / dt : 0;
      const rout = prev && dt > 0 ? (p.out - prev.out) / dt : 0;
      const bar = (r) => `<div class="bar"><i style="height:max(calc(3 * var(--u)), ${level(r) * 100}%)"></i></div>`;
      return `<div class="port${p.up ? "" : " down"}" data-hue="${portHue(p.name)}">
        <div class="bars">${bar(rin)}${bar(rout)}</div><span class="label">${p.name}</span></div>`;
    })
    .join("");
}

function renderStatus() {
  const s = status;
  $("#name").textContent = s.name;
  document.title = s.name;
  renderMeters();
  const b = s.battery;
  tile($("#t-batt"), "batt", b.usb ? "usb" : b.pct, b.usb ? 1 : b.pct / 100);
  const rssi = s.wifi.rssi;
  tile($("#t-signal"), "wifi", rssi ? rssi : "off", rssi ? (rssi + 100) / 70 : 0);
  document.querySelectorAll("#mode a").forEach((a) => a.classList.toggle("on", a.dataset.v === s.mode));
  const facts = [
    ["version", s.version],
    ["board", s.board],
    ["up", fmtUptime(s.uptime_ms)],
    ["reset", s.reset_reason],
    ["wifi", s.wifi.state + (s.wifi.ip ? " " + s.wifi.ip : "")],
    ["device", s.device.target
      ? `${s.device.state}${s.device.name ? " " + esc(s.device.name) : ""}${s.device.interval_ms ? " " + s.device.interval_ms + " ms" : ""}`
      : "none set"],
    ["ble", s.ble.map((c, i) => `${i + 1} ${c.state}${c.interval_ms ? " " + c.interval_ms + " ms" : ""}`).join(", ")],
    ["net", s.net.map((c, i) => c.active
      ? `${i + 1} ${c.peer} (midi ${c.protocol}.0), lost ${c.lost}, recovered ${c.recovered}` : `${i + 1} idle`).join(", ")],
    ["rtp", s.rtp.map((c, i) => c.active
      ? `${i + 1} ${c.peer}, lost ${c.lost}, repaired ${c.repairs}` : `${i + 1} idle`).join(", ")],
    ["trs", `in ${s.trs.rx} bytes, out ${s.trs.tx} bytes, dropped ${s.trs.rx_dropped + s.trs.tx_dropped}`],
    ["midi-ci", s.ci.scanning ? "asking…" : s.ci.device ? `${s.ci.device} (${s.ci.result})` : s.ci.result || "not asked yet"],
    ["heap", `${Math.round(s.heap / 1024)} kb free`],
  ];
  $("#facts").innerHTML = facts.map(([k, v]) => `<dt>${k}</dt><dd>${v}</dd>`).join("");
}

function fmtUptime(ms) {
  const s = Math.floor(ms / 1000), m = Math.floor(s / 60), h = Math.floor(m / 60);
  return h ? `${h} h ${m % 60} min` : m ? `${m} min ${s % 60} s` : `${s} s`;
}

document.querySelectorAll("#mode a").forEach((a) => (a.onclick = async () => {
  if (a.dataset.v === "setup" && !confirm("setup mode leaves this network and opens the box's own access point")) return;
  await post("/api/mode", { mode: a.dataset.v });
  poll();
}));

// ---- play page: a generic control surface over the WebSocket, under the device's own controls ------------------

const FADERS = [["mod", 1], ["vol", 7], ["expr", 11], ["cut", 74], ["res", 71], ["atk", 73], ["rel", 72], ["fx", 91]];
const DEFAULTS = { 7: 100, 11: 127, 74: 64 };
const NAMES = ["c", "c#", "d", "d#", "e", "f", "f#", "g", "g#", "a", "a#", "b"];
const play = { ch: 0, oct: 4, cc: {}, held: new Set(), log: [] };
let ws = null;

function wsConnect() {
  if (ws && ws.readyState <= 1) return;
  ws = new WebSocket(`ws://${location.host}/ws`);
  ws.binaryType = "arraybuffer";
  ws.onmessage = (e) => {
    const v = new DataView(e.data);
    for (let i = 0; i + 4 <= v.byteLength; ) {
      const w = v.getUint32(i);
      const mt = w >>> 28, words = [1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4][mt];
      if (mt === 2) received((w >> 16) & 0xff, (w >> 8) & 0x7f, w & 0x7f);
      if (mt === 2 || (mt === 4 && i + 8 <= v.byteLength))
        deviceReceived(mt, (w >> 16) & 0xff, (w >> 8) & 0x7f, w & 0x7f, mt === 4 ? v.getUint32(i + 4) : 0);
      i += 4 * words;
    }
  };
  ws.onclose = () => setTimeout(() => location.hash === "#play" && wsConnect(), 1000);
}

function sendUmp(words, text) {
  if (!ws || ws.readyState !== 1) return;
  const b = new DataView(new ArrayBuffer(4 * words.length));
  words.forEach((w, k) => b.setUint32(4 * k, w));
  ws.send(b.buffer);
  logText(text);
}

function sendMidi(st, d1, d2) {
  sendUmp([(0x20000000 | (st << 16) | (d1 << 8) | d2) >>> 0], midiText("out", st, d1, d2));
}

function received(st, d1, d2) {
  logMidi("in", st, d1, d2);
  if ((st & 0x0f) !== play.ch) return;
  if ((st & 0xf0) === 0xb0) play.cc[d1] = d2, renderFaders();
  const note = d1 - 12 * play.oct;
  if (((st & 0xf0) === 0x90 || (st & 0xf0) === 0x80) && note >= 0 && note < 12) {
    $(`#pads .pad[data-n="${note}"]`)?.classList.toggle("on", (st & 0xf0) === 0x90 && d2 > 0);
  }
}

function midiText(dir, st, d1, d2) {
  const kind = { 0x80: "off", 0x90: "on", 0xa0: "poly", 0xb0: "cc", 0xc0: "pgm", 0xd0: "press", 0xe0: "bend" }[st & 0xf0];
  return `${dir}  ch ${(st & 0x0f) + 1}  ${kind} ${d1} ${d2}`;
}

function logMidi(dir, st, d1, d2) {
  logText(midiText(dir, st, d1, d2));
}

function logText(text) {
  play.log.unshift(esc(text));
  play.log.length = Math.min(play.log.length, 8);
  if (!logQueued) (logQueued = true), requestAnimationFrame(() => {
    logQueued = false;
    $("#monitor").innerHTML = play.log.map((l) => `<li>${l}</li>`).join("");
  });
}
let logQueued = false;

const TRI_L = `<svg viewBox="0 0 28 36"><path d="M28 0v36L0 18z"/></svg>`;
const TRI_R = `<svg viewBox="0 0 28 36"><path d="M0 0v36l28-18z"/></svg>`;

function stepper(el, name, get, set, min, max) {
  el.innerHTML = `<div class="ctl"><a class="dn">${TRI_L}</a><span class="value"></span><a class="up">${TRI_R}</a></div>
    <span class="label">${name}</span>`;
  const show = () => (el.querySelector(".value").textContent = get());
  el.querySelector(".dn").onclick = () => (set(Math.max(min, get() - 1)), show());
  el.querySelector(".up").onclick = () => (set(Math.min(max, get() + 1)), show());
  show();
}

function renderFaders() {
  document.querySelectorAll("#faders .tile").forEach((t) => {
    const cc = +t.dataset.cc, v = play.cc[cc] ?? 0;
    tile(t, t.dataset.name, v, v / 127);
  });
}

function buildPlay() {
  stepper($("#s-ch"), "ch", () => play.ch + 1, (v) => (play.ch = v - 1), 1, 16);
  stepper($("#s-oct"), "oct", () => play.oct - 1, (v) => (play.oct = v + 1), -1, 8);
  $("#faders").innerHTML = FADERS.map(([n, cc]) => `<div class="tile drag" data-hue="salmon" data-cc="${cc}" data-name="${n}"></div>`).join("");
  document.querySelectorAll("#faders .tile").forEach((t) => {
    const cc = +t.dataset.cc;
    play.cc[cc] = DEFAULTS[cc] ?? 0;
    let startY = 0, startV = 0;
    t.onpointerdown = (e) => {
      t.setPointerCapture(e.pointerId);
      startY = e.clientY, startV = play.cc[cc];
    };
    t.onpointermove = (e) => {
      if (!t.hasPointerCapture(e.pointerId)) return;
      const h = t.getBoundingClientRect().height, fine = e.shiftKey ? 4 : 1;
      const v = Math.max(0, Math.min(127, Math.round(startV + ((startY - e.clientY) / h) * 127 / fine)));
      if (v !== play.cc[cc]) play.cc[cc] = v, sendMidi(0xb0 | play.ch, cc, v), renderFaders();
    };
    t.ondblclick = () => {
      play.cc[cc] = DEFAULTS[cc] ?? 0;
      sendMidi(0xb0 | play.ch, cc, play.cc[cc]);
      renderFaders();
    };
  });
  renderFaders();
  $("#pads").innerHTML = NAMES.map((n, i) => `<div class="pad" data-hue="salmon" data-n="${i}"><span class="small">${n}</span></div>`).join("");
  document.querySelectorAll("#pads .pad").forEach((p) => {
    const note = () => 12 * play.oct + +p.dataset.n;
    p.onpointerdown = (e) => {
      p.setPointerCapture(e.pointerId);
      p.classList.add("on");
      sendMidi(0x90 | play.ch, note(), 100);
    };
    const up = () => {
      if (!p.classList.contains("on")) return;
      p.classList.remove("on");
      sendMidi(0x80 | play.ch, note(), 0);
    };
    p.onpointerup = up;
    p.onpointercancel = up;
  });
}

// ---- play page: the device's own controls, from its MIDI-CI control list (M2-117 AllCtrlList) or a profile -------

const MAX32 = 0xffffffff;
// ctrlType -> MIDI 2.0 channel voice opcode; per-note types need a note and are not drawn
const OPCODES = { cc: 0xb, chPress: 0xd, pBend: 0xe, rpn: 0x2, nrpn: 0x3 };
const dev = { ctrls: [], vals: [], maps: {}, key: "" };

async function loadDevice() {
  let d;
  try {
    d = await api("/api/ci/device");
  } catch (e) {
    $("#dev-msg").textContent = e.message;
    return;
  }
  // a profile is either a bare AllCtrlList or {"info", "controls", "maps"}
  const p = Array.isArray(d.profile) ? { controls: d.profile } : d.profile || {};
  const fromCi = Array.isArray(d.controls);
  const info = (fromCi ? d.info : p.info) || d.info;
  dev.maps = (fromCi ? d.maps : p.maps) || {};
  const all = (fromCi ? d.controls : p.controls) || [];
  dev.ctrls = all
    .filter((c) => c && c.title && OPCODES[c.ctrlType] !== undefined && (c.priority ?? 3) < 5 && c.recognize !== "none")
    .sort((a, b) => (a.priority ?? 3) - (b.priority ?? 3));
  dev.vals = dev.ctrls.map((c) => c.default ?? (c.ctrlType === "pBend" ? 0x80000000 : 0));
  $("#dev-name").textContent = info ? [info.manufacturer, info.model].filter(Boolean).join(" ") || "device" : "no device";
  $("#dev-clear").hidden = !d.profile;
  const skipped = all.length - dev.ctrls.length;
  $("#dev-msg").textContent = skipped > 0 ? `${skipped} controls not shown (hidden, per-note or not recognized)` : "";
  buildDevice();
}

// What a control looks like: a fader, a picker over named values, or a toggle.
function kind(c) {
  if (c.typeHint === "toggle" || c.typeHint === "momentary") return "toggle";
  if (dev.maps[c.ctrlMapId] && c.typeHint === "valueSelect") return "select";
  if (c.stepCount > 1 && c.stepCount <= 8) return "select";
  return "fader";
}

function options(c) {
  const m = dev.maps[c.ctrlMapId];
  if (Array.isArray(m) && kind(c) === "select") return m.map((o) => [o.title, o.value]);
  // stepped values without names: step k at k * 2^32 / n (the same spacing the stand-in's map uses)
  return Array.from({ length: c.stepCount }, (_, k) => [String(k + 1), Math.floor((k * 2 ** 32) / c.stepCount)]);
}

function range(c) {
  return Array.isArray(c.minMax) ? c.minMax : [0, MAX32];
}

function shown(c, v) {
  const m = dev.maps[c.ctrlMapId];
  if (Array.isArray(m) && m.length) {
    // the named value at or below v (a cutoff map names points along the range)
    const below = m.filter((o) => o.value <= v).sort((a, b) => b.value - a.value)[0];
    if (below) return below.title;
  }
  if (c.ctrlType === "pBend") return Math.round(((v - 0x80000000) / 0x80000000) * 100);
  const [lo, hi] = range(c);
  return Math.round(((v - lo) / (hi - lo || 1)) * 100);
}

function sendCtrl(i) {
  const c = dev.ctrls[i], v = dev.vals[i] >>> 0;
  const ch = (c.channel ?? 1) - 1, idx = c.ctrlIndex || [];
  const op = OPCODES[c.ctrlType];
  const i1 = op === 0xb || op === 0x2 || op === 0x3 ? idx[0] ?? 0 : 0;
  const i2 = op === 0x2 || op === 0x3 ? idx[1] ?? 0 : 0;
  // MIDI 2.0 channel voice (MT 0x4): the 32-bit value goes out as it is; the box scales it for MIDI 1.0 outputs
  sendUmp([(0x40000000 | ((ch >> 4) & 15) << 24 | (op << 4 | (ch & 15)) << 16 | (i1 & 0x7f) << 8 | (i2 & 0x7f)) >>> 0, v],
    `out  ch ${(ch & 15) + 1}  ${c.title} ${shown(c, v)}`);
}

function renderDevice() {
  dev.ctrls.forEach((c, i) => {
    const el = document.querySelector(`.dev [data-i="${i}"]`);
    if (!el) return;
    const v = dev.vals[i], k = kind(c);
    if (k === "fader") {
      const [lo, hi] = range(c);
      tile(el, c.title, shown(c, v), (v - lo) / (hi - lo || 1));
    } else if (k === "select") {
      const opts = options(c);
      // the option nearest to the value (a value from the device need not be one of the listed ones)
      let best = 0;
      opts.forEach(([, ov], j) => Math.abs(ov - v) < Math.abs(opts[best][1] - v) && (best = j));
      el.querySelectorAll(".options a").forEach((a, j) => a.classList.toggle("on", j === best));
    } else {
      el.classList.toggle("on", v >= 0x80000000);
    }
  });
}

function buildDevice() {
  const faders = [], switches = [];
  dev.ctrls.forEach((c, i) => {
    const k = kind(c);
    if (k === "fader") faders.push(`<div class="tile drag" data-hue="fir" data-i="${i}"></div>`);
    else if (k === "select")
      switches.push(`<div class="picker" data-hue="heather" data-i="${i}"><div class="options">${options(c)
        .map(([t]) => `<a>${esc(t)}</a>`).join("")}</div><div class="label name">${esc(c.title)}</div></div>`);
    else switches.push(`<div class="toggle" data-hue="slate" data-i="${i}"><a class="box"></a><span class="label">${esc(c.title)}</span></div>`);
  });
  $("#dev-faders").innerHTML = faders.join("");
  $("#dev-switches").innerHTML = switches.join("");
  document.querySelectorAll(".dev [data-i]").forEach((el) => {
    const i = +el.dataset.i, c = dev.ctrls[i], k = kind(c);
    const set = (v) => {
      v = Math.max(0, Math.min(MAX32, Math.round(v)));
      if (v === dev.vals[i]) return;
      dev.vals[i] = v;
      sendCtrl(i);
      renderDevice();
    };
    if (k === "fader") {
      const [lo, hi] = range(c);
      let startY = 0, startV = 0;
      el.onpointerdown = (e) => (el.setPointerCapture(e.pointerId), (startY = e.clientY), (startV = dev.vals[i]));
      el.onpointermove = (e) => {
        if (!el.hasPointerCapture(e.pointerId)) return;
        const h = el.getBoundingClientRect().height, fine = e.shiftKey ? 4 : 1;
        set(Math.max(lo, Math.min(hi, startV + (((startY - e.clientY) / h) * (hi - lo)) / fine)));
      };
      el.ondblclick = () => set(c.default ?? (c.ctrlType === "pBend" ? 0x80000000 : lo));
      // a pitch bend springs back to the centre when let go
      if (c.ctrlType === "pBend") el.onpointerup = () => set(0x80000000);
    } else if (k === "select") {
      el.querySelectorAll(".options a").forEach((a, j) => (a.onclick = () => set(options(c)[j][1])));
    } else if (c.typeHint === "momentary") {
      el.onpointerdown = () => set(MAX32);
      el.onpointerup = el.onpointercancel = () => set(0);
    } else {
      el.onclick = () => set(dev.vals[i] >= 0x80000000 ? 0 : MAX32);
    }
  });
  renderDevice();
}

// Incoming messages move the device's controls too (the device sends its CCs when its own knobs turn).
function deviceReceived(mt, st, i1, i2, data) {
  const ch = st & 0x0f, type = st & 0xf0;
  dev.ctrls.forEach((c, i) => {
    if (((c.channel ?? 1) - 1) % 16 !== ch) return;
    const t = c.ctrlType, idx = c.ctrlIndex || [];
    let v = null;
    if (mt === 2 && t === "cc" && type === 0xb0 && i1 === idx[0]) v = (i2 / 127) * MAX32;
    else if (mt === 2 && t === "chPress" && type === 0xd0) v = (i1 / 127) * MAX32;
    else if (mt === 2 && t === "pBend" && type === 0xe0) v = ((i1 | (i2 << 7)) / 16383) * MAX32;
    else if (mt === 4 && t === "cc" && type === 0xb0 && i1 === idx[0]) v = data;
    else if (mt === 4 && t === "pBend" && type === 0xe0) v = data;
    if (v !== null) dev.vals[i] = Math.round(v);
  });
  renderDevice();
}

$("#dev-scan").onclick = async () => {
  $("#dev-msg").textContent = "asking the device on trs…";
  await post("/api/ci/scan", { force: true }).catch((e) => ($("#dev-msg").textContent = e.message));
};

$("#dev-file").onchange = async (e) => {
  const f = e.target.files[0];
  e.target.value = "";
  if (!f) return;
  const text = await f.text();
  try {
    const j = JSON.parse(text);
    if (!Array.isArray(j) && !Array.isArray(j.controls)) throw new Error("expected [...] or {\"controls\": [...]}");
    await api("/api/ci/profile", { method: "POST", headers: { "content-type": "application/json" }, body: text });
    $("#dev-msg").textContent = `profile ${f.name} stored`;
    loadDevice();
  } catch (err) {
    $("#dev-msg").textContent = `profile not stored: ${err.message}`;
  }
};

$("#dev-clear").onclick = async () => {
  await api("/api/ci/profile", { method: "DELETE" });
  $("#dev-msg").textContent = "profile removed";
  loadDevice();
};

// The device part of the status: reload the controls when it changes (a scan finished, a profile came or went).
function watchDevice(ci) {
  const src = { "midi-ci": "midi-ci", cache: "midi-ci, cached", profile: "profile" }[ci.source] ?? "";
  $("#dev-src").textContent = ci.scanning ? "asking…" : src || (ci.result ? `no control list (${ci.result})` : "");
  const key = `${ci.source}|${ci.device}|${ci.result}|${ci.scanning}`;
  if (key !== dev.key) {
    dev.key = key;
    if (!ci.scanning) loadDevice();
  }
}

// ---- settings page -----------------------------------------------------------------------------------------------

let settings = null;

async function loadSettings() {
  settings = await api("/api/settings");
  renderSettings();
  loadBle();
}

function renderSettings() {
  if (!settings) return;
  wide($("#w-name"), "name", settings.name, 0);
  const t = settings.sysex_timeout_ms;
  wide($("#w-sysex"), "sysex wait", `${t} ms`, Math.log(t / 50) / Math.log(100));
  $("#t-blenet").classList.toggle("on", settings.ble_in_network);
  const d = settings.ble_device;
  wide($("#w-bledev"), "ble device", d === "*" ? "any" : d ? esc(d) : "none", 0);
}

// BLE MIDI devices around (the box scans 5 s when asked); clicking one makes it the device to connect to.
let bleTimer = null;
async function loadBle() {
  const b = await api("/api/ble");
  $("#ble-found").innerHTML = b.found
    .map((f) => [f.name || f.address, f])
    .map(([id, f]) => `<li class="${id === b.name && b.state === "connected" ? "on" : ""}"><a data-name="${esc(id)}">${esc(id)}</a>
      <span class="small">${f.rssi} dBm</span></li>`)
    .join("") || `<li>${b.scanning ? "looking…" : "none found"}</li>`;
  document.querySelectorAll("#ble-found a").forEach((a) => (a.onclick = () => saveSetting("ble_device", a.dataset.name)));
  clearTimeout(bleTimer);
  if (b.scanning) bleTimer = setTimeout(loadBle, 1000);
}
$("#ble-scan").onclick = async () => {
  try {
    await post("/api/ble/scan", {});
    setTimeout(loadBle, 500);
  } catch (e) {
    $("#settings-msg").textContent = e.message;
  }
};
editable($("#w-bledev"), "ble device", () => settings.ble_device,
  (v) => saveSetting("ble_device", v.trim().toLowerCase() === "any" ? "*" : v.trim()));

$("#t-blenet").onclick = () => saveSetting("ble_in_network", !settings.ble_in_network);

async function saveSetting(key, value) {
  try {
    const r = await post("/api/settings", { ...settings, [key]: value });
    settings = r.settings;
    $("#settings-msg").textContent = r.restart ? "saved: restarting to apply it" : "saved";
  } catch (e) {
    $("#settings-msg").textContent = e.message;
  }
}

editable($("#w-name"), "name", () => settings.name, (v) => saveSetting("name", v));
editable($("#w-sysex"), "sysex wait", () => String(settings.sysex_timeout_ms),
  (v) => saveSetting("sysex_timeout_ms", parseInt(v, 10)));

// ---- wifi page ---------------------------------------------------------------------------------------------------

let newSsid = "", newPass = "";

let networks = [];

async function loadNetworks() {
  networks = (await api("/api/wifi")).networks;
  renderNetworks();
}

function renderNetworks() {
  const joined = status?.wifi.ssid;
  const r = { networks };
  $("#networks").innerHTML = r.networks
    .map((n) => `<li class="${n === joined ? "on" : ""}"><span>${esc(n)}</span>
      <a class="glyph" data-ssid="${esc(n)}" title="forget"><svg viewBox="0 0 24 24"><path d="M5 5l14 14M19 5L5 19"/></svg></a></li>`)
    .join("") || `<li>no networks stored</li>`;
  document.querySelectorAll("#networks .glyph").forEach((g) => (g.onclick = async () => {
    await api("/api/wifi?ssid=" + encodeURIComponent(g.dataset.ssid), { method: "DELETE" });
    loadNetworks();
  }));
}

function renderWifiInputs() {
  wide($("#w-ssid"), "network", esc(newSsid), 0);
  wide($("#w-pass"), "password", newPass ? "*".repeat(Math.min(newPass.length, 12)) : "", 0);
}

editable($("#w-ssid"), "network", () => newSsid, (v) => (newSsid = v));
editable($("#w-pass"), "password", () => newPass, (v) => (newPass = v), true);
$("#add-net").onclick = async () => {
  try {
    await post("/api/wifi", { ssid: newSsid, pass: newPass });
    newSsid = newPass = "";
    $("#wifi-msg").textContent = "stored; the box joins it in network mode";
  } catch (e) {
    $("#wifi-msg").textContent = e.message;
  }
  loadNetworks();
};

function esc(s) {
  return String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
}

// ---- update page -------------------------------------------------------------------------------------------------

let uploading = false;

function renderUpdate() {
  const o = status.ota;
  const facts = [
    ["version", status.version],
    ["running", `${o.running}, ${o.state}`],
    ["rolled back", o.rolled_back || "no"],
    ["crash", o.coredump.present ? `core dump, ${o.coredump.size} bytes, task ${o.coredump.task}` : "none"],
  ];
  $("#image").innerHTML = facts.map(([k, v]) => `<dt>${k}</dt><dd>${esc(v)}</dd>`).join("") +
    (o.coredump.present ? `<dt></dt><dd><a href="/api/coredump">download core dump</a> &nbsp;
      <a id="forget-dump">forget it</a></dd>` : "");
  const f = $("#forget-dump");
  if (f) f.onclick = async () => (await api("/api/coredump", { method: "DELETE" }), poll());
  if (!uploading) wide($("#w-upload"), "firmware", "", 0);
  renderRelease();
}

let release = null;

function renderRelease() {
  const f = status.ota.fetch;
  if (f.state === "downloading") wide($("#w-release"), "download", f.percent, f.percent / 100);
  else if (release) wide($("#w-release"), "release", esc(release.latest), 0);
  else wide($("#w-release"), "release", "", 0);
  if (f.state === "failed") $("#update-msg").textContent = "download failed: " + f.error;
  if (f.state === "installed") $("#update-msg").textContent = "installed: restarting into it";
}

$("#check").onclick = async () => {
  try {
    release = await post("/api/update/check", {});
    $("#install").hidden = !release.newer;
    $("#update-msg").textContent = release.newer ? `${release.latest} is available` : "this is the newest release";
  } catch (e) {
    $("#update-msg").textContent = e.message;
  }
  render();
};
$("#install").onclick = async () => {
  try {
    await post("/api/update/install", {});
    $("#install").hidden = true;
  } catch (e) {
    $("#update-msg").textContent = e.message;
  }
};

$("#file").onchange = () => {
  const f = $("#file").files[0];
  if (!f) return;
  uploading = true;
  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/api/update");
  xhr.upload.onprogress = (e) => wide($("#w-upload"), "firmware", Math.round((100 * e.loaded) / e.total), e.loaded / e.total);
  xhr.onload = () => {
    uploading = false;
    $("#update-msg").textContent = xhr.status === 200
      ? "image accepted: restarting into it; it confirms itself after its self-test"
      : "rejected: " + xhr.responseText;
  };
  xhr.onerror = () => ((uploading = false), ($("#update-msg").textContent = "upload failed"));
  xhr.send(f);
};

// ---- loop --------------------------------------------------------------------------------------------------------

function render() {
  if (!status) return;
  renderStatus();
  renderSettings();
  renderWifiInputs();
  if ($("#wifi").classList.contains("on")) renderNetworks();
  renderUpdate();
}

async function poll() {
  try {
    const s = await api("/api/status");
    last = status;
    status = s;
    const moved = last && s.ports.some((p, i) => p.in !== last.ports[i].in || p.out !== last.ports[i].out);
    $("#activity").classList.toggle("on", !!moved);
    render();
    if (s.ci) watchDevice(s.ci);
  } catch (e) {
    $("#activity").classList.remove("on");
  }
}

buildPlay();
show(location.hash.slice(1) || "status");
poll();
setInterval(poll, 1000);
