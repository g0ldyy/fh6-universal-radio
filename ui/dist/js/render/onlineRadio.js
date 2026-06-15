import { api } from "../api.js";
import { $, el } from "../dom.js";
import { toast } from "../toast.js";
import { searchStations, registerClick } from "../radioBrowser.js";
import { t } from "../i18n.js";

// Lowercase + strip diacritics, so "jazz" matches "Jázz" in the station filter.
const fold = s => (s || "").normalize("NFD").replace(/\p{Diacritic}/gu, "").toLowerCase();

// Genre chips shown on the empty state and above the directory search.
const GENRES = () => [
    t("genre.lofi"),
    t("genre.jazz"),
    t("genre.rock"),
    t("genre.electronic"),
    t("genre.classical"),
    t("genre.news"),
    t("genre.pop"),
    t("genre.hip_hop"),
];

// A small curated country list for the directory filter (ISO 3166-1 alpha-2).
const COUNTRIES = () => [
  ["", t("online_radio.any_country")],
  ["US", "United States"],
  ["GB", "United Kingdom"],
  ["FR", "France"],
  ["DE", "Germany"],
  ["ES", "Spain"],
  ["IT", "Italy"],
  ["JP", "Japan"],
  ["BR", "Brazil"],
  ["CA", "Canada"],
  ["AU", "Australia"],
  ["NL", "Netherlands"],
];

const RECENTS_KEY = "or.recents.v1";
const RECENTS_CAP = 12;

const readJson = (key, fallback) => {
  try {
    return JSON.parse(localStorage.getItem(key)) ?? fallback;
  } catch {
    return fallback;
  }
};
const writeJson = (key, value) => {
  try {
    localStorage.setItem(key, JSON.stringify(value));
  } catch {
    // private mode / quota — recents are best-effort
  }
};

// Full station shape; mirrors the backend RadioStation so it round-trips via config.
const normalize = st => ({
  name: st.name || st.url || "New station",
  url: st.url || "",
  favicon: st.favicon || "",
  tags: st.tags || "",
  country: st.country || "",
  codec: st.codec || "",
  bitrate: st.bitrate || 0,
  uuid: st.uuid || "",
  favorite: !!st.favorite,
});

const badgeText = s =>
  [s.tags?.split(",")[0]?.trim(), s.country, s.bitrate ? `${s.bitrate}kbps` : ""]
    .filter(Boolean)
    .join(" · ");

