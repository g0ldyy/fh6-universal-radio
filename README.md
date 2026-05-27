<h1 align="center" id="title">📻 FH6 Universal Radio</h1>

<p align="center">
  <a href="https://discord.gg/NyZUcATqWZ"><img src="https://img.shields.io/badge/Discord-Join%20Us-5865F2?style=flat-square&logo=discord&logoColor=white" /></a>
</p>

<p align="center"><img src="assets/banner.png" alt="FH6 Universal Radio" /></p>

An open-source radio mod for **Forza Horizon 6**. Adds a new in-game radio station fed from your **local music**, **YouTube Music**, or **Apple Music (streamed from your phone over AirPlay)**, controlled from a browser dashboard.

<p align="center">
  <img src="assets/ingame.png" alt="In-game radio station" width="49%" />
  <img src="assets/webui.png" alt="Web dashboard" width="49%" />
</p>

## Features

- **Local files**: point it at any folder, plays MP3 / FLAC / WAV / OGG / M4A / OPUS.
- **YouTube Music**: paste any video, playlist, or YT Music URL from the dashboard.
- **Apple Music (AirPlay)**: the PC becomes an AirPlay speaker; stream Apple Music (or anything) from your phone and control playback from your pocket. Uses your own subscription, no DRM workarounds.
- **In-game radio integration**: audio is routed through FH6's radio bus, fades with menus and reacts to in-game volume like every other station.
- **Live dashboard** at `http://localhost:8420`: switch source, transport controls, volume, settings.

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

### Apple Music (AirPlay)

Rather than fighting Apple's DRM, the mod turns the PC into an AirPlay receiver and lets your phone do the playback. Your phone streams Apple Music to the PC, the PC feeds it into the in-game radio, and you keep full control (skip, pause, search) from the Music app.

It relies on two external tools on disk:

- An AirPlay receiver that can write raw PCM to stdout — [`shairport-sync`](https://github.com/mikebrady/shairport-sync) is the reference implementation. Put it on your `PATH` or point at it in the dashboard under **Settings > Apple Music (AirPlay)**.
- [`ffmpeg`](https://www.gyan.dev/ffmpeg/builds/) (shared with YouTube Music) to resample the receiver's output to the radio's 48 kHz stereo format.

Network discovery uses Bonjour, so install **Apple Bonjour** (bundled with iTunes / Apple Devices) so the PC shows up in your phone's AirPlay picker. Then:

1. Enable **Apple Music (AirPlay)** in the dashboard and (optionally) set the device name.
2. Select it as the active source.
3. On your phone, open Control Center → AirPlay and pick the device name (default **FH6 Radio**).
4. Play anything in Apple Music — it comes out of the in-game radio.

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
| YouTube Music produces no audio | Check `%TEMP%\fh6-yt-stderr.log` (child stderr lands there). Usually missing yt-dlp/ffmpeg, expired cookies, or geo/format restrictions. |
| Phone can't find the AirPlay device | Apple Bonjour not installed, or PC and phone are on different networks/VLANs. Install Bonjour and confirm both are on the same Wi-Fi. |
| AirPlay connects but no audio | Check `%TEMP%\fh6-airplay-stderr.log`. Usually a missing shairport-sync/ffmpeg, or shairport-sync built without the `stdout` backend. |

## Why this exists

[Big John](https://www.nexusmods.com/forzahorizon6/mods/95) released a great **Spotify** radio mod for FH6 that I drew a lot of inspiration from. The catch: it requires Spotify Premium, and the author chose to keep it closed-source. I built FH6 Universal Radio because I believe the project can go much further once the community is allowed to contribute: adding sources (TIDAL, internet radio, etc.), polishing the UI, fixing edge cases, supporting more game builds. So this one is **fully open and GPLv3-licensed** to make that possible.

## Support the Project

FH6 Universal Radio is a community-driven project, and your support helps it grow! 🚀

- ❤️ **Donate** via [GitHub Sponsors](https://github.com/sponsors/g0ldyy) or [Ko-fi](https://ko-fi.com/g0ldyy) to support development
- ⭐ **Star the repository** here on GitHub
- 🐛 **Contribute** by reporting issues, suggesting features, or submitting PRs

## License

Released under the [GNU General Public License v3.0](LICENSE). You're free to use, modify, and redistribute the code; forks and derivatives must remain GPLv3 and credit the original project.

## Disclaimer

Unofficial fan-made mod. Not affiliated with, endorsed by, or connected to Turn 10 Studios, Playground Games, Xbox Game Studios, Microsoft, Google, or YouTube. All trademarks belong to their respective owners. Use at your own risk.
