# FH6 Universal Radio - Linux Guide

![FH6 Radio Working on Linux](WorkingOnLinux.png)

## Warning

This part of the mod is entirely vibecoded, i have no knowlage of low level dll injection nor wanted to educate my self. I just wanted semi working mod on my linux system and fastest way was to vibecode fix for linux. I do not take resposibility for quality or stability of the code. -DEVU

## Main Guide

This guide covers how to cross-compile, install, and configure the FH6 Universal Radio mod for use with Proton on Linux.

## 1. Compile on Linux

You do not need a Windows VM. The mod can be cross-compiled using MinGW-w64.

### Install Prerequisites

**Fedora/RHEL:**
```bash
sudo dnf install -y mingw64-gcc mingw64-gcc-c++ mingw64-cmake mingw64-binutils

```

**Ubuntu/Debian:**

```bash
sudo apt install -y mingw-w64 cmake build-essential

```

**Arch:**

```bash
sudo pacman -S mingw-w64 cmake

```

### Download Dependencies

The project requires four single-header libraries. Run these commands in the root of the source directory:

```bash
mkdir -p third_party/{cpp-httplib,nlohmann,toml11,miniaudio}

curl -sS [https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h](https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h) -o third_party/cpp-httplib/httplib.h
curl -sS [https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp](https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp) -o third_party/nlohmann/json.hpp
curl -sS [https://raw.githubusercontent.com/ToruNiina/toml11/main/single_include/toml.hpp](https://raw.githubusercontent.com/ToruNiina/toml11/main/single_include/toml.hpp) -o third_party/toml11/toml.hpp
curl -sS [https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h](https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h) -o third_party/miniaudio/miniaudio.h

mkdir -p third_party/nlohmann/nlohmann
mv third_party/nlohmann/json.hpp third_party/nlohmann/nlohmann/

```

### Build the Mod

```bash
mkdir -p build
cd build

cmake -DCMAKE_TOOLCHAIN_FILE=../mingw64-toolchain.cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --config Release

```

This generates `version.dll` along with the necessary MinGW runtime DLLs in the `build/` directory.

---

## 2. Install on Linux

### Copy DLLs

Because the mod is cross-compiled with MinGW, you must copy all four generated DLLs to your Forza Horizon 6 game directory (where the game executable is located):

* `version.dll`
* `libgcc_s_seh-1.dll`
* `libstdc++-6.dll`
* `libwinpthread-1.dll`

```bash
cp build/*.dll ~/.local/share/Steam/steamapps/common/ForzaHorizon6/

```

### Setup Mod Structure

You need the web UI and media assets from the original Windows mod release.

1. Download the original mod ZIP from Nexus Mods.
2. Extract the `media/` and `ui/` folders.
3. Place them in the `fh6-radio` folder inside the game directory.

The final structure should look like this:

```text
~/.local/share/Steam/steamapps/common/ForzaHorizon6/
├── version.dll
├── libgcc_s_seh-1.dll
├── libstdc++-6.dll
├── libwinpthread-1.dll
└── fh6-radio/
    ├── media/
    └── ui/

```

### Configure Steam Launch Options

Force Proton to load the custom DLL instead of the built-in Windows one. Right-click Forza Horizon 6 in Steam > Properties > General > Launch Options and add:

```text
WINEDLLOVERRIDES="version=n,b" %command%

```

---

## 3. Use on Linux

### Prepare Local Music

Create a folder for your music and add supported files (MP3, FLAC, WAV, OGG, M4A, Opus).

```bash
mkdir -p ~/.local/share/Steam/steamapps/common/ForzaHorizon6/Music
cp /path/to/your/music/*.mp3 ~/.local/share/Steam/steamapps/common/ForzaHorizon6/Music/

```

### Configure YouTube Music (Proton)

YouTube playback requires three external tools on disk: `yt-dlp`, `ffmpeg`, and `deno`. Because the mod is running through Proton's Windows translation layer, you **must use the Windows `.exe` versions** of these tools, not your native Linux packages.

1. Download the Windows `.exe` builds for all three dependencies:
* [`yt-dlp.exe`](https://github.com/yt-dlp/yt-dlp/releases)
* [`ffmpeg.exe`](https://www.gyan.dev/ffmpeg/builds/)
* [`deno.exe`](https://github.com/denoland/deno/releases)


2. Place all three `.exe` files directly into your game directory, right next to `forzahorizon6.exe`:
```text
~/.local/share/Steam/steamapps/common/ForzaHorizon6/

```


*(Note: Placing them here ensures they are included in Proton's implicit `PATH` for the game process).*
3. Open the mod's Web Dashboard.
4. Go to **Settings > YouTube Music**.
5. Point the paths explicitly to the `.exe` files if they are not automatically detected.

*(Note: Playing private or age-restricted content requires exporting a Netscape `cookies.txt` file from your browser and placing it where the mod can read it)*.

### Configure the Mod

Launch the game once to generate the default configuration file, then exit.

Edit the config file:

```bash
nano ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/config.toml

```

Find the `[local_files]` section and update the `music_dir`. Use a relative path if the folder is inside the game directory, or an absolute Linux path:

```toml
[local_files]
enabled = true
music_dir = "Music" 
recursive = true
shuffle = true

```

### In-Game Settings

1. Launch Forza Horizon 6.
2. Go to **Settings > Audio**.
3. Set **Radio DJ** to **Off**.
4. Set **Streamer Mode** to **On**.

### Access the Web Dashboard

While the game is running, open a web browser on the same machine and go to:

```text
http://localhost:8420

```

*(Note: If port 8420 does not work, check your `config.toml` to see if it defaulted to 8080).*

From the dashboard, you can control playback, adjust volume, see the current track, and switch audio sources. Select the new radio station in-game to start listening.
