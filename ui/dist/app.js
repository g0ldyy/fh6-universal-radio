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

// ── Smooth progress interpolation ──────────────────────────────────────────
// Between server updates (~1 s), animate the fill in real time.
let _raf = null;
let _progressBase = { pos: 0, ts: 0, dur: 0 };

function stopProgressTick() {
  if (_raf) { cancelAnimationFrame(_raf); _raf = null; }
}

function startProgressTick() {
  stopProgressTick();
  const tick = () => {
    if (!_progressBase.dur) { _raf = null; return; }
    const pos = Math.min(_progressBase.pos + (Date.now() - _progressBase.ts), _progressBase.dur);
    const pct = (pos / _progressBase.dur) * 100;
    const fill = $("#np-fill");
    if (fill) fill.style.width = pct + "%";
    const posEl = $("#np-pos");
    const str = fmt(pos);
    if (posEl && posEl.textContent !== str) posEl.textContent = str;
    _raf = requestAnimationFrame(tick);
  };
  _raf = requestAnimationFrame(tick);
}

// ── Tab navigation ─────────────────────────────────────────────────────────
function switchTab(name) {
  $$(".tab").forEach(t => t.classList.toggle("active", t.dataset.tab === name));
  $$(".tab-panel").forEach(p => p.classList.toggle("hidden", p.id !== `panel-${name}`));
}

// ── Render functions ───────────────────────────────────────────────────────
function renderStatus() {
  const ok = state?.game?.attached;
  const el = $("#status");
  el.className = "status-pill " + (ok ? "ok" : "err");
  el.textContent = ok ? "connected" : "bridge offline";
}

function renderNowPlaying() {
  const t   = state?.track || {};
  const a   = state?.sources?.active;
  const src = state?.sources?.available?.find(s => s.name === a);
  const playing = src?.playback_state === "playing";

  setText($("#np-title"),  t.title  || "Nothing playing");
  setText($("#np-artist"), t.artist ? `${t.artist}${t.album ? " · " + t.album : ""}` : "");

  // Source name pill above the track title.
  setText($("#np-source"), src?.display_name || "");

  // Album artwork — show real image when available, CSS vinyl ring otherwise.
  const art = $("#np-art");
  const artUrl = t.artwork_url || "";
  if (art.dataset.artUrl !== artUrl) {
    art.dataset.artUrl = artUrl;
    if (artUrl) {
      art.classList.add("has-art");
      art.innerHTML = `<img src="${artUrl}" alt="">`;
    } else {
      art.classList.remove("has-art");
      art.innerHTML = "";
    }
  }

  // Seekable bar — only interactive when the active source supports it.
  const bar = $("#np-bar");
  const canSeek = !!(src?.capabilities?.seek) && !!t.duration_ms;
  bar.classList.toggle("seekable", canSeek);

  // Progress — live RAF interpolation while playing, static update otherwise.
  const pos = t.position_ms || 0;
  const dur = t.duration_ms || 0;
  if (playing && dur) {
    _progressBase = { pos, ts: Date.now(), dur };
    startProgressTick();
  } else {
    stopProgressTick();
    const pct = dur ? Math.min(100, (pos / dur) * 100) : 0;
    $("#np-fill").style.width = pct + "%";
    setText($("#np-pos"), fmt(pos));
  }
  setText($("#np-dur"), fmt(dur));

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
    `${s.name}:${s.playback_state}:${s.auth_state}:${s.details?.track_count ?? ""}:${s.name===active}:${s.details?.shuffle ?? ""}`
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
    const detail   = sourceDetailLine(s);
    const showNote = (s.auth_state === "needs_auth" || s.auth_state === "error") && s.auth_instructions;
    const isPlaying = s.name === active && s.playback_state === "playing";

    const waveform = isPlaying
      ? `<span class="waveform" aria-hidden="true"><i></i><i></i><i></i><i></i></span>` : "";
    const shuffleBadge = s.name === "youtube_music" && s.details?.shuffle
      ? ` <span class="src-badge">shuffle</span>` : "";

    const stateText = `${s.playback_state}`
      + (s.auth_state !== "none_required" ? " · " + s.auth_state.replace("_", " ") : "")
      + (detail ? " · " + detail : "");

    tile.innerHTML = `
      <div class="name">${s.display_name}${waveform}${shuffleBadge}</div>
      <div class="state ${stateCls}">${stateText}</div>
      ${showNote ? `<div class="auth-note">${s.auth_instructions}</div>` : ""}
    `;
    tile.addEventListener("click", async () => {
      try { await api.send("/api/source/switch", { source: s.name }); }
      catch (e) { toast(e.message, true); }
    });
    wrap.appendChild(tile);
  }

  $("#yt-cast-card").hidden = !available.some(s => s.name === "youtube_music");
  $("#jf-cast-card").hidden = !available.some(s => s.name === "jellyfin");

  renderExternalAudioCard();
}

