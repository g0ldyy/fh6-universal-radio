"use strict";

function ipv4ToInt(value) {
  if (typeof value !== "string") throw new TypeError("IPv4 address must be a string");
  const parts = value.split(".");
  if (parts.length !== 4) throw new TypeError(`Invalid IPv4 address: ${value}`);
  return parts.reduce((acc, part) => {
    if (!/^\d+$/.test(part)) throw new TypeError(`Invalid IPv4 address: ${value}`);
    const octet = Number(part);
    if (octet < 0 || octet > 255) throw new TypeError(`Invalid IPv4 address: ${value}`);
    return ((acc << 8) | octet) >>> 0;
  }, 0);
}

function intToIpv4(value) {
  return [
    (value >>> 24) & 255,
    (value >>> 16) & 255,
    (value >>> 8) & 255,
    value & 255,
  ].join(".");
}

function subnet(address, mask) {
  const ip = ipv4ToInt(address);
  const netmask = ipv4ToInt(mask);
  const networkAddress = ip & netmask;
  const broadcastAddress = (networkAddress | (~netmask >>> 0)) >>> 0;
  return {
    networkAddress: intToIpv4(networkAddress >>> 0),
    broadcastAddress: intToIpv4(broadcastAddress),
  };
}

module.exports = { subnet };
