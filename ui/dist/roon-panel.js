import { renderStepPanel } from "./roon-step-panel.js";
const ROON_DOWNLOAD_URL = "https://roon.app/en/downloads";
const VB_AUDIO_URL = "https://vb-audio.com/Cable/";
export function createRoonPanel(deps) {
  const { api, $, toast, getState, getConfig, setConfig, requestRender } = deps;
  const roon = {
    status: null,
    setup: null,
    zones: [],
    outputs: [],
    devices: [],
    error: "",
    captureTest: null,
    captureTesting: false,
    captureTimer: 0,
    dialogStep: "",
    loading: false,
    fetchedAt: 0,
  };
  const stepOrder = ["environment", "authorize", "zone", "endpoint", "verify"];
  const requiredStepIds = ["environment", "authorize", "zone", "endpoint"];
  function cfgRoon() {
    const next = getConfig() || {};
    next.roon ??= {};
    if (!getConfig()) setConfig(next);
    return next.roon;
  }
  function roonAvailable() {
    return getState()?.sources?.available?.some(s => s.name === "roon");
  }

  function roonNowPlaying() {
    if (getState()?.sources?.active !== "roon") return null;
    const np = roon.status?.now_playing;
    if (!np) return null;
    return {
      title:       np.title || np.three_line?.line1 || "",
      artist:      np.artist || np.three_line?.line2 || "",
      album:       np.album || np.three_line?.line3 || "",
      artwork_url: np.artwork_url || (np.image_key ? "/api/source/roon/artwork/current" : ""),
      duration_ms: np.duration_ms || (np.length ? Math.round(np.length * 1000) : 0),
      position_ms: np.position_ms || (np.seek_position ? Math.round(np.seek_position * 1000) : 0),
    };
  }
  function selectedEndpointId() {
    const r = getConfig()?.roon || {};
    return r.render_loopback_endpoint_id || r.capture_device_id || "";
  }
  function selectedEndpointName() {
    const r = getConfig()?.roon || {};
    const id = selectedEndpointId();
    return r.render_loopback_endpoint_name || r.capture_device_name ||
      roon.devices.find(d => d.id === id)?.name || "";
  }
  function autoTestTarget() {
    return selectedEndpointId() || recommendedEndpoint()?.id || "";
  }
  async function runAudioTest(showToast = false) {
    const endpoint = autoTestTarget();
    if (!endpoint || roon.captureTesting) return null;
    roon.captureTesting = true;
    try {
      roon.captureTest = await api.send("/api/source/roon/test-capture", { device_id: endpoint });
      if (showToast) toast(`Audio level ${Math.round((roon.captureTest.peak || 0) * 100)}%`);
      requestRender();
      return roon.captureTest;
    } catch (err) {
      if (showToast) toast(err.message, true);
      else {
        roon.captureTest = { ok: false, peak: 0, message: err.message };
        requestRender();
      }
      return null;
    } finally {
      roon.captureTesting = false;
    }
  }
  function startAutoAudioTest() {
    if (roon.captureTimer || !autoTestTarget()) return;
    void runAudioTest();
    roon.captureTimer = window.setInterval(() => { void runAudioTest(); }, 3000);
  }
  function stopAutoAudioTest() {
    if (!roon.captureTimer) return;
    window.clearInterval(roon.captureTimer);
    roon.captureTimer = 0;
  }

  function recommendedEndpoint() {
    const endpoint = roon.setup?.recommended_endpoint;
    if (endpoint?.id) return endpoint;
    return roon.devices.find(d => d.recommendation === "preferred") ||
      roon.devices.find(d => d.recommendation === "fallback") || null;
  }

  function sourceDetailLine() {
    const parts = [];
    const status = roon.status || {};
    const loopback = selectedEndpointName();
    if (status.pairing_state) parts.push(status.pairing_state);
    if (status.core?.name) parts.push(status.core.name);
    if (status.selected_zone_name) parts.push(`Zone: ${status.selected_zone_name}`);
    if (loopback) parts.push(`Loopback: ${loopback}`);
    if (roon.captureTest?.peak != null) {
      parts.push(`Level: ${Math.round(roon.captureTest.peak * 100)}%`);
    }
    return parts.join(" - ") || null;
  }

  function signature() {
    return [
      roon.status?.pairing_state,
      roon.status?.core?.name,
      roon.status?.selected_zone_name,
      roon.setup?.roon_environment,
      roon.setup?.cable_environment,
      recommendedEndpoint()?.id,
      selectedEndpointId(),
      selectedEndpointName(),
      roon.captureTest?.peak,
    ].join(":");
  }

  async function roonGet(path) {
    const body = await api.get(path);
    if (body?.error) throw new Error(body.error);
    return body;
  }

  async function refreshRoon(force = false) {
    if (!roonAvailable() || roon.loading) return;
    const interval = Math.max(500, getConfig()?.roon?.metadata_poll_ms || 750);
    if (!force && roon.fetchedAt && Date.now() - roon.fetchedAt < interval) return;
    roon.loading = true;
    try {
      if (!getConfig()) setConfig(await api.get("/api/config"));
      const results = await Promise.allSettled([
        roonGet("/api/source/roon/setup"),
        roonGet("/api/source/roon/status"),
        roonGet("/api/source/roon/zones"),
        roonGet("/api/source/roon/outputs"),
        roonGet("/api/source/roon/loopback-endpoints"),
      ]);
      const [setup, status, zones, outputs, devices] = results;
      if (setup.status === "fulfilled") roon.setup = setup.value;
      if (status.status === "fulfilled") roon.status = status.value;
      if (zones.status === "fulfilled") roon.zones = zones.value.zones || [];
      if (outputs.status === "fulfilled") roon.outputs = outputs.value.outputs || [];
      if (devices.status === "fulfilled") roon.devices = devices.value.devices || [];
      roon.error = results
        .filter(r => r.status === "rejected")
        .map(r => r.reason.message)
        .join("; ");
      roon.fetchedAt = Date.now();
    } catch (e) {
      roon.error = e.message;
    } finally {
      roon.loading = false;
      requestRender();
    }
  }

  function labelForEnvironment(value) {
    return ({
      local_server: "Roon Server detected",
      bridge_only: "Roon Bridge detected without a local server",
      not_found: "Roon Server not detected",
    })[value] || "Waiting for diagnostics";
  }

  function labelForCable(value) {
    return ({
      hifi: "Hi-Fi Cable render endpoint detected",
      vb_cable: "VB-CABLE render endpoint detected",
      multiple: "Multiple VB-Audio render endpoints detected",
      partial: "Only a recording-side endpoint was detected",
      missing: "No VB-Audio render endpoint detected",
    })[value] || "Waiting for endpoint scan";
  }

  function esc(value) {
    return String(value ?? "")
      .replaceAll("&", "&amp;")
      .replaceAll("<", "&lt;")
      .replaceAll(">", "&gt;")
      .replaceAll('"', "&quot;")
      .replaceAll("'", "&#39;");
  }

  function issuesMarkup() {
    const issues = roon.setup?.issues || [];
    if (!issues.length) {
      return '<li class="ok">No setup warnings detected.</li>';
    }
    return issues.map(i =>
      `<li class="${i.severity === "error" ? "err" : "warn"}">${esc(i.message)}</li>`
    ).join("");
  }

  function roonSetupEnabled() {
    return roonAvailable();
  }

  function setupComplete() {
    const r = getConfig()?.roon || {};
    const status = roon.status || {};
    return status.pairing_state === "authorized" &&
      !!(r.selected_zone_id || status.selected_zone_id) &&
      !!selectedEndpointId();
  }

  function shouldOpenSetupDialog() {
    return roonAvailable() && roonSetupEnabled() && !setupComplete() &&
      sessionStorage.getItem("fh6-roon-setup-dismissed") !== "1";
  }

  function renderStep(label, ok, detail) {
    return `<li class="${ok ? "ok" : "warn"}">
      <span>${esc(label)}</span>
      <strong>${ok ? "Ready" : "Action needed"}</strong>
      <small>${esc(detail)}</small>
    </li>`;
  }

  function renderOptionalStep(label, ok, detail) {
    return `<li class="${ok ? "ok" : "warn"}">
      <span>${esc(label)}</span>
      <strong>${ok ? "Ready" : "Optional validation"}</strong>
      <small>${esc(detail)}</small>
    </li>`;
  }

  function renderOptions(items, selected, emptyLabel) {
    return `<option value="">${esc(emptyLabel)}</option>` + items.map(item =>
      `<option value="${esc(item.value)}"${item.value === selected ? " selected" : ""}>${esc(item.label)}</option>`
    ).join("");
  }

  function setupContext() {
    const r = getConfig()?.roon || {};
    const status = roon.status || {};
    const source = getState()?.sources?.available?.find(s => s.name === "roon");
    const endpoint = recommendedEndpoint();
    const setupNote = source?.auth_state === "authenticated" ? "" : source?.auth_instructions || "";
    const pairing = status.pairing_state || "unknown";
    const selectedZone = r.selected_zone_id || status.selected_zone_id;
    const selectedEndpoint = selectedEndpointId();
    const testOk = !!roon.captureTest?.ok;
    const route = endpoint?.name
      ? `Set the Roon zone output to ${endpoint.name}, then select the same endpoint here.`
      : "Install or enable a VB-Audio render endpoint, then recheck.";
    const testResult = roon.captureTest?.peak == null
      ? "Audio test starts automatically while this panel is open."
      : roon.captureTest.message || `Audio level ${Math.round(roon.captureTest.peak * 100)}%.`;
    const zoneOptions = renderOptions(roon.zones.map(z => ({
      value: z.id || z.zone_id,
      label: `${z.display_name}${z.state ? " (" + z.state + ")" : ""}`,
    })), selectedZone, "Select zone");
    const endpointOptions = renderOptions(roon.devices.map(d => ({
      value: d.id,
      label: `${d.name}${d.is_default ? " (default)" : ""}`,
    })), selectedEndpoint, "Select loopback endpoint");
    return {
      r, status, source, endpoint, setupNote, pairing, selectedZone, selectedEndpoint,
      testOk, route, testResult, zoneOptions, endpointOptions,
    };
  }

  function stepInfo(id, ctx = setupContext()) {
    if (id === "environment") {
      const nodeOk = !!roon.setup?.node_available;
      const serverOk = roon.setup?.roon_environment === "local_server";
      return {
        label: "Environment",
        ok: nodeOk && serverOk,
        detail: `${nodeOk ? "Node.js ready." : "Node.js is required."} ${labelForEnvironment(roon.setup?.roon_environment)}`,
      };
    }
    if (id === "authorize") {
      return {
        label: "Authorize extension",
        ok: ctx.pairing === "authorized",
        detail: ctx.pairing === "authorized" ? "FH6 Universal Radio is authorized." : ctx.setupNote || ctx.pairing,
      };
    }
    if (id === "zone") {
      return {
        label: "Select zone",
        ok: !!ctx.selectedZone,
        detail: ctx.status.selected_zone_name || "Choose the Roon zone to control.",
      };
    }
    if (id === "endpoint") {
      return {
        label: "Select loopback endpoint",
        ok: !!ctx.selectedEndpoint,
        detail: selectedEndpointName() || ctx.endpoint?.name || "Use the recommended render endpoint.",
      };
    }
    return {
      label: "Test audio",
      ok: ctx.testOk,
      detail: ctx.testResult,
      optional: true,
    };
  }

  function firstIncompleteStep(ctx = setupContext()) {
    return requiredStepIds.find(id => !stepInfo(id, ctx).ok) || "verify";
  }

  function activeDialogStep() {
    return stepOrder.includes(roon.dialogStep) ? roon.dialogStep : firstIncompleteStep();
  }

  function renderRoonSettingsSummary() {
    const ctx = setupContext();
    const next = stepInfo(firstIncompleteStep(ctx), ctx);
    const summary = requiredStepIds.map(id => {
      const item = stepInfo(id, ctx);
      return renderStep(item.label, item.ok, item.detail);
    }).join("");
    const statusText = setupComplete()
      ? "Roon source is ready. Audio testing is optional."
      : `Next step: ${next.label}`;
    return `<div class="roon-panel roon-setup-surface" data-roon-setup="settings">
      <div class="card-head">
        <div>
          <h2>Roon setup</h2>
          <p class="muted">${esc(statusText)}</p>
        </div>
        <div class="row action-row">
          <button class="ghost" data-roon-action="open-dialog" type="button">Open setup wizard</button>
          <button class="ghost" data-roon-action="recheck" type="button">Recheck</button>
        </div>
      </div>
      <ol class="roon-step-list">${summary}</ol>
      <p class="muted">${esc(roon.error || ctx.status.error || ctx.setupNote)}</p>
    </div>`;
  }

  function renderStepNav(ctx, active) {
    return stepOrder.map(id => {
      const item = stepInfo(id, ctx);
      return `<li class="${item.ok ? "ok" : "warn"}${id === active ? " active" : ""}" data-roon-action="goto-step" data-roon-step="${esc(id)}">
        <span>${esc(item.label)}</span>
        <strong>${item.ok ? "Ready" : item.optional ? "Optional validation" : "Action needed"}</strong>
        <small>${esc(item.detail)}</small>
      </li>`;
    }).join("");
  }

  function renderRoonSetupDialog() {
    const ctx = setupContext();
    const active = activeDialogStep();
    const index = stepOrder.indexOf(active);
    return `<div class="roon-panel roon-setup-surface" data-roon-setup="dialog">
      <div class="card-head">
        <div>
          <h2>Roon setup</h2>
          <p class="muted">${esc(ctx.status.selected_zone_name || "Complete each required step before using Roon in-game.")}</p>
        </div>
        <div class="row action-row">
          <button class="ghost" data-roon-action="recheck" type="button">Recheck</button>
          <button class="ghost" data-roon-action="reconnect" type="button">Reconnect</button>
        </div>
      </div>
      <ol class="roon-step-list">${renderStepNav(ctx, active)}</ol>
      <div class="roon-wizard">${renderStepPanel(active, ctx, {
        esc, renderStep, renderOptionalStep, issuesMarkup,
        labelForCable, labelForEnvironment, setup: roon.setup,
      })}</div>
      <div class="roon-controls">
        <button class="ghost" data-roon-action="prev-step" type="button"${index <= 0 ? " disabled" : ""}>Previous</button>
        <button class="primary" data-roon-action="next-step" type="button"${index >= stepOrder.length - 1 ? " disabled" : ""}>Next</button>
      </div>
      <p class="muted">${esc(roon.error || ctx.status.error || ctx.setupNote)}</p>
    </div>`;
  }

  function renderRoonPanel() {
    const showSetup = roonSetupEnabled();
    const settings = $("#roon-settings-wizard");
    if (settings) {
      settings.hidden = !showSetup;
      settings.innerHTML = showSetup ? renderRoonSettingsSummary() : "";
    }
    const dialog = $("#roon-setup-dialog");
    if (!showSetup && dialog?.open) dialog.close();
    if (dialog && shouldOpenSetupDialog() && !dialog.open) {
      roon.dialogStep = firstIncompleteStep();
      dialog.showModal();
    }
    const dialogBody = $("#roon-dialog-wizard");
    if (dialogBody) dialogBody.innerHTML = showSetup ? renderRoonSetupDialog() : "";
    const drawerOpen = !!$("#drawer")?.classList.contains("open");
    if (showSetup && (dialog?.open || (drawerOpen && settings && !settings.hidden))) startAutoAudioTest();
    else stopAutoAudioTest();
  }

  function openSetupUrl(key, fallback) {
    const url = roon.setup?.official_urls?.[key] || fallback;
    window.open(url, "_blank", "noopener,noreferrer");
  }

  function setSelectedEndpoint(endpoint) {
    const r = cfgRoon();
    r.render_loopback_endpoint_id = endpoint.id;
    r.render_loopback_endpoint_name = endpoint.name || "";
    r.capture_device_id = endpoint.id;
    r.capture_device_name = endpoint.name || "";
  }

  async function selectEndpoint(endpoint) {
    await api.send("/api/source/roon/select-loopback-endpoint", {
      endpoint_id: endpoint.id,
      name: endpoint.name || "",
    });
    setSelectedEndpoint(endpoint);
    requestRender();
  }

  function wireRoonPanel() {
    document.addEventListener("change", async e => {
      if (!e.target.matches("[data-roon-zone]") && !e.target.matches("[data-roon-endpoint]")) return;
      if (e.target.matches("[data-roon-endpoint]")) {
        const id = e.target.value;
        if (!id) return;
        const endpoint = roon.devices.find(d => d.id === id) || { id, name: "" };
        try {
          await selectEndpoint(endpoint);
          toast("Loopback endpoint selected");
        } catch (err) { toast(err.message, true); }
        return;
      }
      const zone_id = e.target.value;
      if (!zone_id) return;
      try {
        await api.send("/api/source/roon/select-zone", { zone_id });
        cfgRoon().selected_zone_id = zone_id;
        await refreshRoon(true);
        toast("Roon zone selected");
      } catch (err) { toast(err.message, true); }
    });

    document.addEventListener("click", async e => {
      const button = e.target.closest("[data-roon-action]");
      const action = button?.dataset.roonAction;
      if (!action) return;
      try {
        if (action === "close-dialog") {
          sessionStorage.setItem("fh6-roon-setup-dismissed", "1");
          $("#roon-setup-dialog")?.close();
          stopAutoAudioTest();
        } else if (action === "open-dialog") {
          sessionStorage.removeItem("fh6-roon-setup-dismissed");
          roon.dialogStep = firstIncompleteStep();
          $("#roon-setup-dialog")?.showModal();
          requestRender();
        } else if (action === "goto-step") {
          if (button.dataset.roonStep) roon.dialogStep = button.dataset.roonStep;
          requestRender();
        } else if (action === "prev-step" || action === "next-step") {
          const current = stepOrder.indexOf(activeDialogStep());
          const delta = action === "next-step" ? 1 : -1;
          roon.dialogStep = stepOrder[Math.max(0, Math.min(stepOrder.length - 1, current + delta))];
          requestRender();
        } else if (action === "open-roon-download") {
          openSetupUrl("roon", ROON_DOWNLOAD_URL);
        } else if (action === "open-vb-download") {
          openSetupUrl("vb_hifi_cable", VB_AUDIO_URL);
        } else if (action === "reconnect") {
          await api.send("/api/source/roon/reconnect", {});
          await refreshRoon(true);
          toast("Roon reconnecting");
        } else if (action === "recheck") {
          await refreshRoon(true);
          toast("Roon setup checked");
        } else if (action === "use-recommended") {
          const endpoint = recommendedEndpoint();
          if (!endpoint?.id) return toast("No recommended endpoint found", true);
          await selectEndpoint(endpoint);
          toast("Recommended endpoint selected");
        } else if (action === "test-audio") {
          if (!autoTestTarget()) return toast("Select a loopback endpoint first", true);
          await runAudioTest(true);
        }
      } catch (err) { toast(err.message, true); }
    });
  }

  return {
    refresh: refreshRoon,
    renderPanel: renderRoonPanel,
    wire: wireRoonPanel,
    nowPlaying: roonNowPlaying,
    sourceDetailLine,
    signature,
  };
}
