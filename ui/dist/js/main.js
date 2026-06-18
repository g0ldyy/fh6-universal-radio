import { $ } from "./dom.js";
import { api } from "./api.js";
import { connect } from "./events.js";
import { icons } from "./icons.js";
import { toast } from "./toast.js";
import { renderStatus } from "./render/status.js";
import { renderNowPlaying } from "./render/nowPlaying.js";
import { renderSources } from "./render/sources.js";
import { createOutput } from "./render/output.js";
import { renderSettings, collectSettings } from "./render/settings.js";
import { createDeps } from "./render/deps.js";
import { createExternalAudio } from "./render/externalAudio.js";
import { createLocalFiles } from "./render/localFiles.js";
import { createOnlineRadio } from "./render/onlineRadio.js";

let state = null;
let cfg = null;

const refs = {
  status: $("#status"),
  np: {
    art: $("#np-art"),
    backdrop: $("#np-backdrop"),
    img: $("#np-art-img"),
    title: $("#np-title"),
    artist: $("#np-artist"),
    fill: $("#np-fill"),
    pos: $("#np-pos"),
    dur: $("#np-dur"),
    play: $("#t-play"),
  },
  sources: $("#sources"),
  sourceCard: $("#source-card"),
  outputCard: $("#output-card"),
  ytCard: $("#yt-cast-card"),
  jfCard: $("#jf-cast-card"),
  koelCard: $("#koel-cast-card"),
  ytShuffle: $("#yt-shuffle"),
  drawer: $("#drawer"),
  scrim: $("#scrim"),
  form: $("#settings-form"),
};

$("#brand-mark").innerHTML = icons.broadcast;
$("#open-settings").innerHTML = icons.gear;
$("#close-settings").innerHTML = icons.close;
$("#t-prev").innerHTML = icons.prev;
$("#t-next").innerHTML = icons.next;
$("#yt-shuffle").innerHTML = icons.shuffle;

const mainEl = $("main");

const renderOutput = createOutput($("#vol"), $("#vol-out"), async gain => {
  try {
    await api.setGain(gain);
  } catch (e) {
    toast(e.message, true);
  }
});

const deps = createDeps(mainEl);

const externalAudio = createExternalAudio(mainEl, {
  getState: () => state,
  getConfig: () => cfg,
  onSaved: async patch => {
    cfg = { ...cfg, external_audio: { ...(cfg?.external_audio || {}), ...patch } };
    state = await api.getState().catch(() => state);
    render();
  },
});

const localFiles = createLocalFiles(mainEl, {
  getState: () => state,
  getConfig: () => cfg,
  onSaved: async () => {
    cfg = await api.getConfig().catch(() => cfg);
    state = await api.getState().catch(() => state);
    render();
  },
});

const onlineRadio = createOnlineRadio(mainEl, {
  getState: () => state,
  getConfig: () => cfg,
  onSaved: async () => {
    cfg = await api.getConfig().catch(() => cfg);
    state = await api.getState().catch(() => state);
    render();
  },
});

async function switchSource(name) {
  try {
    await api.switchSource(name);
  } catch (e) {
    toast(e.message, true);
  }
}

async function transport(action) {
  const source = state?.sources?.active;
  if (!source) return;
  let act = action;
  if (act === "play") {
    const s = state.sources.available.find(x => x.name === source);
    if (s?.playback_state === "playing") act = "pause";
  }
  try {
    await api.transport(source, act);
  } catch (e) {
    toast(e.message, true);
  }
}

const background = [$("header"), mainEl, $(".credits")];
let lastFocus = null;

function openDrawer() {
  lastFocus = document.activeElement;
  refs.drawer.classList.add("open");
  refs.drawer.inert = false;
  refs.scrim.hidden = false;
  background.forEach(n => n && (n.inert = true));
  $("#close-settings").focus();
}

function closeDrawer() {
  if (!refs.drawer.classList.contains("open")) return;
  refs.drawer.classList.remove("open");
  refs.drawer.inert = true;
  refs.scrim.hidden = true;
  background.forEach(n => n && (n.inert = false));
  lastFocus?.focus?.();
}

function render() {
  if (!state) return;
  renderStatus(refs.status, state);
  renderNowPlaying(refs.np, state);
  renderSources(refs.sources, state, cfg, switchSource);
  renderOutput(state);
  externalAudio.render();
  localFiles.render();
  onlineRadio.render();

  refs.sourceCard.hidden = false;
  refs.outputCard.hidden = !state.sources?.active;

  const available = state.sources?.available || [];
  const active = state.sources?.active;
  // Source-specific cards only show while that source is on air.
  refs.ytCard.hidden = active !== "youtube_music";
  refs.jfCard.hidden = active !== "jellyfin";
  refs.koelCard.hidden = active !== "koel";
  const shuffleOn = !!available.find(s => s.name === "youtube_music")?.details?.shuffle;
  refs.ytShuffle.classList.toggle("toggled", shuffleOn);
  refs.ytShuffle.setAttribute("aria-pressed", String(shuffleOn));
}

