import { api } from "../data/api.js";
import { $, el } from "../lib/dom.js";
import { icons } from "../icons.js";
import { toast } from "../toast.js";
import { t } from "../i18n.js";
import { createStationManager } from "./stationManager.js";

function newStation(name) {
    return { name, url: "" };
}

export function createSoundcloud(main, ctx) {
    const urlInput = el("input", { type: "text", class: "path-input", placeholder: t("soundcloud.url_placeholder"), autocomplete: "off" });
    const castBtn = el("button", { type: "button", class: "btn ghost", dataset: { i18n: "btn.cast" } }, t("btn.cast"));
    const saveBtn = el("button", { type: "button", class: "btn filled", dataset: { i18n: "btn.save" } }, t("btn.save"));
    const exportBtn = el("button", { type: "button", class: "btn ghost", dataset: { i18n: "settings.export_stations" } }, t("settings.export_stations"));
    const importBtn = el("button", { type: "button", class: "btn ghost", dataset: { i18n: "settings.import_stations" } }, t("settings.import_stations"));
    const importFileInput = el("input", { type: "file", accept: ".json", hidden: true });
    const shuffleBtn = el("button", {
        type: "button", class: "icon-btn", "aria-label": "Toggle Shuffle",
        dataset: { i18nAriaLabel: "soundcloud.shuffle" }, html: icons.shuffle,
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
        api: api.soundcloud,
        newStation,
        defaultNameKey: "soundcloud.default_station_name",
        ctx,
        queue: {
            getTitle: track => track.title || track.url,
            getSubtitle: track => track.artist || track.url,
            getCoverUrl: track => track.cover_url,
            getSearchFields: track => [track.title || "", track.url || ""],
        },
        getSig: details => `${details?.queue_size ?? -1}|${details?.queue_cursor ?? -1}|${details?.queue_version ?? -1}`,
        onAir: async () => {
            await api.switchSource("soundcloud");
            await api.transport("soundcloud", "play");
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

    const card = el("section", { class: "card", id: "soundcloud-card", hidden: true }, [
        el("h2", { dataset: { i18n: "soundcloud.title" } }, t("soundcloud.title")),
        summaryEl,
        el("div", { class: "stationbar row" }, [stationSelect, onAirBtn]),
        el("div", { class: "row stationtools" }, [newBtn, duplicateBtn, renameBtn, deleteBtn, exportBtn, importBtn, importFileInput]),
        el("div", { class: "editor" }, [
            el("label", { class: "field-label", dataset: { i18n: "soundcloud.playlist_or_track_url" } }, t("soundcloud.playlist_or_track_url")),
            urlInput,
            el("div", { class: "row editor-foot" }, [castBtn, saveBtn]),
        ]),
        el("div", { class: "queue" }, [
            el("div", { class: "queue-head row" }, [el("label", { class: "field-label", dataset: { i18n: "label.queue" } }, t("label.queue")), queueCount, shuffleBtn]),
            searchInput,
            trackList,
        ]),
    ]);

    const extCard = document.getElementById("youtube-music-card");
    if (extCard) extCard.insertAdjacentElement("afterend", card);
    else main.append(card);

    saveBtn.addEventListener("click", async () => {
        if (station.cur()) station.cur().url = urlInput.value.trim();
        try {
            await station.save();
            toast(t("soundcloud.saved_stations"));
        } catch {}
    });

    castBtn.addEventListener("click", async () => {
        const url = urlInput.value.trim();
        if (!url) return;
        urlInput.disabled = true;
        castBtn.disabled = true;
        try {
            await api.soundcloud.cast(url);
            toast(t("toast.casting", { service: "SoundCloud" }));
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
            await api.soundcloud.shuffle(!isShuffled);
            toast(!isShuffled ? t("toast.shuffle_on") : t("toast.shuffle_off"));
            await ctx.onSaved?.();
            station.loadQueue();
        } catch (e) {
            toast(e.message, true);
        }
    });

    function render() {
        const state = ctx.getState();
        const isActive = state?.sources?.active === "soundcloud";
        card.hidden = !isActive;
        if (!isActive) return;
        station.load();
        const details = state?.sources?.available?.find(s => s.name === "soundcloud")?.details;
        station.sync(details);
    }

    return { render, invalidate: station.invalidate, els: { stationSelect, onAirBtn, newBtn, duplicateBtn, renameBtn, deleteBtn, queueCount, searchInput, trackList, exportBtn, importBtn, importFileInput } };
}