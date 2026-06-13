import { api } from "../api.js";
import { $, el } from "../dom.js";
import { toast } from "../toast.js";

const fold = s => (s || "").normalize("NFD").replace(/\p{Diacritic}/gu, "").toLowerCase();

export function createTidal(main, ctx) {
  let queue = { cursor: 0, tracks: [] };
  let search = "";

  const container = $("#tidal-queue");
  const countEl = $("#tidal-queue-count");
  const searchInput = $("#tidal-queue-search");
  const trackList = $("#tidal-track-list");

  async function loadQueue() {
    try {
      queue = await api.getTidalQueue();
    } catch {
      queue = { cursor: 0, tracks: [] };
    }
    renderQueue();
  }

  function renderQueue() {
    if (!queue.tracks || !queue.tracks.length) {
      container.hidden = true;
      return;
    }
    container.hidden = false;

    const terms = fold(search).split(/\s+/).filter(Boolean);
    const rows = queue.tracks.filter(t => {
      if (!terms.length) return true;
      const hay = fold(`${t.title} ${t.artist || ""} ${t.album || ""}`);
      return terms.every(w => hay.includes(w));
    });

    countEl.textContent = `${queue.tracks.length} tracks`;
    trackList.replaceChildren(
      ...rows.map(t => {
        const art = t.artwork_url
          ? el("img", {
              src: t.artwork_url,
              alt: "",
              style: "width: 40px; height: 40px; border-radius: var(--radius-sm); object-fit: cover; flex-shrink: 0;",
            })
          : el("div", {
              style: "width: 40px; height: 40px; border-radius: var(--radius-sm); background: var(--surface-raise); flex-shrink: 0; display: flex; align-items: center; justify-content: center; font-size: 20px; color: var(--text-muted);",
              html: "♪",
            });

        const li = el(
          "li",
          {
            class: "lf-track" + (t.index === queue.cursor ? " current" : ""),
            style: "flex-direction: row; align-items: center; gap: 12px;",
          },
          [
            art,
            el("div", { style: "display: flex; flex-direction: column; gap: 2px; flex-grow: 1; min-width: 0;" }, [
              el("span", { class: "lf-track-title", style: "white-space: nowrap; overflow: hidden; text-overflow: ellipsis;" }, t.artist ? `${t.title} — ${t.artist}` : t.title),
              t.album ? el("span", { class: "lf-track-folder muted", style: "white-space: nowrap; overflow: hidden; text-overflow: ellipsis;" }, t.album) : null,
            ]),
          ],
        );
        li.addEventListener("click", async () => {
          try {
            await api.playTidalIndex(t.index);
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
  }

  searchInput.addEventListener("input", () => {
    search = searchInput.value;
    renderQueue();
  });

  // --- lifecycle ------------------------------------------------------------
  let lastTrackCount = -1;
  let lastCurrentIndex = -1;

  function render() {
    const state = ctx.getState();
    const isActive = state?.sources?.active === "tidal";
    if (!isActive) {
      container.hidden = true;
      return;
    }

    const td = state?.sources?.available?.find(s => s.name === "tidal");
    const tc = td?.details?.track_count ?? -1;
    const ci = td?.details?.current_index ?? -1;

    if (tc !== lastTrackCount || ci !== lastCurrentIndex) {
      lastTrackCount = tc;
      lastCurrentIndex = ci;
      loadQueue();
    }
  }

  return {
    render,
    invalidate: () => {
      lastTrackCount = -1;
      lastCurrentIndex = -1;
    },
  };
}
