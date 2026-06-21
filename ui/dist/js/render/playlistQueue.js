import { el } from "../dom.js";
import { t } from "../i18n.js";

/**
 * Renders a shared track-queue UI used by Local Files, YouTube Music, and Jellyfin.
 * Each source has slightly different track shapes, so the caller provides small
 * accessor functions instead of this module assuming a fixed track structure.
 *
 * @param {HTMLUListElement} trackList - the <ul> to render into
 * @param {object} queue - { cursor, tracks }
 * @param {string} search - current search/filter text
 * @param {object} opts
 * @param {(track: object) => string} opts.getTitle - main line text
 * @param {(track: object) => string|null} opts.getSubtitle - secondary line text (artist/folder/url), or null to omit
 * @param {(track: object) => string} opts.getCoverUrl - cover image URL, or "" for none
 * @param {(track: object) => string[]} opts.getSearchFields - strings to match against the search filter
 * @param {(track: object, index: number) => void} opts.onTrackClick - called when a row is clicked
 * @param {string} opts.emptyKey - i18n key for "queue is empty"
 * @param {string} opts.noMatchesKey - i18n key for "no matches"
 * @param {string} opts.unknownTitleKey - i18n key for missing title fallback
 */
export function renderTrackQueue(trackList, queue, search, opts) {
    const fold = s => (s || "").normalize("NFD").replace(/\p{Diacritic}/gu, "").toLowerCase();
    const terms = fold(search).split(/\s+/).filter(Boolean);

    const rows = (queue.tracks || []).filter(track => {
        if (!terms.length) return true;
        const hay = fold(opts.getSearchFields(track).join(" "));
        return terms.every(w => hay.includes(w));
    });

    trackList.replaceChildren(
        ...rows.map(track => {
            const coverUrl = opts.getCoverUrl(track) || "";

            const coverImg = el("img", {
                class: "lf-track-cover-img",
                src: coverUrl,
                alt: "",
                loading: "lazy",
                "aria-hidden": "true",
            });

            const coverWrap = el("div", { class: "lf-track-cover" }, [coverImg]);
            if (!coverUrl) {
                coverWrap.dataset.noart = "1";
                coverWrap.append(
                    el("div", { class: "lf-eq" }, [
                        el("span", { class: "lf-eq-bar" }),
                        el("span", { class: "lf-eq-bar" }),
                        el("span", { class: "lf-eq-bar" }),
                    ])
                );
            }

            const title = opts.getTitle(track) || t(opts.unknownTitleKey);
            const subtitle = opts.getSubtitle(track);

            const infoWrap = el("div", { class: "lf-track-info" }, [
                el("span", { class: "lf-track-title" }, title),
                subtitle ? el("span", { class: "lf-track-folder muted" }, subtitle) : null,
            ]);

            const li = el("li", {
                class: "lf-track" + (track.index === queue.cursor ? " current" : ""),
            }, [coverWrap, infoWrap]);

            li.addEventListener("click", () => opts.onTrackClick(track, track.index));

            return li;
        }),
    );

    if (!rows.length) {
        trackList.append(
            el("li", { class: "muted" }, terms.length ? t(opts.noMatchesKey) : t(opts.emptyKey)),
        );
    }

    const current = trackList.querySelector(".lf-track.current");
    if (current) {
        trackList.scrollTo({
            top: current.offsetTop - trackList.offsetTop - trackList.clientHeight / 2 + current.clientHeight / 2,
            behavior: "smooth",
        });
    }
}