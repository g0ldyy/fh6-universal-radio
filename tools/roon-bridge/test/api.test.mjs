import { after, before, describe, it } from "node:test";
import assert from "node:assert/strict";

import { createApiServer } from "../lib/api-server.mjs";

class MockRuntime {
  constructor() {
    this.selectedZoneId = "";
    this.transportCalls = [];
    this.volumeCalls = [];
    this.reconnects = 0;
    this.artwork = null;
    this.zones = [
      {
        zone_id: "zone-1",
        display_name: "Main Room",
        state: "paused",
        outputs: [{ output_id: "output-1", display_name: "Speakers", volume: { value: 35 } }],
      },
    ];
  }

  async health() {
    return { ok: true, roon_api_loaded: true };
  }

  async status() {
    return {
      core: { id: "core-1", name: "Roon Server", paired: true },
      pairing_state: "authorized",
      selected_zone_id: this.selectedZoneId,
      error: "",
    };
  }

  async listZones() {
    return this.zones;
  }

  async listOutputs() {
    return this.zones.flatMap(zone => zone.outputs);
  }

  async selectZone(zoneId) {
    if (!this.zones.some(zone => zone.zone_id === zoneId)) {
      const err = new Error("unknown zone_id");
      err.statusCode = 404;
      throw err;
    }
    this.selectedZoneId = zoneId;
    return this.status();
  }

  async transport(request) {
    this.transportCalls.push(request);
    return { ok: true };
  }

  async volume(request) {
    this.volumeCalls.push(request);
    return { ok: true };
  }

  async currentArtwork() {
    return this.artwork;
  }

  async reconnect() {
    this.reconnects += 1;
    return { ok: true, attempts: this.reconnects };
  }
}

async function readJson(response) {
  assert.match(response.headers.get("content-type") ?? "", /application\/json/);
  return response.json();
}

describe("Roon sidecar local API", () => {
  let runtime;
  let server;

  before(async () => {
    runtime = new MockRuntime();
    server = await createApiServer({ runtime, host: "127.0.0.1", port: 0, logger: () => {} });
  });

  after(async () => {
    await server.close();
  });

  it("returns JSON health and status", async () => {
    const health = await readJson(await fetch(`${server.url}/health`));
    assert.equal(health.ok, true);
    assert.equal(health.service, "fh6-roon-bridge");

    const status = await readJson(await fetch(`${server.url}/status`));
    assert.equal(status.core.id, "core-1");
    assert.equal(status.pairing_state, "authorized");
  });

  it("lists zones and outputs", async () => {
    const zones = await readJson(await fetch(`${server.url}/zones`));
    assert.equal(zones.zones[0].zone_id, "zone-1");

    const outputs = await readJson(await fetch(`${server.url}/outputs`));
    assert.equal(outputs.outputs[0].output_id, "output-1");
  });

  it("validates zone selection payloads", async () => {
    const missing = await fetch(`${server.url}/select-zone`, { method: "POST", body: "{}" });
    assert.equal(missing.status, 400);
    assert.match((await readJson(missing)).error, /zone_id/);

    const unknown = await fetch(`${server.url}/select-zone`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ zone_id: "missing" }),
    });
    assert.equal(unknown.status, 404);

    const selected = await readJson(await fetch(`${server.url}/select-zone`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ zone_id: "zone-1" }),
    }));
    assert.equal(selected.selected_zone_id, "zone-1");
  });

  it("accepts only supported transport controls", async () => {
    const bad = await fetch(`${server.url}/transport`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ control: "shuffle" }),
    });
    assert.equal(bad.status, 400);

    const ok = await readJson(await fetch(`${server.url}/transport`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ control: "playpause", zone_id: "zone-1" }),
    }));
    assert.equal(ok.ok, true);
    assert.deepEqual(runtime.transportCalls.at(-1), { control: "playpause", zone_id: "zone-1" });
  });

  it("validates volume payloads", async () => {
    const bad = await fetch(`${server.url}/volume`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ output_id: "output-1", how: "absolute" }),
    });
    assert.equal(bad.status, 400);

    const ok = await readJson(await fetch(`${server.url}/volume`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ output_id: "output-1", how: "absolute", value: 40 }),
    }));
    assert.equal(ok.ok, true);
    assert.deepEqual(runtime.volumeCalls.at(-1), {
      output_id: "output-1",
      how: "absolute",
      value: 40,
    });
  });

  it("returns artwork bytes or 404", async () => {
    const missing = await fetch(`${server.url}/artwork/current`);
    assert.equal(missing.status, 404);

    runtime.artwork = { contentType: "image/png", bytes: Buffer.from([1, 2, 3]) };
    const image = await fetch(`${server.url}/artwork/current`);
    assert.equal(image.status, 200);
    assert.equal(image.headers.get("content-type"), "image/png");
    assert.deepEqual(new Uint8Array(await image.arrayBuffer()), new Uint8Array([1, 2, 3]));
  });

  it("triggers reconnect", async () => {
    const body = await readJson(await fetch(`${server.url}/reconnect`, { method: "POST" }));
    assert.equal(body.ok, true);
    assert.equal(body.attempts, 1);
  });
});
