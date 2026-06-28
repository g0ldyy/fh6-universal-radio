import { el, debounce } from "../lib/dom.js";
import { toast } from "../toast.js";
import { t } from "../i18n.js";
import { renderTrackQueue } from "./playlistQueue.js";
import { promptInput } from "./promptModal.js";
import { confirmAction } from "./confirmModal.js";

/**
 * Shared scaffold for "station-based" sources (Local Files, YouTube Music,
 * Jellyfin, and any future source of the same kind): station selector +
 * on-air button, new/rename/delete tools, and a filterable track queue.
 * Each source only provides its API calls, the shape of its "new station",
 * and track accessors for the queue.
 *
 * @param {object} cfg
 * @param {object} cfg.api - { getStations, putStations, activateStation, getQueue, playIndex }
 * @param {(name: string) => object} cfg.newStation - factory for a new station object
 * @param {string} cfg.defaultNameKey - i18n key for the default name / "new station"
 * @param {object} cfg.ctx - { onSaved }, the context received by createXxx(main, ctx)
 * @param {object} cfg.queue - { getTitle, getSubtitle, getCoverUrl, getSearchFields }
 * @param {(details: object, extra: any) => string} [cfg.getSig] - cheap signature to know when to re-fetch the queue
 * @param {(name: string) => Promise<void>} [cfg.onAir] - extra steps during "set on air" (switchSource, play...)
 * @param {(station: object) => void} [cfg.onStationChange] - called whenever the selected/edited station changes
 * @param {(details: object) => void} [cfg.onSync] - called on every render() tick with live details from the source
 */
