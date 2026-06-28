import { api } from "../api.js";
import { el } from "../dom.js";
import { toast } from "../toast.js";

/**
 * createStartupMelody — self-contained Startup Melody dashboard card.
 *
 * ctx: { getConfig: () => cfg, onSaved: async () => void }
 */
export function createStartupMelody(main, ctx) {
  let files = [];
  let folder = "";
  let loaded = false;
  let loading = false;

  // ── Controls ──────────────────────────────────────────────────────────
  const enabledChk = el("input", { type: "checkbox", id: "mel-enabled" });
  const raceChk    = el("input", { type: "checkbox", id: "mel-race" });
  const carChk     = el("input", { type: "checkbox", id: "mel-car" });
  const debounceIn = el("input", {
    type: "number", id: "mel-debounce", min: "500", max: "30000", step: "500", value: "3000",
  });
  const portIn = el("input", {
    type: "number", id: "mel-port", min: "1024", max: "65535", step: "1", value: "7777",
  });

  const fileList  = el("ul", { class: "melody-file-list" });
  const folderEl  = el("code", { class: "melody-folder" }, "—");
  const refreshBtn = el("button", { type: "button", class: "btn ghost" }, "Refresh files");
  const saveBtn    = el("button", { type: "button", class: "btn filled" }, "Save");
  const statusEl   = el("p",  { class: "muted" }, "");

  // ── Card DOM ──────────────────────────────────────────────────────────
  const card = el("section", { class: "card", id: "startup-melody-card" }, [
    el("h2", {}, "🎵 Startup Melody"),
    el("p", { class: "muted" },
      "Play a random audio clip from the melodies folder whenever you start a race or switch cars."),

    // Enable toggle
    el("div", { class: "field checkbox" }, [
      enabledChk,
      el("label", { for: "mel-enabled" }, "Enable startup melody"),
    ]),

    // Trigger checkboxes
    el("fieldset", { class: "mel-triggers" }, [
      el("legend", {}, "Triggers"),
      el("div", { class: "field checkbox" }, [
        raceChk,
        el("label", { for: "mel-race" }, "On race start"),
      ]),
      el("div", { class: "field checkbox" }, [
        carChk,
        el("label", { for: "mel-car" }, "On car switch (requires FH6 Data Out)"),
      ]),
    ]),

    // Debounce + port row
    el("div", { class: "mel-row" }, [
      el("div", { class: "field" }, [
        el("label", { for: "mel-debounce" }, "Debounce (ms)"),
        debounceIn,
      ]),
      el("div", { class: "field" }, [
        el("label", { for: "mel-port" }, "FH6 Data Out port"),
        portIn,
      ]),
    ]),

    // Save button
    el("div", { class: "row mel-actions" }, [saveBtn]),

    // File browser section
    el("h3", { class: "mel-sub" }, "Melody Files"),
    el("p", { class: "muted" },
      "Drop .wav / .mp3 / .flac / .ogg / .opus / .m4a files into the folder below. " +
      "One file is picked at random on each trigger."),
    el("div", { class: "mel-folder-row" }, [
      el("span", { class: "muted" }, "Folder: "),
      folderEl,
      refreshBtn,
    ]),
    fileList,
    statusEl,
  ]);

  // Insert after the output card if present, else append
  const outputCard = main.querySelector("#output-card");
  if (outputCard) outputCard.insertAdjacentElement("afterend", card);
  else main.append(card);

  // ── Populate from config ───────────────────────────────────────────────
  function syncFromConfig() {
    const m = ctx.getConfig()?.startup_melody;
    if (!m) return;
    enabledChk.checked = !!m.enabled;
    raceChk.checked    = m.trigger_on_race_start !== false;
    carChk.checked     = m.trigger_on_car_change !== false;
    debounceIn.value   = String(m.debounce_ms   ?? 3000);
    portIn.value       = String(m.data_out_port  ?? 7777);
  }

  // ── File list loader ───────────────────────────────────────────────────
  async function loadFiles(force = false) {
    if ((loaded && !force) || loading) return;
    loading = true;
    statusEl.textContent = "Loading…";
    fileList.replaceChildren();
    try {
      const r = await api.getMelodyFiles();
      files  = Array.isArray(r.files) ? r.files : [];
      folder = r.folder || "";
      loaded = true;
    } catch (e) {
      files  = [];
      folder = "";
      loaded = false;
      statusEl.textContent = "Could not load melody files: " + e.message;
    } finally {
      loading = false;
    }
    renderFileList();
  }

  function renderFileList() {
    folderEl.textContent = folder || "—";

    if (files.length === 0) {
      fileList.replaceChildren(
        el("li", { class: "melody-empty" },
          "No audio files found — add some to the melodies folder and click Refresh.")
      );
      statusEl.textContent = "";
      return;
    }

    fileList.replaceChildren(
      ...files.map(f =>
        el("li", { class: "melody-file" }, [
          el("span", { class: "melody-icon" }, "♪"),
          el("span", { class: "melody-name" }, f),
        ])
      )
    );
    statusEl.textContent = `${files.length} file${files.length === 1 ? "" : "s"} found`;
  }

  // ── Events ────────────────────────────────────────────────────────────
  refreshBtn.addEventListener("click", () => loadFiles(true));

  saveBtn.addEventListener("click", async () => {
    saveBtn.disabled = true;
    try {
      await api.putMelodyConfig({
        enabled:               enabledChk.checked,
        trigger_on_race_start: raceChk.checked,
        trigger_on_car_change: carChk.checked,
        debounce_ms:           parseInt(debounceIn.value, 10) || 3000,
        data_out_port:         parseInt(portIn.value,     10) || 7777,
      });
      toast("Startup Melody settings saved ✓");
      await ctx.onSaved();
    } catch (e) {
      toast(e.message, true);
    } finally {
      saveBtn.disabled = false;
    }
  });

  // ── Public API ────────────────────────────────────────────────────────
  function render() {
    syncFromConfig();
    loadFiles();
  }

  return {
    render,
    invalidate: () => { loaded = false; },
  };
}
