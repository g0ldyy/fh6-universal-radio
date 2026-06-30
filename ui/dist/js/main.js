import { $ } from "./lib/dom.js";
import { api } from "./data/api.js";
import { connect } from "./data/events.js";
import { icons } from "./icons.js";
import { toast } from "./toast.js";
import { renderStatus } from "./render/status.js";
import { renderNowPlaying, extractDominantColor } from "./render/nowPlaying.js";
import { renderSources } from "./render/sources.js";
import { createOutput } from "./render/output.js";
import {
    renderSettings, collectSettings, applyInterfacePrefs,
    markModifiedFields, resetFieldToBaseline,
} from "./render/settings.js";
import { createDeps } from "./render/deps.js";
import { createExternalAudio } from "./render/externalAudio.js";
import { createLocalFiles } from "./render/localFiles.js";
import { createOnlineRadio } from "./render/onlineRadio.js";
import { createYoutubeMusic } from "./render/youtubeMusic.js";
import { createSoundcloud } from "./render/soundcloud.js";
import { createJellyfin } from "./render/jellyfin.js";
import { initI18n, onLangChange, t, setLang, getLang } from "./i18n.js";
import { prefs } from "./preferences.js";
import { downloadJson, todayStamp } from "./lib/download.js";
import { buildStationPack, mergeStationPack } from "./data/stationPack.js";
import { resetMemo } from "./lib/store.js";
import { confirmAction } from "./render/confirmModal.js";

let state = null;
let cfg = null;

const refs = {
    status: $("#status"),
    np: {
        art: $("#np-art"),
        backdrop: $("#np-backdrop"),
        img: $("#np-art-img"),
        title: $("#np-title"),
        artist: $("#np-artist"),
        fill: $("#np-fill"),
        pos: $("#np-pos"),
        dur: $("#np-dur"),
        play: $("#t-play"),
        mini: {
            player: document.querySelector("#mini-player"),
            art: document.querySelector("#mini-art-img"),
            title: document.querySelector("#mini-title"),
            artist: document.querySelector("#mini-artist"),
            play: document.querySelector("#mini-play"),
            prev: document.querySelector("#mini-prev"),
            next: document.querySelector("#mini-next"),
            pos: document.querySelector("#mini-pos"),
            dur: document.querySelector("#mini-dur"),
            fill: document.querySelector("#mini-fill")
        }
    },
    sources: $("#sources"),
    sourceCard: $("#source-card"),
    outputCard: $("#output-card"),
    drawer: $("#drawer"),
    scrim: $("#scrim"),
    form: $("#settings-form"),
};

$("#brand-mark").innerHTML = icons.broadcast;
$("#open-settings").innerHTML = icons.gear;
$("#close-settings").innerHTML = icons.close;
$("#t-prev").innerHTML = icons.prev;
$("#t-next").innerHTML = icons.next;

if (refs.np.mini && refs.np.mini.prev) refs.np.mini.prev.innerHTML = icons.prev;
if (refs.np.mini && refs.np.mini.next) refs.np.mini.next.innerHTML = icons.next;

const mainEl = $("main");

let renderOutput;
let deps;
let externalAudio;
let localFiles;
let onlineRadio;
let youtubeMusic;
let soundcloud;
let jellyfin;

async function switchSource(name) {
    try {
        await api.switchSource(name);
    } catch (e) {
        toast(e.message, true);
    }
}

async function transport(action) {
    const source = state?.sources?.active;
    if (!source) return;
    let act = action;
    if (act === "play") {
        const s = state.sources.available.find(x => x.name === source);
        if (s?.playback_state === "playing") act = "pause";
    }
    try {
        await api.transport(source, act);
        await new Promise(r => setTimeout(r, 300));
        state = await api.getState().catch(() => state);
        render();
    } catch (e) {
        toast(e.message, true);
    }
}

const background = [$("header"), mainEl, $(".credits")];
let lastFocus = null;
let formBaseline = null;

// Called whenever the form is freshly rendered from a known-saved source
// (opened, reloaded from disk, or just saved) so later dirty-checks and the
// "modified field" markers compare against that baseline instead of
// whatever was on screen before.
function snapshotForm() {
    formBaseline = collectSettings(refs.form);
    markModifiedFields(refs.form, formBaseline);
}

function isFormDirty() {
    return formBaseline !== null && JSON.stringify(collectSettings(refs.form)) !== JSON.stringify(formBaseline);
}

function openDrawer() {
    lastFocus = document.activeElement;
    refs.drawer.classList.add("open");
    refs.drawer.inert = false;
    refs.scrim.hidden = false;
    background.forEach(n => n && (n.inert = true));
    $("#close-settings").focus();
    document.body.style.overflow = "hidden";
}

