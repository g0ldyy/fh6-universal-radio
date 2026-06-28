import { el, highlightText } from "../lib/dom.js";
import { t } from "../i18n.js";
import { translateLoadingPlaceholder } from "../lib/format.js";

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
 */
export function renderTrackQueue(trackList, queue, search, opts) {
    const fold = s => (s || "").normalize("NFD").replace(/\p{Diacritic}/gu, "").toLowerCase();
    const terms = fold(search).split(/\s+/).filter(Boolean);
    // Highlighting matches the raw (non-folded) query against the raw text,
    // so it can't reuse `terms` above — see highlightText()'s doc comment.
    const rawTerms = (search || "").split(/\s+/).filter(Boolean);

    const rows = (queue.tracks || []).filter(track => {
        if (!terms.length) return true;
        const hay = fold(opts.getSearchFields(track).join(" "));
        return terms.every(w => hay.includes(w));
    });

    trackList.replaceChildren(
        ...rows.map(track => {
            const coverUrl = opts.getCoverUrl(track) || "";

            const coverImg = el("img", {
                class: "track-cover-img",
                src: coverUrl,
                alt: "",
                loading: "lazy",
                "aria-hidden": "true",
            });

            const isCurrent = track.index === queue.cursor;
            const coverWrap = el("div", { class: "track-cover" }, [coverImg]);
            if (!coverUrl) {
                coverWrap.dataset.noart = "1";
                coverWrap.append(
                    el("div", { class: "eq" }, [
                        el("span", { class: "eq-bar" }),
                        el("span", { class: "eq-bar" }),
                        el("span", { class: "eq-bar" }),
                    ])
                );
            }

            const title = translateLoadingPlaceholder(opts.getTitle(track), t) || t("label.unknown_title");
            const subtitle = translateLoadingPlaceholder(opts.getSubtitle(track), t);

            const infoWrap = el("div", { class: "track-info" }, [
                el("span", { class: "track-title" }, highlightText(title, rawTerms)),
                subtitle ? el("span", { class: "track-folder muted" }, highlightText(subtitle, rawTerms)) : null,
            ]);

            const li = el("li", {
                class: "track" + (isCurrent ? " current" : ""),
            }, [coverWrap, infoWrap]);

            li.addEventListener("click", () => opts.onTrackClick(track, track.index));

            return li;
        }),
    );

    if (!rows.length) {
        trackList.append(
            el("li", { class: "muted" }, terms.length ? t("label.no_matches") : t("label.queue_empty")),
        );
    }

    const current = trackList.querySelector(".track.current");
    if (current) {
        trackList.scrollTo({
            top: current.offsetTop - trackList.offsetTop - trackList.clientHeight / 2 + current.clientHeight / 2,
            behavior: "smooth",
        });
    }
}