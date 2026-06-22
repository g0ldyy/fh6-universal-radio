import { api } from "../api.js";
import { $, el } from "../dom.js";
import { toast } from "../toast.js";
import { icons } from "../icons.js";
import { t } from "../i18n.js";
import { renderTrackQueue } from "./playlistQueue.js";

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
    const onAirBtn = el("button", { type: "button", class: "btn filled" }, t("jellyfin.set_on_air"));
    const newBtn = el("button", { type: "button", class: "btn ghost" }, t("local_files.new"));
    const renameBtn = el("button", { type: "button", class: "btn ghost" }, t("local_files.rename"));
    const deleteBtn = el("button", { type: "button", class: "btn ghost" }, t("local_files.delete"));

    const idInput = el("input", { type: "text", class: "lf-path-input", placeholder: t("jellyfin.playlist_placeholder"), autocomplete: "off" });
    const favCheck = el("input", { type: "checkbox" });
    const castBtn = el("button", { type: "button", class: "btn ghost" }, t("jellyfin.cast"));
    const saveBtn = el("button", { type: "button", class: "btn filled" }, t("local_files.save"));

    const queueCount = el("span", { class: "muted" });
    const searchInput = el("input", { type: "text", placeholder: t("jellyfin.search_placeholder"), autocomplete: "off" });
    const shuffleBtn = el("button", { type: "button", class: "icon-btn", "aria-label": t("jellyfin.toggle_shuffle"), html: icons.shuffle });
    const trackList = el("ul", { class: "lf-tracklist" });

    const card = el("section", { class: "card", id: "jellyfin-card", hidden: true }, [
        el("h2", {}, t("jellyfin.title")),
        el("div", { class: "yt-stationbar row" }, [stationSelect, onAirBtn]),
        el("div", { class: "row yt-stationtools" }, [newBtn, renameBtn, deleteBtn]),
        el("div", { class: "lf-editor" }, [
            el("label", { class: "field-label" }, t("jellyfin.playlist_id")),
            idInput,
            el("label", { class: "field-label field checkbox" }, [
                favCheck, t("jellyfin.use_favorites")
            ]),
            el("div", { class: "row lf-editor-foot" }, [castBtn, saveBtn]),
        ]),
        el("div", { class: "lf-queue" }, [
            el("div", { class: "lf-queue-head row" }, [
                el("label", { class: "field-label" }, t("local_files.queue")),
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
            stations = Array.isArray(r.stations) && r.stations.length ? r.stations : [newStation(t("jellyfin.default_station_name"))];
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

  let lastQueueSize = -1;
  let lastQueueCursor = -1;

  function render() {
    const state = ctx.getState();
    const isActive = state?.sources?.active === "jellyfin";
    card.hidden = !isActive;
    if (!isActive) return;
    load();

    const jfDetails = state?.sources?.available?.find(s => s.name === "jellyfin")?.details;

    const liveActiveStation = jfDetails?.active_station;
    if (loaded && liveActiveStation && liveActiveStation !== activeStation) {
      activeStation = liveActiveStation;
      selected = Math.max(0, stations.findIndex(s => s.name === activeStation));
      renderEditor();
      loadQueue();
    }
    
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

    function renderEditor() {
        if (!cur()) return;
        renderStations();
        idInput.value = cur().playlist_id || "";
        favCheck.checked = !!cur().use_favorites;
    }

    function renderQueue() {
        queueCount.textContent = `${queue.tracks?.length || 0} ${t("local_files.tracks")}`;

        renderTrackQueue(trackList, queue, search, {
            getTitle: track => track.title,
            getSubtitle: track => track.artist || null,
            getCoverUrl: track => track.cover_url,
            getSearchFields: track => [track.title || "", track.artist || ""],
            onTrackClick: async track => {
                try {
                    await api.playJellyfinIndex(track.index);
                    queue.cursor = track.index;
                    renderQueue();
                } catch (e) {
                    toast(e.message, true);
                }
            },
            emptyKey: "jellyfin.queue_empty",
            noMatchesKey: "jellyfin.no_matches",
            unknownTitleKey: "jellyfin.unknown_title",
        });
    }

    stationSelect.addEventListener("change", () => {
        selected = parseInt(stationSelect.value, 10) || 0;
        renderEditor();
    });

    newBtn.addEventListener("click", () => {
        let name = t("jellyfin.new_station_name");
        let n = 2;
        while (stations.some(s => s.name === name)) name = `${t("jellyfin.new_station_name")} ${n++}`;
        stations.push(newStation(name));
        selected = stations.length - 1;
        renderEditor();
    });

    renameBtn.addEventListener("click", () => {
        const s = cur();
        const name = window.prompt(t("local_files.station_name"), s.name)?.trim();
        if (!name || name === s.name) return;
        if (stations.some(x => x !== s && x.name === name)) return toast(t("jellyfin.station_exists"), true);
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
            toast(t("jellyfin.stations_saved"));
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
            toast(t("jellyfin.casting"));
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
            toast(!isShuffled ? t("jellyfin.shuffle_on") : t("jellyfin.shuffle_off"));
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
            toast(`${t("jellyfin.on_air")}: ${name}`);
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

    function refreshTexts() {
        card.querySelector("h2").textContent = t("jellyfin.title");
        onAirBtn.textContent = cur()?.name === activeStation ? t("jellyfin.already_on_air") : t("jellyfin.set_on_air");
        newBtn.textContent = t("local_files.new");
        renameBtn.textContent = t("local_files.rename");
        deleteBtn.textContent = t("local_files.delete");
        idInput.placeholder = t("jellyfin.playlist_placeholder");
        castBtn.textContent = t("jellyfin.cast");
        saveBtn.textContent = t("local_files.save");
        searchInput.placeholder = t("jellyfin.search_placeholder");
        shuffleBtn.setAttribute("aria-label", t("jellyfin.toggle_shuffle"));
        card.querySelector(".lf-editor .field-label").textContent = t("jellyfin.playlist_id");
        card.querySelector(".lf-queue-head .field-label").textContent = t("local_files.queue");
        renderStations();
        renderQueue();
    }

    return {
        render,
        invalidate: () => { loaded = false; },
        refreshTexts,
    };
}