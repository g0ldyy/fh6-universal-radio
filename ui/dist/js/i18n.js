import { toast } from "./toast.js";

const STORAGE_KEY = "fh6-language";
const FALLBACK = "en";

// To add a new language, simply add its object to this array:
export const SUPPORTED = [
    { code: "en", label: "🇬🇧 English" },
    { code: "fr", label: "🇫🇷 Français" },
    { code: "de", label: "🇩🇪 Deutsch (AI Generate)" },
    { code: "es", label: "🇪🇸 Español (AI Generate)" },
    { code: "it", label: "🇮🇹 Italiano (AI Generate)" },
    { code: "pt", label: "🇵🇹 Português (AI Generate)" },
    { code: "ja", label: "🇯🇵 日本語 (AI Generate)" },
    { code: "zh", label: "🇨🇳 中文 (AI Generate)" }
];

let strings = {};
let currentLang = FALLBACK;
const listeners = new Set();

/**
 * Helper to check if a language code is supported
 */
function isSupported(langCode) {
  return SUPPORTED.some(lang => lang.code === langCode);
}

/**
 * detect language in the following order:
 * 1. localStorage
 * 2. browser language
 * 3. Fallback "en"
 */
function detectLang() {
  const stored = localStorage.getItem(STORAGE_KEY);
  if (stored && isSupported(stored)) return stored;

  const browser = navigator.language?.slice(0, 2).toLowerCase();
  if (browser && isSupported(browser)) return browser;

  return FALLBACK;
}

/**
 * Load the translation strings for a given language.
 */
async function loadStrings(lang) {
  try {
    const res = await fetch(`/lang/${lang}.json`);
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    return await res.json();
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
 * init i18n: detect the language and load the strings.
 */
export async function initI18n() {
  currentLang = detectLang();
  strings = await loadStrings(currentLang);
}

/**
 * change the language and reload the page.
 * @param {string} lang — code ISO 639-1 ("en", "fr")
 */
export async function setLang(lang) {
  if (!isSupported(lang)) return;
  localStorage.setItem(STORAGE_KEY, lang);
  window.location.reload();
}

/** return the current language code (ISO 639-1) */
export function getLang() {
  return currentLang;
}

/** list of supported languages (ISO 639-1) */
export function getSupportedLangs() {
  return SUPPORTED;
}

/**
 * save a listener to be called when the language changes.
 * @param {Function} fn
 * @returns {Function} unsubscribe
 */
export function onLangChange(fn) {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

/**
 * Return the translated string for a given key.
 * Supports style interpolations {variable}.
 *
 * @param {string} key
 * @param {Record<string, string>} [vars]
 * @returns {string}
 *
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