import { api } from "../api.js";
import { $, el } from "../dom.js";
import { toast } from "../toast.js";
import { icons } from "../icons.js";

function newStation(name) {
  return { name, playlist_id: "", use_favorites: false };
}

const fold = s => (s || "").normalize("NFD").replace(/\p{Diacritic}/gu, "").toLowerCase();

export function createJellyfin(main, ctx) {
  let stations = [];
  let activeStation = "";
  let selected = 0;
  let loaded = false;
  let queue = { cursor: 0, tracks: [] };
  let search = "";

  const stationSelect = el("select", { id: "jf-station", "aria-label": "Station preset" });
  const onAirBtn = el("button", { type: "button", class: "btn filled" }, "Set on air");
  const newBtn = el("button", { type: "button", class: "btn ghost" }, "New");
  const renameBtn = el("button", { type: "button", class: "btn ghost" }, "Rename");
  const deleteBtn = el("button", { type: "button", class: "btn ghost" }, "Delete");

  const idInput = el("input", { type: "text", class: "lf-path-input", placeholder: "Playlist ID", autocomplete: "off" });
  const favCheck = el("input", { type: "checkbox" });
  const castBtn = el("button", { type: "button", class: "btn ghost" }, "Cast");
  const saveBtn = el("button", { type: "button", class: "btn filled" }, "Save");

  const queueCount = el("span", { class: "muted" });
  const searchInput = el("input", { type: "text", placeholder: "Search the playlist…", autocomplete: "off" });
  const shuffleBtn = el("button", { type: "button", class: "icon-btn", "aria-label": "Toggle Shuffle", html: icons.shuffle });
  const trackList = el("ul", { class: "lf-tracklist" });

  const card = el("section", { class: "card", id: "jellyfin-card", hidden: true }, [
    el("h2", {}, "Jellyfin"),
    el("div", { class: "yt-stationbar row" }, [stationSelect, onAirBtn]),
    el("div", { class: "row yt-stationtools", style: "margin-bottom: 1.5rem;" }, [newBtn, renameBtn, deleteBtn]),
    el("div", { class: "lf-editor" }, [
      el("label", { class: "field-label" }, "Playlist ID"),
      idInput,
      el("label", { class: "field-label", style: "display: flex; align-items: center; gap: 0.5rem; margin-top: 0.5rem;" }, [
        favCheck, "Use Favorites (ignores Playlist ID)"
      ]),
      el("div", { class: "row lf-editor-foot", style: "margin-top: 1rem;" }, [castBtn, saveBtn]),
    ]),
    el("div", { class: "lf-queue" }, [
      el("div", { class: "lf-queue-head row" }, [
        el("label", { class: "field-label" }, "Queue"),
        queueCount,
        shuffleBtn,
      ]),
      searchInput,
      trackList,
    ]),
  ]);

  const sourcesCard = $("#sources", main)?.closest(".card");
  if (sourcesCard) sourcesCard.insertAdjacentElement("afterend", card);
  else main.append(card);

  const cur = () => stations[selected];

  async function load(force = false) {
    if (loaded && !force) return;
    try {
      const r = await api.getJellyfinStations();
      stations = Array.isArray(r.stations) && r.stations.length ? r.stations : [newStation("My Playlist")];
      activeStation = r.active_station || stations[0].name;
      selected = Math.max(0, stations.findIndex(s => s.name === activeStation));
      loaded = true;
    } catch {
      return;
    }
    renderEditor();
    loadQueue();
  }

  async function loadQueue() {
    try {
      queue = await api.getJellyfinQueue();
    } catch {
      queue = { cursor: 0, tracks: [] };
    }
    renderQueue();
  }

  function renderStations() {
    stationSelect.replaceChildren(
      ...stations.map((s, i) =>
        el("option", { value: String(i), selected: i === selected },
          s.name + (s.name === activeStation ? "  • on air" : "")),
      ),
    );
    deleteBtn.disabled = stations.length <= 1;
    onAirBtn.disabled = cur()?.name === activeStation;
    onAirBtn.textContent = cur()?.name === activeStation ? "On air" : "Set on air";
  }

  function renderEditor() {
    if (!cur()) return;
    renderStations();
    idInput.value = cur().playlist_id || "";
    favCheck.checked = !!cur().use_favorites;
  }

  function renderQueue() {
    const terms = fold(search).split(/\s+/).filter(Boolean);
    const rows = (queue.tracks || []).filter(t => {
      if (!terms.length) return true;
      const hay = fold(`${t.title || ""} ${t.artist || ""}`); 
      return terms.every(w => hay.includes(w));
    });
    queueCount.textContent = `${queue.tracks?.length || 0} tracks`;
    trackList.replaceChildren(
      ...rows.map(t => {
        const li = el("li", { class: "lf-track" + (t.index === queue.cursor ? " current" : "") }, [
          el("div", { class: "lf-track-main" }, [
            el("span", { class: "lf-track-title" }, t.title || "Unknown Track"),
          ]),
          t.artist ? el("span", { class: "lf-track-folder muted" }, t.artist) : null,
        ]);
        li.addEventListener("click", async () => {
          try {
            await api.playJellyfinIndex(t.index);
            queue.cursor = t.index;
            renderQueue();
          } catch (e) {
            toast(e.message, true);
          }
        });
        return li;
      }),
    );
    if (!rows.length) {
      trackList.append(el("li", { class: "muted" }, terms.length ? "No matches." : "Queue is empty."));
    }
  }

  stationSelect.addEventListener("change", () => {
    selected = parseInt(stationSelect.value, 10) || 0;
    renderEditor();
  });
  
  newBtn.addEventListener("click", () => {
    let name = "New Station";
    let n = 2;
    while (stations.some(s => s.name === name)) name = `New Station ${n++}`;
    stations.push(newStation(name));
    selected = stations.length - 1;
    renderEditor();
  });
  
  renameBtn.addEventListener("click", () => {
    const s = cur();
    const name = window.prompt("Station name", s.name)?.trim();
    if (!name || name === s.name) return;
    if (stations.some(x => x !== s && x.name === name)) return toast("A station with that name exists", true);
    if (s.name === activeStation) activeStation = name;
    s.name = name;
    renderEditor();
  });
  
  deleteBtn.addEventListener("click", () => {
    if (stations.length <= 1) return;
    const wasActive = cur().name === activeStation;
    stations.splice(selected, 1);
    selected = Math.min(selected, stations.length - 1);
    if (wasActive) activeStation = stations[0].name;
    renderEditor();
  });

  searchInput.addEventListener("input", () => {
    search = searchInput.value;
    renderQueue();
  });

  saveBtn.addEventListener("click", async () => {
    if (cur()) {
      cur().playlist_id = idInput.value.trim();
      cur().use_favorites = favCheck.checked;
    }
    try {
      await api.putJellyfinStations(stations, activeStation);
      toast("Saved Jellyfin stations");
      await ctx.onSaved?.();
      loadQueue();
    } catch (e) {
      toast(e.message, true);
    }
  });

  castBtn.addEventListener("click", async () => {
    const id = idInput.value.trim();
    const isFav = favCheck.checked;
    if (!id && !isFav) return;

    idInput.disabled = true;
    favCheck.disabled = true;
    castBtn.disabled = true;
    
    try {
      await api.castJellyfin(id, isFav);
      toast("Casting to Jellyfin...");
      await ctx.onSaved?.();
      loadQueue();
    } catch (e) {
      toast(e.message, true);
    } finally {
      idInput.disabled = false;
      favCheck.disabled = false;
      castBtn.disabled = false;
    }
  });

  shuffleBtn.addEventListener("click", async () => {
    const isShuffled = shuffleBtn.classList.contains("toggled");
    try {
        await api.shuffleJellyfin(!isShuffled);
        toast(!isShuffled ? "Shuffle on" : "Shuffle off");
        await ctx.onSaved?.(); 
        loadQueue();
    } catch (err) {
        toast(err.message, true);
    }
  });

  onAirBtn.addEventListener("click", async () => {
    const name = cur().name;
    try {
      await api.putJellyfinStations(stations, name); 
      await api.activateJellyfinStation(name); 

      await api.switchSource("jellyfin");
      await api.transport("jellyfin", "play");

      activeStation = name;
      renderStations();
      toast(`On air: ${name}`);
      await ctx.onSaved?.();
      loadQueue();
    } catch (e) {
      toast(e.message, true);
    }
  });

  let lastQueueSize = -1;
  let lastQueueCursor = -1;

  function render() {
    const state = ctx.getState();
    const isActive = state?.sources?.active === "jellyfin";
    card.hidden = !isActive;
    if (!isActive) return;
    load();

    const jfDetails = state?.sources?.available?.find(s => s.name === "jellyfin")?.details;
    const shuffleOn = !!jfDetails?.shuffle;
    shuffleBtn.classList.toggle("toggled", shuffleOn);
    shuffleBtn.setAttribute("aria-pressed", String(shuffleOn));

    const qs = jfDetails?.queue_size ?? -1;
    const qc = jfDetails?.queue_cursor ?? -1;
    if (loaded && (qs !== lastQueueSize || qc !== lastQueueCursor)) {
      lastQueueSize = qs;
      lastQueueCursor = qc;
      loadQueue();
    }
  }

  return {
    render,
    invalidate: () => { loaded = false; },
  };
}