import { api } from "../data/api.js";
import { $, el } from "../lib/dom.js";
import { icons } from "../icons.js";
import { toast } from "../toast.js";
import { t } from "../i18n.js";
import { createStationManager } from "./stationManager.js";

/**
 * Creates a default station object structure.
 *
 * @param {string} name - The name of the station
 * @returns {object} The new station instance { name, url }
 */
function newStation(name) {
    return { name, url: "" };
}

/**
 * Renders and manages the YouTube Music station source UI component.
 * Integrates with the shared StationManager for CRUD operations and track queues.
 *
 * @param {HTMLElement} main - The main container element to inject the card into
 * @param {object} ctx - Application context containing state and lifecycle hooks
 * @param {() => object} ctx.getState - Returns the global application state
 * @param {() => Promise<void>} [ctx.onSaved] - Optional callback triggered after successful mutations
 */
export function createYoutubeMusic(main, ctx) {
    const urlInput = el("input", { type: "text", class: "path-input", placeholder: "https://music.youtube.com/playlist?list=...", autocomplete: "off" });
    const castBtn = el("button", { type: "button", class: "btn ghost", dataset: { i18n: "btn.cast" } }, t("btn.cast"));
    const saveBtn = el("button", { type: "button", class: "btn filled", dataset: { i18n: "btn.save" } }, t("btn.save"));
    const shuffleBtn = el("button", {
        type: "button", class: "icon-btn", "aria-label": "Toggle Shuffle",
        dataset: { i18nAriaLabel: "youtube_music.shuffle" }, html: icons.shuffle,
    });
    const summaryEl = el("p", { class: "muted", hidden: true });
    let lastDetails = null;

    function renderSummary() {
        const s = station.cur();
        if (!s) {
            summaryEl.hidden = true;
            return;
        }
        const isOnAir = s.name === station.getActiveStation();
        const trackCount = isOnAir ? lastDetails?.queue_size : null;

        const parts = [`${stationSelect.options.length} ${t("label.stations")}`];
        if (trackCount != null) parts.unshift(`${trackCount} ${t("label.tracks")}`);

        summaryEl.textContent = parts.join(" · ");
        summaryEl.hidden = false;
    }

    const station = createStationManager({
        api: api.youtubeMusic,
        newStation,
        defaultNameKey: "youtube_music.default_station_name",
        ctx,
        queue: {
            getTitle: track => track.title || track.url?.split("v=")[1] || track.url,
            getSubtitle: track => (track.url ? (track.url.split("v=")[1] || track.url) : null),
            getCoverUrl: track => track.cover_url,
            getSearchFields: track => [track.title || "", track.url || ""],
        },
        getSig: details => `${details?.queue_size ?? -1}|${details?.queue_cursor ?? -1}`,
        onAir: async () => {
            await api.switchSource("youtube_music");
            await api.transport("youtube_music", "play");
        },
        onStationChange: s => {
            urlInput.value = s?.url || "";
            renderSummary();
        },
        onSync: details => {
            lastDetails = details;
            const shuffleOn = !!details?.shuffle;
            shuffleBtn.classList.toggle("toggled", shuffleOn);
            shuffleBtn.setAttribute("aria-pressed", String(shuffleOn));
            renderSummary();
        },
    });

    const { stationSelect, onAirBtn, newBtn, duplicateBtn, renameBtn, deleteBtn, queueCount, searchInput, trackList } = station.els;

    const card = el("section", { class: "card", id: "youtube-music-card", hidden: true }, [
        el("h2", { dataset: { i18n: "youtube_music.title" } }, t("youtube_music.title")),
        summaryEl,
        el("div", { class: "stationbar row" }, [stationSelect, onAirBtn]),
        el("div", { class: "row stationtools" }, [newBtn, duplicateBtn, renameBtn, deleteBtn]),
        el("div", { class: "editor" }, [
            el("label", { class: "field-label", dataset: { i18n: "youtube_music.playlist_or_video_url" } }, t("youtube_music.playlist_or_video_url")),
            urlInput,
            el("div", { class: "row editor-foot" }, [castBtn, saveBtn]),
        ]),
        el("div", { class: "queue" }, [
            el("div", { class: "queue-head row" }, [el("label", { class: "field-label", dataset: { i18n: "label.queue" } }, t("label.queue")), queueCount, shuffleBtn]),
            searchInput,
            trackList,
        ]),
    ]);

    const sourcesCard = $("#sources", main)?.closest(".card");
    if (sourcesCard) sourcesCard.insertAdjacentElement("afterend", card);
    else main.append(card);

    saveBtn.addEventListener("click", async () => {
        if (station.cur()) station.cur().url = urlInput.value.trim();
        try {
            await station.save();
            toast(t("youtube_music.saved_stations"));
        } catch {
            // Error handling already deferred to the station manager
        }
    });

    castBtn.addEventListener("click", async () => {
        const url = urlInput.value.trim();
        if (!url) return;
        urlInput.disabled = true;
        castBtn.disabled = true;
        try {
            await api.youtubeMusic.cast(url);
            toast(t("toast.casting", { service: "YouTube Music" }));
            urlInput.value = "";
            await ctx.onSaved?.();
            station.loadQueue();
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
            await api.youtubeMusic.shuffle(!isShuffled);
            toast(!isShuffled ? t("toast.shuffle_on") : t("toast.shuffle_off"));
            await ctx.onSaved?.();
            station.loadQueue();
        } catch (e) {
            toast(e.message, true);
        }
    });

    function render() {
        const state = ctx.getState();
        const isActive = state?.sources?.active === "youtube_music";
        card.hidden = !isActive;
        if (!isActive) return;
        station.load();
        const details = state?.sources?.available?.find(s => s.name === "youtube_music")?.details;
        station.sync(details);
    }

    return { render, invalidate: station.invalidate };
}