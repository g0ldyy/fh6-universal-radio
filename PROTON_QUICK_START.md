# FH6 Universal Radio: Quick Start (Proton/Linux)

**TL;DR:** Set up local music, copy DLLs, enable Streamer Mode, access dashboard at http://localhost:8420

---

## 5-Minute Setup

### 1️⃣ Prepare Music (2 min)

```bash
# Create a folder with your music
mkdir -p ~/Games/fh6-music
cp ~/Music/*.mp3 ~/Games/fh6-music/   # or .flac, .wav, .ogg, .m4a, .opus
chmod 755 ~/Games/fh6-music
chmod 644 ~/Games/fh6-music/*
```

**Supported formats:** MP3, FLAC, WAV, OGG, M4A, Opus  
**Recommended:** MP3 or FLAC (most compatible)

### 2️⃣ Install DLLs (1 min)

```bash
# Copy all 4 DLLs to game directory
cp build/*.dll ~/.local/share/Steam/steamapps/common/ForzaHorizon6/

# Verify
ls -lh ~/.local/share/Steam/steamapps/common/ForzaHorizon6/*.dll
```

**Should show:**
- version.dll (3.1 MB)
- libgcc_s_seh-1.dll
- libstdc++-6.dll
- libwinpthread-1.dll

### 3️⃣ Enable In-Game Settings (1 min)

1. Launch FH6 and wait for it to load
2. Go to **Settings > Audio**
3. Set:
   - **Radio DJ** = Off
   - **Streamer Mode** = On
4. Restart the game

### 4️⃣ Set Music Directory (1 min)

1. Open browser: `http://localhost:8420`
2. Click **Settings**
3. Set **Music Directory** to `/home/yourusername/Games/fh6-music`
4. Click **Save**
5. Dashboard should show track count (e.g., "42 tracks found")

---

## Test It

| Step | What to Do | Expected Result |
|------|-----------|-----------------|
| Get in car | - | See new radio station in list |
| Select station | Cycle through stations | Music starts within 5 sec |
| Open dashboard | Go to http://localhost:8420 | See current track + controls |
| Click "Next" | Use dashboard button | Skip to next track |
| Wait 30 sec | Listen | No stuttering, audio plays smoothly |

---

## Troubleshooting Quick Fix

| Problem | Fix |
|---------|-----|
| No radio station appears | Enable Streamer Mode (Settings > Audio) |
| Dashboard says "0 tracks" | Check path is absolute: `/home/you/Music`, not `~/Music` |
| Dashboard won't connect | Restart game; check `ps aux \| grep forzahorizon6` |
| No audio | Check volume up + right station selected in-game |
| Stuttering | Use MP3 instead of FLAC; ensure music on SSD |

---

## Logs for Debugging

```bash
# View mod log (50 last lines)
tail -50 ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log

# Watch in real-time
tail -f ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log

# Check if mod initialized
grep -i "INFO\|ERROR" ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log | tail -10
```

---

## What's Next?

- **Everything works?** Try the full testing guide: [PROTON_LOCAL_FILES_TESTING_GUIDE.md](PROTON_LOCAL_FILES_TESTING_GUIDE.md)
- **Want YouTube?** See [README.md](README.md) (requires yt-dlp, ffmpeg, deno)
- **Issues?** Check the full troubleshooting section in [PROTON_LOCAL_FILES_TESTING_GUIDE.md](PROTON_LOCAL_FILES_TESTING_GUIDE.md#8-troubleshooting)

---

## Key Paths

| Item | Path |
|------|------|
| Game directory | `~/.local/share/Steam/steamapps/common/ForzaHorizon6/` |
| Mod config | `~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/config.toml` |
| Mod log | `~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log` |
| DLL files (source) | `build/*.dll` |
| Music directory | `~/Games/fh6-music/` (or your choice) |
| Web dashboard | `http://localhost:8420` |

---

## Port Usage

- **8420** = Default dashboard port
- If busy, edit `config.toml`: `port = 8421` (or any free port)

---

**Created:** 2026-05-26  
**For full details:** See [PROTON_LOCAL_FILES_TESTING_GUIDE.md](PROTON_LOCAL_FILES_TESTING_GUIDE.md)
