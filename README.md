<h1 align="center" id="title">📻 FH6 Universal Radio</h1>

<p align="center">
  <a href="https://discord.gg/NyZUcATqWZ"><img src="https://img.shields.io/badge/Discord-Join%20Us-5865F2?style=flat-square&logo=discord&logoColor=white" /></a>
</p>

<p align="center"><img src="assets/banner.png" alt="FH6 Universal Radio" /></p>

An open-source radio mod for **Forza Horizon 6**. Adds a new in-game radio station fed from your **local music**, **YouTube Music**, **Jellyfin** server, or **any Windows app** (Spotify, Deezer, a browser tab...), controlled from a browser dashboard.

<p align="center">
  <img src="assets/ingame.png" alt="In-game radio station" width="49%" />
  <img src="assets/webui.png" alt="Web dashboard" width="49%" />
</p>

## Features

- **Local files**: point it at any folder. MP3 / FLAC / WAV / OGG play out of the box; M4A / AAC / OPUS / WMA / etc. play if `ffmpeg` is installed (same binary as YouTube Music below).
- **YouTube Music**: paste any video, playlist, or YT Music URL from the dashboard.
- **Jellyfin**: stream playlists from your own Jellyfin server.
- **External audio**: capture any Windows app (Spotify, Deezer, a browser tab) and pipe it into the radio through a virtual audio cable; metadata and next/previous come from the Windows media session.
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
6. Open <http://localhost:8420> in any browser on the same machine. From another device on the same network, use your PC's local IP (e.g. `http://192.168.1.42:8420`), run `ipconfig` in a Command Prompt to find it.

### YouTube Music

YouTube playback requires three external tools. Open a **Command Prompt** and run:

```
winget install yt-dlp.yt-dlp
winget install Gyan.FFmpeg
winget install DenoLand.Deno
```

Then restart the game.

`yt-dlp` can also be pointed at explicitly in the dashboard under **Settings > YouTube Music** if you prefer a manual install.
`ffmpeg` can also be configured under **Settings > General > ffmpeg path**.

Private/age-restricted content also needs a Netscape `cookies.txt` exported from your browser. Use an extension like **Get cookies.txt LOCALLY** to export it.

### External audio

External Audio is a loopback capture of a Windows playback device, so the app has to play onto a device you don't otherwise hear, or it reaches your speakers directly instead of through the radio. Route it through a virtual audio cable:

1. Install a virtual audio cable, e.g. [VB-Audio Virtual Cable](https://vb-audio.com/Cable/).
2. Send the app's audio to the cable. Apps without their own output picker (like Deezer) can still be routed from Windows **Settings > System > Sound > Volume mixer**, setting the app's output to `CABLE Input`; for a browser tab, an extension like **AuRo** does the same.
3. In the dashboard, pick that cable as the **Capture device**.
4. Pick the app as the **Media session** for the in-game title/artist and the next/previous controls.

The app then pauses and resumes with the game radio (menus, radio off) instead of playing on in the background.

## Uninstall

- Delete `version.dll` from the game directory.
- Delete the `fh6-radio` folder.
- Verify game files through Steam / Xbox / Microsoft Store to restore the patched assets.

## Build from source

The output is always a Windows `version.dll`. You also need the radio-station media overlay from any existing radio mod ZIP. It's mod-agnostic, but the assets are modified copies of game files so we don't ship them.

### Windows

Requires **Visual Studio 2022+** with the *Desktop development with C++* workload (CMake is bundled).

```powershell
.\scripts\get-deps.ps1                                                  # one-time: header-only deps
.\scripts\build.ps1                                                     # compile + stage dist\
.\scripts\fetch-media.ps1 -Source "C:\path\to\radio-mod.zip"            # radio-station overlay
.\scripts\install.ps1 -GameDir "C:\XboxGames\Forza Horizon 6\Content"   # copy into game
```

### Linux (cross-compile to Windows)

Requires **CMake** and **llvm-mingw** (the Clang-based MinGW-w64 toolchain, since the codebase uses MSVC SEH which GCC-mingw doesn't implement). On Arch: `sudo pacman -S llvm-mingw cmake`. On other distros, grab a release from [mstorsjo/llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) and unpack it under `/opt/llvm-mingw` (the build script auto-detects that path).

```bash
./scripts/get-deps.sh                                                   # one-time: header-only deps
./scripts/build.sh                                                      # compile + stage dist/
./scripts/fetch-media.sh /path/to/radio-mod.zip                         # radio-station overlay
./scripts/install.sh ~/.steam/steam/steamapps/common/ForzaHorizon6      # copy into game (Proton prefix)
```

## Troubleshooting

| Symptom | Fix |
|---|---|
| Dashboard says **bridge offline** | Media overlay not installed. Re-run `install.ps1` with `dist\media\` present. |
| New radio station doesn't show in-game | **Audio > Streamer Mode** is off. Turn it on, restart the game. |
| Game crashes on launch | Antivirus quarantined `version.dll`. Add an exclusion for the game folder. |
| Local files don't play | No `music_dir` set, or the folder only has unsupported formats. Set one from the dashboard. |
| `[local] failed to open ... .m4a` (or `.opus`, `.aac`, ...) | The built-in decoder handles MP3/FLAC/WAV/OGG only; other formats are routed through `ffmpeg`. Install it (`winget install Gyan.FFmpeg`) and either put it on `PATH` or set the path under **Settings > General > ffmpeg path**. |
| YouTube Music produces no audio | Check `%TEMP%\fh6-stderr.log` (helper-process stderr lands there). Usually missing yt-dlp/ffmpeg, expired cookies, or geo/format restrictions. |
| Jellyfin cast returns "fetch failed" (502) | Check server URL, API key, and user ID under **Settings > Jellyfin**, that the playlist ID exists, and that the server is reachable from this machine. Jellyfin transcodes to PCM via `ffmpeg`, so the configured ffmpeg path must be valid. |
| External Audio plays in the background, not through the radio | You're capturing the same device the app plays on. Route the app's output to a **virtual audio cable** and select that cable as the **Capture device** (see [External audio](#external-audio)). |
| External Audio has clicks / artifacts | Set the virtual cable to **48000 Hz** (2 ch). Other sample rates caused artifacts in testing. |

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

Unofficial fan-made mod. Not affiliated with, endorsed by, or connected to Turn 10 Studios, Playground Games, Xbox Game Studios, Microsoft, Google, YouTube, or Jellyfin (Jellyfin LLC). All trademarks belong to their respective owners. Use at your own risk.
