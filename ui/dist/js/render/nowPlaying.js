import { setText } from "../dom.js";
import { fmt, progressRatio } from "../format.js";
import { icons } from "../icons.js";
import { t } from "../i18n.js";

export function activeSource(state) {
    return state?.sources?.available?.find(s => s.name === state?.sources?.active) || null;
}

export function renderNowPlaying(refs, state) {
    const track = state?.track || {};
    const source = activeSource(state);
    const playing = source?.playback_state === "playing";

    const hasArt = !!track.artwork_url;
    refs.art.classList.toggle("has-art", hasArt);
    if (hasArt && refs.img.getAttribute("src") !== track.artwork_url) {
        const isLocalUrl = /^https?:\/\/(localhost|127\.0\.0\.1|192\.168\.|10\.|172\.(1[6-9]|2\d|3[01])\.)/.test(track.artwork_url);
        const isExternal = track.artwork_url.startsWith("http") && !isLocalUrl;
        const src = isExternal
            ? `https://wsrv.nl/?url=${encodeURIComponent(track.artwork_url)}`
            : track.artwork_url;
        refs.img.crossOrigin = "anonymous";
        refs.img.src = src;
        refs.img.onload = () => {
            if (localStorage.getItem("fh6-dynamic-color") === "false") return;
            try {
                const color = extractDominantColor(refs.img);
                if (color) document.documentElement.style.setProperty("--accent", color);
            } catch {
                // CORS: ignore errors from cross-origin images
            }
        };
    }
    if (!hasArt) {
        refs.img.removeAttribute("src");
        document.documentElement.style.setProperty("--accent", "var(--color-sunset-yellow)");
    }
    if (refs.backdrop) refs.backdrop.style.backgroundImage = hasArt ? `url("${track.artwork_url}")` : "";

    setText(refs.title, track.title || t("np.nothing_playing"));
    setText(
        refs.artist,
        track.artist ? (track.album ? `${track.artist}` : track.artist) : "",
    );
    setText(refs.pos, fmt(track.position_ms));
    setText(refs.dur, fmt(track.duration_ms));
    refs.fill.style.width = progressRatio(track.position_ms, track.duration_ms) * 100 + "%";

    const want = playing ? "pause" : "play";
    if (refs.play.dataset.icon !== want) {
        refs.play.dataset.icon = want;
        refs.play.innerHTML = icons[want];
        refs.play.setAttribute("aria-label", playing ? t("np.pause") : t("np.play"));
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

    return `rgb(${r}, ${g}, ${b})`;
}