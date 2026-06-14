import { $ } from "./dom.js";
import { api } from "./api.js";
import { connect } from "./events.js";
import { icons } from "./icons.js";
import { toast } from "./toast.js";
import { renderStatus } from "./render/status.js";
import { renderNowPlaying } from "./render/nowPlaying.js";
import { renderSources } from "./render/sources.js";
import { createOutput } from "./render/output.js";
import { renderSettings, collectSettings } from "./render/settings.js";
import { createDeps } from "./render/deps.js";
import { createExternalAudio } from "./render/externalAudio.js";
import { createLocalFiles } from "./render/localFiles.js";
import { createOnlineRadio } from "./render/onlineRadio.js";
import { extractDominantColor } from "./render/nowPlaying.js";
import { initI18n, onLangChange, t, setLang } from "./i18n.js";

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
    },
    sources: $("#sources"),
    sourceCard: $("#source-card"),
    outputCard: $("#output-card"),
    ytCard: $("#yt-cast-card"),
    jfCard: $("#jf-cast-card"),
    ytShuffle: $("#yt-shuffle"),
    drawer: $("#drawer"),
    scrim: $("#scrim"),
    form: $("#settings-form"),
};

$("#brand-mark").innerHTML = icons.broadcast;
$("#open-settings").innerHTML = icons.gear;
$("#close-settings").innerHTML = icons.close;
$("#t-prev").innerHTML = icons.prev;
$("#t-next").innerHTML = icons.next;
$("#yt-shuffle").innerHTML = icons.shuffle;

const mainEl = $("main");

// Composants initialisés dans boot() après initI18n()
let renderOutput;
let deps;
let externalAudio;
let localFiles;
let onlineRadio;

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
        // Some transports may change the queue so refresh it if needed.
        // Maybe fix the playback ??
        if (source === "local_files" && (act === "next" || act === "previous")) localFiles.reloadQueue();
    } catch (e) {
        toast(e.message, true);
    }
}

const background = [$("header"), mainEl, $(".credits")];
let lastFocus = null;

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

function render() {
    if (!state) return;
    renderStatus(refs.status, state);
    renderNowPlaying(refs.np, state);
    renderSources(refs.sources, state, cfg, switchSource);
    renderOutput(state);
    externalAudio.render();
    localFiles.render();
    onlineRadio.render();

    refs.sourceCard.hidden = false;
    refs.outputCard.hidden = !state.sources?.active;

    const available = state.sources?.available || [];
    const active = state.sources?.active;
    refs.ytCard.hidden = active !== "youtube_music";
    refs.jfCard.hidden = active !== "jellyfin";
    const shuffleOn = !!available.find(s => s.name === "youtube_music")?.details?.shuffle;
    refs.ytShuffle.classList.toggle("toggled", shuffleOn);
    refs.ytShuffle.setAttribute("aria-pressed", String(shuffleOn));
}

function applyNpLayout(layout) {
    refs.np.art.classList.remove("layout-split", "layout-vinyl", "layout-magazine");
    if (layout !== "default") refs.np.art.classList.add(`layout-${layout}`);
}

$("#t-play").addEventListener("click", () => transport("play"));
$("#t-next").addEventListener("click", () => transport("next"));
$("#t-prev").addEventListener("click", () => transport("previous"));

$("#yt-cast").addEventListener("submit", async e => {
    e.preventDefault();
    const url = $("#yt-url").value.trim();
    if (!url) return;
    try {
        await api.castYoutube(url);
        $("#yt-url").value = "";
        toast(t("yt.casting"));
    } catch (err) {
        toast(err.message, true);
    }
});

$("#yt-shuffle").addEventListener("click", async () => {
    const yt = state?.sources?.available?.find(s => s.name === "youtube_music");
    if (!yt) return;
    const shuffle = !yt.details?.shuffle;
    try {
        await api.shuffleYoutube(shuffle);
        toast(shuffle ? t("yt.shuffle_on") : t("yt.shuffle_off"));
    } catch (err) {
        toast(err.message, true);
    }
});

$("#jf-cast").addEventListener("submit", async e => {
    e.preventDefault();
    const playlistId = $("#jf-url").value.trim();
    if (!playlistId) return;
    try {
        await api.castJellyfin(playlistId);
        $("#jf-url").value = "";
        toast(t("jellyfin.playing"));
    } catch (err) {
        toast(err.message, true);
    }
});

$("#open-settings").addEventListener("click", async () => {
    try {
        cfg = await api.getConfig();
    } catch (e) {
        toast(e.message, true);
        return;
    }
    renderSettings(refs.form, cfg);
    openDrawer();
});

$("#close-settings").addEventListener("click", closeDrawer);
refs.scrim.addEventListener("click", closeDrawer);
document.addEventListener("keydown", e => {
    if (e.key === "Escape") closeDrawer();
});

$("#save-config").addEventListener("click", async () => {
    try {
        cfg = await api.putConfig(collectSettings(refs.form));
        externalAudio.invalidate();
        localFiles.invalidate();
        onlineRadio.invalidate();
        state = await api.getState().catch(() => state);
        render();
        toast(t("settings.saved"));
        closeDrawer();

        const dynamicCheckbox = document.querySelector("#f-dynamic-color");
        const enabled = dynamicCheckbox ? dynamicCheckbox.checked : true;
        const langSelect = document.querySelector("#f-language");
        const img = refs.np.img;

        localStorage.setItem("fh6-dynamic-color", String(enabled));

        if (langSelect) await setLang(langSelect.value);
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
        renderSettings(refs.form, cfg);
        render();
        toast(t("settings.reloaded"));
    } catch (e) {
        toast(e.message, true);
    }
});

// Apply i18n to the entire page
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
}

async function boot() {
    await initI18n();
    applyI18n();
    onLangChange(() => { applyI18n(); render(); });

    // initialize the rest of the app after i18n is ready
    renderOutput = createOutput($("#vol"), $("#vol-out"), async gain => {
        try {
            await api.setGain(gain);
        } catch (e) {
            toast(e.message, true);
        }
    });

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

    applyNpLayout(localStorage.getItem("fh6-np-layout") || "default");

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