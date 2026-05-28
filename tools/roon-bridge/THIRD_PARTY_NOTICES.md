# Third-Party Notices

The Roon control sidecar depends on official Roon Labs JavaScript API packages:

- `node-roon-api` - Apache-2.0
- `node-roon-api-status` - Apache-2.0
- `node-roon-api-transport` - Apache-2.0
- `node-roon-api-image` - Apache-2.0

These packages are provided by Roon Labs from GitHub and are licensed under Apache-2.0.
FH6 Universal Radio remains GPLv3 licensed.

The official Roon API package currently pulls these transitive runtime npm
dependencies from the lockfile:

- `ip` - MIT
- `node-uuid` - MIT
- `options` - MIT
- `ultron` - MIT
- `ws` - MIT

They are not vendored in this repository and are installed into the staged
sidecar package at build/install time.
