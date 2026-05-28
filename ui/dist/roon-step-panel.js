export function renderStepPanel(id, ctx, h) {
  if (id === "environment") return renderEnvironment(ctx, h);
  if (id === "authorize") return renderAuthorize(ctx, h);
  if (id === "zone") return renderZone(ctx);
  if (id === "endpoint") return renderEndpoint(ctx, h);
  return renderVerify(ctx, h);
}

function renderEnvironment(ctx, h) {
  return `<div class="roon-step roon-step-panel" data-roon-step="environment">
    <div class="step-head"><span>Environment</span></div>
    <ol class="roon-step-list">
      ${h.renderStep("Node.js", !!h.setup?.node_available, "Required for the Roon control sidecar.")}
      ${h.renderStep("Roon Server", h.setup?.roon_environment === "local_server", h.labelForEnvironment(h.setup?.roon_environment))}
    </ol>
    <p>${h.esc(h.labelForCable(h.setup?.cable_environment))}</p>
    <div class="row action-row">
      <button class="ghost" data-roon-action="open-roon-download" type="button">Open Roon download</button>
      <button class="ghost" data-roon-action="open-vb-download" type="button">Open VB-Audio download</button>
    </div>
  </div>`;
}

function renderAuthorize(ctx, h) {
  const text = ctx.setupNote || (ctx.pairing === "authorized" ?
    "FH6 Universal Radio is authorized." : ctx.pairing);
  return `<div class="roon-step roon-step-panel" data-roon-step="authorize">
    <div class="step-head"><span>Authorize extension</span></div>
    <p>${h.esc(text)}</p>
    <div class="row action-row">
      <button class="ghost" data-roon-action="reconnect" type="button">Reconnect</button>
      <button class="ghost" data-roon-action="recheck" type="button">Recheck</button>
    </div>
  </div>`;
}

function renderZone(ctx) {
  return `<div class="roon-step roon-step-panel" data-roon-step="zone">
    <div class="step-head"><span>Select zone</span></div>
    <label class="field">
      <span>Zone</span>
      <select data-roon-zone>${ctx.zoneOptions}</select>
    </label>
  </div>`;
}

function renderEndpoint(ctx, h) {
  return `<div class="roon-step roon-step-panel" data-roon-step="endpoint">
    <div class="step-head"><span>Loopback endpoint</span></div>
    <p>${h.esc(ctx.route)}</p>
    <p class="value-line">${h.esc(ctx.endpoint?.name || "No recommended endpoint found")}</p>
    <button class="primary" data-roon-action="use-recommended" type="button"${ctx.endpoint?.id ? "" : " disabled"}>Use recommended device</button>
    <label class="field">
      <span>Roon Output / Loopback Capture Device</span>
      <select data-roon-endpoint>${ctx.endpointOptions}</select>
    </label>
  </div>`;
}

function renderVerify(ctx, h) {
  return `<div class="roon-step roon-step-panel" data-roon-step="verify">
    <div class="step-head"><span>Verify audio</span></div>
    <ol class="roon-step-list">${h.renderOptionalStep("Test audio", ctx.testOk, ctx.testResult)}</ol>
    <p class="muted">This panel runs Test audio every 3 seconds while a loopback endpoint is selected.</p>
    <button class="primary" data-roon-action="test-audio" type="button"${ctx.selectedEndpoint || ctx.endpoint?.id ? "" : " disabled"}>Test audio</button>
    <div class="step-head"><span>Targeted warnings</span></div>
    <ul class="warning-list">${h.issuesMarkup()}</ul>
  </div>`;
}
