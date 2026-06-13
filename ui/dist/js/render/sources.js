import { el } from "../dom.js";
import { changed } from "../store.js";
import { toast } from "../toast.js";

function detailLine(s) {
  if (s.name === "local_files" && s.details?.track_count != null) {
    const n = s.details.track_count;
    return `${n} track${n === 1 ? "" : "s"} indexed`;
  }
  return null;
}

function stateText(s) {
  const parts = [s.playback_state];
  if (s.auth_state !== "none_required") parts.push(s.auth_state.replace("_", " "));
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
      el(
        "p",
        { class: "source-empty" },
        "No source enabled yet. Open Settings to turn one on.",
      ),
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
      if (showNote) children.push(el("div", { class: "source-note", html: s.auth_instructions }));

      const isTidalUnauthed = s.name === "tidal" && s.auth_state !== "none_required" && s.auth_state !== "authenticated";
      const classes = ["source"];
      if (s.name === active) classes.push("active");
      if (isTidalUnauthed) classes.push("unauthorized");

      const tile = el(
        "button",
        { type: "button", class: classes.join(" ") },
        children,
      );
      tile.addEventListener("click", (e) => {
        if (e.target.tagName === "A") return;
        if (isTidalUnauthed) {
          toast("Please link your TIDAL account using the link below.", true);
          return;
        }
        onSwitch(s.name);
      });
      return tile;
    }),
  );
}
