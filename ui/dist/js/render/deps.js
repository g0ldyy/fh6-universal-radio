import { api } from "../api.js";
import { el } from "../dom.js";
import { changed } from "../store.js";
import { t } from "../i18n.js";

export function depRow(tool) {
  let label = t("deps.ready");
  let cls = "";
  if (tool.downloading) {
    const pct = tool.total_bytes ? Math.round((tool.downloaded_bytes / tool.total_bytes) * 100) : null;
    label = pct === null
      ? t("deps.downloading_mb", { mb: (tool.downloaded_bytes / 1e6).toFixed(1) })
      : t("deps.downloading_pct", { pct });
  } else if (tool.error) {
    label = tool.error;
    cls = "err";
  }
  const fill = el("div", { class: "dep-fill" });
  fill.style.width = (tool.downloading && tool.total_bytes ? (tool.downloaded_bytes / tool.total_bytes) * 100 : 0) + "%";
  return el("div", { class: "dep-row" }, [
    el("span", { class: "dep-name" }, tool.name),
    el("span", { class: "dep-state " + cls }, label),
    el("div", { class: "dep-bar" }, [fill]),
  ]);
}

export function createDeps(main) {
  let tools = [];
  const list = el("div", {});
  const card = el("section", { class: "card", id: "deps-card", hidden: true }, [
    el("h2", {}, t("deps.title")),
    el("p", {
      class: "muted",
      html: t("deps.description"),
    }),
    list,
  ]);
  main.append(card);

  function render() {
    card.hidden = !tools.some(t => t.downloading || t.error);
    if (card.hidden) return;
    const sig = tools.map(t => `${t.name}:${t.downloading}:${t.downloaded_bytes}:${t.error}`).join("|");
    if (changed("deps", sig)) list.replaceChildren(...tools.map(depRow));
  }

  async function poll() {
    try {
      const r = await api.getDeps();
      tools = Array.isArray(r.tools) ? r.tools : [];
      render();
    } catch {
      // keep last
    }
    setTimeout(poll, tools.some(t => t.downloading) ? 1000 : 5000);
  }

  return { start: poll };
}
