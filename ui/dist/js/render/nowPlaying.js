import { setText } from "../dom.js";
import { fmt, progressRatio } from "../format.js";
import { icons } from "../icons.js";

const APPLE_MUSIC_FALLBACK =
  "data:image/svg+xml;utf8," +
  encodeURIComponent(
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">
      <defs>
        <linearGradient id="g" x1="48" x2="212" y1="230" y2="34" gradientUnits="userSpaceOnUse">
          <stop stop-color="#ff2d55"/>
          <stop offset=".46" stop-color="#ff375f"/>
          <stop offset="1" stop-color="#bf5af2"/>
        </linearGradient>
      </defs>
      <rect width="256" height="256" rx="56" fill="url(#g)"/>
      <path fill="#fff" d="M174 52v109.6c0 20.7-17.7 34.4-37.1 31.2-15.4-2.5-26-13.5-23.8-26.5 2.1-12.6 15.1-21 30.8-18.6 5.7.9 10.6 2.8 14.2 5.4V95.2L91.5 108v80.3c0 20.7-17.7 34.4-37.1 31.2-15.4-2.5-26-13.5-23.8-26.5 2.1-12.6 15.1-21 30.8-18.6 5.7.9 10.6 2.8 14.2 5.4V82.5c0-6.1 3.8-10.3 10-11.5l69-13.2c10.8-2.1 19.4 1.4 19.4 9.2Z"/>
    </svg>`,
  );

export function activeSource(state) {
  return state?.sources?.available?.find(s => s.name === state?.sources?.active) || null;
}

export function renderNowPlaying(refs, state) {
  const track = state?.track || {};
  const source = activeSource(state);
  const playing = source?.playback_state === "playing";

  const artUrl = track.artwork_url || (source?.name === "apple_music" ? APPLE_MUSIC_FALLBACK : "");
  const hasArt = !!artUrl;
  refs.art.classList.toggle("has-art", hasArt);
  if (hasArt && refs.img.getAttribute("src") !== artUrl) refs.img.src = artUrl;
  if (!hasArt) refs.img.removeAttribute("src");
  if (refs.backdrop) refs.backdrop.style.backgroundImage = hasArt ? `url("${artUrl}")` : "";

  setText(refs.title, track.title || "Nothing playing");
  setText(
    refs.artist,
    track.artist ? (track.album ? `${track.artist} · ${track.album}` : track.artist) : "",
  );
  setText(refs.pos, fmt(track.position_ms));
  setText(refs.dur, fmt(track.duration_ms));
  refs.fill.style.width = progressRatio(track.position_ms, track.duration_ms) * 100 + "%";

  const want = playing ? "pause" : "play";
  if (refs.play.dataset.icon !== want) {
    refs.play.dataset.icon = want;
    refs.play.innerHTML = icons[want];
    refs.play.setAttribute("aria-label", playing ? "Pause" : "Play");
  }
}