function closeDrawer() {
    if (!refs.drawer.classList.contains("open")) return;
    refs.drawer.classList.remove("open");
    refs.drawer.inert = true;
    refs.scrim.hidden = true;
    background.forEach(n => n && (n.inert = false));
    lastFocus?.focus?.();
    document.body.style.overflow = "";
}

// Guards closeDrawer() with a confirmation if the form has unsaved changes —
// used by the close button, the scrim, and Escape, but not by the Save
// handler itself (which closes immediately after persisting).
async function requestCloseDrawer() {
    if (!refs.drawer.classList.contains("open")) return;
    if (isFormDirty()) {
        const ok = await confirmAction({
            title: t("settings.discard_title"),
            message: t("settings.discard_message"),
            confirmLabel: t("settings.discard_confirm"),
        });
        if (!ok) return;
    }
    closeDrawer();
}

function render() {
    if (!state) return;
    renderStatus(refs.status, state);
    renderNowPlaying(refs.np, state);
    renderSources(refs.sources, state, cfg, switchSource);
    renderOutput(state);
    externalAudio.render();
    localFiles.render();
    onlineRadio.render();
    youtubeMusic.render();
    soundcloud.render();
    jellyfin.render();

    refs.sourceCard.hidden = false;
    refs.outputCard.hidden = !state.sources?.active;

    document.body.classList.toggle("view-minimal", prefs.viewMode.get() === "minimal");
}

const backupBtn = document.getElementById("backup-config");
if (backupBtn) {
    backupBtn.addEventListener("click", async () => {
        try {
            const config = await api.getConfig();
            downloadJson(config, `fh6-radio-settings-${todayStamp()}.json`);
            toast(t("settings.backup_downloaded"));
        } catch (err) {
            console.error("Failed to backup config:", err);
            toast(t("error.backup_failed"), true);
        }
    });
}

const restoreBtn = document.getElementById("restore-config");
const restoreFile = document.getElementById("restore-file");

if (restoreBtn && restoreFile) {
    restoreBtn.addEventListener("click", () => restoreFile.click());
    restoreFile.addEventListener("change", async (e) => {
        const file = e.target.files[0];
        if (!file) return;
        try {
            const ok = await confirmAction({
                title: t("settings.restore_confirm_title"),
                message: t("settings.restore_confirm_message"),
                confirmLabel: t("settings.restore"),
            });
            if (!ok) return;

            const text = await file.text();
            const patch = JSON.parse(text);
            cfg = await api.putConfig(patch);
            externalAudio.invalidate();
            localFiles.invalidate();
            onlineRadio.invalidate();
            youtubeMusic.invalidate();
            soundcloud.invalidate();
            jellyfin.invalidate();
            renderSettings(refs.form, cfg);
            snapshotForm();
            state = await api.getState().catch(() => state);
            render();
            toast(t("settings.restored"));
        } catch (err) {
            console.error("Failed to restore config:", err);
            toast(t("error.restore_failed"), true);
        } finally {
            e.target.value = "";
        }
    });
}

$("#t-play").addEventListener("click", () => transport("play"));
$("#t-next").addEventListener("click", () => transport("next"));
$("#t-prev").addEventListener("click", () => transport("previous"));

if (refs.np.mini && refs.np.mini.play) refs.np.mini.play.addEventListener("click", () => transport("play"));
if (refs.np.mini && refs.np.mini.next) refs.np.mini.next.addEventListener("click", () => transport("next"));
if (refs.np.mini && refs.np.mini.prev) refs.np.mini.prev.addEventListener("click", () => transport("previous"));

const miniVolToggle = document.getElementById("mini-vol-toggle");
const miniVolume = document.getElementById("mini-volume");
if (miniVolToggle && miniVolume) {
    miniVolToggle.addEventListener("click", e => {
        e.stopPropagation();
        miniVolume.classList.toggle("open");
    });
    document.addEventListener("click", e => {
        if (miniVolume.classList.contains("open") && !miniVolume.contains(e.target)) {
            miniVolume.classList.remove("open");
        }
    });
}

$("#open-settings").addEventListener("click", async () => {
    try {
        cfg = await api.getConfig();
    } catch (e) {
        toast(e.message, true);
        return;
    }
    renderSettings(refs.form, cfg);
    snapshotForm();
    openDrawer();
});

$("#close-settings").addEventListener("click", requestCloseDrawer);
refs.scrim.addEventListener("click", requestCloseDrawer);
document.addEventListener("keydown", e => {
    if (e.key === "Escape") requestCloseDrawer();
});

refs.form.addEventListener("input", () => markModifiedFields(refs.form, formBaseline));
refs.form.addEventListener("change", () => markModifiedFields(refs.form, formBaseline));

