<h1 align="center" id="title">📻 FH6 Universal Radio</h1>

<p align="center">
  <a href="https://discord.gg/NyZUcATqWZ"><img src="https://img.shields.io/badge/Discord-Join%20Us-5865F2?style=flat-square&logo=discord&logoColor=white" /></a>
</p>

<p align="center"><img src="assets/banner.png" alt="FH6 Universal Radio" /></p>

An open-source radio mod for **Forza Horizon 6**. Adds a new in-game radio station fed from your **local music**, **YouTube Music**, or a **Roon Source**, controlled from a browser dashboard.

<p align="center">
  <img src="assets/ingame.png" alt="In-game radio station" width="49%" />
  <img src="assets/webui.png" alt="Web dashboard" width="49%" />
</p>

## Features

- **Local files**: point it at any folder. MP3 / FLAC / WAV / OGG play out of the box; M4A / AAC / OPUS / WMA / etc. play if `ffmpeg` is installed (same binary as YouTube Music below).
- **YouTube Music**: paste any video, playlist, or YT Music URL from the dashboard.
- **Roon Source**: control a Roon zone from Web Control and capture its Windows output through WASAPI loopback.
- **In-game radio integration**: audio is routed through FH6's radio bus, fades with menus and reacts to in-game volume like every other station.
- **Live dashboard** at `http://localhost:8420`: switch source, transport controls, volume, settings.
- **Race start action**: on race begin, advance to next track, restart the current one, or leave it alone.
- **Quick station skip**: tune the radio knob away and back within 1s to skip the current track.
- **Loudness normalization**: For consistent volume across tracks.
- **5-band equalizer**: 60 Hz / 250 Hz / 1 kHz / 4 kHz / 12 kHz peaking biquads, ±6 dB per band, applied producer-side at 48 kHz before audio hits the game.

## Install

