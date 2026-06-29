import { api } from "../data/api.js";
import { $, el } from "../lib/dom.js";
import { icons } from "../icons.js";
import { toast } from "../toast.js";
import { t, tNode } from "../i18n.js";
import { createStationManager } from "./stationManager.js";

// [value, i18n key] — kept separate from the translated string so the
// <option> built from it can carry data-i18n and refresh on language change.
const ORDERS = [
    ["shuffle", "local_files.order.shuffle"],
    ["album", "local_files.order.album"],
    ["name", "local_files.order.name"],
    ["folder", "local_files.order.folder"],
];
const GROUPINGS = [
    ["folder", "local_files.grouping.folder"],
    ["tags", "local_files.grouping.tags"],
];
const REPEATS = [
    ["all", "local_files.repeat.all"],
    ["one", "local_files.repeat.one"],
    ["off", "local_files.repeat.off"],
];
const optionList = pairs => pairs.map(([value, key]) => el("option", { value, dataset: { i18n: key } }, t(key)));

/**
 * Normalizes a path string to be case-insensitive and separator-agnostic.
 *
 * @param {string} p - The path to normalize
 * @returns {string} The normalized lowercased path
 */
const norm = p => (p || "").replace(/\\/g, "/").replace(/\/+$/, "").toLowerCase();

/**
 * Determines whether a child path resides within a parent directory path.
 *
 * @param {string} child - The potential subfolder path
 * @param {string} parent - The base directory path
 * @returns {boolean} True if the child is within or equal to the parent
 */
const within = (child, parent) => {
    const c = norm(child);
    const a = norm(parent);
    return c === a || c.startsWith(a + "/");
};

/**
 * Creates a default local files station object structure.
 *
 * @param {string} name - The name of the station
 * @returns {object} The new station instance configuration
 */
function newStation(name) {
    return { name, roots: [], excluded: [], recursive: true, order: "shuffle", grouping: "folder", repeat: "all" };
}

/**
 * Instantiates a lazy folder browser modal instance.
 *
 * @returns {object} An object containing an `open(onPick)` method
 */
function createBrowser() {
    let dir = "";
    let onPick = null;

    const list = el("div", { class: "browse-list" });
    const crumb = el("div", { class: "browse-crumb muted" });
    const useBtn = el("button", { type: "button", class: "btn filled", dataset: { i18n: "local_files.browser.use" } }, t("local_files.browser.use"));
    const upBtn = el("button", { type: "button", class: "btn ghost", dataset: { i18n: "local_files.browser.up" } }, t("local_files.browser.up"));
    const closeBtn = el("button", { type: "button", class: "btn ghost danger", dataset: { i18n: "btn.cancel" } }, t("btn.cancel"));

    const modal = el("div", { class: "modal-overlay", hidden: true }, [
        el("div", { class: "modal-card" }, [
            el("div", { class: "modal-head" }, [el("h3", { dataset: { i18n: "local_files.browser.title" } }, t("local_files.browser.title")), crumb]),
            list,
            el("div", { class: "modal-foot row" }, [closeBtn, upBtn, useBtn]),
        ]),
    ]);
    document.body.append(modal);

    async function go(path) {
        dir = path || "";
        let r;
        try {
            r = await api.browseFs(dir);
        } catch (e) {
            toast(e.message, true);
            return;
        }
        dir = r.path || "";
        crumb.textContent = dir || t("local_files.browser.crumb_root");
        upBtn.disabled = false;
        useBtn.disabled = !dir;
        list.replaceChildren(
            ...(r.entries || []).map(entry => {
                const row = el("button", { type: "button", class: "browse-row" }, [
                    el("span", { class: "browse-name" }, entry.name || entry.path),
                    entry.has_children ? el("span", { class: "chevron" }, "›") : null,
                ]);
                row.dataset.parent = r.parent || "";
                row.addEventListener("click", () => go(entry.path));
                return row;
            }),
        );
        list.dataset.parent = r.parent || "";
        if (!(r.entries || []).length) {
            list.append(el("p", { class: "muted" }, dir ? t("local_files.browser.no_subfolders") : t("local_files.browser.no_drives")));
        }
    }

    upBtn.addEventListener("click", () => go(list.dataset.parent || ""));
    closeBtn.addEventListener("click", () => {
        modal.hidden = true;
        document.body.style.overflow = "";
    });
    modal.addEventListener("click", e => {
        if (e.target === modal) modal.hidden = true;
        document.body.style.overflow = "";
    });
    useBtn.addEventListener("click", () => {
        if (!dir) return;
        modal.hidden = true;
        onPick?.(dir);
    });

    return {
        open(cb) {
            onPick = cb;
            modal.hidden = false;
            document.body.style.overflow = "hidden";
            go("");
        },
    };
}

/**
 * Renders and manages the Local Files station source UI component.
 * Allows file system browsing, root configuration, exclusion tree nesting, and sorting.
 *
 * @param {HTMLElement} main - The main container element to inject the card into
 * @param {object} ctx - Application context containing state and lifecycle hooks
 * @param {() => object} ctx.getState - Returns the global application state
 * @param {() => Promise<void>} [ctx.onSaved] - Optional callback triggered after successful mutations
 */
