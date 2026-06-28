import { api } from "../data/api.js";
import { $, el } from "../lib/dom.js";
import { toast } from "../toast.js";
import { icons } from "../icons.js";
import { t, tNode } from "../i18n.js";
import { createStationManager } from "./stationManager.js";

/**
 * Creates a default station object structure.
 *
 * @param {string} name - The name of the station
 * @returns {object} The new station instance { name, playlist_id, use_favorites }
 */
function newStation(name) {
    return { name, playlist_id: "", use_favorites: false };
}

/**
 * Renders and manages the Jellyfin station source UI component.
 * Integrates with the shared StationManager for CRUD operations and track queues.
 *
 * @param {HTMLElement} main - The main container element to inject the card into
 * @param {object} ctx - Application context containing state and lifecycle hooks
 * @param {() => object} ctx.getState - Returns the global application state
 * @param {() => Promise<void>} [ctx.onSaved] - Optional callback triggered after successful mutations
 */
export function createJellyfin(main, ctx) {
    const idInput = el("input", {
        type: "text", class: "path-input", placeholder: t("jellyfin.playlist_placeholder"),
        dataset: { i18nPlaceholder: "jellyfin.playlist_placeholder" }, autocomplete: "off",
    });
    const favCheck = el("input", { type: "checkbox" });
    const castBtn = el("button", { type: "button", class: "btn ghost", dataset: { i18n: "btn.cast" } }, t("btn.cast"));
    const saveBtn = el("button", { type: "button", class: "btn filled", dataset: { i18n: "btn.save" } }, t("btn.save"));
    const shuffleBtn = el("button", {
        type: "button", class: "icon-btn", "aria-label": t("jellyfin.toggle_shuffle"),
        dataset: { i18nAriaLabel: "jellyfin.toggle_shuffle" }, html: icons.shuffle,
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
        api: api.jellyfin,
        newStation,
        defaultNameKey: "jellyfin.default_station_name",
        ctx,
        queue: {
            getTitle: track => track.title,
            getSubtitle: track => track.artist || null,
            getCoverUrl: track => track.cover_url,
            getSearchFields: track => [track.title || "", track.artist || ""],
        },
        getSig: details => `${details?.queue_size ?? -1}|${details?.queue_cursor ?? -1}`,
        onAir: async () => {
            await api.switchSource("jellyfin");
            await api.transport("jellyfin", "play");
        },
        onStationChange: s => {
            idInput.value = s?.playlist_id || "";
            favCheck.checked = !!s?.use_favorites;
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

    const card = el("section", { class: "card", id: "jellyfin-card", hidden: true }, [
        el("h2", { dataset: { i18n: "jellyfin.title" } }, t("jellyfin.title")),
        summaryEl,
        el("div", { class: "stationbar row" }, [stationSelect, onAirBtn]),
        el("div", { class: "row stationtools" }, [newBtn, duplicateBtn, renameBtn, deleteBtn]),
        el("div", { class: "editor" }, [
            el("label", { class: "field-label", dataset: { i18n: "jellyfin.playlist_id" } }, t("jellyfin.playlist_id")),
            idInput,
            el("label", { class: "field-label field checkbox" }, [favCheck, tNode("jellyfin.use_favorites")]),
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
        if (station.cur()) {
            station.cur().playlist_id = idInput.value.trim();
            station.cur().use_favorites = favCheck.checked;
        }
        try {
            await station.save();
            toast(t("jellyfin.stations_saved"));
        } catch {
            // Error handling already deferred to the station manager
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
            await api.jellyfin.cast(id, isFav);
            toast(t("toast.casting", { service: "Jellyfin" }));
            await ctx.onSaved?.();
            station.loadQueue();
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
            await api.jellyfin.shuffle(!isShuffled);
            toast(!isShuffled ? t("toast.shuffle_on") : t("toast.shuffle_off"));
            await ctx.onSaved?.();
            station.loadQueue();
        } catch (e) {
            toast(e.message, true);
        }
    });

    function render() {
        const state = ctx.getState();
        const isActive = state?.sources?.active === "jellyfin";
        card.hidden = !isActive;
        if (!isActive) return;
        station.load();
        const details = state?.sources?.available?.find(s => s.name === "jellyfin")?.details;
        station.sync(details);
    }

    return { render, invalidate: station.invalidate };
}