// ── External Audio card (dynamically injected into Sources tab) ────────────
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

  card = document.createElement("div");
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
    const deviceSelect  = $("#external-audio-device", card);
    const sessionSelect = $("#external-audio-session", card);
    try {
      const enabled = !!cfg?.external_audio?.enabled;
      const r = await api.send("/api/external_audio/config", {
        enabled,
        endpoint_id:      deviceSelect.value,
        media_session_id: sessionSelect.value,
      }, "PUT");
      cfg = { ...(cfg || {}), external_audio: {
        ...(cfg?.external_audio || {}),
        enabled:          !!r.enabled,
        endpoint_id:      r.endpoint_id      ?? deviceSelect.value,
        media_session_id: r.media_session_id ?? sessionSelect.value,
      } };
      extEndpoint      = r.endpoint_id      ?? deviceSelect.value;
      extMediaSessionId = r.media_session_id ?? sessionSelect.value;
      extLoaded = false;
      state = await api.get("/api/state");
      await loadExternalAudioDevices(true);
      render();
      toast("External Audio settings saved");
    } catch (e) { toast(e.message, true); }
  });

  return card;
}

async function loadExternalAudioDevices(force = false) {
  if ((extLoaded && !force) || extLoading) return;
  extLoading = true;
  try {
    const r = await api.get("/api/external_audio/devices");
    extDevices             = Array.isArray(r.devices)       ? r.devices       : [];
    extEndpoint            = r.endpoint_id || "";
    extMediaSessions       = Array.isArray(r.media_sessions) ? r.media_sessions : [];
    extMediaSessionId      = r.media_session_id || "";
    extMediaSessionsAvailable = !!r.media_sessions_available;
    extLoaded = true;
  } catch {
    extDevices = []; extMediaSessions = []; extMediaSessionsAvailable = false; extLoaded = false;
  } finally { extLoading = false; }
  renderExternalAudioCard();
}

function renderExternalAudioCard() {
  const card    = ensureExternalAudioCard();
  const enabled = !!cfg?.external_audio?.enabled;
  card.hidden   = !enabled;
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
    sessionSelect.disabled = !extMediaSessionsAvailable;
    if (!extMediaSessionsAvailable)
      sessionSelect.add(new Option("Media session API is not available in this build", "", false, true));
  }

  const active = state?.sources?.active === "external_audio";
  const available = state?.sources?.available?.some(s => s.name === "external_audio");
  $("#external-audio-hint", card).textContent = !available
    ? "Enabled, but the source hasn't registered yet."
    : active ? "Active and on air."
             : "Ready. Switch to External Audio in Sources to go on air.";
}

// ── Output ─────────────────────────────────────────────────────────────────
let volDirty = false;

function renderOutput() {
  const gain = state?.audio?.output_gain ?? 0;
  if (!volDirty) {
    const slider = $("#vol");
    if (Math.abs(parseFloat(slider.value) - gain) > 0.005) slider.value = gain;
    const pct = Math.round(gain * 100);
    $("#vol-out").value = pct + "%";
    setVolFill(slider, pct);
  }
}

function setVolFill(slider, pct) {
  slider.style.background =
    `linear-gradient(to right, var(--accent) ${pct}%, var(--line) ${pct}%)`;
}

function renderAudioStats() {
  const panel = $("#audio-stats");
  if (!panel) return;
  const audio = state?.audio || {};
  const rows = [
    { label: "DSP Mode",  value: audio.native_dsp_mode || "—" },
    { label: "Underruns", value: audio.underruns ?? "—", warn: (audio.underruns ?? 0) > 0 },
    { label: "Ring",      value: audio.ring_capacity ? `${audio.ring_avail}/${audio.ring_capacity}` : "—" },
    { label: "Channels",  value: audio.out_channels ?? "—" },
  ];
  const sig = rows.map(r => r.value).join("|");
  if (panel.dataset.sig === sig) return;
  panel.dataset.sig = sig;
  panel.innerHTML = rows.map(r =>
    `<div class="stat"><div class="label">${r.label}</div><div class="value${r.warn ? " warn" : ""}">${r.value}</div></div>`
  ).join("");
}

// ── Settings schema ────────────────────────────────────────────────────────
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
    ["enabled",   "Enabled",        "checkbox"],
    ["music_dir", "Music directory","text"],
    ["recursive", "Scan subfolders","checkbox"],
    ["shuffle",   "Shuffle",        "checkbox"],
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
    ["enabled", "Enabled", "checkbox"],
  ]],
  ["audio", "Audio", [
    ["output_gain", "Output gain", "number", 0, 1, 0.01],
  ]],
  ["playback", "Playback", [
    ["race_start_playback",  "Race start",            "select",   ["next", "restart", "ignore"]],
    ["quick_station_skip",   "Quick station skip",    "checkbox"],
    ["volume_normalization", "Normalize loudness",    "checkbox"],
    ["equalizer_enabled",    "Equalizer",             "checkbox"],
    ["equalizer_bands",      "Equalizer bands",       "bands"],
    ["force_stereo_audio",   "Force stereo audio",    "checkbox"],
    ["prebuffer_next_track", "Pre-buffer next track", "checkbox"],
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
  const attrs = type === "number" ? ` min="${a ?? ''}" max="${b ?? ''}" step="${c ?? 1}"` : "";
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
    if (el.type === "checkbox")                    patch[sec][key] = el.checked;
    else if (el.type === "number" || el.type === "range") patch[sec][key] = parseFloat(el.value);
    else                                           patch[sec][key] = el.value;
  });
  return patch;
}

