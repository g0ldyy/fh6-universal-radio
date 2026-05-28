import { createRoonPanel } from "./roon-panel.js";

// FH6 Universal Radio dashboard. Vanilla JS, no build step. `state` holds
// the latest /api/state; `cfg` holds the latest /api/config. Render functions
// are idempotent and only touch nodes whose displayed value changed.
const $  = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const api = {
  async get(path)        { return (await fetch(path)).json(); },
  async send(path, body, method = "POST") {
    const r = await fetch(path, {
      method,
      headers: body ? { "content-type": "application/json" } : {},
      body:    body ? JSON.stringify(body) : undefined,
    });
    if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error || r.statusText);
    return r.json().catch(() => ({}));
  },
};
let state = null;
let cfg   = null;
const fmt = ms => {
  if (!ms || ms < 0) return "0:00";
  const s = Math.floor(ms / 1000);
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;
};
const toast = (msg, isErr = false) => {
  const el = document.createElement("div");
  el.className = "toast" + (isErr ? " err" : "");
  el.textContent = msg;
  document.body.appendChild(el);
  setTimeout(() => el.remove(), 2400);
};
// Only write when the displayed value changes, to avoid cursor jumps in inputs.
const setText = (el, v) => { if (el && el.textContent !== String(v)) el.textContent = v; };
const roonPanel = createRoonPanel({
  api,
  $,
  setText,
  toast,
  getState: () => state,
  getConfig: () => cfg,
  setConfig: next => { cfg = next; },
  requestRender: () => render(),
});

function renderStatus() {
  const ok = state?.game?.attached;
  const sub = $("#status");
  sub.className = "subtitle " + (ok ? "ok" : "err");
  sub.textContent = ok ? "connected" : "bridge offline";
}

function renderNowPlaying() {
  const t = roonPanel.nowPlaying() || state?.track || {};
  const a = state?.sources?.active;
  setText($("#np-title"),  t.title  || "Nothing playing");
  setText($("#np-artist"), t.artist ? `${t.artist}${t.album ? " · " + t.album : ""}` : "");
  setText($("#np-pos"), fmt(t.position_ms));
  setText($("#np-dur"), fmt(t.duration_ms));
  const pct = (t.duration_ms && t.position_ms)
    ? Math.min(100, (t.position_ms / t.duration_ms) * 100)
    : 0;
  $("#np-fill").style.width = pct + "%";

  const src = state?.sources?.available?.find(s => s.name === a);
  const playing = src?.playback_state === "playing";
  $("#t-play").textContent = playing ? "⏸" : "▶";

  const art = $("#np-art");
  if (t.artwork_url) {
    art.classList.add("has-artwork");
    art.style.backgroundImage = `url("${t.artwork_url}")`;
  } else {
    art.classList.remove("has-artwork");
    art.style.backgroundImage = "";
  }
}

function sourceDetailLine(s) {
  if (s.name === "local_files" && s.details?.track_count != null) {
    const n = s.details.track_count;
    return `${n} track${n === 1 ? "" : "s"} indexed`;
  }
  if (s.name === "roon") return roonPanel.sourceDetailLine();
  return null;
}

function renderSources() {
  const wrap = $("#sources");
  const available = state?.sources?.available || [];
  const active = state?.sources?.active;
  const roonSig = roonPanel.signature();
  const sig = available.map(s =>
    `${s.name}:${s.playback_state}:${s.auth_state}:${s.details?.track_count ?? ""}:${s.name===active}`
  ).join("|") + "|" + roonSig;
  if (wrap.dataset.sig === sig) return;
  wrap.dataset.sig = sig;

  wrap.innerHTML = "";
  for (const s of available) {
    const tile = document.createElement("button");
    tile.className = "source" + (s.name === active ? " active" : "");
    tile.type = "button";
    const stateCls = s.auth_state === "needs_auth" ? "warn"
                   : s.auth_state === "error"       ? "err" : "";
    const detail = sourceDetailLine(s);
    const showNote = (s.auth_state === "needs_auth" || s.auth_state === "error") && s.auth_instructions;
    tile.innerHTML = `
      <div class="name">${s.display_name}</div>
      <div class="state ${stateCls}">${s.playback_state}${s.auth_state !== "none_required" ? " - " + s.auth_state.replace("_", " ") : ""}${detail ? " - " + detail : ""}</div>
      ${showNote ? `<div class="auth-note">${s.auth_instructions}</div>` : ""}
    `;
    tile.addEventListener("click", async () => {
      try { await api.send("/api/source/switch", { source: s.name }); }
      catch (e) { toast(e.message, true); }
    });
    wrap.appendChild(tile);
  }

  // Cast box only makes sense while YT is registered.
  $("#yt-cast-card").hidden = !available.some(s => s.name === "youtube_music");
}

