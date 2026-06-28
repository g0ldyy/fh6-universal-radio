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

// Toute source "à stations" expose exactement le même contrat REST :
// GET/PUT stations, POST activate, GET queue, POST play. Cette factory évite
// de réécrire ces cinq appels à chaque nouvelle source du même genre.
function sourceApi(name) {
	return {
		getStations: () => request(`/api/source/${name}/stations`),
		putStations: (stations, activeStation) =>
			request(`/api/source/${name}/stations`, {
				method: "PUT",
				body: { stations, active_station: activeStation },
			}),
		activateStation: stationName =>
			request(`/api/source/${name}/activate`, { method: "POST", body: { name: stationName } }),
		getQueue: () => request(`/api/source/${name}/queue`),
		playIndex: index => request(`/api/source/${name}/play`, { method: "POST", body: { index } }),
	};
}

export const api = {
	getState: () => request("/api/state"),
	getConfig: () => request("/api/config"),
	putConfig: patch => request("/api/config", { method: "PUT", body: patch }),
	reloadConfig: () => request("/api/config/reload", { method: "POST" }),
	switchSource: source => request("/api/source/switch", { method: "POST", body: { source } }),
	transport: (source, action) => request(`/api/source/${source}/${action}`, { method: "POST" }),
	castOnlineRadio: (url, opts = {}) =>
		request("/api/source/online_radio/cast", { method: "POST", body: { url, ...opts } }),
	setGain: gain => request("/api/options", { method: "POST", body: { output_gain: gain } }),
	getExternalAudio: () => request("/api/external_audio/devices"),
	putExternalAudio: config => request("/api/external_audio/config", { method: "PUT", body: config }),
	getDeps: () => request("/api/deps"),

	// Local Files
	browseFs: path => request("/api/fs/browse", { method: "POST", body: { path: path || "" } }),
	reshuffleLocal: () => request("/api/source/local_files/reshuffle", { method: "POST" }),
	localFiles: sourceApi("local_files"),

	// YouTube Music
	youtubeMusic: {
		...sourceApi("youtube_music"),
		cast: url => request("/api/source/youtube_music/cast", { method: "POST", body: { url } }),
		shuffle: shuffle => request("/api/source/youtube_music/shuffle", { method: "POST", body: { shuffle } }),
	},

	// Jellyfin
	jellyfin: {
		...sourceApi("jellyfin"),
		cast: (playlistId, useFavorites) =>
			request("/api/source/jellyfin/cast", { method: "POST", body: { playlist_id: playlistId, use_favorites: useFavorites } }),
		shuffle: shuffle => request("/api/source/jellyfin/shuffle", { method: "POST", body: { shuffle } }),
	},
};