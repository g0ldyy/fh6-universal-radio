// "Station pack" = a shareable subset of the config: just the saved YouTube
// Music stations. Unlike the full settings backup, it carries no
// machine-specific paths or credentials, so a pack exported by one player
// can be imported by another to pick up their curated playlists in one shot.
const PACK_SECTIONS = ["youtube_music", "soundcloud"];

export function buildStationPack(cfg, targetSection = null) {
    const pack = { _meta: { type: "fh6-radio-station-pack", version: 1 } };
    const sections = targetSection ? [targetSection] : PACK_SECTIONS;
    
    for (const section of sections) {
        pack[section] = { stations: cfg?.[section]?.stations || [] };
    }
    return pack;
}

/**
 * Merges a station pack into the current config, skipping stations whose
 * `url` already exists so importing twice doesn't create duplicates.
 * @returns {{ patch: object, added: number }}
 */
export function mergeStationPack(cfg, pack, targetSection = null) {
    const patch = {};
    let added = 0;
    const sections = targetSection ? [targetSection] : PACK_SECTIONS;
    
    for (const section of sections) {
        const existing = cfg?.[section]?.stations || [];
        const incoming = Array.isArray(pack?.[section]?.stations) ? pack[section].stations : [];
        const existingUrls = new Set(existing.map(s => s.url));
        const fresh = incoming.filter(s => s?.url && !existingUrls.has(s.url));
        added += fresh.length;
        patch[section] = { stations: [...existing, ...fresh] };
    }
    return { patch, added };
}
