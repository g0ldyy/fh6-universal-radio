import { setText } from "../lib/dom.js";
import { fmt, progressRatio, translateLoadingPlaceholder, isLocalUrl } from "../lib/format.js";
import { icons } from "../icons.js";
import { t } from "../i18n.js";
import { prefs } from "../preferences.js";
import { ensureContrast, getPageBackgroundRgb } from "../lib/color.js";

export function activeSource(state) {
    return state?.sources?.available?.find(s => s.name === state?.sources?.active) || null;
}

// Vanilla Radio has no real track metadata to show, so the backend sends a
// static, hardcoded-in-English placeholder for title/artist instead. Map
// those known literal strings to their translated equivalents — same
// best-effort approach as translateLoadingPlaceholder().
const BACKEND_LITERALS = {
    "Vanilla Radio": "source.vanilla_radio",
    "In-game Audio": "source.in_game_audio",
    "Streaming via Spotify Connect": "source.spotify_streaming",
};

function translateBackendLiteral(value, t) {
    const key = BACKEND_LITERALS[value];
    return key ? t(key) : value;
}

export function renderNowPlaying(refs, state) {
    const track = state?.track || {};
    const source = activeSource(state);
    const playing = source?.playback_state === "playing";

    const hasArt = !!track.artwork_url;
    refs.art.classList.toggle("has-art", hasArt);
    refs.art.classList.toggle("is-playing", playing);

    let src = "";
    if (hasArt) {
        const isExternal = track.artwork_url.startsWith("http") && !isLocalUrl(track.artwork_url);
        src = isExternal
            ? `https://wsrv.nl/?url=${encodeURIComponent(track.artwork_url)}`
            : track.artwork_url;

        if (refs.img.getAttribute("src") !== src) {
            refs.img.crossOrigin = "anonymous";
            refs.img.src = src;
            refs.img.onload = () => {
                if (!prefs.dynamicColor.get()) return;
                try {
                    const color = extractDominantColor(refs.img);
                    if (color) document.documentElement.style.setProperty("--accent", color);
                } catch {
                    // CORS: ignore errors from cross-origin images
                }
            };
        }
    } else {
        if (refs.img.getAttribute("src") !== "../assets/default_artwork_2048.png") {
            refs.img.src = "../assets/default_artwork_2048.png";
        }
        // Only reset to the static fallback when there's genuinely nothing
        // loaded (no title either) — playback_state can briefly report
        // "stopped" between tracks (e.g. YouTube Music re-fetching the next
        // video's metadata) while a title is still set and playback is about
        // to resume, which caused a yellow flash if we reset on that alone.
        if (!track.title) {
            document.documentElement.style.setProperty("--accent", "var(--color-sunset-yellow)");
        }
    }

    if (refs.backdrop) refs.backdrop.style.backgroundImage = hasArt ? `url("${track.artwork_url}")` : "";

    const title = translateBackendLiteral(translateLoadingPlaceholder(track.title, t), t);
    const artist = translateBackendLiteral(translateLoadingPlaceholder(track.artist, t), t);
    const album = translateLoadingPlaceholder(track.album, t);

    setText(refs.title, title || t("now_playing.nothing_playing"));
    setText(refs.artist, artist ? (album ? `${artist} · ${album}` : artist) : "");
    setText(refs.pos, fmt(track.position_ms));
    setText(refs.dur, fmt(track.duration_ms));
    refs.fill.style.width = progressRatio(track.position_ms, track.duration_ms) * 100 + "%";

    const want = playing ? "pause" : "play";
    if (refs.play.dataset.icon !== want) {
        refs.play.dataset.icon = want;
        refs.play.innerHTML = icons[want];
        refs.play.setAttribute("aria-label", playing ? t("now_playing.pause") : t("now_playing.play"));
    }

    if (refs.mini) {
        setText(refs.mini.title, title || t("now_playing.nothing_playing"));
        setText(refs.mini.artist, artist ? (album ? `${artist} · ${album}` : artist) : "");

        if (hasArt) {
            if (refs.mini.art.getAttribute("src") !== src) {
                refs.mini.art.src = src;
            }
        } else {
            refs.mini.art.src = "../assets/default_artwork_128.png";
        }

        setText(refs.mini.pos, fmt(track.position_ms));
        setText(refs.mini.dur, fmt(track.duration_ms));
        refs.mini.fill.style.width = progressRatio(track.position_ms, track.duration_ms) * 100 + "%";

        if (refs.mini.play.dataset.icon !== want) {
            refs.mini.play.dataset.icon = want;
            refs.mini.play.innerHTML = icons[want];
            refs.mini.play.setAttribute("aria-label", playing ? t("now_playing.pause") : t("now_playing.play"));
        }
    }
}

export function extractDominantColor(imgEl) {
    const canvas = document.createElement("canvas");
    const SIZE = 64;
    canvas.width = SIZE;
    canvas.height = SIZE;
    const ctx = canvas.getContext("2d");
    ctx.drawImage(imgEl, 0, 0, SIZE, SIZE);

    const data = ctx.getImageData(0, 0, SIZE, SIZE).data;
    let r = 0, g = 0, b = 0, count = 0;

    for (let i = 0; i < data.length; i += 4) {
        const pr = data[i], pg = data[i + 1], pb = data[i + 2];
        const brightness = (pr + pg + pb) / 3;
        if (brightness < 30 || brightness > 220) continue;
        r += pr; g += pg; b += pb; count++;
    }

    if (!count) return null;
    r = Math.round(r / count);
    g = Math.round(g / count);
    b = Math.round(b / count);

    const boost = 1.4;
    r = Math.min(255, Math.round(r * boost));
    g = Math.min(255, Math.round(g * boost));
    b = Math.min(255, Math.round(b * boost));

    // Pale/light covers can otherwise produce an accent that's nearly
    // invisible as link/focus-ring text against the current background
    // (mostly a light-theme issue — dark theme rarely has this problem).
    const [cr, cg, cb] = ensureContrast([r, g, b], getPageBackgroundRgb());

    return `rgb(${cr}, ${cg}, ${cb})`;
}