import { setText } from "../lib/dom.js";
import { t } from "../i18n.js";

export function renderStatus(node, state) {
  const ok = !!state?.game?.attached;
  node.className = "status " + (ok ? "ok" : "err");
  setText(node, ok ? t("status.ok") : t("status.error"));
}