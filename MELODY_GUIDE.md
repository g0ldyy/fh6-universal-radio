# FH6 Universal Radio — Startup Melody Feature Guide

> A mod for **Forza Horizon 6** that plays a random audio clip (like a JDM ETC reader / melody box)
> every time you **start a race** or **switch cars** — on top of your normal custom radio music.

---

## Table of Contents
1. [Prerequisites](#1-prerequisites)
2. [Build the DLL](#2-build-the-dll)
3. [Install the Mod](#3-install-the-mod)
4. [Adding Your Melody Files](#4-adding-your-melody-files)
5. [Enable Car Change Detection](#5-enable-car-change-detection)
6. [Configure via the Dashboard](#6-configure-via-the-dashboard)
7. [Config Reference](#7-config-reference)
8. [FAQ & Troubleshooting](#8-faq--troubleshooting)

---

## 1. Prerequisites

Install the cross-compilation toolchain on Arch Linux:

```bash
sudo pacman -S cmake llvm-mingw
```

> These tools compile Windows `.dll` files from Linux without needing Windows at all.

---

## 2. Build the DLL

Run these commands in order — copy and paste the whole block:

```bash
# Navigate to the cloned repo
cd "/home/kazuha/Videos/FH6 radio/fh6-universal-radio"

# Step 1: Fetch third-party headers (nlohmann, toml11, miniaudio)
./scripts/get-deps.sh

# Step 2: Configure the build with the MinGW cross-compiler
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
  -DCMAKE_BUILD_TYPE=Release

# Step 3: Compile (uses all CPU cores)
cmake --build build --parallel
```

### Output files (after build)
```
fh6-universal-radio/build/
├── version.dll           ← main mod DLL  (drop into game folder)
└── fh6-radio-worker.exe  ← worker process (drop into game folder)
```

---

## 3. Install the Mod

### Find your Forza Horizon 6 folder
Usually one of:
```
C:\XboxGames\Forza Horizon 6\Content\
C:\Program Files\WindowsApps\Microsoft.ForzaHorizon6_...\
```
The folder that contains `ForzaHorizon6.exe`.

### Copy the build output
```bash
# From your Linux machine, copy via your Windows partition or USB:
build/version.dll
build/fh6-radio-worker.exe
```

Paste them **next to** `ForzaHorizon6.exe`.

### Create the mod's config and folder structure
Inside the **same folder as ForzaHorizon6.exe**, create:
```
ForzaHorizon6.exe
version.dll                        ← copied from build/
fh6-radio-worker.exe               ← copied from build/
fh6-radio/
├── config.toml                    ← copy & rename config.example.toml
├── ui/                            ← web dashboard (copy from repo: ui/dist/)
└── assets/
    └── melodies/                  ← PUT YOUR AUDIO FILES HERE
        ├── startup.wav
        ├── jingle.mp3
        └── (any other audio files)
```

### Copy the config template
```bash
cp config.example.toml  /path/to/fh6-radio/config.toml
```

---

## 4. Adding Your Melody Files

Drop any audio files into:
```
fh6-radio/assets/melodies/
```

### Supported formats
| Format | Extension |
|--------|-----------|
| WAV    | `.wav`    |
| MP3    | `.mp3`    |
| FLAC   | `.flac`   |
| OGG Vorbis | `.ogg` |
| Opus   | `.opus`   |
| AAC/M4A | `.m4a`  |

### How random selection works
- Every time a trigger fires (race start OR car switch), the mod scans the folder
- It picks **one file at random** from all supported files found
- Put as many files as you want — more files = more variety
- A 3-second debounce prevents the same clip from overlapping itself

**Example melodies folder:**
```
melodies/
├── honda_etc.wav        ← JDM ETC reader beep
├── nsx_startup.mp3      ← NSX startup melody
├── supra_chime.flac     ← Supra door chime
└── civic_theme.ogg      ← Civic startup theme
```

---

## 5. Enable Car Change Detection

The car-change trigger needs FH6 to broadcast its telemetry data over UDP.

### In-game setup (do this once)
```
FH6 → Settings → HUD and Gameplay → scroll down to "Data Out"

  Data Out              →  On
  Data Out IP Address   →  127.0.0.1
  Data Out Port         →  7777
  Data Out Packet Format →  Car Dash
```

> ⚠️ If you change the port from `7777`, update `data_out_port` in your `config.toml` to match.

---

## 6. Configure via the Dashboard

Once FH6 is running with the mod, open your browser at:
```
http://localhost:8420
```

### Enable the melody feature via the Dashboard UI

Scroll down the dashboard to the **🎵 Startup Melody** card. From there you can:

| Control | Description |
|---------|-------------|
| **Enable startup melody** | Master on/off toggle |
| **On race start** | Trigger a melody when a race begins |
| **On car switch** | Trigger when car ordinal changes (requires Data Out) |
| **Debounce (ms)** | Minimum gap between plays (default 3000 ms) |
| **FH6 Data Out port** | UDP port FH6 sends telemetry to (default 7777) |

Click **Save** to apply. Changes take effect immediately without restarting the game.

### View detected melody files

The same card shows a live list of all audio files found in the melodies folder. Click **Refresh files** to re-scan after adding new files.

### Advanced: using curl or the browser console

You can still configure the feature programmatically if needed:

```bash
curl -X PUT http://localhost:8420/api/config \
  -H "Content-Type: application/json" \
  -d '{"startup_melody":{"enabled":true,"debounce_ms":3000,"trigger_on_race_start":true,"trigger_on_car_change":true,"data_out_port":7777}}'
```

### Check what melody files were detected
```bash
curl http://localhost:8420/api/startup_melody/files
```
Returns:
```json
{
  "files": ["honda_etc.wav", "nsx_startup.mp3", "supra_chime.flac"],
  "folder": "C:\\...\\fh6-radio\\assets\\melodies"
}
```

### Check current config
```bash
curl http://localhost:8420/api/config
```

---

## 7. Config Reference

In `fh6-radio/config.toml`, the melody section looks like this:

```toml
[startup_melody]
enabled              = true
debounce_ms          = 3000    # minimum milliseconds between plays (3 seconds)
trigger_on_race_start = true   # play when a race begins
trigger_on_car_change = true   # play when you switch cars
data_out_port        = 7777    # must match FH6 Data Out port setting
```

| Key | Default | Description |
|-----|---------|-------------|
| `enabled` | `false` | Master switch. Set to `true` to activate |
| `debounce_ms` | `3000` | Minimum gap between plays (ms). Prevents overlapping |
| `trigger_on_race_start` | `true` | Play at the start of each race |
| `trigger_on_car_change` | `true` | Play when car ordinal changes (requires Data Out) |
| `data_out_port` | `7777` | UDP port that FH6 sends telemetry to |

---

## 8. FAQ & Troubleshooting

### ❓ Melody doesn't play at all
- Check `enabled = true` in config
- Verify files exist in `fh6-radio/assets/melodies/` with a supported extension
- Check the mod log (`fh6-radio/fh6-radio.log`) for `[melody]` lines

### ❓ Car change not detected
- Confirm FH6 Data Out is **On** in game settings
- Confirm the IP is `127.0.0.1` and port matches `data_out_port`
- The log will show: `[car_det] listening for FH6 telemetry on UDP port 7777`
- If it shows a bind error, another app may be using that port — change to `7778` in both FH6 and config

### ❓ Melody plays too often / overlaps
- Increase `debounce_ms` (e.g. `5000` for 5 seconds)

### ❓ Only `.wav` files work, not MP3
- Make sure the file has the exact extension `.mp3` (not `.MP3` — case shouldn't matter but verify)
- The mod uses **miniaudio** which supports MP3 natively, no codec install needed

### ❓ Build fails: `cmake: command not found`
```bash
sudo pacman -S cmake llvm-mingw
```

### ❓ Build fails: `x86_64-w64-mingw32-clang++ not found`
```bash
sudo pacman -S llvm-mingw
# Then re-run cmake
```

### ❓ Where are the build output files?
```
fh6-universal-radio/build/version.dll
fh6-universal-radio/build/fh6-radio-worker.exe
```

---

## Quick Reference Card

```
1. sudo pacman -S cmake llvm-mingw
2. cd "/home/kazuha/Videos/FH6 radio/fh6-universal-radio"
3. ./scripts/get-deps.sh
4. cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
5. cmake --build build --parallel
6. Copy build/version.dll + build/fh6-radio-worker.exe → next to ForzaHorizon6.exe
7. Create fh6-radio/assets/melodies/ and drop audio files in
8. In FH6: Settings → HUD → Data Out → On, IP=127.0.0.1, Port=7777
9. Open http://localhost:8420 in your browser
10. Scroll to the 🎵 Startup Melody card → toggle Enable → Save
```
