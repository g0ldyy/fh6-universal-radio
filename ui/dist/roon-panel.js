const ROON_DOWNLOAD_URL = "https://roon.app/en/downloads";
const VB_AUDIO_URL = "https://vb-audio.com/Cable/";

export function createRoonPanel(deps) {
  const { api, $, setText, toast, getState, getConfig, setConfig, requestRender } = deps;
  const roon = {
    status: null,
    setup: null,
    zones: [],
    outputs: [],
    devices: [],
    error: "",
    captureTest: null,
    loading: false,
    fetchedAt: 0,
  };

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

  function syncOptions(select, items, value, emptyLabel) {
    const sig = items.map(i => `${i.value}\u0000${i.label}`).join("\u0001");
    if (select.dataset.sig !== sig) {
      const empty = document.createElement("option");
      empty.value = "";
      empty.textContent = emptyLabel;
      select.replaceChildren(empty, ...items.map(item => {
        const opt = document.createElement("option");
        opt.value = item.value;
        opt.textContent = item.label;
        return opt;
      }));
      select.dataset.sig = sig;
    }
    if (select.value !== value) select.value = value || "";
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

  function setButtonState(id, disabled) {
    const button = $(id);
    if (button) button.disabled = !!disabled;
  }

  function renderIssues() {
    const list = $("#roon-warning-list");
    if (!list) return;
    const issues = roon.setup?.issues || [];
    if (!issues.length) {
      list.innerHTML = '<li class="ok">No setup warnings detected.</li>';
      return;
    }
    list.innerHTML = issues.map(i =>
      `<li class="${i.severity === "error" ? "err" : "warn"}">${i.message}</li>`
    ).join("");
  }

  function renderRoonPanel() {
    const card = $("#roon-setup-card");
    if (!card) return;
    const available = roonAvailable();
    card.hidden = !available;
    if (!available) return;

    const r = getConfig()?.roon || {};
    const status = roon.status || {};
    const source = getState()?.sources?.available?.find(s => s.name === "roon");
    const endpoint = recommendedEndpoint();
    const selected = selectedEndpointName();
    const setupNote = source?.auth_state === "authenticated" ? "" : source?.auth_instructions || "";
    const pairing = status.pairing_state || "unknown";
    const level = roon.captureTest?.peak == null
      ? "untested"
      : `${Math.round(roon.captureTest.peak * 100)}%`;

    setText($("#roon-summary"), status.selected_zone_name
      ? `${status.selected_zone_name}${selected ? " - " + selected : ""}`
      : "No zone selected");
    setText($("#roon-pairing"), pairing);
    $("#roon-pairing").className = "value " + (pairing === "authorized" ? "" : "warn");
    setText($("#roon-core"), status.core?.name || "Core not found");
    setText($("#roon-level"), level);
    setText($("#roon-environment"), labelForEnvironment(roon.setup?.roon_environment));
    setText($("#roon-cable-environment"), labelForCable(roon.setup?.cable_environment));
    setText($("#roon-recommended"), endpoint?.name || "No recommended endpoint found");
    setText($("#roon-routing"), endpoint?.name
      ? `Set the Roon zone output to ${endpoint.name}, then select the same endpoint here.`
      : "Install or enable a VB-Audio render endpoint, then recheck.");
    setText($("#roon-test-result"), roon.captureTest?.peak == null
      ? "Audio has not been tested."
      : roon.captureTest.message || `Audio level ${Math.round(roon.captureTest.peak * 100)}%.`);
    setText($("#roon-error"), roon.error || status.error || setupNote);
    renderIssues();

    syncOptions($("#roon-zone"), roon.zones.map(z => ({
      value: z.id || z.zone_id,
      label: `${z.display_name}${z.state ? " (" + z.state + ")" : ""}`,
    })), r.selected_zone_id || status.selected_zone_id, "Select zone");
    syncOptions($("#roon-capture"), roon.devices.map(d => ({
      value: d.id,
      label: `${d.name}${d.is_default ? " (default)" : ""}`,
    })), selectedEndpointId(), "Select loopback endpoint");
    setButtonState("#roon-use-recommended", !endpoint?.id);
    setButtonState("#roon-test-capture", !selectedEndpointId() && !endpoint?.id);
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
    await api.send("/api/source/roon/select-capture-device", {
      device_id: endpoint.id,
      name: endpoint.name || "",
    });
    setSelectedEndpoint(endpoint);
    requestRender();
  }

  function wireRoonPanel() {
    $("#roon-zone").addEventListener("change", async e => {
      const zone_id = e.target.value;
      if (!zone_id) return;
      try {
        await api.send("/api/source/roon/select-zone", { zone_id });
        cfgRoon().selected_zone_id = zone_id;
        await refreshRoon(true);
        toast("Roon zone selected");
      } catch (err) { toast(err.message, true); }
    });

    $("#roon-capture").addEventListener("change", async e => {
      const id = e.target.value;
      if (!id) return;
      const endpoint = roon.devices.find(d => d.id === id) || { id, name: "" };
      try {
        await selectEndpoint(endpoint);
        toast("Loopback endpoint selected");
      } catch (err) { toast(err.message, true); }
    });

    $("#roon-reconnect").onclick = async () => {
      try {
        await api.send("/api/source/roon/reconnect", {});
        await refreshRoon(true);
        toast("Roon reconnecting");
      } catch (err) { toast(err.message, true); }
    };

    $("#roon-recheck").onclick = async () => {
      await refreshRoon(true);
      toast("Roon setup checked");
    };

    $("#roon-use-recommended").onclick = async () => {
      const endpoint = recommendedEndpoint();
      if (!endpoint?.id) return toast("No recommended endpoint found", true);
      try {
        await selectEndpoint(endpoint);
        toast("Recommended endpoint selected");
      } catch (err) { toast(err.message, true); }
    };

    $("#roon-test-capture").onclick = async () => {
      const endpoint = selectedEndpointId() || recommendedEndpoint()?.id;
      if (!endpoint) return toast("Select a loopback endpoint first", true);
      try {
        roon.captureTest = await api.send("/api/source/roon/test-capture", { device_id: endpoint });
        requestRender();
        toast(`Audio level ${Math.round((roon.captureTest.peak || 0) * 100)}%`);
      } catch (err) { toast(err.message, true); }
    };

    $("#roon-open-download").onclick = () => openSetupUrl("roon", ROON_DOWNLOAD_URL);
    $("#roon-open-vb-download").onclick = () => openSetupUrl("vb_hifi_cable", VB_AUDIO_URL);
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