// Online Radio card: cast bar, saved stations, recents, and a directory browser.
export function createOnlineRadio(main, ctx) {
  let recents = readJson(RECENTS_KEY, []);
  let stations = []; // full station objects, mirrored to config.online_radio.stations
  let cfgSig = null;
  let filter = "";
  let historySig = "";

  // --- cast bar -------------------------------------------------------------
  const urlInput = el("input", { type: "text", id: "or-url", placeholder: t("online_radio.url_placeholder"), autocomplete: "off" });
  const playBtn = el("button", { type: "submit", class: "btn filled" }, t("online_radio.play"));
  const saveBtn = el("button", { type: "button", class: "btn ghost" }, t("online_radio.save"));
  const castForm = el("form", { id: "or-cast", class: "row" }, [urlInput, playBtn, saveBtn]);

  // --- live track history ---------------------------------------------------
  const history = el("div", { class: "or-history", hidden: true });

  // --- tabs -----------------------------------------------------------------
  const mineTab = el("button", { type: "button", class: "or-tab on" }, t("online_radio.my_stations"));
  const discoverTab = el("button", { type: "button", class: "or-tab" }, t("online_radio.discover"));
  const tabs = el("div", { class: "or-tabs" }, [mineTab, discoverTab]);

  // --- my stations panel ----------------------------------------------------
  const filterInput = el("input", { type: "text", placeholder: t("online_radio.filter_placeholder"), autocomplete: "off" });
  const recentsWrap = el("div", { class: "or-recents", hidden: true });
  const recentsRow = el("div", { class: "or-chiprow" });
  const mineList = el("div", { class: "or-stations" });
  const minePanel = el("div", { class: "or-panel" }, [filterInput, recentsWrap, mineList]);
  recentsWrap.append(el("label", { class: "field-label" }, t("online_radio.recently_played")), recentsRow);

  // --- discover panel -------------------------------------------------------
  const dq = el("input", { type: "text", placeholder: t("online_radio.search_placeholder"), autocomplete: "off" });
  const dCountry = el("select", { "aria-label": "Country" }, COUNTRIES().map(([v, label]) => el("option", { value: v }, label)));
  const dSearchBtn = el("button", { type: "submit", class: "btn filled" }, t("online_radio.search"));
  const dForm = el("form", { class: "row or-discover-form" }, [dq, dCountry, dSearchBtn]);
  const genreRow = el("div", { class: "or-chiprow or-genres" }, genreChips(GENRES()));
  const dResults = el("div", { class: "or-stations" });

  function genreChips(list) {
    return list.map(g => {
      const chip = el("button", { type: "button", class: "or-chip" }, g);
      chip.addEventListener("click", () => {
        switchTab("discover");
        dq.value = g;
        runSearch();
      });
      return chip;
    });
  }
  const discoverPanel = el("div", { class: "or-panel", hidden: true }, [dForm, genreRow, dResults]);

  const card = el("section", { class: "card", id: "online-radio-card", hidden: true }, [
    el("h2", {}, t("online_radio.title")),
    el("p", { class: "muted" }, t("online_radio.description")),
    castForm,
    history,
    tabs,
    minePanel,
    discoverPanel,
  ]);

  const sourcesCard = $("#sources", main)?.closest(".card");
  if (sourcesCard) sourcesCard.insertAdjacentElement("afterend", card);
  else main.append(card);

  const editor = createEditor();

  // --- persistence ----------------------------------------------------------
  async function persist() {
    cfgSig = JSON.stringify(stations); // optimistic: skip the echo re-render
    try {
      await api.putConfig({ online_radio: { stations } });
      await ctx.onSaved?.();
    } catch (e) {
      toast(e.message, true);
    }
  }
  function pushRecent(st) {
    recents = [normalize(st), ...recents.filter(r => r.url !== st.url)].slice(0, RECENTS_CAP);
    writeJson(RECENTS_KEY, recents);
    renderRecents();
  }

  // --- playback -------------------------------------------------------------
  async function play(st) {
    if (!st?.url) return;
    try {
      await api.castOnlineRadio(st.url, { name: st.name || "", logo: st.favicon || "" });
      pushRecent(st);
      if (st.uuid) registerClick(st.uuid);
      toast(t("online_radio.tuning", { name: st.name || "stream" }));
    } catch (e) {
      toast(e.message, true);
    }
  }

  function addStation(st) {
    if (!st.url) return;
    if (stations.some(s => s.url === st.url)) return toast(t("online_radio.station_already_saved"));
    stations = [...stations, normalize(st)];
    renderMine();
    persist();
  }

  // --- rendering: my stations ----------------------------------------------
  function stationCard(s, idx) {
    const favBtn = el("button", {
      type: "button",
      class: "or-icon" + (s.favorite ? " on" : ""),
      title: s.favorite ? t("online_radio.unfavourite") : t("online_radio.favourite"),
    }, s.favorite ? "★" : "☆");
    favBtn.addEventListener("click", () => {
      s.favorite = !s.favorite;
      renderMine();
      persist();
    });
    const editBtn = el("button", { type: "button", class: "or-icon", title: t("online_radio.edit") }, "✎");
    editBtn.addEventListener("click", () =>
      editor.open(s, ({ name, url }) => {
        Object.assign(s, { name, url });
        renderMine();
        persist();
      }));
    const delBtn = el("button", { type: "button", class: "or-icon", title: t("online_radio.delete") }, "🗑");
    delBtn.addEventListener("click", () => {
      stations = stations.filter((_, i) => i !== idx);
      renderMine();
      persist();
      toast(t("online_radio.station_removed"));
    });
    const upBtn = el("button", { type: "button", class: "or-icon", title: t("online_radio.move_up"), disabled: idx === 0 }, "↑");
    upBtn.addEventListener("click", () => move(idx, -1));
    const downBtn = el("button", { type: "button", class: "or-icon", title: t("online_radio.move_down"), disabled: idx === stations.length - 1 }, "↓");
    downBtn.addEventListener("click", () => move(idx, 1));

    const playBtn0 = el("button", { type: "button", class: "btn filled or-play" }, t("online_radio.play"));
    playBtn0.addEventListener("click", () => play(s));

    return el("div", { class: "or-card" }, [
      stationLogo(s.favicon),
      el("div", { class: "or-card-main" }, [
        el("span", { class: "or-name" }, s.name || s.url),
        badgeText(s) ? el("span", { class: "or-badges muted" }, badgeText(s)) : null,
      ]),
      el("div", { class: "or-card-actions" }, [playBtn0, favBtn, editBtn, upBtn, downBtn, delBtn]),
    ]);
  }

  function move(idx, dir) {
    const next = idx + dir;
    if (next < 0 || next >= stations.length) return;
    [stations[idx], stations[next]] = [stations[next], stations[idx]];
    renderMine();
    persist();
  }

  function matches(s) {
    if (!filter) return true;
    const f = fold(filter);
    return fold(s.name).includes(f) || fold(s.tags).includes(f) || fold(s.country).includes(f);
  }

  function renderMine() {
    const order = stations
      .map((s, i) => ({ s, i }))
      .filter(({ s }) => matches(s))
      .sort((a, b) => (b.s.favorite ? 1 : 0) - (a.s.favorite ? 1 : 0));

    if (!stations.length) return void mineList.replaceChildren(emptyState());
    if (!order.length) {
      return void mineList.replaceChildren(el("p", { class: "muted" }, t("online_radio.no_filter_match")));
    }
    mineList.replaceChildren(...order.map(({ s, i }) => stationCard(s, i)));
  }

  function emptyState() {
    return el("div", { class: "or-empty" }, [
      el("p", {}, t("online_radio.no_stations")),
      el("p", { class: "muted" }, t("online_radio.no_stations_hint")),
      el("div", { class: "or-chiprow" }, genreChips(GENRES().slice(0, 6))),
    ]);
  }

  function renderRecents() {
    recentsWrap.hidden = !recents.length;
    recentsRow.replaceChildren(
      ...recents.map(r => {
        const chip = el("button", { type: "button", class: "or-recent", title: `${t("online_radio.play")} ${r.name}` }, [
          stationLogo(r.favicon, true),
          el("span", { class: "or-recent-name" }, r.name || r.url),
        ]);
        chip.addEventListener("click", () => play(r));
        return chip;
      }),
    );
  }

  // --- rendering: discover --------------------------------------------------
  function resultCard(s) {
    const playBtn0 = el("button", { type: "button", class: "btn filled or-play" }, t("online_radio.play"));
    playBtn0.addEventListener("click", () => play(s));
    const addBtn = el("button", { type: "button", class: "btn ghost" }, t("online_radio.add"));
    addBtn.addEventListener("click", () => addStation(s));
    return el("div", { class: "or-card" }, [
      stationLogo(s.favicon),
      el("div", { class: "or-card-main" }, [
        el("span", { class: "or-name" }, s.name),
        badgeText(s) ? el("span", { class: "or-badges muted" }, badgeText(s)) : null,
      ]),
      el("div", { class: "or-card-actions" }, [playBtn0, addBtn]),
    ]);
  }

  let searchToken = 0;
  async function runSearch() {
    const token = ++searchToken;
    dResults.replaceChildren(el("p", { class: "muted" }, t("online_radio.searching")));
    try {
      const rows = await searchStations({ name: dq.value.trim(), country: dCountry.value, limit: 40 });
      if (token !== searchToken) return;
      if (!rows.length) {
        return void dResults.replaceChildren(el("p", { class: "muted" }, t("online_radio.no_results")));
      }
      dResults.replaceChildren(...rows.map(resultCard));
    } catch (e) {
      if (token !== searchToken) return;
      const retry = el("button", { type: "button", class: "btn ghost" }, t("online_radio.retry"));
      retry.addEventListener("click", runSearch);
      dResults.replaceChildren(el("div", { class: "or-empty" }, [el("p", { class: "muted" }, e.message), retry]));
    }
  }

  function switchTab(next) {
    mineTab.classList.toggle("on", next === "mine");
    discoverTab.classList.toggle("on", next === "discover");
    minePanel.hidden = next !== "mine";
    discoverPanel.hidden = next !== "discover";
  }

  // --- live track history (ICY titles, from /api/state details) -------------
  function renderHistory(items) {
    const rows = items.slice(0, 6);
    const sig = rows.join("");
    if (sig === historySig) return;
    historySig = sig;
    history.hidden = !rows.length;
    if (!rows.length) return;
    history.replaceChildren(
      el("label", { class: "field-label" }, t("online_radio.track_history")),
      el("ul", { class: "or-history-list" }, rows.map(row => el("li", {}, row))),
    );
  }

  // --- events ---------------------------------------------------------------
  castForm.addEventListener("submit", e => {
    e.preventDefault();
    const url = urlInput.value.trim();
    if (!url) return;
    play({ name: url, url });
    urlInput.value = "";
  });
  saveBtn.addEventListener("click", () => {
    const url = urlInput.value.trim();
    if (!url) return toast(t("online_radio.url_required_save"), true);
    editor.open({ name: "", url }, st => {
      addStation(st);
      urlInput.value = "";
    });
  });
  mineTab.addEventListener("click", () => switchTab("mine"));
  discoverTab.addEventListener("click", () => switchTab("discover"));
  filterInput.addEventListener("input", () => {
    filter = filterInput.value;
    renderMine();
  });
  dForm.addEventListener("submit", e => {
    e.preventDefault();
    runSearch();
  });

  // --- lifecycle ------------------------------------------------------------
  function syncFromConfig() {
    const cfgStations = ctx.getConfig()?.online_radio?.stations || [];
    const sig = JSON.stringify(cfgStations);
    if (sig === cfgSig) return;
    cfgSig = sig;
    stations = cfgStations.map(normalize);
    renderMine();
  }

  let inited = false;
  function render() {
    const state = ctx.getState();
    const isActive = state?.sources?.active === "online_radio";
    card.hidden = !isActive;
    if (!isActive) return;
    if (!inited) {
      renderRecents();
      inited = true;
    }
    syncFromConfig();
    const details = state?.sources?.available?.find(s => s.name === "online_radio")?.details;
    renderHistory(details?.song_history || []);
  }

  return {
    render,
    invalidate: () => {
      cfgSig = null;
    },
  };
}

