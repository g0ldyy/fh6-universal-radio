import { $, el } from "./dom.js";
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
  ytShuffle: $("#yt-shuffle"),
  orCard: $("#or-cast-card"),
  orForm: $("#or-cast"),
  orUrl: $("#or-url"),
  orStationView: $("#or-station-view"),
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

  refs.sourceCard.hidden = false;
  refs.outputCard.hidden = !state.sources?.active;

  const available = state.sources?.available || [];
  refs.ytCard.hidden = !available.some(s => s.name === "youtube_music");
  refs.jfCard.hidden = !available.some(s => s.name === "jellyfin");
  const shuffleOn = !!available.find(s => s.name === "youtube_music")?.details?.shuffle;
  refs.ytShuffle.classList.toggle("toggled", shuffleOn);
  refs.ytShuffle.setAttribute("aria-pressed", String(shuffleOn));

  refs.orCard.hidden = !available.some(s => s.name === "online_radio");

  if (cfg?.online_radio?.stations && refs.orStationView) {
    refs.orStationView.innerHTML = "";
    cfg.online_radio.stations.forEach((station, index) => {
      const row = el("div", { class: "row", style: "justify-content: space-between; padding: 6px 12px; background: var(--surface-raise); border-radius: var(--radius-md); border: 1px solid var(--line);" }, [
        el("span", { style: "font-weight: 500;" }, station.name || `Station ${index + 1}`),
        el("div", { class: "row", style: "gap: 8px;" }, [
          el("button", { type: "button", class: "btn ghost", html: "Tune" }),
          el("button", { type: "button", class: "btn ghost", html: "Delete" })
        ])
      ]);
      
      const btns = row.querySelectorAll("button");
      btns[0].onclick = () => {
        api.castOnlineRadio(station.url);
        toast(`Tuning into ${station.name}`);
      };
      btns[1].onclick = async () => {
        if (!confirm(`Delete ${station.name}?`)) return;
        const newStations = cfg.online_radio.stations.filter((_, i) => i !== index);
        try {
          cfg = await api.putConfig({ online_radio: { stations: newStations } });
          render();
          toast("Station deleted");
        } catch(e) { toast(e.message, true); }
      };
      refs.orStationView.appendChild(row);
    });
  }
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

const orForm = document.getElementById("or-cast");
const orUrlInput = $("#or-url");

$("#or-cast").addEventListener("submit", async e => {
  e.preventDefault();
  const url = orUrlInput.value.trim();
  if (!url) return;
  try {
    await api.castOnlineRadio(url);
    orUrlInput.value = "";
    toast("Casting audio stream...");
  } catch (err) {
    toast(err.message, true);
  }
});

$("#or-save").addEventListener("click", async () => {
  const url = orUrlInput.value.trim();
  if (!url) return toast("Enter a URL first to save.", true);

  const name = prompt("Enter a name for this radio station:", "New Station");
  if (!name) return; // cancelled

  const updatedStations = [...(cfg?.online_radio?.stations || []), { name, url }];

  try {
    cfg = await api.putConfig({ online_radio: { stations: updatedStations } });
    render();
    toast("Station saved!");
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
