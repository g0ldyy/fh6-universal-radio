# FH6 Roon Control Sidecar

This sidecar is a local-only control bridge between FH6 Universal Radio and a Roon Core.
It is not Roon Ready, does not implement RAAT, and does not carry PCM audio.

## Usage

```powershell
npm install
npm start -- --host 127.0.0.1 --port 47821
```

The HTTP API binds to `127.0.0.1` by default and exposes JSON endpoints for health,
status, zone selection, transport, volume, reconnect, and current artwork.

Authorize `FH6 Universal Radio` in Roon Settings when the extension appears.
The sidecar stores only Roon extension authorization state through the official Roon API.
It does not store Roon account credentials.

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
