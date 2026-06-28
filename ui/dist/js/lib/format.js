export function fmt(ms) {
  if (!ms || ms < 0) return "0:00";
  const total = Math.floor(ms / 1000);
  return `${Math.floor(total / 60)}:${String(total % 60).padStart(2, "0")}`;
}

export const clamp = (n, lo, hi) => Math.min(hi, Math.max(lo, n));

export const percent = ratio => `${Math.round(ratio * 100)}%`;

export const progressRatio = (positionMs, durationMs) =>
  durationMs && positionMs ? clamp(positionMs / durationMs, 0, 1) : 0;

export const db = value => `${Number(value).toFixed(1)} dB`;

// True for loopback/LAN hosts (the backend's own HTTP server, mainly) — those
// URLs are reachable directly and don't need the wsrv.nl proxy used for
// external artwork/favicons (which also dodges hotlink/CORS restrictions).
const LOCAL_HOST_RE = /^https?:\/\/(localhost|127\.0\.0\.1|192\.168\.|10\.|172\.(1[6-9]|2\d|3[01])\.)/;
export const isLocalUrl = url => LOCAL_HOST_RE.test(url || "");

/**
 * The backend can send the literal placeholder string "(loading)" as track
 * metadata (title/artist/song history) while it's still fetching real info
 * (e.g. YouTube). That string is hardcoded server-side and never goes through
 * i18n, so it always shows up in English regardless of the chosen language.
 *
 * This is a best-effort fix from the front-end only: it pattern-matches the
 * placeholder (case/space insensitive) and swaps it for a translated string.
 * If the backend ever changes that exact wording, this simply stops matching
 * and the original (untranslated) text is shown again, exactly like today.
 *
 * @param {string|null|undefined} value
 * @param {(key: string) => string} t - the i18n translate function
 * @returns {string|null|undefined} the translated placeholder, or `value` unchanged
 */
export function translateLoadingPlaceholder(value, t) {
  if (typeof value === "string" && value.trim().toLowerCase() === "(loading)") {
    return t("label.loading");
  }
  return value;
}