1. Download the latest `fh6-universal-radio.zip` from [Nexus Mods](https://www.nexusmods.com/forzahorizon6/mods/215).
2. Close FH6.
3. Extract the ZIP into your Forza Horizon 6 install folder (next to `forzahorizon6.exe`). Overwrite when prompted.
4. Launch the game. In **Audio settings**, set **Radio DJ = Off** and **Streamer Mode = On**.
5. Cycle through radio stations until you land on the new one.
6. Open <http://localhost:8420> in any browser on the same machine or LAN.

### YouTube Music

YouTube playback requires three external tools on disk:

- [`yt-dlp`](https://github.com/yt-dlp/yt-dlp/releases) and [`ffmpeg`](https://www.gyan.dev/ffmpeg/builds/) either on your `PATH`, or pointed at explicitly in the dashboard under **Settings > YouTube Music**.
- [`deno`](https://deno.com/) on `PATH`. Install with `winget install DenoLand.Deno` (or `irm https://deno.land/install.ps1 | iex`).

Private/age-restricted content also needs a Netscape `cookies.txt` exported from your browser.

### Roon Source

Roon support is configured from Web Control. It is not Roon Ready and not RAAT. FH6 Universal Radio uses the official Roon API for pairing, zone metadata, artwork, and transport controls, then captures the selected zone's Windows render endpoint with WASAPI loopback.

To set it up:

1. Open <http://localhost:8420>, go to **Settings > Roon**, and enable Roon.
2. Use the Roon panel's setup checks to confirm Node.js, Roon Server, and a VB-Audio render endpoint are present. Normal Roon setup does not require PowerShell or CLI commands.
3. Open Roon, go to **Settings > Extensions**, and authorize **FH6 Universal Radio** when it appears.
4. In Web Control, select the Roon zone to control.
5. Set that Roon zone's audio output to the recommended render endpoint, normally **Hi-Fi Cable Input** or **CABLE Input**, then click **Use recommended device** and **Test audio**.

For the most reliable setup, route the Roon zone to a dedicated Windows output or a virtual audio cable. Do not pick the same output that FH6 itself is using unless you are comfortable with feedback/monitoring tradeoffs. **Hi-Fi Cable Output** is the recording side of the VB-Audio device and is not the current MVP target. The correct loopback endpoint should report a non-silent peak while Roon is playing; wrong endpoints report a silent capture error.

Roon Bridge by itself is not a replacement for Roon Server. The sidecar controls Roon and reads metadata only; PCM audio stays inside `version.dll` through WASAPI loopback capture.

Roon logs are written next to the game in `fh6-radio\bridge.log`, `fh6-radio\roon-sidecar.out.log`, and `fh6-radio\roon-sidecar.err.log`.

## Uninstall

- Delete `version.dll` from the game directory.
- Delete the `fh6-radio` folder.
- Verify game files through Steam / Xbox / Microsoft Store to restore the patched assets.

## Build from source

Requires **Visual Studio 2022+** with the *Desktop development with C++* workload (CMake is bundled) and the **Forza Horizon 6** radio-station media overlay from any existing radio mod ZIP. The overlay is mod-agnostic and the assets are modified copies of game files, so we don't ship them.

```powershell
.\scripts\get-deps.ps1                                                  # one-time: header-only deps
.\scripts\build.ps1                                                     # compile + stage dist\
.\scripts\fetch-media.ps1 -Source "C:\path\to\radio-mod.zip"            # radio-station overlay
.\scripts\install.ps1 -GameDir "C:\XboxGames\Forza Horizon 6\Content"   # copy into game
```

## Troubleshooting

| Symptom | Fix |
|---|---|
| Dashboard says **bridge offline** | Media overlay not installed. Re-run `install.ps1` with `dist\media\` present. |
| New radio station doesn't show in-game | **Audio > Streamer Mode** is off. Turn it on, restart the game. |
| Game crashes on launch | Antivirus quarantined `version.dll`. Add an exclusion for the game folder. |
| Local files don't play | No `music_dir` set, or the folder only has unsupported formats. Set one from the dashboard. |
| `[local] failed to open ... .m4a` (or `.opus`, `.aac`, ...) | The built-in decoder handles MP3/FLAC/WAV/OGG only; other formats are routed through `ffmpeg`. Install it (`winget install Gyan.FFmpeg`) and either put it on `PATH` or set the path under **Settings > YouTube Music > ffmpeg_path**. |
| YouTube Music produces no audio | Check `%TEMP%\fh6-stderr.log` (helper-process stderr lands there). Usually missing yt-dlp/ffmpeg, expired cookies, or geo/format restrictions. |
| Roon panel says authorization is pending | Open Roon **Settings > Extensions** and authorize **FH6 Universal Radio**. |
| Roon zone list is empty | Confirm Roon Core is running, the extension is authorized, and `fh6-radio\roon-sidecar.out.log` shows a successful registry response. |
| Roon loopback is silent | Make sure the selected Roon zone is playing to **Hi-Fi Cable Input** or **CABLE Input**. Do not select **Hi-Fi Cable Output** for the MVP loopback path. |
| Roon sidecar does not start | Install Node.js 20+ or set `[roon].node_path`; verify `fh6-radio\tools\roon-bridge\index.mjs` exists. |
| Roon sidecar crashes or disconnects | The game should continue running. Check `fh6-radio\roon-sidecar.err.log`, then use **Reconnect** in the Roon panel. |

## Why this exists

[Big John](https://www.nexusmods.com/forzahorizon6/mods/95) released a great **Spotify** radio mod for FH6 that I drew a lot of inspiration from. The catch: it requires Spotify Premium, and the author chose to keep it closed-source. I built FH6 Universal Radio because I believe the project can go much further once the community is allowed to contribute: adding sources (TIDAL, internet radio, etc.), polishing the UI, fixing edge cases, supporting more game builds. So this one is **fully open and GPLv3-licensed** to make that possible.

## Support the Project

FH6 Universal Radio is a community-driven project, and your support helps it grow! 🚀

- ❤️ **Donate** via [GitHub Sponsors](https://github.com/sponsors/g0ldyy) or [Ko-fi](https://ko-fi.com/g0ldyy) to support development
- ⭐ **Star the repository** here on GitHub
- 🐛 **Contribute** by reporting issues, suggesting features, or submitting PRs

## License

Released under the [GNU General Public License v3.0](LICENSE). You're free to use, modify, and redistribute the code; forks and derivatives must remain GPLv3 and credit the original project. Roon sidecar dependency notices are listed in `tools/roon-bridge/THIRD_PARTY_NOTICES.md`.

## Disclaimer

Unofficial fan-made mod. Not affiliated with, endorsed by, or connected to Turn 10 Studios, Playground Games, Xbox Game Studios, Microsoft, Google, or YouTube. All trademarks belong to their respective owners. Use at your own risk.
