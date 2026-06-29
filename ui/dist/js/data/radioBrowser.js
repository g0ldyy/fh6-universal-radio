// Thin client for the radio-browser.info community directory. Runs entirely in
// the browser: the API sends `Access-Control-Allow-Origin: *`, so no backend
// proxy is needed. No key, no account.

const FALLBACK_MIRRORS = [
  "https://de2.api.radio-browser.info",
  "https://nl1.api.radio-browser.info",
  "https://at1.api.radio-browser.info",
];

let mirror = null;
let mirrorPromise = null;

async function resolveMirror() {
  if (mirror) return mirror;
  if (mirrorPromise) return mirrorPromise;
  mirrorPromise = (async () => {
    try {
      const res = await fetch("https://all.api.radio-browser.info/json/servers", {
        headers: { accept: "application/json" },
      });
      if (!res.ok) throw new Error("servers");
      const names = (await res.json()).map(s => s && s.name).filter(Boolean);
      if (!names.length) throw new Error("empty");
      mirror = `https://${names[Math.floor(Math.random() * names.length)]}`;
    } catch {
      mirror = FALLBACK_MIRRORS[Math.floor(Math.random() * FALLBACK_MIRRORS.length)];
    }
    return mirror;
  })();
  return mirrorPromise;
}

async function call(path, params) {
  const base = await resolveMirror();
  const qs = params ? "?" + new URLSearchParams(params) : "";
  let res;
  try {
    res = await fetch(`${base}${path}${qs}`, { headers: { accept: "application/json" } });
  } catch {
    mirror = null; // a dead mirror shouldn't stick — re-pick on the next call
    mirrorPromise = null;
    throw new Error("Radio directory unavailable");
  }
  if (!res.ok) throw new Error("Radio directory error");
  return res.json();
}

// Map a directory record onto the station shape the rest of the UI uses
// (field names mirror the backend RadioStation / config so they round-trip 1:1).
export function normalizeStation(s) {
  return {
    name: (s.name || "").trim() || "Unknown station",
    url: s.url_resolved || s.url || "",
    favicon: s.favicon || "",
    tags: s.tags || "",
    country: s.countrycode || s.country || "",
    codec: s.codec || "",
    bitrate: Number(s.bitrate) || 0,
    uuid: s.stationuuid || "",
  };
}

export async function searchStations({ name = "", tag = "", country = "", limit = 40 } = {}) {
  const params = {
    limit: String(limit),
    order: "clickcount",
    reverse: "true",
    hidebroken: "true",
  };
  if (name) params.name = name;
  if (tag) params.tagList = tag;
  if (country) params.countrycode = country;
  const rows = await call("/json/stations/search", params);
  return (Array.isArray(rows) ? rows : []).map(normalizeStation).filter(s => s.url);
}

// Fire-and-forget: bumps the community click counter and resolves the playable URL.
export function registerClick(uuid) {
  if (!uuid) return Promise.resolve(null);
  return call(`/json/url/${encodeURIComponent(uuid)}`).catch(() => null);
}
