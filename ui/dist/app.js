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

function renderStatus() {
  const ok = state?.game?.attached;
  const sub = $("#status");
  sub.className = "subtitle " + (ok ? "ok" : "err");
  sub.textContent = ok ? "connected" : "bridge offline";
}

function renderNowPlaying() {
  const t = state?.track || {};
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
}

function sourceDetailLine(s) {
  if (s.name === "local_files" && s.details?.track_count != null) {
    const n = s.details.track_count;
    return `${n} track${n === 1 ? "" : "s"} indexed`;
  }
  return null;
}

function renderSources() {
  const wrap = $("#sources");
  const allAvailable = state?.sources?.available || [];
  const externalAudioEnabled = !!cfg?.external_audio?.enabled;
  const available = allAvailable.filter(s => s.name !== "external_audio" || externalAudioEnabled);
  const active = state?.sources?.active;
  const sig = available.map(s =>
    `${s.name}:${s.playback_state}:${s.auth_state}:${s.details?.track_count ?? ""}:${s.name===active}`
  ).join("|");
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

  // Cast box only makes sense while Jellyfin is registered.
  $("#jf-cast-card").hidden = !available.some(s => s.name === "jellyfin");

  renderExternalAudioCard();
}

let extDevices = [];
let extEndpoint = "";
let extMediaSessions = [];
let extMediaSessionId = "";
let extMediaSessionsAvailable = false;
let extLoaded = false;
let extLoading = false;

function ensureExternalAudioCard() {
  let card = $("#external-audio-card");
  if (card) return card;

  card = document.createElement("section");
  card.id = "external-audio-card";
  card.className = "card";
  card.hidden = true;
  card.innerHTML = `
    <h2>External Audio</h2>
    <p class="muted">Select the Windows playback device used for audio capture and the media session used for metadata and next/previous commands.</p>
    <label class="external-audio-label" for="external-audio-device">Capture device</label>
    <div class="row external-audio-row">
      <select id="external-audio-device" aria-label="External Audio capture device"></select>
      <button id="external-audio-refresh" class="ghost" type="button">Refresh</button>
    </div>
    <label class="external-audio-label" for="external-audio-session">Media session</label>
    <div class="row external-audio-row">
      <select id="external-audio-session" aria-label="External Audio media session"></select>
      <button id="external-audio-save" class="primary" type="button">Save</button>
    </div>
    <p id="external-audio-hint" class="muted"></p>
  `;

  const sources = $("#sources");
  const sourceCard = sources?.closest?.(".card") || sources?.parentElement;
  if (sourceCard?.parentElement) sourceCard.insertAdjacentElement("afterend", card);
  else document.querySelector("main")?.appendChild(card);

  $("#external-audio-refresh", card).addEventListener("click", async () => {
    await loadExternalAudioDevices(true);
  });

  $("#external-audio-save", card).addEventListener("click", async () => {
    const deviceSelect = $("#external-audio-device", card);
    const sessionSelect = $("#external-audio-session", card);
    try {
      const enabled = !!cfg?.external_audio?.enabled;
      const r = await api.send("/api/external_audio/config", {
        enabled,
        endpoint_id: deviceSelect.value,
        media_session_id: sessionSelect.value,
      }, "PUT");
      cfg = { ...(cfg || {}), external_audio: {
        ...(cfg?.external_audio || {}),
        enabled: !!r.enabled,
        endpoint_id: r.endpoint_id ?? deviceSelect.value,
        media_session_id: r.media_session_id ?? sessionSelect.value,
      } };
      extEndpoint = r.endpoint_id ?? deviceSelect.value;
      extMediaSessionId = r.media_session_id ?? sessionSelect.value;
      extLoaded = false;
      state = await api.get("/api/state");
      await loadExternalAudioDevices(true);
      render();
      toast("External Audio settings saved");
    } catch (e) {
      toast(e.message, true);
    }
  });

  return card;
}

async function loadExternalAudioDevices(force = false) {
  if ((extLoaded && !force) || extLoading) return;
  extLoading = true;
  try {
    const r = await api.get("/api/external_audio/devices");
    extDevices = Array.isArray(r.devices) ? r.devices : [];
    extEndpoint = r.endpoint_id || "";
    extMediaSessions = Array.isArray(r.media_sessions) ? r.media_sessions : [];
    extMediaSessionId = r.media_session_id || "";
    extMediaSessionsAvailable = !!r.media_sessions_available;
    extLoaded = true;
  } catch {
    extDevices = [];
    extMediaSessions = [];
    extMediaSessionsAvailable = false;
    extLoaded = false;
  } finally {
    extLoading = false;
  }
  renderExternalAudioCard();
}