export function createStationManager(cfg) {
    let stations = [];
    let activeStation = "";
    let selected = 0;
    let loaded = false;
    let queue = { cursor: 0, tracks: [] };
    let search = "";
    let lastSig = "";

    const stationSelect = el("select", { "aria-label": "Station preset", dataset: { i18nAriaLabel: "label.station_preset" } });
    const onAirBtn = el("button", { type: "button", class: "btn filled" }, t("btn.set_on_air"));
    const newBtn = el("button", { type: "button", class: "btn ghost confirm", dataset: { i18n: "btn.new" } }, t("btn.new"));
    const duplicateBtn = el("button", { type: "button", class: "btn ghost", dataset: { i18n: "btn.duplicate" } }, t("btn.duplicate"));
    const renameBtn = el("button", { type: "button", class: "btn ghost", dataset: { i18n: "btn.rename" } }, t("btn.rename"));
    const deleteBtn = el("button", { type: "button", class: "btn ghost danger", dataset: { i18n: "btn.delete" } }, t("btn.delete"));
    const queueCount = el("span", { class: "muted" });
    const searchInput = el("input", {
        type: "text",
        placeholder: t("label.search_playlist_placeholder"),
        dataset: { i18nPlaceholder: "label.search_playlist_placeholder" },
        autocomplete: "off",
    });
    const trackList = el("ul", { class: "tracklist" });

    const cur = () => stations[selected];

    async function load(force = false) {
        if (loaded && !force) return;
        try {
            const r = await cfg.api.getStations();
            stations = Array.isArray(r.stations) && r.stations.length ? r.stations : [cfg.newStation(t(cfg.defaultNameKey))];
            activeStation = r.active_station || stations[0].name;
            selected = Math.max(0, stations.findIndex(s => s.name === activeStation));
            loaded = true;
        } catch {
            return;
        }
        renderStations();
        cfg.onStationChange?.(cur());
        loadQueue();
    }

    async function loadQueue() {
        try {
            queue = await cfg.api.getQueue();
        } catch {
            queue = { cursor: 0, tracks: [] };
        }
        renderQueue();
    }

    function renderStations() {
        stationSelect.replaceChildren(
            ...stations.map((s, i) =>
                el("option", { value: String(i), selected: i === selected },
                    s.name + (s.name === activeStation ? `  • ${t("btn.on_air")}` : "")),
            ),
        );
        deleteBtn.disabled = stations.length <= 1;
        const isOnAir = cur()?.name === activeStation;
        onAirBtn.disabled = isOnAir;
        onAirBtn.textContent = isOnAir ? t("btn.already_on_air") : t("btn.set_on_air");
    }

    function renderQueue() {
        queueCount.textContent = `${queue.tracks?.length || 0} ${t("label.tracks")}`;
        renderTrackQueue(trackList, queue, search, {
            ...cfg.queue,
            onTrackClick: async track => {
                try {
                    await cfg.api.playIndex(track.index);
                    queue.cursor = track.index;
                    renderQueue();
                } catch (e) {
                    toast(e.message, true);
                }
            },
        });
    }

    // --- Station CRUD --------------------------------------------------------
    stationSelect.addEventListener("change", () => {
        selected = parseInt(stationSelect.value, 10) || 0;
        renderStations();
        cfg.onStationChange?.(cur());
    });

    newBtn.addEventListener("click", () => {
        let name = t(cfg.defaultNameKey);
        let n = 2;
        while (stations.some(s => s.name === name)) name = `${t(cfg.defaultNameKey)} ${n++}`;
        stations.push(cfg.newStation(name));
        selected = stations.length - 1;
        renderStations();
        cfg.onStationChange?.(cur());
    });

    duplicateBtn.addEventListener("click", () => {
        const s = cur();
        let name = t("label.copy_of", { name: s.name });
        let n = 2;
        while (stations.some(x => x.name === name)) name = t("label.copy_of_n", { name: s.name, n: n++ });
        stations.push({ ...structuredClone(s), name });
        selected = stations.length - 1;
        renderStations();
        cfg.onStationChange?.(cur());
    });

    renameBtn.addEventListener("click", async () => {
        const s = cur();
        const name = await promptInput({ title: t("label.station_name"), value: s.name });
        if (!name || name === s.name) return;
        if (stations.some(x => x !== s && x.name === name)) return toast(t("error.station_name_exists"), true);
        if (s.name === activeStation) activeStation = name;
        s.name = name;
        renderStations();
        cfg.onStationChange?.(cur());
    });

    deleteBtn.addEventListener("click", async () => {
        if (stations.length <= 1) return;
        const s = cur();
        const ok = await confirmAction({
            title: t("local_files.delete_station"),
            message: t("local_files.delete_confirm", { name: s.name }),
        });
        if (!ok) return;

        const wasActive = s.name === activeStation;
        stations.splice(selected, 1);
        selected = Math.min(selected, stations.length - 1);
        if (wasActive) activeStation = stations[0].name;
        renderStations();
        cfg.onStationChange?.(cur());

        // Persists immediately (unlike rename/new, which wait for the
        // explicit Save button) — once confirmed, a deletion shouldn't be
        // silently reverted by an unrelated reload (e.g. importing a station
        // pack) just because it was never saved.
        try {
            await save();
            toast(t("local_files.station_deleted"));
        } catch {
            // error toast already shown by save()
        }
    });

    const applySearch = debounce(() => {
        search = searchInput.value;
        renderQueue();
    });
    searchInput.addEventListener("input", applySearch);

    // --- Save / On-air -------------------------------------------------------
    // Does not trigger a toast by itself: each source displays its own message
    // (e.g., localFiles wants to show the number of discovered tracks).
    async function save() {
        try {
            const r = await cfg.api.putStations(stations, activeStation);
            await cfg.ctx?.onSaved?.();
            loadQueue();
            return r;
        } catch (e) {
            toast(e.message, true);
            throw e;
        }
    }

    async function setOnAir() {
        const name = cur().name;
        try {
            await cfg.api.putStations(stations, name);
            await cfg.api.activateStation(name);
            await cfg.onAir?.(name);
            activeStation = name;
            renderStations();
            toast(`${t("btn.on_air")}: ${name}`);
            await cfg.ctx?.onSaved?.();
            loadQueue();
        } catch (e) {
            toast(e.message, true);
        }
    }
    onAirBtn.addEventListener("click", setOnAir);

    // --- Sync with backend live state on each render() tick ------------------
    function sync(details, extra) {
        const liveActiveStation = details?.active_station;
        if (loaded && liveActiveStation && liveActiveStation !== activeStation) {
            activeStation = liveActiveStation;
            selected = Math.max(0, stations.findIndex(s => s.name === activeStation));
            renderStations();
            cfg.onStationChange?.(cur());
        }

        cfg.onSync?.(details);

        const sig = cfg.getSig ? cfg.getSig(details, extra) : "";
        if (loaded && sig !== lastSig) {
            lastSig = sig;
            loadQueue();
        }
    }

    return {
        els: { stationSelect, onAirBtn, newBtn, duplicateBtn, renameBtn, deleteBtn, queueCount, searchInput, trackList },
        cur,
        getActiveStation: () => activeStation,
        load,
        loadQueue,
        save,
        setOnAir,
        sync,
        invalidate: () => { loaded = false; },
    };
}