refs.form.addEventListener("click", e => {
    const btn = e.target.closest(".field-reset-btn");
    if (!btn) return;
    const field = btn.closest(".field");
    const { resetSection, resetKey } = field.dataset;
    resetFieldToBaseline(field, resetSection, resetKey, formBaseline);
});

$("#save-config").addEventListener("click", async () => {
    try {
        const serverConfigPatch = applyInterfacePrefs(collectSettings(refs.form));
        cfg = await api.putConfig(serverConfigPatch);
        externalAudio.invalidate();
        localFiles.invalidate();
        onlineRadio.invalidate();
        youtubeMusic.invalidate();
        soundcloud.invalidate();
        jellyfin.invalidate();
        state = await api.getState().catch(() => state);
        render();
        toast(t("settings.saved"));
        closeDrawer();

        const dynamicCheckbox = document.querySelector("#f-interface-dynamic_color");
        const perfCheckbox = document.querySelector("#f-interface-perf_mode");
        const langSelect = document.querySelector("#f-interface-language");
        const viewSelect = document.querySelector("#f-interface-view_mode");
        const currentMode = viewSelect ? viewSelect.value : prefs.viewMode.get();
        const themeSelect = document.querySelector("#f-interface-theme_mode");
        const enabled = dynamicCheckbox ? dynamicCheckbox.checked : true;
        const img = refs.np.img;

        document.body.classList.toggle("view-minimal", currentMode === "minimal");
        document.body.classList.toggle("perf-mode", perfCheckbox ? perfCheckbox.checked : false);
        if (themeSelect) applyTheme(themeSelect.value);

        if (langSelect && langSelect.value !== getLang()) {
            await setLang(langSelect.value);
        }

        if (enabled && img?.complete && img?.naturalWidth) {
            const color = extractDominantColor(img);
            if (color) document.documentElement.style.setProperty("--accent", color);
        } else if (!enabled) {
            document.documentElement.style.setProperty("--accent", "var(--color-sunset-yellow)");
        }

    } catch (e) {
        toast(e.message, true);
    }
});

$("#reload-config").addEventListener("click", async () => {
    try {
        cfg = await api.reloadConfig();
        externalAudio.invalidate();
        localFiles.invalidate();
        onlineRadio.invalidate();
        youtubeMusic.invalidate();
        soundcloud.invalidate();
        jellyfin.invalidate();
        renderSettings(refs.form, cfg);
        snapshotForm();
        render();
        toast(t("settings.reloaded"));
    } catch (e) {
        toast(e.message, true);
    }
});

function applyI18n() {
    document.querySelectorAll("[data-i18n]").forEach(el => {
        el.textContent = t(el.dataset.i18n);
    });
    document.querySelectorAll("[data-i18n-placeholder]").forEach(el => {
        el.placeholder = t(el.dataset.i18nPlaceholder);
    });
    document.querySelectorAll("[data-i18n-aria-label]").forEach(el => {
        el.setAttribute("aria-label", t(el.dataset.i18nAriaLabel));
    });
    document.querySelectorAll("[data-i18n-title]").forEach(el => {
        el.title = t(el.dataset.i18nTitle);
    });
    document.querySelectorAll("[data-i18n-html]").forEach(el => {
        el.innerHTML = t(el.dataset.i18nHtml);
    });
}

const BASE_THEME_CLASSES = ["theme-light", "theme-dark"];

function applyTheme(themeMode) {
    const body = document.body;
    const isSquared = themeMode.endsWith("-squared");
    const base = isSquared ? themeMode.slice(0, -"-squared".length) : themeMode;

    body.classList.remove(...BASE_THEME_CLASSES);
    const cls = BASE_THEME_CLASSES.includes(`theme-${base}`) ? `theme-${base}` : "theme-dark";
    body.classList.add(cls);
    body.classList.toggle("theme-squared", isSquared);
}

function refreshAfterLangChange() {
    document.documentElement.lang = getLang();
    applyI18n();
    resetMemo();
    externalAudio.invalidate();
    localFiles.invalidate();
    onlineRadio.invalidate();
    youtubeMusic.invalidate();
    soundcloud.invalidate();
    jellyfin.invalidate();
    if (refs.drawer.classList.contains("open")) {
        renderSettings(refs.form, cfg);
        snapshotForm();
    }
    if (state) render();
}