function renderExternalAudioCard() {
  const card = ensureExternalAudioCard();
  const available = state?.sources?.available?.some(s => s.name === "external_audio");
  const enabled = !!cfg?.external_audio?.enabled;
  card.hidden = !enabled;
  if (!enabled) return;

  loadExternalAudioDevices();

  const deviceSelect = $("#external-audio-device", card);
  const deviceSig = `${extEndpoint}|${extDevices.map(d => `${d.id}:${d.name}:${d.is_default}`).join("|")}`;
  if (deviceSelect.dataset.sig !== deviceSig) {
    deviceSelect.dataset.sig = deviceSig;
    deviceSelect.innerHTML = "";
    deviceSelect.add(new Option("Default Windows playback device", "", false, extEndpoint === ""));
    for (const d of extDevices) {
      const label = `${d.name || d.id}${d.is_default ? " (current default)" : ""}`;
      deviceSelect.add(new Option(label, d.id, false, extEndpoint === d.id));
    }
  }

  const sessionSelect = $("#external-audio-session", card);
  const sessionSig = `${extMediaSessionId}|${extMediaSessionsAvailable}|${extMediaSessions.map(s => `${s.id}:${s.name}:${s.is_current}`).join("|")}`;
  if (sessionSelect.dataset.sig !== sessionSig) {
    sessionSelect.dataset.sig = sessionSig;
    sessionSelect.innerHTML = "";
    sessionSelect.add(new Option("Current Windows media session", "", false, extMediaSessionId === ""));
    for (const s of extMediaSessions) {
      const label = `${s.name || s.id}${s.is_current ? " (current)" : ""}`;
      sessionSelect.add(new Option(label, s.id, false, extMediaSessionId === s.id));
    }
    if (!extMediaSessionsAvailable) {
      sessionSelect.add(new Option("Media session API is not available in this build", "", false, true));
      sessionSelect.disabled = true;
    } else {
      sessionSelect.disabled = false;
    }
  }

  const active = state?.sources?.active === "external_audio";
  $("#external-audio-hint", card).textContent = !available
    ? "Enabled, but the source hasn't registered yet."
    : active
      ? "Active and on air."
      : "Ready. Switch to External Audio in Sources to go on air.";
}

let volDirty = false;
function renderOutput() {
  const gain = state?.audio?.output_gain ?? 0;
  if (!volDirty) {
    const slider = $("#vol");
    if (Math.abs(parseFloat(slider.value) - gain) > 0.005) slider.value = gain;
    $("#vol-out").value = Math.round(gain * 100) + "%";
  }
}

const EQ_BAND_LABELS = ["60 Hz", "250 Hz", "1 kHz", "4 kHz", "12 kHz"];

const SCHEMA = [
  ["general", "General", [
    ["port",            "Port",                   "number", 1, 65535],
    ["ring_buffer_mb",  "Ring buffer (MB)",       "number", 1, 64],
    ["default_source",  "Default source",         "text"],
    ["fallback_source", "Fallback source",        "text"],
    ["ffmpeg_path",     "ffmpeg path (optional)", "text"],
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
    ["default_playlist", "Default playlist URL",   "text"],
    ["shuffle",          "Shuffle",                "checkbox"],
  ]],
  ["jellyfin", "Jellyfin", [
    ["enabled",          "Enabled",          "checkbox"],
    ["server_url",       "Server URL",       "text"],
    ["user_id",          "User ID",          "text"],
    ["api_key",          "API Key",          "text"],
    ["default_playlist", "Default Playlist", "text"],
    ["use_favorites",    "Use Favorites",    "checkbox"],
    ["shuffle",          "Shuffle",          "checkbox"],
  ]],
  ["external_audio", "External Audio", [
    ["enabled",          "Enabled",          "checkbox"],
  ]],
  ["audio", "Audio", [
    ["output_gain", "Output gain", "number", 0, 1, 0.01],
  ]],
  ["playback", "Playback", [
    ["race_start_playback",  "Race start",                "select",   ["next", "restart", "ignore"]],
    ["quick_station_skip",   "Quick station skip",        "checkbox"],
    ["volume_normalization", "Normalize loudness",        "checkbox"],
    ["equalizer_enabled",    "Equalizer",                 "checkbox"],
    ["equalizer_bands",      "Equalizer bands",           "bands"],
    ["force_stereo_audio",   "Force stereo audio",        "checkbox"],
    ["prebuffer_next_track", "Pre-buffer next track",     "checkbox"],
  ]],
];

