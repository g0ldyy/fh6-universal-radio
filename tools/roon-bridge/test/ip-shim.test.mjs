import { describe, it } from "node:test";
import assert from "node:assert/strict";
import { createRequire } from "node:module";

const require = createRequire(import.meta.url);
const ip = require("ip");

describe("FH6 ip shim", () => {
  it("computes IPv4 broadcast addresses used by Roon SOOD discovery", () => {
    assert.equal(ip.subnet("192.168.1.10", "255.255.255.0").broadcastAddress, "192.168.1.255");
    assert.equal(ip.subnet("10.2.3.4", "255.255.0.0").broadcastAddress, "10.2.255.255");
  });
});
