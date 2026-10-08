// airmidi web UI: plain JS, no framework, no build step besides gzip. Polls /api/status once a second; every
// control is drawn from that state, so the page is always what the box reports, never what we last sent.
"use strict";

const $ = (s) => document.querySelector(s);
const HUES = { trs: "fir", ble: "slate", net: "heather", web: "salmon", usb: "fir", espnow: "salmon" };
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
    ["ble", s.ble.map((c, i) => `${i + 1} ${c.state}${c.interval_ms ? " " + c.interval_ms + " ms" : ""}`).join(", ")],
    ["trs", `in ${s.trs.rx} bytes, out ${s.trs.tx} bytes, dropped ${s.trs.rx_dropped + s.trs.tx_dropped}`],
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

// ---- settings page -----------------------------------------------------------------------------------------------

let settings = null;

async function loadSettings() {
  settings = await api("/api/settings");
  renderSettings();
}

function renderSettings() {
  if (!settings) return;
  wide($("#w-name"), "name", settings.name, 0);
  const t = settings.sysex_timeout_ms;
  wide($("#w-sysex"), "sysex wait", `${t} ms`, Math.log(t / 50) / Math.log(100));
}

async function saveSetting(key, value) {
  try {
    const r = await post("/api/settings", { ...settings, [key]: value });
    settings = r.settings;
    $("#settings-msg").textContent = r.restart ? "restarting with the new name" : "saved";
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
  } catch (e) {
    $("#activity").classList.remove("on");
  }
}

show(location.hash.slice(1) || "status");
poll();
setInterval(poll, 1000);
