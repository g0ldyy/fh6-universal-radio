import { createRequire } from "node:module";

const require = createRequire(import.meta.url);

function installNodeRoonWebSocket() {
  globalThis.WebSocket = require("ws");
}

function normalizeZone(zone) {
  return {
    zone_id: zone.zone_id,
    display_name: zone.display_name,
    state: zone.state,
    seek_position: zone.seek_position,
    now_playing: zone.now_playing ?? null,
    outputs: Array.isArray(zone.outputs) ? zone.outputs.map(normalizeOutput) : [],
    is_previous_allowed: !!zone.is_previous_allowed,
    is_next_allowed: !!zone.is_next_allowed,
    is_pause_allowed: !!zone.is_pause_allowed,
    is_play_allowed: !!zone.is_play_allowed,
    is_seek_allowed: !!zone.is_seek_allowed,
  };
}

function normalizeOutput(output) {
  return {
    output_id: output.output_id,
    zone_id: output.zone_id,
    display_name: output.display_name,
    state: output.state,
    volume: output.volume ?? null,
  };
}

function callbackResult(resolve, reject) {
  return (err, body) => {
    if (err) reject(new Error(String(err)));
    else resolve(body ?? { ok: true });
  };
}

export function createRoonRuntime(options = {}) {
  installNodeRoonWebSocket();

  const RoonApi = require("node-roon-api");
  const RoonApiStatus = require("node-roon-api-status");
  const RoonApiTransport = require("node-roon-api-transport");
  const RoonApiImage = require("node-roon-api-image");

  const state = {
    core: null,
    transport: null,
    image: null,
    svcStatus: null,
    pairingState: "pending",
    selectedZoneId: options.selectedZoneId ?? "",
    zones: new Map(),
    lastError: "",
  };

  function selectedZone() {
    if (!state.selectedZoneId) return null;
    return state.zones.get(state.selectedZoneId) ?? null;
  }

  function updateZones(cmd, data) {
    if (cmd === "Subscribed") {
      state.zones = new Map((data.zones ?? []).map(zone => [zone.zone_id, zone]));
    } else if (cmd === "Changed") {
      for (const id of data.zones_removed ?? []) state.zones.delete(id);
      for (const zone of data.zones_added ?? []) state.zones.set(zone.zone_id, zone);
      for (const zone of data.zones_changed ?? []) state.zones.set(zone.zone_id, zone);
      for (const seek of data.zones_seek_changed ?? []) {
        const zone = state.zones.get(seek.zone_id);
        if (zone?.now_playing) zone.now_playing.seek_position = seek.seek_position;
      }
    } else if (cmd === "Unsubscribed") {
      state.zones.clear();
    }
  }

  const roon = new RoonApi({
    extension_id: "com.g0ldyy.fh6_universal_radio.roon_source",
    display_name: "FH6 Universal Radio",
    display_version: options.version ?? "0.1.0",
    publisher: "FH6 Universal Radio",
    email: "noreply@example.invalid",
    website: "https://github.com/g0ldyy/fh6-universal-radio",
    core_paired(core) {
      state.core = core;
      state.transport = core.services.RoonApiTransport;
      state.image = core.services.RoonApiImage;
      state.pairingState = "authorized";
      state.lastError = "";
      state.transport?.subscribe_zones((cmd, data) => updateZones(cmd, data ?? {}));
      state.svcStatus?.set_status("Connected to Roon Core", false);
    },
    core_unpaired(core) {
      if (state.core?.core_id === core.core_id) {
        state.core = null;
        state.transport = null;
        state.image = null;
        state.zones.clear();
      }
      state.pairingState = "disconnected";
      state.svcStatus?.set_status("Disconnected from Roon Core", true);
    },
  });

  state.svcStatus = new RoonApiStatus(roon);
  roon.init_services({
    required_services: [RoonApiTransport, RoonApiImage],
    provided_services: [state.svcStatus],
  });
  state.svcStatus.set_status("Waiting for Roon authorization", false);

  return {
    start() {
      roon.start_discovery();
    },

    stop() {
      state.svcStatus?.set_status("FH6 Universal Radio sidecar stopped", false);
    },

    async health() {
      return { roon_api_loaded: true, pairing_state: state.pairingState };
    },

    async status() {
      const zone = selectedZone();
      return {
        core: state.core
          ? {
              id: state.core.core_id,
              name: state.core.display_name,
              version: state.core.display_version,
              paired: true,
            }
          : null,
        pairing_state: state.pairingState,
        selected_zone_id: state.selectedZoneId,
        selected_zone_name: zone?.display_name ?? "",
        error: state.lastError,
        now_playing: zone?.now_playing ?? null,
      };
    },

    async listZones() {
      return [...state.zones.values()].map(normalizeZone);
    },

    async listOutputs() {
      return [...state.zones.values()].flatMap(zone => (zone.outputs ?? []).map(normalizeOutput));
    },

    async selectZone(zoneId) {
      if (!state.zones.has(zoneId)) {
        const err = new Error("unknown zone_id");
        err.statusCode = 404;
        throw err;
      }
      state.selectedZoneId = zoneId;
      return this.status();
    },

    async transport({ control, zone_id: zoneId }) {
      const zone = state.zones.get(zoneId || state.selectedZoneId);
      if (!state.transport || !zone) {
        const err = new Error("selected zone is unavailable");
        err.statusCode = 409;
        throw err;
      }
      await new Promise((resolve, reject) =>
        state.transport.control(zone, control, callbackResult(resolve, reject)),
      );
      return { ok: true };
    },

    async volume({ output_id: outputId, zone_id: zoneId, how, value }) {
      const zones = [...state.zones.values()];
      const zone = zoneId ? state.zones.get(zoneId) : selectedZone();
      const outputs = zone ? zone.outputs ?? [] : zones.flatMap(item => item.outputs ?? []);
      const output = outputs.find(item => item.output_id === outputId) ?? outputs[0];
      if (!state.transport || !output) {
        const err = new Error("selected output is unavailable");
        err.statusCode = 409;
        throw err;
      }
      await new Promise((resolve, reject) =>
        state.transport.change_volume(output, how, value, callbackResult(resolve, reject)),
      );
      return { ok: true };
    },

    async currentArtwork() {
      const imageKey = selectedZone()?.now_playing?.image_key;
      if (!state.image || !imageKey) return null;
      return new Promise((resolve, reject) => {
        state.image.get_image(
          imageKey,
          { scale: "fit", width: 600, height: 600, format: "image/jpeg" },
          (err, contentType, image) => {
            if (err) reject(new Error(String(err)));
            else resolve({ contentType, bytes: image });
          },
        );
      });
    },

    async reconnect() {
      state.lastError = "";
      roon.start_discovery();
      return { ok: true };
    },
  };
}
