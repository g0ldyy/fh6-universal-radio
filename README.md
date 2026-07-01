<h1 align="center" id="title">📻 FH6 Universal Radio</h1>

<p align="center">
  <a href="https://discord.gg/NyZUcATqWZ"><img src="https://img.shields.io/badge/Discord-Join%20Us-5865F2?style=flat-square&logo=discord&logoColor=white" /></a>
</p>

<p align="center"><img src="assets/banner.png" alt="FH6 Universal Radio" /></p>

An open-source radio mod for **Forza Horizon 6**. Adds a new in-game radio station fed from your **local music**, **online radio** stations, **Spotify**, **YouTube Music**, **SoundCloud**, **Jellyfin** server, or **any Windows app** (Deezer, a browser tab...), controlled from a browser dashboard.

<p align="center">
  <img src="assets/ingame.png" alt="In-game radio station" width="49%" />
  <img src="assets/webui.png" alt="Web dashboard" width="49%" />
</p>

## Features

- **Local files**: build named **stations** from one or more folders, exclude subfolders you don't want, and pick a play order (shuffle / albums / name / folder) with repeat modes and a searchable queue. MP3 / FLAC / WAV / OGG / M4A / AAC / OPUS / WMA / M3U / M3U8 etc.
- **Online radio**: search a directory of thousands of internet stations by name, genre, or country (via [radio-browser.info](https://www.radio-browser.info)) or paste any stream URL; save favourites with logos and genre/bitrate badges, with live track info.
- **YouTube Music**: paste any video, playlist, or YT Music URL from the dashboard. Tracks are added to a searchable queue, with support for saving playlists as reusable stations.
- **SoundCloud**: paste any playlist, or track URL from the dashboard. Tracks are added to a searchable queue, with support for saving playlists as reusable stations.
- **Spotify Connect**: cast from the Spotify app to an "FH6 Universal Radio" device (requires Spotify Premium).
- **Jellyfin**: stream playlists from your own Jellyfin server, view queued tracks, and save playlists as stations for quick access later.
- **External audio**: capture any Windows app (Deezer, a browser tab...) and pipe it into the radio through a virtual audio cable.
- **In-game radio integration**: audio is routed through FH6's radio bus, fades with menus and reacts to in-game volume like every other station.
- **Live dashboard** at `http://localhost:8420`: switch source, transport controls, volume, settings.
- **Race start action**: on race begin, advance to next track, restart the current one, turn radio off on race begin, or leave it alone.
- **Quick station skip**: tune the radio knob away and back within 1s to skip the current track.
- **Loudness normalization**: For consistent volume across tracks.
- **5-band equalizer**: 60 Hz / 250 Hz / 1 kHz / 4 kHz / 12 kHz peaking biquads, ±6 dB per band, applied producer-side at 48 kHz before audio hits the game.
- **Vanilla radio passthrough**: broadcast FH6's built-in radio stations through the mod. Requires **Streamer Mode = Off** while in use.
- **Media hotkeys**: Play/Pause and Next Track/Source media keys continue to work while FH6 is focused.

## Install

> 📺 Prefer a video? Watch the [installation guide on YouTube](https://www.youtube.com/watch?v=9Uwy3pDf4SQ).

1. Download the latest `fh6-universal-radio.zip` from [Nexus Mods](https://www.nexusmods.com/forzahorizon6/mods/215).
2. Close FH6.
3. Extract the ZIP into your Forza Horizon 6 install folder (next to `forzahorizon6.exe`). Overwrite when prompted.
4. Launch the game. In **Audio settings**, set **Radio DJ = Off**.
   - For custom stations (local files, Spotify, YouTube Music, Jellyfin, etc.), enable **Streamer Mode**.
   - For **Vanilla Radio passthrough**, disable **Streamer Mode**.
5. Cycle through radio stations until you land on the new one.
6. Open <http://localhost:8420> in any browser on the same machine. From another device on the same network, use your PC's local IP (e.g. `http://192.168.1.42:8420`), run `ipconfig` in a Command Prompt to find it.

### Dependencies

Online radio, YouTube Music, SoundCloud, Spotify, Jellyfin, and non-native local formats rely on external binaries: `yt-dlp`, `ffmpeg`, and `librespot`. The mod **downloads them automatically** on first launch into `fh6-radio\bin`, so there's nothing to install by hand.

To manage them yourself instead, set the paths in the dashboard (**Settings > General > yt_dlp path** for yt-dlp, **Settings > General > ffmpeg path** for ffmpeg, **Settings > Spotify Connect** for librespot).

### YouTube Music

Private/age-restricted content needs a Netscape `cookies.txt` exported from your browser. Use an extension like **Get cookies.txt LOCALLY** to export it.

### SoundCloud

Private/age-restricted content needs a Netscape `cookies.txt` exported from your browser. Use an extension like **Get cookies.txt LOCALLY** to export it.

### Spotify Connect

Enable Spotify under **Settings**, then open the Spotify app on a device on the same Wi-Fi network, tap the **Devices** icon, and pick **FH6 Universal Radio**. Requires an old Spotify Premium account (a Spotify Connect limitation).

### External audio

External Audio is a loopback capture of a Windows playback device, so the app has to play onto a device you don't otherwise hear, or it reaches your speakers directly instead of through the radio. Route it through a virtual audio cable:

1. Install a virtual audio cable, e.g. [VB-Audio Virtual Cable](https://vb-audio.com/Cable/).
2. Send the app's audio to the cable. Apps without their own output picker (like Deezer) can still be routed from Windows **Settings > System > Sound > Volume mixer**, setting the app's output to `CABLE Input`; for a browser tab, an extension like **AuRo** does the same.
3. In the dashboard, pick that cable as the **Capture device**.
4. Pick the app as the **Media session** for the in-game title/artist and the next/previous controls.

The app then pauses and resumes with the game radio (menus, radio off) instead of playing on in the background.

### Vanilla Radio

Vanilla Radio passthrough lets listeners hear FH6's built-in radio stations through the mod instead of only custom audio sources.

> ⚠️ Requires **Streamer Mode = Off**. FH6 disables radio playback in Streamer Mode, so vanilla stations cannot be captured while it is enabled.

When Vanilla Radio is active, switching between FH6's built-in stations works normally and the currently selected station is forwarded through the radio system.

## Uninstall

- Delete `version.dll` from the game directory.
- Delete the `fh6-radio` folder.
- Verify game files through Steam / Xbox / Microsoft Store to restore the patched assets.

## Build from source

The output is always a Windows `version.dll`.

### Windows

Requires **Visual Studio 2022+** with the *Desktop development with C++* workload.

```powershell
.\scripts\get-deps.ps1                                                  # one-time: header-only deps
.\scripts\build.ps1                                                     # compile + stage dist\
.\scripts\install.ps1 -GameDir "C:\XboxGames\Forza Horizon 6\Content"   # copy into game
```

### Linux (cross-compile to Windows)

Requires **CMake** and **llvm-mingw**. On Arch: `sudo pacman -S llvm-mingw cmake`. On other distros, grab a release from [mstorsjo/llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) and unpack it under `/opt/llvm-mingw` (the build script auto-detects that path).

```bash
./scripts/get-deps.sh                                                   # one-time: header-only deps
./scripts/build.sh                                                      # compile + stage dist/
./scripts/install.sh ~/.steam/steam/steamapps/common/ForzaHorizon6      # copy into game (Proton prefix)
```

## Troubleshooting

| Symptom | Fix |
|---|---|
| New radio station doesn't show in-game | **Audio > Streamer Mode** is off. Turn it on, restart the game. |
| Local files don't play | The active station has no folders, or its folders only hold unsupported formats. Add a folder in the dashboard's Local Files card. |
| YouTube Music / SoundCloud produces no audio | Check `%TEMP%\fh6-stderr.log` (helper-process stderr lands there). Usually expired cookies, or geo/format restrictions. |
| SoundCloud tracks are unavailable | Confirm that the account has SoundCloud GO+ and that cookies have been provided. |
| Spotify device doesn't appear or won't play | Wait for `librespot` to finish downloading, confirm your phone/PC is on the same network, and that the account is Spotify Premium. |
| External Audio plays in the background, not through the radio | You're capturing the same device the app plays on. Route the app's output to a **virtual audio cable** and select that cable as the **Capture device** (see [External audio](#external-audio)). |
| External Audio has clicks / artifacts | Set the virtual cable to **48000 Hz** (2 ch). Other sample rates caused artifacts in testing. |
| Vanilla Radio produces no audio | Vanilla Radio passthrough requires **Streamer Mode = Off**. Disable Streamer Mode and restart the game if needed. |
| Media hotkeys don't work | Ensure another application is not intercepting media key events. Play/Pause and Next Source should work while FH6 is focused. |

## Why this exists

[Big John](https://www.nexusmods.com/forzahorizon6/mods/95) released a great **Spotify** radio mod for FH6 that I drew a lot of inspiration from. The catch: it requires Spotify Premium, and the author chose to keep it closed-source. I built FH6 Universal Radio because I believe the project can go much further once the community is allowed to contribute: adding sources (TIDAL, etc.), polishing the UI, fixing edge cases, supporting more game builds. So this one is **fully open and GPLv3-licensed** to make that possible.

## Support the Project

FH6 Universal Radio is a community-driven project, and your support helps it grow! 🚀

- ❤️ **Donate** via [GitHub Sponsors](https://github.com/sponsors/g0ldyy) or [Ko-fi](https://ko-fi.com/g0ldyy) to support development
- ⭐ **Star the repository** here on GitHub
- 🐛 **Contribute** by reporting issues, suggesting features, or submitting PRs

## License

Released under the [GNU General Public License v3.0](LICENSE). You're free to use, modify, and redistribute the code; forks and derivatives must remain GPLv3 and credit the original project.

## Disclaimer

Unofficial fan-made mod. Not affiliated with, endorsed by, or connected to Turn 10 Studios, Playground Games, Xbox Game Studios, Microsoft, Google, YouTube, SoundCloud (SoundCloud Global Limited & Co. KG), Jellyfin (Jellyfin LLC), or Spotify (Spotify AB). All trademarks belong to their respective owners. Use at your own risk.