let volDirty = false;
function volumeSliderMax() {
  return (cfg?.audio?.allow_volume_over_100 ?? state?.audio?.allow_volume_over_100) ? 2 : 1;
}
function renderOutput() {
  const gain = state?.audio?.output_gain ?? 0;
  const slider = $("#vol");
  slider.max = String(volumeSliderMax());
  if (!volDirty) {
    if (Math.abs(parseFloat(slider.value) - gain) > 0.005) slider.value = gain;
    $("#vol-out").value = Math.round(gain * 100) + "%";
  }
}

const SCHEMA = [
  ["general", "General", [
    ["port",            "Port",            "number", 1, 65535],
    ["ring_buffer_mb",  "Ring buffer (MB)","number", 1, 64],
    ["default_source",  "Default source",  "text"],
    ["fallback_source", "Fallback source", "text"],
  ]],
  ["local_files", "Local files", [
    ["enabled",     "Enabled",        "checkbox"],
    ["music_dir",   "Music directory","text"],
    ["recursive",   "Scan subfolders","checkbox"],
    ["shuffle",     "Shuffle",        "checkbox"],
  ]],
  ["youtube_music", "YouTube Music", [
    ["enabled",          "Enabled",                "checkbox"],
    ["cookies_path",     "cookies.txt (optional)", "text"],
    ["yt_dlp_path",      "yt-dlp path (optional)", "text"],
    ["ffmpeg_path",      "ffmpeg path (optional)", "text"],
    ["default_playlist", "Default playlist URL",   "text"],
    ["shuffle",          "Shuffle",                "checkbox"],
  ]],
  ["roon", "Roon", [
    ["enabled",             "Enabled",                "checkbox"],
    ["node_path",           "Node path (optional)",   "text"],
    ["bridge_path",         "Bridge script path",     "text"],
    ["selected_zone_id",    "Selected zone ID",       "text"],
    ["selected_output_id",  "Selected output ID",     "text"],
    ["render_loopback_endpoint_id",   "Loopback endpoint ID",   "text"],
    ["render_loopback_endpoint_name", "Loopback endpoint name", "text"],
    ["control_volume",      "Control Roon volume",    "checkbox"],
    ["auto_start_bridge",   "Auto-start sidecar",     "checkbox"],
    ["auto_reconnect",      "Auto-reconnect",         "checkbox"],
    ["latency_ms",          "Capture latency (ms)",   "number", 50, 2000],
    ["metadata_poll_ms",    "Metadata poll (ms)",     "number", 250, 5000],
  ]],
  ["audio", "Audio", [
    ["output_gain", "Output gain", "number", 0, "volumeSliderMax", 0.01],
    ["allow_volume_over_100", "Allow volume over 100%", "checkbox"],
  ]],
];

function field(section, [key, label, type, min, max, step]) {
  const id = `f-${section}-${key}`;
  const cur = cfg?.[section]?.[key];
  if (type === "checkbox") {
    return `<div class="field checkbox">
      <input type="checkbox" id="${id}" data-section="${section}" data-key="${key}" ${cur ? "checked" : ""}>
      <label for="${id}">${label}</label>
    </div>`;
  }
  const maxValue = max === "volumeSliderMax" ? volumeSliderMax() : max;
  const attrs = type === "number"
    ? ` min="${min ?? ''}" max="${maxValue ?? ''}" step="${step ?? 1}"`
    : "";
  return `<div class="field">
    <label for="${id}">${label}</label>
    <input id="${id}" type="${type}" data-section="${section}" data-key="${key}"${attrs} value="${cur ?? ''}">
  </div>`;
}

function renderSettings() {
  $("#settings-form").innerHTML = SCHEMA.map(([sec, title, fields]) =>
    `<fieldset><legend>${title}</legend>${fields.map(f => field(sec, f)).join("")}${sec === "roon" ? '<div id="roon-settings-wizard"></div>' : ""}</fieldset>`
  ).join("");
  roonPanel.renderPanel();
}