async function boot() {
    await initI18n();
    document.documentElement.lang = getLang();
    applyI18n();
    onLangChange(refreshAfterLangChange);

    document.body.classList.toggle("view-minimal", prefs.viewMode.get() === "minimal");
    document.body.classList.toggle("perf-mode", prefs.perfMode.get());

    renderOutput = createOutput($("#vol"), $("#mini-vol"), $("#vol-out"), async gain => {
        try {
            await api.setGain(gain);
        } catch (e) {
            toast(e.message, true);
        }
    }, $("#mini-vol-tooltip"));

    deps = createDeps(mainEl);

    externalAudio = createExternalAudio(mainEl, {
        getState: () => state,
        getConfig: () => cfg,
        onSaved: async patch => {
            cfg = { ...cfg, external_audio: { ...(cfg?.external_audio || {}), ...patch } };
            state = await api.getState().catch(() => state);
            render();
        },
    });

    localFiles = createLocalFiles(mainEl, {
        getState: () => state,
        getConfig: () => cfg,
        onSaved: async () => {
            cfg = await api.getConfig().catch(() => cfg);
            state = await api.getState().catch(() => state);
            render();
        },
    });

    onlineRadio = createOnlineRadio(mainEl, {
        getState: () => state,
        getConfig: () => cfg,
        onSaved: async () => {
            cfg = await api.getConfig().catch(() => cfg);
            state = await api.getState().catch(() => state);
            render();
        },
    });

    youtubeMusic = createYoutubeMusic(mainEl, {
        getState: () => state,
        getConfig: () => cfg,
        onSaved: async () => {
            cfg = await api.getConfig().catch(() => cfg);
            state = await api.getState().catch(() => state);
            render();
        },
    });

    const { exportBtn, importBtn, importFileInput } = youtubeMusic.els;
    exportBtn.addEventListener("click", async () => {
        try {
            const config = await api.getConfig();
            downloadJson(buildStationPack(config, "youtube_music"), `fh6-radio-youtubemusic-stations-${todayStamp()}.json`);
            toast(t("settings.stations_exported"));
        } catch (err) {
            console.error("Failed to export station pack:", err);
            toast(t("error.unknown"), true);
        }
    });
    
    importBtn.addEventListener("click", () => importFileInput.click());
    importFileInput.addEventListener("change", async e => {
        const file = e.target.files[0];
        if (!file) return;
        try {
            const text = await file.text();
            const pack = JSON.parse(text);
            const freshCfg = await api.getConfig();
            const { patch, added } = mergeStationPack(freshCfg, pack, "youtube_music");
            cfg = await api.putConfig(patch);
            youtubeMusic.invalidate();
            state = await api.getState().catch(() => state);
            render();
            toast(t("settings.stations_imported", { count: String(added) }));
        } catch (err) {
            console.error("Failed to import station pack:", err);
            toast(t("error.invalid_station_pack"), true);
        } finally {
            e.target.value = "";
        }
    });

    soundcloud = createSoundcloud(mainEl, {
        getState: () => state,
        getConfig: () => cfg,
        onSaved: async () => {
            cfg = await api.getConfig().catch(() => cfg);
            state = await api.getState().catch(() => state);
            render();
        },
    });
    
    const scEls = soundcloud.els;
    
    scEls.exportBtn.addEventListener("click", async () => {
        try {
            const config = await api.getConfig();
            downloadJson(buildStationPack(config, "soundcloud"), `fh6-radio-soundcloud-stations-${todayStamp()}.json`);
            toast(t("settings.stations_exported"));
        } catch (err) {
            console.error("Failed to export station pack:", err);
            toast(t("error.unknown"), true);
        }
    });

    scEls.importBtn.addEventListener("click", () => scEls.importFileInput.click());
    scEls.importFileInput.addEventListener("change", async e => {
        const file = e.target.files[0];
        if (!file) return;
        try {
            const text = await file.text();
            const pack = JSON.parse(text);
            const freshCfg = await api.getConfig();
            const { patch, added } = mergeStationPack(freshCfg, pack, "soundcloud");
            cfg = await api.putConfig(patch);
            
            soundcloud.invalidate();
            state = await api.getState().catch(() => state);
            render();
            
            toast(t("settings.stations_imported", { count: String(added) }));
        } catch (err) {
            console.error("Failed to import station pack:", err);
            toast(t("error.invalid_station_pack"), true);
        } finally {
            e.target.value = "";
        }
    });

    jellyfin = createJellyfin(mainEl, {
        getState: () => state,
        getConfig: () => cfg,
        onSaved: async () => {
            cfg = await api.getConfig().catch(() => cfg);
            state = await api.getState().catch(() => state);
            render();
        },
    });

    let savedTheme = prefs.themeMode.get();
    if (savedTheme === "auto") {
        savedTheme = "dark";
        prefs.themeMode.set(savedTheme);
    }
    applyTheme(savedTheme);

    try {
        cfg = await api.getConfig();
    } catch {
        cfg = {};
    }

    connect(next => {
        state = next;
        render();
    });

    deps.start();
}

boot();