function field(section, [key, label, type, a, b, c]) {
  const id  = `f-${section}-${key}`;
  const cur = cfg?.[section]?.[key];
  if (type === "checkbox") {
    return `<div class="field checkbox">
      <input type="checkbox" id="${id}" data-section="${section}" data-key="${key}" ${cur ? "checked" : ""}>
      <label for="${id}">${label}</label>
    </div>`;
  }
  if (type === "select") {
    const opts = (a || []).map(v =>
      `<option value="${v}" ${cur === v ? "selected" : ""}>${v}</option>`).join("");
    return `<div class="field">
      <label for="${id}">${label}</label>
      <select id="${id}" data-section="${section}" data-key="${key}">${opts}</select>
    </div>`;
  }
  if (type === "bands") {
    const vals = Array.isArray(cur) ? cur : [0, 0, 0, 0, 0];
    const rows = EQ_BAND_LABELS.map((lbl, i) => `
      <div class="band">
        <span class="band-label">${lbl}</span>
        <input type="range" min="-6" max="6" step="0.5" value="${vals[i] ?? 0}"
               data-section="${section}" data-key="${key}" data-index="${i}">
        <output>${(vals[i] ?? 0).toFixed(1)} dB</output>
      </div>`).join("");
    return `<div class="field bands"><label>${label}</label>${rows}</div>`;
  }
  const attrs = type === "number"
    ? ` min="${a ?? ''}" max="${b ?? ''}" step="${c ?? 1}"`
    : "";
  return `<div class="field">
    <label for="${id}">${label}</label>
    <input id="${id}" type="${type}" data-section="${section}" data-key="${key}"${attrs} value="${cur ?? ''}">
  </div>`;
}

function renderSettings() {
  const form = $("#settings-form");
  form.innerHTML = SCHEMA.map(([sec, title, fields]) =>
    `<fieldset><legend>${title}</legend>${fields.map(f => field(sec, f)).join("")}</fieldset>`
  ).join("");
  // Live "X.X dB" readout next to each EQ slider.
  $$(".field.bands input[type='range']", form).forEach(r => {
    const out = r.nextElementSibling;
    r.addEventListener("input", () => { out.textContent = `${parseFloat(r.value).toFixed(1)} dB`; });
  });
}

function collectSettings() {
  const patch = {};
  $$("#settings-form [data-section]").forEach(el => {
    const sec = el.dataset.section;
    const key = el.dataset.key;
    (patch[sec] ??= {});
    if (el.dataset.index !== undefined) {
      const arr = (patch[sec][key] ??= []);
      arr[parseInt(el.dataset.index, 10)] = parseFloat(el.value);
      return;
    }
    if (el.type === "checkbox")     patch[sec][key] = el.checked;
    else if (el.type === "number" ||
             el.type === "range")   patch[sec][key] = parseFloat(el.value);
    else                            patch[sec][key] = el.value;
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

  $("#yt-shuffle").addEventListener("click", async () => {
    const yt = state?.sources?.available?.find(s => s.name === "youtube_music");
    if (!yt) return;
    const shuffle = !yt.details?.shuffle;
    try {
      await api.send("/api/source/youtube_music/shuffle", { shuffle });
      toast(shuffle ? "Shuffle on" : "Shuffle off");
    } catch (err) { toast(err.message, true); }
  });

  $("#jf-cast").addEventListener("submit", async e => {
    e.preventDefault();
    const playlist_id = $("#jf-url").value.trim();
    if (!playlist_id) return;
    try {
      await api.send("/api/source/jellyfin/cast", { playlist_id });
      $("#jf-url").value = "";
      toast("Playing playlist...");
    } catch (err) { toast(err.message, true); }
  });

  $("#open-settings").onclick  = async () => { cfg = await api.get("/api/config"); renderSettings(); openDrawer(); };
  $("#close-settings").onclick = closeDrawer;
  $("#scrim").onclick          = closeDrawer;
  $("#save-config").onclick    = async () => {
    try {
      cfg = await api.send("/api/config", collectSettings(), "PUT");
      extLoaded = false;
      state = await api.get("/api/state");
      render();
      toast("Saved");
      closeDrawer();
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
  renderOutput();

  const yt = state?.sources?.available?.find(s => s.name === "youtube_music");
  const shuffleBtn = $("#yt-shuffle");
  if (shuffleBtn) {
    shuffleBtn.classList.toggle("active", !!yt?.details?.shuffle);
  }
}

async function startDashboard() {
  try { cfg = await api.get("/api/config"); }
  catch { cfg = {}; }
  connect();
}

wire();
startDashboard();
