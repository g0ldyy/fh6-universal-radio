import { toast } from "./toast.js";
import { prefs } from "./preferences.js";
import { el } from "./lib/dom.js";

const FALLBACK = "en";

/** * List of supported languages.
 * To add a new language, simply add its object to this array.
 * @type {Array<{code: string, label: string}>}
 */
export const SUPPORTED = [
    { code: "en", label: "🇬🇧 English" },
    { code: "fr", label: "🇫🇷 Français" }
];

let strings = {};
let currentLang = FALLBACK;
const listeners = new Set();

/**
 * Checks if a given language code is supported.
 * @param {string} langCode - The language code to check (e.g., "en", "fr").
 * @returns {boolean} True if supported, false otherwise.
 */
function isSupported(langCode) {
    return SUPPORTED.some(lang => lang.code === langCode);
}

/**
 * Detects the user's language based on a priority order:
 * 1. Local storage preference.
 * 2. Browser language settings.
 * 3. Fallback language ("en").
 * @returns {string} The detected language code.
 */
function detectLang() {
    const stored = prefs.language.get();
    if (stored && isSupported(stored)) return stored;

    const browser = navigator.language?.slice(0, 2).toLowerCase();
    if (browser && isSupported(browser)) return browser;

    return FALLBACK;
}

// Lang files use full-line "//" comments to group and annotate keys for
// translators — strip them before parsing, since JSON itself has no comment
// syntax. Each entry lives on its own line (no real line breaks inside a
// translated string, only "\n" escapes), so this is unambiguous.
function parseJsonWithComments(text) {
    const stripped = text
        .split("\n")
        .filter(line => !line.trim().startsWith("//"))
        .join("\n");
    return JSON.parse(stripped);
}

/**
 * Fetches and loads translation strings for a specific language.
 * Falls back to the default language if the request fails.
 * @param {string} lang - The language code to load.
 * @returns {Promise<Record<string, string>>} The translation strings object.
 */
async function loadStrings(lang) {
    try {
        const res = await fetch(`/lang/${lang}.json`);
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        return parseJsonWithComments(await res.text());
    } catch (e) {
        console.warn(`[i18n] Failed to load "${lang}", falling back to "${FALLBACK}".`, e);

        if (lang !== FALLBACK) {
            toast(`Language file "${lang}.json" not found. Falling back to English.`, true);
            return loadStrings(FALLBACK);
        }
        return {};
    }
}

/**
 * Initializes the internationalization (i18n) system.
 * Detects the language and pre-loads the matching translation strings.
 * @returns {Promise<void>}
 */
export async function initI18n() {
    currentLang = detectLang();
    strings = await loadStrings(currentLang);
}

/**
 * Changes the current language, stores it in local storage, and notifies
 * onLangChange() listeners so the UI can refresh in place (no page reload).
 * @param {string} lang - The new language code to apply.
 * @returns {Promise<void>}
 */
export async function setLang(lang) {
    if (!isSupported(lang)) return;
    if (lang === currentLang) return;
    prefs.language.set(lang);
    currentLang = lang;
    strings = await loadStrings(lang);
    listeners.forEach(fn => fn(lang));
}

/**
 * Retrieves the currently active language code.
 * @returns {string} The active language code.
 */
export function getLang() {
    return currentLang;
}

/**
 * Retrieves the list of all supported languages.
 * @returns {Array<{code: string, label: string}>} The list of supported languages.
 */
export function getSupportedLangs() {
    return SUPPORTED;
}

/**
 * Registers a listener function to be triggered when the language changes.
 * @param {Function} fn - The callback function to register.
 * @returns {Function} An unsubscribe function to remove the listener.
 */
export function onLangChange(fn) {
    listeners.add(fn);
    return () => listeners.delete(fn);
}

/**
 * Translates a key into the current language string and replaces placeholders.
 * @param {string} key - The translation key.
 * @param {Record<string, string>} [vars] - Optional variables to interpolate (e.g., {variable}).
 * @returns {string} The translated and formatted string, or the raw key if missing.
 * @example
 * t("online_radio.tuning", { name: "NRJ" }) // → "Tuning into NRJ…"
 */
export function t(key, vars) {
    let str = strings[key];

    if (str === undefined) {
        console.warn(`[i18n] Missing key: "${key}" (lang: ${currentLang})`);
        str = key; // display the raw key as fallback
    }

    if (vars) {
        str = str.replace(/\{(\w+)\}/g, (_, k) => vars[k] ?? `{${k}}`);
    }

    return str;
}

/**
 * A <span> carrying the translation key as data-i18n, so applyI18n() can
 * re-translate it in place after a language change — for static labels that
 * sit alongside other elements (e.g. a <select>) inside the same parent,
 * where setting the parent's textContent directly would wipe out the sibling.
 * @param {string} key
 * @returns {HTMLSpanElement}
 */
export function tNode(key) {
    return el("span", { dataset: { i18n: key } }, t(key));
}