function collectSettings() {
  const patch = {};
  $$("#settings-form [data-section]").forEach(el => {
    const sec = el.dataset.section;
    const key = el.dataset.key;
    (patch[sec] ??= {});
    if (el.type === "checkbox")    patch[sec][key] = el.checked;
    else if (el.type === "number") patch[sec][key] = parseFloat(el.value);
    else                           patch[sec][key] = el.value;
  });
  return patch;
}

function openDrawer() {
  $("#drawer").classList.add("open");
  $("#scrim").hidden = false;
  $("#drawer").setAttribute("aria-hidden", "false");
}
function closeDrawer() {
  $("#drawer").classList.remove("open");
  $("#scrim").hidden = true;
  $("#drawer").setAttribute("aria-hidden", "true");
}

async function transport(action) {
  const src = state?.sources?.active;
  if (!src) return;
  // Centre button is a smart play/pause toggle.
  if (action === "play") {
    const s = state.sources.available.find(x => x.name === src);
    if (s?.playback_state === "playing") action = "pause";
  }
  try { await api.send(`/api/source/${src}/${action}`); }
  catch (e) { toast(e.message, true); }
}

function wire() {
  $("#t-play").onclick = () => transport("play");
  $("#t-next").onclick = () => transport("next");
  $("#t-prev").onclick = () => transport("previous");

  const vol = $("#vol");
  vol.addEventListener("input", () => {
    volDirty = true;
    $("#vol-out").value = Math.round(parseFloat(vol.value) * 100) + "%";
  });
  vol.addEventListener("change", async () => {
    try { await api.send("/api/options", { output_gain: parseFloat(vol.value) }); }
    catch (e) { toast(e.message, true); }
    setTimeout(() => { volDirty = false; }, 400);
  });

  $("#yt-cast").addEventListener("submit", async e => {
    e.preventDefault();
    const url = $("#yt-url").value.trim();
    if (!url) return;
    try {
      await api.send("/api/source/youtube_music/cast", { url });
      $("#yt-url").value = "";
      toast("Casting...");
    } catch (err) { toast(err.message, true); }
  });

  roonPanel.wire();

  $("#yt-shuffle").addEventListener("click", async () => {
    const yt = state?.sources?.available?.find(s => s.name === "youtube_music");
    if (!yt) return;
    const shuffle = !yt.details?.shuffle;
    try {
      await api.send("/api/source/youtube_music/shuffle", { shuffle });
      toast(shuffle ? "Shuffle on" : "Shuffle off");
    } catch (err) { toast(err.message, true); }
  });

  $("#open-settings").onclick  = async () => { cfg = await api.get("/api/config"); renderSettings(); openDrawer(); void roonPanel.refresh(true); };
  $("#close-settings").onclick = closeDrawer;
  $("#scrim").onclick          = closeDrawer;
  $("#save-config").onclick    = async () => {
    try {
      cfg = await api.send("/api/config", collectSettings(), "PUT");
      toast("Saved");
      closeDrawer();
      await roonPanel.refresh(true);
    } catch (e) { toast(e.message, true); }
  };
  $("#reload-config").onclick  = async () => {
    cfg = await api.send("/api/config/reload");
    renderSettings();
    toast("Reloaded from disk");
  };
}

// SSE if available, polling fallback otherwise.
function connect() {
  let es;
  try {
    es = new EventSource("/api/events");
    es.onmessage = e => { state = JSON.parse(e.data); render(); };
    es.onerror   = () => { es.close(); setTimeout(poll, 1000); };
  } catch { poll(); }
}
async function poll() {
  try { state = await api.get("/api/state"); render(); }
  catch { /* keep last state */ }
  setTimeout(poll, 1000);
}

function render() {
  renderStatus();
  renderNowPlaying();
  renderSources();
  roonPanel.renderPanel();
  renderOutput();

  const yt = state?.sources?.available?.find(s => s.name === "youtube_music");
  const shuffleBtn = $("#yt-shuffle");
  if (shuffleBtn) {
    shuffleBtn.classList.toggle("active", !!yt?.details?.shuffle);
  }
  void roonPanel.refresh();
}

wire();
connect();
