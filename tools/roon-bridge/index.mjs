#!/usr/bin/env node

import { createApiServer } from "./lib/api-server.mjs";
import { createRoonRuntime } from "./lib/roon-runtime.mjs";

function readOptions(argv) {
  const options = {
    host: process.env.FH6_ROON_HOST || "127.0.0.1",
    port: Number.parseInt(process.env.FH6_ROON_PORT || "47821", 10),
    selectedZoneId: process.env.FH6_ROON_ZONE_ID || "",
  };

  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === "--host") options.host = argv[++i] ?? options.host;
    else if (arg === "--port") options.port = Number.parseInt(argv[++i] ?? "", 10);
    else if (arg === "--zone-id") options.selectedZoneId = argv[++i] ?? "";
    else if (arg === "--help") {
      console.log("Usage: node tools/roon-bridge/index.mjs [--host 127.0.0.1] [--port 47821]");
      process.exit(0);
    }
  }

  if (!Number.isInteger(options.port) || options.port <= 0 || options.port > 65535) {
    throw new Error("port must be between 1 and 65535");
  }
  return options;
}

function logEvent(event) {
  console.log(typeof event === "string" ? event : JSON.stringify(event));
}

const options = readOptions(process.argv.slice(2));
const runtime = createRoonRuntime({ selectedZoneId: options.selectedZoneId });
const server = await createApiServer({
  runtime,
  host: options.host,
  port: options.port,
  logger: logEvent,
});

runtime.start();
logEvent({ event: "started", api_url: server.url });

async function shutdown(signal) {
  logEvent({ event: "shutdown", signal });
  runtime.stop();
  await server.close();
  process.exit(0);
}

process.on("SIGINT", () => {
  void shutdown("SIGINT");
});
process.on("SIGTERM", () => {
  void shutdown("SIGTERM");
});
