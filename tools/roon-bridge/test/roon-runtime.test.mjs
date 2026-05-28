import { createRequire } from "node:module";
import { afterEach, describe, it } from "node:test";
import assert from "node:assert/strict";

import { buildStatus, createRoonRuntime } from "../lib/roon-runtime.mjs";

const require = createRequire(import.meta.url);
const wsWebSocket = require("ws");
const originalWebSocket = globalThis.WebSocket;

describe("Roon runtime compatibility", () => {
  afterEach(() => {
    globalThis.WebSocket = originalWebSocket;
  });

  it("forces node-roon-api to use ws instead of Node's browser WebSocket", () => {
    class BrowserWebSocketStub {}

    globalThis.WebSocket = BrowserWebSocketStub;

    const runtime = createRoonRuntime();
    runtime.stop();

    assert.equal(globalThis.WebSocket, wsWebSocket);
  });

  it("clears selected runtime state on stop", async () => {
    const runtime = createRoonRuntime({ selectedZoneId: "zone-1" });
    runtime.stop();

    const status = await runtime.status();
    assert.equal(status.core, null);
    assert.equal(status.pairing_state, "stopped");
    assert.equal(status.selected_zone_id, "");
    assert.deepEqual(await runtime.listZones(), []);
  });

  it("marks a selected zone unavailable when Roon no longer reports it", () => {
    const status = buildStatus({
      core: { core_id: "core-1", display_name: "Roon Server", display_version: "2.0" },
      pairingState: "authorized",
      selectedZoneId: "stale-zone",
      zones: new Map(),
      lastError: "",
    });

    assert.equal(status.ok, false);
    assert.equal(status.zone_available, false);
    assert.match(status.error, /zone/i);
  });
});
