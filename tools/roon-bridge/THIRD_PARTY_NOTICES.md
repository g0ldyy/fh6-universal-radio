# Third-Party Notices

The Roon control sidecar depends on official Roon Labs JavaScript API packages:

- `node-roon-api`
- `node-roon-api-status`
- `node-roon-api-transport`
- `node-roon-api-image`

These packages are provided by Roon Labs from GitHub and are licensed under Apache-2.0.
FH6 Universal Radio remains GPLv3 licensed.

The official Roon API package currently pulls small transitive npm dependencies
such as `ip`, `node-uuid`, `options`, `ultron`, and `ws`; they are not vendored in
this repository and are installed from the lockfile at setup time.
