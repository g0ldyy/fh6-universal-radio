import { el } from "../lib/dom.js";
import { changed } from "../lib/store.js";
import { t } from "../i18n.js";

const DISPLAY_NAME_MAP = () => ({
    "local_files": t("source.local_files"),
    "online_radio": t("source.online_radio"),
    "external_audio": t("source.external_audio"),
    "vanilla_radio": t("source.vanilla_radio"),
});

const AUTH_INSTRUCTIONS_MAP = () => ({
    [t("source.auth_instructions.spotify")]:
        "1. Ensure your PC and phone are on the same Wi-Fi network.\n2. Open the Spotify app on your phone.\n3. Tap the 'Devices' icon and select 'FH6 Universal Radio'.\nOnce connected, credentials will automatically save to the cache folder.",
    [t("source.auth_instructions.local_files")]:
        "Add a music folder to this station in the Local Files card, then Save.",
});

function translateAuthInstructions(text) {
    if (!text) return text;
    const map = AUTH_INSTRUCTIONS_MAP();
    const normalize = s => s.trim().replace(/\r?\n/g, " ").replace(/\s+/g, " ");
    for (const [translated, original] of Object.entries(map)) {
        if (normalize(text) === normalize(original)) return translated;
    }
    return text;
}

function detailLine(s) {
  if (s.name === "local_files" && s.details?.track_count != null) {
    const n = s.details.track_count;
    return `${n} ${t("label.tracks")} ${t("source.indexed")}`;
  }
  return null;
}

function stateText(s) {
  const rawState = s.playback_state;
  const translatedState = t(`source.state.${rawState}`) !== `source.state.${rawState}`
    ? t(`source.state.${rawState}`)
    : rawState;

  const parts = [translatedState];

  if (s.auth_state !== "none_required") {
    const rawAuth = s.auth_state.replace("_", " ");
    const translatedAuth = t(`source.auth.${s.auth_state}`) !== `source.auth.${s.auth_state}`
      ? t(`source.auth.${s.auth_state}`)
      : rawAuth;
    parts.push(translatedAuth);
  }

  const detail = detailLine(s);
  if (detail) parts.push(detail);
  return parts.join(" · ");
}

export function visibleSources(state, cfg) {
  const all = state?.sources?.available || [];
  const externalEnabled = !!cfg?.external_audio?.enabled;
  return all.filter(s => s.name !== "external_audio" || externalEnabled);
}

export function sourcesSignature(list, active) {
  return list
    .map(s => `${s.name}:${s.playback_state}:${s.auth_state}:${s.details?.track_count ?? ""}:${s.name === active}`)
    .join("|");
}

export function renderSources(node, state, cfg, onSwitch) {
  const active = state?.sources?.active;
  const list = visibleSources(state, cfg);
  if (!changed("sources", sourcesSignature(list, active))) return;

  if (!list.length) {
    node.replaceChildren(
      el("p", { class: "source-empty" }, t("source.no_source")),
    );
    return;
  }

  node.replaceChildren(
    ...list.map(s => {
      const stateCls =
        s.auth_state === "needs_auth" ? "warn" : s.auth_state === "error" ? "err" : "";
      const showNote =
        (s.auth_state === "needs_auth" || s.auth_state === "error") && s.auth_instructions;
      const children = [
        el("div", { class: "source-name" }, DISPLAY_NAME_MAP()[s.name] || s.display_name),
        el("div", { class: "source-state " + stateCls }, stateText(s)),
      ];
      if (showNote) children.push(el("div", { class: "source-note" }, translateAuthInstructions(s.auth_instructions)));
      const tile = el(
        "button",
        { type: "button", class: "source" + (s.name === active ? " active" : "") },
        children,
      );
      tile.addEventListener("click", () => onSwitch(s.name));
      return tile;
    }),
  );
}