function stationLogo(url, small = false) {
  const cls = "or-logo" + (small ? " small" : "");
  if (!url) return el("div", { class: cls + " placeholder", "aria-hidden": "true" }, "♪");
  const img = el("img", { class: cls, alt: "", loading: "lazy", src: `https://wsrv.nl/?url=${encodeURIComponent(url)}&w=64&h=64&fit=cover` });
  img.addEventListener("error", () => img.replaceWith(el("div", { class: cls + " placeholder", "aria-hidden": "true" }, "♪")));
  return img;
}

// Modal for adding / editing a station (name + URL). Replaces window.prompt().
function createEditor() {
  let onSave = null;
  const nameInput = el("input", { type: "text", placeholder: t("online_radio.station_name"), autocomplete: "off" });
  const urlInput = el("input", { type: "text", placeholder: t("online_radio.stream_url"), autocomplete: "off" });
  const saveBtn = el("button", { type: "button", class: "btn filled" }, t("online_radio.save"));
  const cancelBtn = el("button", { type: "button", class: "btn ghost" }, t("online_radio.cancel") ?? "Cancel");
  const title = el("h3", {}, t("online_radio.save_station"));

  const modal = el("div", { class: "or-modal", hidden: true }, [
    el("div", { class: "or-modal-card" }, [
      el("div", { class: "or-modal-head" }, [title]),
      el("label", { class: "field-label" }, t("online_radio.station_name")),
      nameInput,
      el("label", { class: "field-label" }, t("online_radio.stream_url")),
      urlInput,
      el("div", { class: "or-modal-foot row" }, [saveBtn, cancelBtn]),
    ]),
  ]);
  document.body.append(modal);

  const close = () => (modal.hidden = true);
  const submit = () => {
    const name = nameInput.value.trim();
    const url = urlInput.value.trim();
    if (!url) return toast(t("online_radio.url_required"), true);
    close();
    onSave?.({ name: name || url, url });
  };

  cancelBtn.addEventListener("click", close);
  saveBtn.addEventListener("click", submit);
  modal.addEventListener("click", e => e.target === modal && close());
  [nameInput, urlInput].forEach(i => i.addEventListener("keydown", e => e.key === "Enter" && submit()));

  return {
    open(station, cb) {
      onSave = cb;
      title.textContent = station.url && station.name ? t("online_radio.edit_station") : t("online_radio.save_station");
      nameInput.value = station.name || "";
      urlInput.value = station.url || "";
      modal.hidden = false;
      (station.name ? urlInput : nameInput).focus();
    },
  };
}
