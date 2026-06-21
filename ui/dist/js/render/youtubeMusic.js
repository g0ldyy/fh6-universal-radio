import { api } from "../api.js";
import { $, el } from "../dom.js";
import { icons } from "../icons.js";
import { toast } from "../toast.js";
import { t } from "../i18n.js";
import { renderTrackQueue } from "./playlistQueue.js";

function newStation(name) {
    return { name, url: "" };
}

const fold = s => (s || "").normalize("NFD").replace(/\p{Diacritic}/gu, "").toLowerCase();

export function createYoutubeMusic(main, ctx) {
    let stations = [];
    let activeStation = "";
    let selected = 0;
    let loaded = false;
    let queue = { cursor: 0, tracks: [] };
    let search = "";

    const stationSelect = el("select", { id: "yt-station", "aria-label": "Station preset" });
    const onAirBtn = el("button", { type: "button", class: "btn filled" }, t("youtube_music.set_on_air"));
    const newBtn = el("button", { type: "button", class: "btn ghost" }, t("youtube_music.new"));
    const renameBtn = el("button", { type: "button", class: "btn ghost" }, t("youtube_music.rename"));
    const deleteBtn = el("button", { type: "button", class: "btn ghost" }, t("youtube_music.delete"));

    const urlInput = el("input", { type: "text", class: "lf-path-input", placeholder: "https://music.youtube.com/playlist?list=...", autocomplete: "off" });
    const castBtn = el("button", { type: "button", class: "btn ghost" }, t("youtube_music.cast"));
    const saveBtn = el("button", { type: "button", class: "btn filled" }, t("youtube_music.save"));

    const queueCount = el("span", { class: "muted" });
    const searchInput = el("input", { type: "text", placeholder: t("youtube_music.search_placeholder"), autocomplete: "off" });
    const shuffleBtn = el("button", { type: "button", class: "icon-btn", "aria-label": "Toggle Shuffle", html: icons.shuffle });
    const trackList = el("ul", { class: "lf-tracklist" });

    const card = el("section", { class: "card", id: "youtube-music-card", hidden: true }, [
        el("h2", {}, t("youtube_music.title")),
        el("div", { class: "yt-stationbar row" }, [stationSelect, onAirBtn]),
        el("div", { class: "row yt-stationtools" }, [newBtn, renameBtn, deleteBtn]),
        el("div", { class: "lf-editor" }, [
            el("label", { class: "field-label" }, t("youtube_music.playlist_or_video_url")),
            urlInput,
            el("div", { class: "row lf-editor-foot" }, [castBtn, saveBtn]),
        ]),
        el("div", { class: "lf-queue" }, [
            el("div", { class: "lf-queue-head row" }, [
                el("label", { class: "field-label" }, t("youtube_music.queue")),
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
            const r = await api.getYoutubeStations();
            stations = Array.isArray(r.stations) && r.stations.length ? r.stations : [newStation(t("youtube_music.default_station_name"))];
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
            queue = await api.getYoutubeQueue();
        } catch {
            queue = { cursor: 0, tracks: [] };
        }
        renderQueue();
    }

    function renderStations() {
        stationSelect.replaceChildren(
            ...stations.map((s, i) =>
                el("option", { value: String(i), selected: i === selected },
                    s.name + (s.name === activeStation ? `  • ${t("youtube_music.on_air")}` : "")),
            ),
        );
        deleteBtn.disabled = stations.length <= 1;
        onAirBtn.disabled = cur()?.name === activeStation;
        onAirBtn.textContent = cur()?.name === activeStation ? t("youtube_music.already_on_air") : t("youtube_music.set_on_air");
    }

    function renderEditor() {
        if (!cur()) return;
        renderStations();
        urlInput.value = cur().url || "";
    }

    function renderQueue() {
        queueCount.textContent = `${queue.tracks?.length || 0} ${t("youtube_music.tracks")}`;

        renderTrackQueue(trackList, queue, search, {
            getTitle: track => track.title || track.url?.split("v=")[1] || track.url,
            getSubtitle: track => track.url ? (track.url.split("v=")[1] || track.url) : null,
            getCoverUrl: track => track.cover_url,
            getSearchFields: track => [track.title || "", track.url || ""],
            onTrackClick: async track => {
                try {
                    await api.playYoutubeIndex(track.index);
                    queue.cursor = track.index;
                    renderQueue();
                } catch (e) {
                    toast(e.message, true);
                }
            },
            emptyKey: "youtube_music.queue_empty",
            noMatchesKey: "youtube_music.no_matches",
            unknownTitleKey: "youtube_music.unknown_title",
        });
    }

    // --- events ---
    stationSelect.addEventListener("change", () => {
        selected = parseInt(stationSelect.value, 10) || 0;
        renderEditor();
    });

    newBtn.addEventListener("click", () => {
        let name = t("youtube_music.default_station_name");
        let n = 2;
        while (stations.some(s => s.name === name)) name = `${t("youtube_music.default_station_name")} ${n++}`;
        stations.push(newStation(name));
        selected = stations.length - 1;
        renderEditor();
    });

    renameBtn.addEventListener("click", () => {
        const s = cur();
        const name = window.prompt(t("youtube_music.station_name"), s.name)?.trim();
        if (!name || name === s.name) return;
        if (stations.some(x => x !== s && x.name === name)) return toast(t("youtube_music.station_exists"), true);
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
        if (cur()) cur().url = urlInput.value.trim();
        try {
            await api.putYoutubeStations(stations, activeStation);
            toast(t("youtube_music.saved_stations"));
            await ctx.onSaved?.();
            loadQueue();
        } catch (e) {
            toast(e.message, true);
        }
    });

    castBtn.addEventListener("click", async () => {
        const url = urlInput.value.trim();
        if (!url) return;

        urlInput.disabled = true;
        castBtn.disabled = true;

        try {
            await api.castYoutube(url);
            toast(t("youtube_music.casting"));
            urlInput.value = "";
            await ctx.onSaved?.();
            loadQueue();
        } catch (e) {
            toast(e.message, true);
        } finally {
            urlInput.disabled = false;
            castBtn.disabled = false;
        }
    });

    shuffleBtn.addEventListener("click", async () => {
        const isShuffled = shuffleBtn.classList.contains("toggled");
        try {
            await api.shuffleYoutube(!isShuffled);
            toast(!isShuffled ? t("youtube_music.shuffle_on") : t("youtube_music.shuffle_off"));
            await ctx.onSaved?.(); // force state refresh
            loadQueue();
        } catch (err) {
            toast(err.message, true);
        }
    });

    onAirBtn.addEventListener("click", async () => {
        const name = cur().name;
        try {
            await api.putYoutubeStations(stations, name);
            await api.activateYoutubeStation(name);

            await api.switchSource("youtube_music");
            await api.transport("youtube_music", "play");

            activeStation = name;
            renderStations();
            toast(`${t("youtube_music.on_air")}: ${name}`);
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
        const isActive = state?.sources?.active === "youtube_music";
        card.hidden = !isActive;
        if (!isActive) return;
        load();

        const ytDetails = state?.sources?.available?.find(s => s.name === "youtube_music")?.details;
        const shuffleOn = !!ytDetails?.shuffle;
        shuffleBtn.classList.toggle("toggled", shuffleOn);
        shuffleBtn.setAttribute("aria-pressed", String(shuffleOn));

        // refetch the queue if the track count changes (rescan/activate) or cursor moves
        const qs = ytDetails?.queue_size ?? -1;
        const qc = ytDetails?.queue_cursor ?? -1;
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