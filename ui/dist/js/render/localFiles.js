import { api } from "../api.js";
import { $, el } from "../dom.js";
import { icons } from "../icons.js";
import { toast } from "../toast.js";

const ORDERS = [
	["shuffle", "Shuffle"],
	["album", "Albums"],
	["name", "Name (A–Z)"],
	["folder", "Folder"],
];
const GROUPINGS = [
	["folder", "By folder"],
	["tags", "By tags"],
];
const REPEATS = [
	["all", "Repeat all"],
	["one", "Repeat one"],
	["off", "No repeat"],
];

// Lowercase + strip diacritics, so "ete" matches "Été" in queue search.
const fold = s => (s || "").normalize("NFD").replace(/\p{Diacritic}/gu, "").toLowerCase();

// Path helpers: case-insensitive, separator-agnostic.
const norm = p => (p || "").replace(/\\/g, "/").replace(/\/+$/, "").toLowerCase();
const within = (child, parent) => {
	const c = norm(child);
	const a = norm(parent);
	return c === a || c.startsWith(a + "/");
};

function newStation(name) {
	return { name, roots: [], excluded: [], recursive: true, order: "shuffle", grouping: "folder", repeat: "all" };
}

// Lazy folder browser modal. open(onPick) resolves the chosen folder path.
function createBrowser() {
	let dir = "";
	let onPick = null;

	const list = el("div", { class: "lf-browse-list" });
	const crumb = el("div", { class: "lf-browse-crumb muted" });
	const useBtn = el("button", { type: "button", class: "btn filled" }, "Use this folder");
	const upBtn = el("button", { type: "button", class: "btn ghost" }, "Up");
	const closeBtn = el("button", { type: "button", class: "btn ghost" }, "Cancel");

	const modal = el("div", { class: "lf-modal", hidden: true }, [
		el("div", { class: "lf-modal-card" }, [
			el("div", { class: "lf-modal-head" }, [el("h3", {}, "Choose a folder"), crumb]),
			list,
			el("div", { class: "lf-modal-foot row" }, [closeBtn, upBtn, useBtn]),
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
		crumb.textContent = dir || "This PC";
		upBtn.disabled = false;
		useBtn.disabled = !dir;
		list.replaceChildren(
			...(r.entries || []).map(entry => {
				const row = el("button", { type: "button", class: "lf-browse-row" }, [
					el("span", { class: "lf-browse-name" }, entry.name || entry.path),
					entry.has_children ? el("span", { class: "lf-chevron" }, "›") : null,
				]);
				row.dataset.parent = r.parent || "";
				row.addEventListener("click", () => go(entry.path));
				return row;
			}),
		);
		list.dataset.parent = r.parent || "";
		if (!(r.entries || []).length) { list.append(el("p", { class: "muted" }, dir ? "No subfolders here." : "No drives found.")); }
	}

	upBtn.addEventListener("click", () => go(list.dataset.parent || ""));
	closeBtn.addEventListener("click", () => (modal.hidden = true));
	modal.addEventListener("click", e => {
		if (e.target === modal) modal.hidden = true;
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
			go("");
		},
	};
}

// Local Files card: stations (presets), folder selection, ordering and queue.
export function createLocalFiles(main, ctx) {
	let stations = [];
	let activeStation = "";
	let selected = 0;
	let queue = { cursor: 0, tracks: [] };
	let search = "";
	let loaded = false;
	const expanded = new Set(); // expanded tree folder paths (normalized)

	const browser = createBrowser();

	const stationSelect = el("select", { id: "lf-station", "aria-label": "Station preset" });
	const onAirBtn = el("button", { type: "button", class: "btn filled" }, "Set on air");
	const newBtn = el("button", { type: "button", class: "btn ghost" }, "New");
	const renameBtn = el("button", { type: "button", class: "btn ghost" }, "Rename");
	const deleteBtn = el("button", { type: "button", class: "btn ghost" }, "Delete");

	const rootsBox = el("div", { class: "lf-roots" });
	const addFolderBtn = el("button", { type: "button", class: "btn ghost" }, "Add folder");
	const treeBox = el("div", { class: "lf-tree" });
	const orderSelect = el("select", { "aria-label": "Play order" },
		ORDERS.map(([v, t]) => el("option", { value: v }, t)));
	const groupingSelect = el("select", { "aria-label": "Album grouping" },
		GROUPINGS.map(([v, t]) => el("option", { value: v }, t)));
	const repeatSelect = el("select", { "aria-label": "Repeat mode" },
		REPEATS.map(([v, t]) => el("option", { value: v }, t)));
	const saveBtn = el("button", { type: "button", class: "btn filled" }, "Save");

	const searchInput = el("input", { type: "text", placeholder: "Search the queue…", autocomplete: "off" });
	const reshuffleBtn = el("button", { type: "button", class: "icon-btn", "aria-label": "Reshuffle", html: icons.shuffle });
	const trackList = el("ul", { class: "lf-tracklist" });
	const queueCount = el("span", { class: "muted" });

	const card = el("section", { class: "card", id: "local-files-card", hidden: true }, [
		el("h2", {}, "Local Files"),
		el("div", { class: "lf-stationbar row" }, [stationSelect, onAirBtn]),
		el("div", { class: "row lf-stationtools" }, [newBtn, renameBtn, deleteBtn]),
		el("div", { class: "lf-editor" }, [
			el("div", { class: "lf-editor-section lf-editor-section--folders" }, [
				el("label", { class: "field-label lf-editor-section__label" }, "Source folders"),
				el("div", { class: "lf-editor-section__body" }, [
					rootsBox,
					el("div", { class: "row lf-editor-section__actions" }, [addFolderBtn]),
				]),
			]),
			el("div", { class: "lf-editor-section lf-editor-section--exclude" }, [
				el("label", { class: "field-label lf-editor-section__label" }, "Exclude subfolders (uncheck to skip)"),
				el("div", { class: "lf-editor-section__body" }, [
					treeBox,
				]),
			]),
			el("div", { class: "lf-editor-section lf-editor-section--modes" }, [
				el("label", { class: "field-label lf-editor-section__label" }, "Playback options"),
				el("div", { class: "lf-modes row lf-editor-section__body" }, [
					el("label", { class: "lf-mode" }, ["Order", orderSelect]),
					el("label", { class: "lf-mode", id: "lf-grouping-field" }, ["Grouping", groupingSelect]),
					el("label", { class: "lf-mode" }, ["Repeat", repeatSelect]),
				]),
			]),

			el("div", { class: "row lf-editor-foot" }, [saveBtn]),
		]),
		el("div", { class: "lf-queue" }, [
			el("div", { class: "lf-queue-head row" }, [
				el("label", { class: "field-label" }, "Queue"),
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

	const cur = () => stations[selected];

	async function load(force = false) {
		if (loaded && !force) return;
		try {
			const r = await api.getLocalStations();
			stations = Array.isArray(r.stations) && r.stations.length ? r.stations : [newStation("My Music")];
			stations.forEach(s => {
				s.roots ??= [];
				s.excluded ??= [];
			});
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
			queue = await api.getLocalQueue();
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

	function renderRoots() {
		const s = cur();
		rootsBox.replaceChildren(
			...(s.roots.length
				? s.roots.map((root, i) => {
					const remove = el("button", { type: "button", class: "lf-x", "aria-label": "Remove folder" }, "✕");
					remove.addEventListener("click", () => {
						s.roots.splice(i, 1);
						s.excluded = s.excluded.filter(ex => s.roots.some(r => within(ex, r)));
						renderRoots();
						renderTree();
					});
					return el("div", { class: "lf-root" }, [el("span", { class: "lf-root-path" }, root), remove]);
				})
				: [el("p", { class: "muted" }, "No folders yet — add one to build this station.")]),
		);
	}

	function excludedState(s, path) {
		const n = norm(path);
		for (const ex of s.excluded) {
			const e = norm(ex);
			if (e === n) return "self";
			if (n.startsWith(e + "/")) return "ancestor";
		}
		return "none";
	}
	const hasExcludedDescendant = (s, path) =>
		s.excluded.some(ex => norm(ex) !== norm(path) && norm(ex).startsWith(norm(path) + "/"));

	function treeRow(s, entry, depth) {
		const state = excludedState(s, entry.path);
		const box = el("input", { type: "checkbox" });
		box.checked = state === "none";
		box.disabled = state === "ancestor";
		box.indeterminate = state === "none" && hasExcludedDescendant(s, entry.path);
		box.addEventListener("change", () => {
			if (box.checked) {
				s.excluded = s.excluded.filter(ex => norm(ex) !== norm(entry.path));
			} else {
				s.excluded = s.excluded.filter(ex => !within(ex, entry.path));
				s.excluded.push(entry.path);
			}
			renderTree();
		});

		const children = el("div", { class: "lf-tree-children" });
		const isOpen = expanded.has(norm(entry.path));
		const caret = el("button", {
			type: "button",
			class: "lf-caret" + (entry.has_children ? "" : " empty"),
			"aria-label": "Expand",
		}, entry.has_children ? (isOpen ? "▾" : "▸") : "·");
		if (entry.has_children) {
			caret.addEventListener("click", () => {
				if (expanded.has(norm(entry.path))) expanded.delete(norm(entry.path));
				else expanded.add(norm(entry.path));
				renderTree();
			});
		}

		const row = el("div", { class: "lf-tree-row", style: `padding-left:${depth * 16}px` }, [
			caret,
			el("label", { class: "lf-tree-label" + (state === "ancestor" ? " muted" : "") }, [
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
		const s = cur();
		if (!s.roots.length) {
			treeBox.replaceChildren(el("p", { class: "muted" }, "Add a folder above to pick which subfolders to include."));
			return;
		}
		treeBox.replaceChildren(
			...s.roots.map(root => treeRow(s, { name: root, path: root, has_children: true }, 0)),
		);
	}

	function renderModes() {
		const s = cur();
		orderSelect.value = s.order;
		groupingSelect.value = s.grouping;
		repeatSelect.value = s.repeat;
		$("#lf-grouping-field").hidden = s.order !== "album";
	}

	function renderEditor() {
		if (!cur()) return;
		renderStations();
		renderRoots();
		renderTree();
		renderModes();
	}

	function renderQueue() {
		// Accent-insensitive, multi-word search: every word must appear somewhere
		// in title + artist + folder, in any order or position.
		const terms = fold(search).split(/\s+/).filter(Boolean);
		const rows = (queue.tracks || []).filter(t => {
			if (!terms.length) return true;
			const hay = fold(`${t.title} ${t.artist || ""} ${t.folder || ""}`);
			return terms.every(w => hay.includes(w));
		});
		queueCount.textContent = `${queue.tracks?.length || 0} tracks`;
		trackList.replaceChildren(
			...rows.map(t => {
				// Get cover (maybe for later) or add svg placeholder if not found, to avoid layout shift when it loads.
				const coverImg = el("img", {
					class: "lf-track-cover-img",
					src: t.cover_url || "",
					alt: "",
					loading: "lazy",
					"aria-hidden": "true",
				});
				const coverWrap = el("div", { class: "lf-track-cover" }, [coverImg]);
				if (!t.cover_url) coverWrap.dataset.noart = "1";

				const infoWrap = el("div", { class: "lf-track-info" }, [
					el("span", { class: "lf-track-title" }, t.title || "Titre inconnu"),
					t.folder ? el("span", { class: "lf-track-folder muted" }, t.folder) : null,
				]);

				const li = el("li", {
					class: "lf-track" + (t.index === queue.cursor ? " current" : ""),
				}, [coverWrap, infoWrap]);

				li.addEventListener("click", async () => {
					try {
						await api.playLocalIndex(t.index);
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
			trackList.append(
				el("li", { class: "muted" }, terms.length ? "No matches." : "Queue is empty."),
			);
		}

		const current = trackList.querySelector(".lf-track.current");
		if (current) trackList.scrollTo({ top: current.offsetTop - trackList.offsetTop - trackList.clientHeight / 2 + current.clientHeight / 2, behavior: "smooth" });
	}

	// --- events ---------------------------------------------------------------
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
	addFolderBtn.addEventListener("click", () => {
		browser.open(path => {
			const s = cur();
			if (!s.roots.some(r => norm(r) === norm(path))) s.roots.push(path);
			renderRoots();
			renderTree();
		});
	});
	orderSelect.addEventListener("change", () => {
		cur().order = orderSelect.value;
		renderModes();
	});
	groupingSelect.addEventListener("change", () => (cur().grouping = groupingSelect.value));
	repeatSelect.addEventListener("change", () => (cur().repeat = repeatSelect.value));

	saveBtn.addEventListener("click", async () => {
		try {
			const r = await api.putLocalStations(stations, activeStation);
			toast(`Saved · ${r.track_count} tracks`);
			await ctx.onSaved?.();
			loadQueue();
		} catch (e) {
			toast(e.message, true);
		}
	});
	onAirBtn.addEventListener("click", async () => {
		const name = cur().name;
		try {
			await api.putLocalStations(stations, name); // persist edits + set active
			await api.activateLocalStation(name); // switch source + play
			activeStation = name;
			renderStations();
			toast(`On air: ${name}`);
			await ctx.onSaved?.();
			loadQueue();
		} catch (e) {
			toast(e.message, true);
		}
	});
	searchInput.addEventListener("input", () => {
		search = searchInput.value;
		renderQueue();
	});
	reshuffleBtn.addEventListener("click", async () => {
		try {
			await api.reshuffleLocal();
			loadQueue();
		} catch (e) {
			toast(e.message, true);
		}
	});

	// --- lifecycle ------------------------------------------------------------
	let lastTrackCount = -1;
	let lastIndexVersion = -1;
	let lastTrackTitle = "";
	function render() {
		const state = ctx.getState();
		const isActive = state?.sources?.active === "local_files";
		card.hidden = !isActive;
		if (!isActive) return;
		load();
		// Refresh the queue (titles, artists, current-track highlight) when the
		// track count changes (rescan) or the metadata index advances.
		const lf = state?.sources?.available?.find(s => s.name === "local_files");
		const tc = lf?.details?.track_count ?? -1;
		const iv = lf?.details?.index_version ?? -1;
		const title = state?.track?.title ?? "";

		if (loaded && (tc !== lastTrackCount || iv !== lastIndexVersion || title !== lastTrackTitle)) {
			lastTrackCount = tc;
			lastIndexVersion = iv;
			lastTrackTitle = title;
			loadQueue();
		}
	}

	return {
		render,
		invalidate: () => {
			loaded = false;
		},
		reloadQueue: loadQueue,
	};
}