// ── Drawer ─────────────────────────────────────────────────────────────────
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

// ── Transport ──────────────────────────────────────────────────────────────
async function transport(action) {
  const src = state?.sources?.active;
  if (!src) return;
  if (action === "play") {
    const s = state.sources.available.find(x => x.name === src);
    if (s?.playback_state === "playing") action = "pause";
  }
  try { await api.send(`/api/source/${src}/${action}`); }
  catch (e) { toast(e.message, true); }
}

// ── Wire up all interactivity ──────────────────────────────────────────────
function wire() {
  // Tab navigation
  $$(".tab").forEach(tab => tab.addEventListener("click", () => switchTab(tab.dataset.tab)));

  // Transport buttons
  $("#t-play").onclick = () => transport("play");
  $("#t-next").onclick = () => transport("next");
  $("#t-prev").onclick = () => transport("previous");

  // Seekable progress bar — click jumps to that position.
  $("#np-bar").addEventListener("click", async e => {
    const src = state?.sources?.active;
    if (!src) return;
    const s = state?.sources?.available?.find(x => x.name === src);
    if (!s?.capabilities?.seek || !state?.track?.duration_ms) return;
    const rect = e.currentTarget.getBoundingClientRect();
    const pct  = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
    const pos  = Math.floor(pct * state.track.duration_ms);
    _progressBase = { pos, ts: Date.now(), dur: state.track.duration_ms };
    $("#np-fill").style.width = (pct * 100) + "%";
    try { await api.send(`/api/source/${src}/seek`, { position_ms: pos }); }
    catch (err) { toast(err.message, true); }
  });

  // Volume slider with accent fill
  const vol = $("#vol");
  vol.addEventListener("input", () => {
    volDirty = true;
    const pct = Math.round(parseFloat(vol.value) * 100);
    $("#vol-out").value = pct + "%";
    setVolFill(vol, pct);
  });
  vol.addEventListener("change", async () => {
    try { await api.send("/api/options", { output_gain: parseFloat(vol.value) }); }
    catch (e) { toast(e.message, true); }
    setTimeout(() => { volDirty = false; }, 400);
  });

  // Keyboard shortcuts: Space = play/pause, ← = previous, → = next
  document.addEventListener("keydown", e => {
    if (e.target.matches("input, select, textarea, [contenteditable]")) return;
    if      (e.code === "Space")      { e.preventDefault(); transport("play"); }
    else if (e.code === "ArrowRight") { e.preventDefault(); transport("next"); }
    else if (e.code === "ArrowLeft")  { e.preventDefault(); transport("previous"); }
  });

  // YouTube Music cast
  $("#yt-cast").addEventListener("submit", async e => {
    e.preventDefault();
    const url = $("#yt-url").value.trim();
    if (!url) return;
    try { await api.send("/api/source/youtube_music/cast", { url }); $("#yt-url").value = ""; toast("Casting..."); }
    catch (err) { toast(err.message, true); }
  });

  $("#yt-shuffle").addEventListener("click", async () => {
    const yt = state?.sources?.available?.find(s => s.name === "youtube_music");
    if (!yt) return;
    const shuffle = !yt.details?.shuffle;
    try { await api.send("/api/source/youtube_music/shuffle", { shuffle }); toast(shuffle ? "Shuffle on" : "Shuffle off"); }
    catch (err) { toast(err.message, true); }
  });

  // Jellyfin cast
  $("#jf-cast").addEventListener("submit", async e => {
    e.preventDefault();
    const playlist_id = $("#jf-url").value.trim();
    if (!playlist_id) return;
    try { await api.send("/api/source/jellyfin/cast", { playlist_id }); $("#jf-url").value = ""; toast("Playing playlist..."); }
    catch (err) { toast(err.message, true); }
  });

  // Settings drawer
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
  $("#reload-config").onclick = async () => {
    cfg = await api.send("/api/config/reload");
    renderSettings();
    toast("Reloaded from disk");
  };
}

// ── SSE + polling ──────────────────────────────────────────────────────────
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

// ── Main render ────────────────────────────────────────────────────────────
function render() {
  renderStatus();
  renderNowPlaying();
  renderSources();
  renderOutput();
  renderAudioStats();

  const yt = state?.sources?.available?.find(s => s.name === "youtube_music");
  const shuffleBtn = $("#yt-shuffle");
  if (shuffleBtn) shuffleBtn.classList.toggle("active", !!yt?.details?.shuffle);
}

async function startDashboard() {
  try { cfg = await api.get("/api/config"); }
  catch { cfg = {}; }
  connect();
}

wire();
startDashboard();
