import http from "node:http";

const TRANSPORT_CONTROLS = new Set(["play", "pause", "playpause", "stop", "previous", "next"]);
const VOLUME_MODES = new Set(["absolute", "relative", "relative_step"]);

function jsonResponse(res, status, body) {
  const data = Buffer.from(JSON.stringify(body));
  res.writeHead(status, {
    "content-type": "application/json; charset=utf-8",
    "content-length": data.length,
    "cache-control": "no-store",
  });
  res.end(data);
}

function methodNotAllowed(res) {
  jsonResponse(res, 405, { error: "method not allowed" });
}

async function readJson(req) {
  const chunks = [];
  for await (const chunk of req) chunks.push(chunk);
  const raw = Buffer.concat(chunks).toString("utf8").trim();
  if (!raw) return {};
  try {
    return JSON.parse(raw);
  } catch {
    const err = new Error("invalid JSON payload");
    err.statusCode = 400;
    throw err;
  }
}

function requireString(body, key) {
  const value = body?.[key];
  if (typeof value !== "string" || value.trim() === "") {
    const err = new Error(`${key} is required`);
    err.statusCode = 400;
    throw err;
  }
  return value.trim();
}

function requireNumber(body, key) {
  const value = body?.[key];
  if (typeof value !== "number" || !Number.isFinite(value)) {
    const err = new Error(`${key} must be a finite number`);
    err.statusCode = 400;
    throw err;
  }
  return value;
}

async function route(req, res, runtime, startedAt) {
  const url = new URL(req.url ?? "/", "http://127.0.0.1");
  const path = url.pathname;

  if (req.method === "GET" && path === "/health") {
    const health = await runtime.health();
    jsonResponse(res, 200, {
      service: "fh6-roon-bridge",
      ok: true,
      uptime_ms: Date.now() - startedAt,
      ...health,
    });
    return;
  }

  if (req.method === "GET" && path === "/status") {
    jsonResponse(res, 200, await runtime.status());
    return;
  }

  if (req.method === "GET" && path === "/zones") {
    jsonResponse(res, 200, { zones: await runtime.listZones() });
    return;
  }

  if (req.method === "GET" && path === "/outputs") {
    jsonResponse(res, 200, { outputs: await runtime.listOutputs() });
    return;
  }

  if (path === "/select-zone") {
    if (req.method !== "POST") return methodNotAllowed(res);
    const body = await readJson(req);
    jsonResponse(res, 200, await runtime.selectZone(requireString(body, "zone_id")));
    return;
  }

  if (path === "/transport") {
    if (req.method !== "POST") return methodNotAllowed(res);
    const body = await readJson(req);
    const control = requireString(body, "control");
    if (!TRANSPORT_CONTROLS.has(control)) {
      const err = new Error("control must be play, pause, playpause, stop, previous, or next");
      err.statusCode = 400;
      throw err;
    }
    const request = { control };
    if (typeof body.zone_id === "string" && body.zone_id.trim()) request.zone_id = body.zone_id.trim();
    jsonResponse(res, 200, await runtime.transport(request));
    return;
  }

  if (path === "/volume") {
    if (req.method !== "POST") return methodNotAllowed(res);
    const body = await readJson(req);
    const how = typeof body.how === "string" && body.how.trim() ? body.how.trim() : "absolute";
    if (!VOLUME_MODES.has(how)) {
      const err = new Error("how must be absolute, relative, or relative_step");
      err.statusCode = 400;
      throw err;
    }
    const request = { how, value: requireNumber(body, "value") };
    if (typeof body.output_id === "string" && body.output_id.trim())
      request.output_id = body.output_id.trim();
    if (typeof body.zone_id === "string" && body.zone_id.trim()) request.zone_id = body.zone_id.trim();
    jsonResponse(res, 200, await runtime.volume(request));
    return;
  }

  if (req.method === "GET" && path === "/artwork/current") {
    const artwork = await runtime.currentArtwork();
    if (!artwork) {
      jsonResponse(res, 404, { error: "current artwork is unavailable" });
      return;
    }
    res.writeHead(200, {
      "content-type": artwork.contentType,
      "content-length": artwork.bytes.length,
      "cache-control": "no-store",
    });
    res.end(artwork.bytes);
    return;
  }

  if (path === "/reconnect") {
    if (req.method !== "POST") return methodNotAllowed(res);
    jsonResponse(res, 200, await runtime.reconnect());
    return;
  }

  jsonResponse(res, 404, { error: "not found" });
}

export async function createApiServer({ runtime, host = "127.0.0.1", port = 47821, logger = console.log }) {
  const startedAt = Date.now();
  const server = http.createServer((req, res) => {
    route(req, res, runtime, startedAt).catch(err => {
      const status = Number.isInteger(err.statusCode) ? err.statusCode : 500;
      jsonResponse(res, status, { error: err.message || "internal server error" });
    });
  });

  await new Promise((resolve, reject) => {
    server.once("error", reject);
    server.listen(port, host, resolve);
  });

  const address = server.address();
  const actualPort = typeof address === "object" && address ? address.port : port;
  const url = `http://${host}:${actualPort}`;
  logger(JSON.stringify({ event: "listening", host, port: actualPort, url }));

  return {
    url,
    port: actualPort,
    close: () => new Promise(resolve => server.close(resolve)),
  };
}
