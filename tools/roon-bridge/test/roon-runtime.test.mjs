import { createRequire } from "node:module";
import { afterEach, describe, it } from "node:test";
import assert from "node:assert/strict";

import { createRoonRuntime } from "../lib/roon-runtime.mjs";

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
});
