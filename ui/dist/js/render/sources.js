import { el } from "../dom.js";
import { changed } from "../store.js";
import { t } from "../i18n.js";

function detailLine(s) {
  if (s.name === "local_files" && s.details?.track_count != null) {
    const n = s.details.track_count;
    return `${n} ${t("local_files.tracks")} ${t("source.indexed")}`;
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
        el("div", { class: "source-name" }, s.display_name),
        el("div", { class: "source-state " + stateCls }, stateText(s)),
      ];
      if (showNote) children.push(el("div", { class: "source-note" }, s.auth_instructions));
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
