import { api } from "../data/api.js";
import { $, el } from "../lib/dom.js";
import { toast } from "../toast.js";
import { t } from "../i18n.js";

export function createExternalAudio(main, ctx) {
  let devices = [];
  let endpoint = "";
  let sessions = [];
  let sessionId = "";
  let sessionsAvailable = false;
  let loaded = false;
  let loading = false;

  const deviceSelect = el("select", { id: "ext-device", "aria-label": t("ext.device_label"), dataset: { i18nAriaLabel: "ext.device_label" } });
  const sessionSelect = el("select", { id: "ext-session", "aria-label": t("ext.session_label"), dataset: { i18nAriaLabel: "ext.session_label" } });
  const refreshBtn = el("button", { type: "button", class: "btn ghost", dataset: { i18n: "ext.refresh" } }, t("ext.refresh"));
  const saveBtn = el("button", { type: "button", class: "btn filled", dataset: { i18n: "btn.save" } }, t("btn.save"));
  const hint = el("p", { class: "muted" });

  const card = el("section", { class: "card", id: "external-audio-card", hidden: true }, [
    el("h2", { dataset: { i18n: "source.external_audio" } }, t("source.external_audio")),
    el("p", { class: "muted", dataset: { i18n: "ext.description" } }, t("ext.description")),
    el("label", { class: "field-label", for: "ext-device", dataset: { i18n: "ext.capture_device" } }, t("ext.capture_device")),
    el("div", { class: "row" }, [deviceSelect, refreshBtn]),
    el("label", { class: "field-label", for: "ext-session", dataset: { i18n: "ext.media_session" } }, t("ext.media_session")),
    el("div", { class: "row" }, [sessionSelect, saveBtn]),
    hint,
  ]);

  const sourcesCard = $("#sources", main)?.closest(".card");
  if (sourcesCard) sourcesCard.insertAdjacentElement("afterend", card);
  else main.append(card);

  async function load(force = false) {
    if ((loaded && !force) || loading) return;
    loading = true;
    try {
      const r = await api.getExternalAudio();
      devices = Array.isArray(r.devices) ? r.devices : [];
      endpoint = r.endpoint_id || "";
      sessions = Array.isArray(r.media_sessions) ? r.media_sessions : [];
      sessionId = r.media_session_id || "";
      sessionsAvailable = !!r.media_sessions_available;
      loaded = true;
    } catch {
      devices = [];
      sessions = [];
      sessionsAvailable = false;
      loaded = false;
    } finally {
      loading = false;
    }
    render();
  }

  refreshBtn.addEventListener("click", () => load(true));

  saveBtn.addEventListener("click", async () => {
    try {
      const enabled = !!ctx.getConfig()?.external_audio?.enabled;
      const r = await api.putExternalAudio({
        enabled,
        endpoint_id: deviceSelect.value,
        media_session_id: sessionSelect.value,
      });
      loaded = false;
      await ctx.onSaved({
        enabled: !!r.enabled,
        endpoint_id: r.endpoint_id ?? deviceSelect.value,
        media_session_id: r.media_session_id ?? sessionSelect.value,
      });
      toast(t("ext.saved"));
    } catch (e) {
      toast(e.message, true);
    }
  });

  function render() {
    const state = ctx.getState();
    const onAir = state?.sources?.active === "external_audio";
    card.hidden = !onAir;
    if (!onAir) return;

    load();

    const deviceSig = `${endpoint}|${devices.map(d => `${d.id}:${d.name}:${d.is_default}`).join("|")}`;
    if (deviceSelect.dataset.sig !== deviceSig) {
      deviceSelect.dataset.sig = deviceSig;
      deviceSelect.replaceChildren(
        el("option", { value: "", selected: endpoint === "" }, t("ext.default_device")),
        ...devices.map(d =>
          el(
            "option",
            { value: d.id, selected: endpoint === d.id },
            `${d.name || d.id}${d.is_default ? ` (${t("ext.current_default")})` : ""}`,
          ),
        ),
      );
    }

    const sessionSig = `${sessionId}|${sessionsAvailable}|${sessions.map(s => `${s.id}:${s.name}:${s.is_current}`).join("|")}`;
    if (sessionSelect.dataset.sig !== sessionSig) {
      sessionSelect.dataset.sig = sessionSig;
      if (!sessionsAvailable) {
        sessionSelect.replaceChildren(
          el("option", { value: "", selected: true }, t("ext.session_unavailable")),
        );
        sessionSelect.disabled = true;
      } else {
        sessionSelect.replaceChildren(
          el("option", { value: "", selected: sessionId === "" }, t("ext.current_session")),
          ...sessions.map(s =>
            el(
              "option",
              { value: s.id, selected: sessionId === s.id },
              `${s.name || s.id}${s.is_current ? ` (${t("ext.current")})` : ""}`,
            ),
          ),
        );
        sessionSelect.disabled = false;
      }
    }

    const available = state?.sources?.available?.some(s => s.name === "external_audio");
    const active = state?.sources?.active === "external_audio";
    hint.textContent = !available
      ? t("ext.hint.not_available")
      : active
        ? t("ext.hint.active")
        : t("ext.hint.ready");
  }

  return {
    render,
    invalidate: () => {
      loaded = false;
      // Forces the option lists to rebuild even if the device/session data
      // itself hasn't changed — needed after a language change, since their
      // translated bits (e.g. "current default") are baked into option text.
      deviceSelect.dataset.sig = "";
      sessionSelect.dataset.sig = "";
    },
  };
}