export function createLocalFiles(main, ctx) {
    const browser = createBrowser();
    const expanded = new Set(); // holds normalized tree folder paths that are currently expanded

    const rootsBox = el("div", { class: "roots" });
    const addFolderBtn = el("button", { type: "button", class: "btn ghost", dataset: { i18n: "local_files.add_folder" } }, t("local_files.add_folder"));
    const treeBox = el("div", { class: "tree" });
    const orderSelect = el("select", {
        "aria-label": "Play order",
        dataset: { i18nAriaLabel: "local_files.order" },
    }, optionList(ORDERS));
    const groupingSelect = el("select", {
        "aria-label": "Album grouping",
        dataset: { i18nAriaLabel: "local_files.grouping" },
    }, optionList(GROUPINGS));
    const repeatSelect = el("select", {
        "aria-label": "Repeat mode",
        dataset: { i18nAriaLabel: "local_files.repeat" },
    }, optionList(REPEATS));
    const saveBtn = el("button", { type: "button", class: "btn filled", dataset: { i18n: "btn.save" } }, t("btn.save"));
    const summaryEl = el("p", { class: "muted", hidden: true });
    const reshuffleBtn = el("button", {
        type: "button",
        class: "icon-btn",
        "aria-label": "Reshuffle",
        dataset: { i18nAriaLabel: "local_files.reshuffle" },
        html: icons.shuffle,
    });

    let lastDetails = null;

    const station = createStationManager({
        api: api.localFiles,
        newStation,
        defaultNameKey: "local_files.default_station_name",
        ctx,
        queue: {
            getTitle: track => track.title || t("label.unknown_title"),
            getSubtitle: track => track.folder || null,
            getCoverUrl: track => track.cover_url,
            getSearchFields: track => [track.title || "", track.artist || "", track.folder || ""],
        },
        getSig: (details, track) => `${details?.track_count ?? -1}|${details?.index_version ?? -1}|${track?.title ?? ""}|${track?.artist ?? ""}`,
        onStationChange: s => {
            if (s) {
                s.roots ??= [];
                s.excluded ??= [];
            }
            renderRoots();
            renderTree();
            renderModes();
            renderSummary();
        },
        onSync: details => {
            lastDetails = details;
            renderSummary();
        },
    });

    const { stationSelect, onAirBtn, newBtn, duplicateBtn, renameBtn, deleteBtn, queueCount, searchInput, trackList } = station.els;

    const card = el("section", { class: "card", id: "local-files-card", hidden: true }, [
        el("h2", { dataset: { i18n: "local_files.title" } }, t("local_files.title")),
        summaryEl,
        el("div", { class: "stationbar row" }, [stationSelect, onAirBtn]),
        el("div", { class: "row stationtools" }, [newBtn, duplicateBtn, renameBtn, deleteBtn]),
        el("div", { class: "editor" }, [
            el("div", { class: "editor-section editor-section--folders" }, [
                el("label", { class: "field-label editor-section__label", dataset: { i18n: "local_files.source_folders" } }, t("local_files.source_folders")),
                el("div", { class: "editor-section__body" }, [
                    rootsBox,
                    el("div", { class: "row editor-section__actions" }, [addFolderBtn]),
                ]),
            ]),
            el("div", { class: "editor-section editor-section--exclude" }, [
                el("label", { class: "field-label editor-section__label", dataset: { i18n: "local_files.exclude_subfolders" } }, t("local_files.exclude_subfolders")),
                el("div", { class: "editor-section__body" }, [treeBox]),
            ]),
            el("div", { class: "editor-section editor-section--modes" }, [
                el("label", { class: "field-label editor-section__label", dataset: { i18n: "local_files.options" } }, t("local_files.options")),
                el("div", { class: "modes row editor-section__body" }, [
                    el("label", { class: "mode" }, [tNode("local_files.order"), orderSelect]),
                    el("label", { class: "mode", id: "grouping-field" }, [tNode("local_files.grouping"), groupingSelect]),
                    el("label", { class: "mode" }, [tNode("local_files.repeat"), repeatSelect]),
                ]),
            ]),
            el("div", { class: "row editor-foot" }, [saveBtn]),
        ]),
        el("div", { class: "queue" }, [
            el("div", { class: "queue-head row" }, [
                el("label", { class: "field-label", dataset: { i18n: "label.queue" } }, t("label.queue")),
                queueCount,
                reshuffleBtn,
            ]),
            searchInput,
            trackList,
        ]),
    ]);

    const sourcesCard = $("#sources", main)?.closest(".card");
    if (sourcesCard) sourcesCard.insertAdjacentElement("afterend", card);
    else main.append(card);

    function renderRoots() {
        const s = station.cur();
        if (!s) return;
        rootsBox.replaceChildren(
            ...(s.roots.length
                ? s.roots.map((root, i) => {
                    const remove = el("button", { type: "button", class: "x", "aria-label": "Remove folder" }, "✕");
                    remove.addEventListener("click", () => {
                        s.roots.splice(i, 1);
                        s.excluded = s.excluded.filter(ex => s.roots.some(r => within(ex, r)));
                        renderRoots();
                        renderTree();
                        renderSummary();
                    });
                    return el("div", { class: "root" }, [el("span", { class: "root-path" }, root), remove]);
                })
                : []),
        );
    }

    function treeRow(s, entry, depth) {
        const state = s.excluded.some(ex => norm(ex) === norm(entry.path))
            ? "excluded"
            : s.excluded.some(ex => within(entry.path, ex))
                ? "excluded"
                : s.excluded.some(ex => within(ex, entry.path))
                    ? "ancestor"
                    : "included";
        const isOpen = expanded.has(norm(entry.path));

        const box = el("input", { type: "checkbox" });
        box.checked = state !== "excluded";
        box.indeterminate = state === "ancestor";
        box.addEventListener("change", () => {
            if (box.checked) s.excluded = s.excluded.filter(ex => norm(ex) !== norm(entry.path));
            else s.excluded.push(entry.path);
            renderTree();
        });

        const children = el("div", {});

        const caret = el("button", {
            type: "button",
            class: "caret" + (entry.has_children ? "" : " empty"),
            "aria-label": entry.has_children ? (isOpen ? t("local_files.tree.collapse") : t("local_files.tree.expand")) : undefined,
            "aria-expanded": entry.has_children ? String(isOpen) : undefined,
        }, entry.has_children ? (isOpen ? "▾" : "▸") : "·");
        if (entry.has_children) {
            caret.addEventListener("click", () => {
                if (expanded.has(norm(entry.path))) expanded.delete(norm(entry.path));
                else expanded.add(norm(entry.path));
                renderTree();
            });
        }

        const row = el("div", { class: "tree-row", style: `padding-left:${depth * 16}px` }, [
            caret,
            el("label", { class: "tree-label" + (state === "excluded" ? " muted" : "") }, [
                box,
                el("span", {}, entry.name || entry.path),
            ]),
        ]);

        const wrap = el("div", {}, [row, children]);
        if (entry.has_children && isOpen) {
            api
                .browseFs(entry.path)
                .then(r => {
                    children.replaceChildren(...(r.entries || []).map(c => treeRow(s, c, depth + 1)));
                })
                .catch(() => { });
        }
        return wrap;
    }

    function renderTree() {
        const s = station.cur();
        if (!s) return;
        if (!s.roots.length) {
            treeBox.replaceChildren(el("p", { class: "muted" }, t("local_files.tree_empty_hint")));
            return;
        }
        treeBox.replaceChildren(...s.roots.map(root => treeRow(s, { name: root, path: root, has_children: true }, 0)));
    }

    function renderModes() {
        const s = station.cur();
        if (!s) return;
        orderSelect.value = s.order;
        groupingSelect.value = s.grouping;
        repeatSelect.value = s.repeat;
        $("#grouping-field", card).hidden = s.order !== "album";
    }

    function renderSummary() {
        const s = station.cur();
        if (!s) {
            summaryEl.hidden = true;
            return;
        }
        const folderCount = s.roots?.length ?? 0;
        const isOnAir = s.name === station.getActiveStation();
        const trackCount = isOnAir ? lastDetails?.track_count : null;

        const parts = [];
        if (trackCount != null) parts.push(`${trackCount} ${t("label.tracks")}`);
        parts.push(`${folderCount} ${t("local_files.summary.folders")}`);

        summaryEl.textContent = parts.join(" · ");
        summaryEl.hidden = false;
    }

    addFolderBtn.addEventListener("click", () => {
        browser.open(path => {
            const s = station.cur();
            if (!s.roots.some(r => norm(r) === norm(path))) s.roots.push(path);
            renderRoots();
            renderTree();
            renderSummary();
        });
    });
    orderSelect.addEventListener("change", () => {
        station.cur().order = orderSelect.value;
        renderModes();
    });
    groupingSelect.addEventListener("change", () => (station.cur().grouping = groupingSelect.value));
    repeatSelect.addEventListener("change", () => (station.cur().repeat = repeatSelect.value));

    saveBtn.addEventListener("click", async () => {
        try {
            const r = await station.save();
            toast(`${t("btn.save")} · ${r.track_count} ${t("label.tracks")}`);
        } catch {
            // Error handling already deferred to the station manager
        }
    });

    reshuffleBtn.addEventListener("click", async () => {
        try {
            await api.reshuffleLocal();
            station.loadQueue();
        } catch (e) {
            toast(e.message, true);
        }
    });

    function render() {
        const state = ctx.getState();
        const isActive = state?.sources?.active === "local_files";
        card.hidden = !isActive;
        if (!isActive) return;
        station.load();
        const details = state?.sources?.available?.find(s => s.name === "local_files")?.details;
        station.sync(details, state?.track);
    }

    return {
        render,
        invalidate: station.invalidate,
    };
}