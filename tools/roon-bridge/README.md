# FH6 Roon Control Sidecar

This sidecar is a local-only control bridge between FH6 Universal Radio and a Roon Core.
It is not Roon Ready, does not implement RAAT, and does not carry PCM audio.

## Usage

Packaged users should configure Roon from Web Control. The dashboard checks for
Node.js, Roon Server, and the selected VB-Audio render endpoint, then starts this
sidecar automatically when `[roon].auto_start_bridge` is true.

Manual startup is only for development:

```powershell
npm install
npm start -- --host 127.0.0.1 --port 47821
```

The HTTP API binds to `127.0.0.1` by default and exposes JSON endpoints for health,
status, zone selection, transport, volume, reconnect, and current artwork.

The packaged mod starts this process automatically when `[roon].auto_start_bridge`
is true. A local Node.js 20+ runtime is still required; leave `[roon].node_path`
blank to use `node.exe` from `PATH`, or set it to an absolute path.

Roon Bridge is not a replacement for Roon Server. The extension must be authorized
against a running Roon Core before transport and metadata calls can succeed.

Authorize `FH6 Universal Radio` in **Roon Settings > Extensions** when the
extension appears.
The sidecar stores only Roon extension authorization state through the official Roon API.
It does not store Roon account credentials.

This process is control-only. PCM audio is not sent through the sidecar; FH6
Universal Radio captures the selected Windows render endpoint through WASAPI
loopback inside `version.dll`. The usual endpoint is `Hi-Fi Cable Input` or
`CABLE Input`; `Hi-Fi Cable Output` is the recording side and is not the MVP
loopback target.

## Local API

- `GET /health`
- `GET /status`
- `GET /zones`
- `GET /outputs`
- `POST /select-zone` with `{ "zone_id": "..." }`
- `POST /transport` with `{ "control": "playpause", "zone_id": "..." }`
- `POST /volume` with `{ "output_id": "...", "how": "absolute", "value": 40 }`
- `GET /artwork/current`
- `POST /reconnect`

Allowed transport controls are `play`, `pause`, `playpause`, `stop`, `previous`, and `next`.

## Logs and failure behavior

When the C++ mod starts the sidecar, stdout and stderr are written next to the
game:

- `fh6-radio\roon-sidecar.out.log`
- `fh6-radio\roon-sidecar.err.log`

The sidecar binds only to `127.0.0.1` by default. If it exits, cannot find a
Roon Core, or remains unauthorized, the game should keep running and Web Control
should surface an actionable Roon status.
