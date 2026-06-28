// Single source of truth for every localStorage key the UI reads/writes.
// Add a new preference by adding one row here — no string literal should
// ever be typed a second time at the call site.
const PREFS = {
    language: { key: "fh6-language", type: "string", default: null },
    dynamicColor: { key: "fh6-dynamic-color", type: "bool", default: true },
    perfMode: { key: "fh6-perf-mode", type: "bool", default: false },
    viewMode: { key: "fh6-view-mode", type: "string", default: "default" },
    themeMode: { key: "fh6-theme-mode", type: "string", default: "dark" },
    volume: { key: "fh6-volume", type: "number", default: null },
};

function read({ key, type, default: fallback }) {
    const raw = localStorage.getItem(key);
    if (raw === null) return fallback;
    if (type === "bool") return raw === "true";
    if (type === "number") return parseFloat(raw);
    return raw;
}

function write({ key }, value) {
    try {
        localStorage.setItem(key, String(value));
    } catch {
        // private mode / quota exceeded — preferences are best-effort
    }
}

/**
 * Typed getter/setter pairs for every UI preference, keyed by name.
 * @example prefs.viewMode.get() // "default"
 * @example prefs.themeMode.set("dark")
 */
export const prefs = Object.fromEntries(
    Object.entries(PREFS).map(([name, pref]) => [
        name,
        {
            get: () => read(pref),
            set: value => write(pref, value),
        },
    ]),
);