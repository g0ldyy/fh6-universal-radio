export async function request(path, { method = "GET", body } = {}) {
  const res = await fetch(path, {
    method,
    headers: body ? { "content-type": "application/json" } : {},
    body: body ? JSON.stringify(body) : undefined,
  });
  if (!res.ok) {
    const data = await res.json().catch(() => ({}));
    throw new Error(data.error || res.statusText);
  }
  return res.json().catch(() => ({}));
}

export const api = {
  getState: () => request("/api/state"),
  getConfig: () => request("/api/config"),
  putConfig: patch => request("/api/config", { method: "PUT", body: patch }),
  reloadConfig: () => request("/api/config/reload", { method: "POST" }),
  switchSource: source => request("/api/source/switch", { method: "POST", body: { source } }),
  transport: (source, action) => request(`/api/source/${source}/${action}`, { method: "POST" }),
  castYoutube: url => request("/api/source/youtube_music/cast", { method: "POST", body: { url } }),
  castOnlineRadio: url => request("/api/source/online_radio/cast", { method: "POST", body: { url } }),
  shuffleYoutube: shuffle =>
    request("/api/source/youtube_music/shuffle", { method: "POST", body: { shuffle } }),
  castJellyfin: playlistId =>
    request("/api/source/jellyfin/cast", { method: "POST", body: { playlist_id: playlistId } }),
  setGain: gain => request("/api/options", { method: "POST", body: { output_gain: gain } }),
  getExternalAudio: () => request("/api/external_audio/devices"),
  putExternalAudio: config => request("/api/external_audio/config", { method: "PUT", body: config }),
  getDeps: () => request("/api/deps"),
};