$("#t-play").addEventListener("click", () => transport("play"));
$("#t-next").addEventListener("click", () => transport("next"));
$("#t-prev").addEventListener("click", () => transport("previous"));

$("#yt-cast").addEventListener("submit", async e => {
  e.preventDefault();
  const url = $("#yt-url").value.trim();
  if (!url) return;
  try {
    await api.castYoutube(url);
    $("#yt-url").value = "";
    toast("Casting…");
  } catch (err) {
    toast(err.message, true);
  }
});

$("#yt-shuffle").addEventListener("click", async () => {
  const yt = state?.sources?.available?.find(s => s.name === "youtube_music");
  if (!yt) return;
  const shuffle = !yt.details?.shuffle;
  try {
    await api.shuffleYoutube(shuffle);
    toast(shuffle ? "Shuffle on" : "Shuffle off");
  } catch (err) {
    toast(err.message, true);
  }
});

let koelItems = [];

function populateKoelDatalist(items) {
  const dl = $("#koel-datalist");
  dl.innerHTML = "";
  items.forEach(item => {
    const opt = document.createElement("option");
    opt.value = item.name;
    dl.appendChild(opt);
  });
}

function koelLookupId(name) {
  const match = koelItems.find(i => i.name === name);
  return match ? match.id : "";
}

$("#koel-source-type").addEventListener("change", async () => {
  const type = $("#koel-source-type").value;
  const input = $("#koel-source-id");
  if (type === "favorites" || type === "random") {
    input.disabled = true;
    input.value = "";
    input.placeholder = "—";
    koelItems = [];
    populateKoelDatalist([]);
    return;
  }
  input.disabled = false;
  input.value = "";
  input.placeholder = "Loading…";
  try {
    const data = await api.browseKoel(type === "playlist" ? "playlists" : type + "s");
    koelItems = data.items || [];
    populateKoelDatalist(koelItems);
    input.placeholder = "Search and select…";
  } catch (err) {
    input.placeholder = "Error loading";
    toast(err.message, true);
  }
});

let koelSearchTimer = null;

$("#koel-source-id").addEventListener("input", () => {
  const type = $("#koel-source-type").value;
  if (type === "favorites" || type === "random") return;
  clearTimeout(koelSearchTimer);
  const q = $("#koel-source-id").value.trim();
  koelSearchTimer = setTimeout(async () => {
    const browseType = type === "playlist" ? "playlists" : type + "s";
    try {
      const suffix = q ? "?q=" + encodeURIComponent(q) : "";
      const data = await api.browseKoel(browseType + suffix);
      koelItems = data.items || [];
      populateKoelDatalist(koelItems);
    } catch (_) { /* keep previous list on error */ }
  }, 300);
});

$("#koel-cast").addEventListener("submit", async e => {
  e.preventDefault();
  const sourceType = $("#koel-source-type").value;
  const input = $("#koel-source-id");
  const sourceId = input.disabled ? "" : koelLookupId(input.value.trim());
  if (!input.disabled && !sourceId) {
    toast("Pick a valid item from the list", true);
    return;
  }
  try {
    await api.castKoel(sourceType, sourceId);
    toast("Playing…");
  } catch (err) {
    toast(err.message, true);
  }
});

$("#jf-cast").addEventListener("submit", async e => {
  e.preventDefault();
  const playlistId = $("#jf-url").value.trim();
  if (!playlistId) return;
  try {
    await api.castJellyfin(playlistId);
    $("#jf-url").value = "";
    toast("Playing playlist…");
  } catch (err) {
    toast(err.message, true);
  }
});

$("#open-settings").addEventListener("click", async () => {
  try {
    cfg = await api.getConfig();
  } catch (e) {
    toast(e.message, true);
    return;
  }
  renderSettings(refs.form, cfg);
  openDrawer();
});

$("#close-settings").addEventListener("click", closeDrawer);
refs.scrim.addEventListener("click", closeDrawer);
document.addEventListener("keydown", e => {
  if (e.key === "Escape") closeDrawer();
});

$("#save-config").addEventListener("click", async () => {
  try {
    cfg = await api.putConfig(collectSettings(refs.form));
    externalAudio.invalidate();
    localFiles.invalidate();
    onlineRadio.invalidate();
    state = await api.getState().catch(() => state);
    render();
    toast("Saved");
    closeDrawer();
  } catch (e) {
    toast(e.message, true);
  }
});

$("#reload-config").addEventListener("click", async () => {
  try {
    cfg = await api.reloadConfig();
    externalAudio.invalidate();
    localFiles.invalidate();
    onlineRadio.invalidate();
    renderSettings(refs.form, cfg);
    render();
    toast("Reloaded from disk");
  } catch (e) {
    toast(e.message, true);
  }
});

async function boot() {
  try {
    cfg = await api.getConfig();
  } catch {
    cfg = {};
  }
  connect(next => {
    state = next;
    render();
  });
  deps.start();
}

boot();
