# FH6 Universal Radio: Testing Local Files on Proton

This guide walks you through testing the **local files feature** of FH6 Universal Radio running under Proton on Linux. Local files are simpler than YouTube Music (no external dependencies like yt-dlp), making them ideal for initial testing.

**Why test local files first?** YouTube playback requires yt-dlp and ffmpeg, which may not work reliably under Wine/Proton. Local files are a good proving ground for core mod functionality.

---

## 1. Prerequisites

Before you start, ensure:

- **Forza Horizon 6** is installed via Steam with Proton
- **The mod DLL is installed** in your game directory (see [build/README_RUNTIME_DLLS.md](build/README_RUNTIME_DLLS.md))
- **A music collection** with supported audio formats (see [section 3](#3-supported-audio-formats))
- **A web browser** on your Linux machine (same machine as the game)

### Verify the Mod Is Installed

```bash
# Check that all 4 DLLs are present in the FH6 directory
ls -lh ~/.local/share/Steam/steamapps/common/ForzaHorizon6/*.dll

# Should see something like:
# version.dll (3.1 MB)
# libgcc_s_seh-1.dll (970 KB)
# libstdc++-6.dll (29 MB)
# libwinpthread-1.dll (74 KB)
```

If any are missing, copy them from `build/` in the mod source directory.

---

## 2. Setting Up Local Music Files

### 2.1 Where to Put Your Music

Local music files should be in a **single directory** on your Linux machine. The mod will scan this folder and (by default) all subfolders recursively.

**Recommended locations:**

```bash
# Option 1: Your Linux home music folder
~/Music

# Option 2: A custom games music folder
~/Games/fh6-music

# Option 3: Any accessible path (but use absolute paths)
/mnt/music
/home/yourusername/media/radio
```

**Example setup:**

```bash
# Create a test folder
mkdir -p ~/Games/fh6-music

# Copy some music files into it
cp ~/Music/*.mp3 ~/Games/fh6-music/
cp ~/Music/*.flac ~/Games/fh6-music/
```

### 2.2 Supported Directory Structure

The mod can work with flat folders or deeply nested structures—it's up to you:

```
~/Games/fh6-music/
├── Song1.mp3
├── Song2.flac
└── albums/
    ├── Album1/
    │   ├── Track1.ogg
    │   └── Track2.opus
    └── Album2/
        └── music.m4a
```

All files with supported extensions will be discovered and added to the playlist.

### 2.3 File Permissions

Make sure your music files are readable by the user running Steam/Proton:

```bash
# If needed, fix permissions
chmod -R 755 ~/Games/fh6-music
chmod -R 644 ~/Games/fh6-music/*
```

---

## 3. Supported Audio Formats

The FH6 Universal Radio mod supports the following audio formats out of the box:

| Format | Extension | Notes |
|--------|-----------|-------|
| MP3    | `.mp3`    | Most common; widely supported |
| FLAC   | `.flac`   | Lossless; good for archival |
| WAV    | `.wav`    | Uncompressed PCM; larger files |
| OGG    | `.ogg`    | Vorbis codec; good compression |
| M4A    | `.m4a`    | AAC codec; iTunes/Apple format |
| Opus   | `.opus`   | Modern codec; smaller files |

**Recommendation:** For testing, use MP3 or FLAC. These are most widely used and most likely to work correctly.

### Checking Your Files

```bash
# See what formats you have
find ~/Games/fh6-music -type f -exec file {} \; | head -20

# Count files by type
find ~/Games/fh6-music -type f | sed 's/.*\.//' | sort | uniq -c
```

---

## 4. Configuring the Mod

Configuration happens in two ways: the **config file** (persistent) or the **web dashboard** (real-time). The dashboard is easier for testing.

### 4.1 Understanding the Configuration

When the mod initializes, it looks for this file:

```
~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/config.toml
```

On first run, it creates this file with defaults from `config.example.toml`. The key settings for local files are:

```toml
[general]
port              = 8420                           # Web dashboard port
default_source    = "local_files"                 # Start with local files
fallback_source   = "local_files"                 # Fall back to local if YouTube fails

[local_files]
enabled            = true                         # Must be true
music_dir          = ''                           # Leave blank; set from dashboard
recursive          = true                         # Search subfolders
shuffle            = true                         # Random playback order
supported_formats  = ["mp3", "flac", "wav", "ogg", "m4a", "opus"]
```

### 4.2 Editing config.toml Manually (Optional)

If you prefer to set the music directory in the config file instead of the dashboard:

```bash
# Open the config file in your favorite editor
nano ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/config.toml
```

Find the `[local_files]` section and set the absolute path to your music folder:

```toml
[local_files]
enabled            = true
music_dir          = '/home/yourusername/Games/fh6-music'  # Use absolute path
recursive          = true
shuffle            = true
```

**Important:** Use **absolute paths** and ensure the path exists before the mod starts.

---

## 5. Launching the Game and Testing

### 5.1 Pre-Launch Checklist

- [ ] Music files are in a directory (e.g., `~/Games/fh6-music`)
- [ ] All 4 DLLs are in the FH6 install directory
- [ ] Forza Horizon 6 is **fully closed** (not running)
- [ ] You're ready to test

### 5.2 Launch the Game

```bash
# Option 1: Via Steam GUI
# Just launch the game normally from Steam

# Option 2: Via command line with Proton logging (for debugging)
cd ~/.local/share/Steam/steamapps/common/ForzaHorizon6/
PROTON_LOG=1 proton run forzahorizon6.exe &
```

If using the command-line method, the game will run in the background; you can close it or let it keep running.

### 5.3 In-Game Radio Setup

Once the game loads:

1. **Go to Audio Settings**
   - Press the menu key (usually ESC)
   - Navigate to **Settings > Audio**
   
2. **Configure these two settings:**
   - **Radio DJ:** Set to **Off** (no dialogue interruptions)
   - **Streamer Mode:** Set to **On** (this enables the new radio station)

3. **Save and restart the game**

   The changes may require a restart to take effect.

### 5.4 Finding the New Radio Station

After restarting:

1. **Get in any car** in the game world
2. **Cycle through radio stations** (default: X or Y button, depending on controller)
3. **Look for a new station** at the end of the list (often called "Universal Radio" or similar)

If you don't see it after cycling through all stations:
- Check that **Streamer Mode is On** (see 5.3)
- Restart the game completely
- Check the logs (see [section 6](#6-accessing-the-web-dashboard))

---

## 6. Accessing the Web Dashboard

The web dashboard is the control center for the mod. It shows what's playing, lets you switch sources, adjust volume, and configure the music directory.

### 6.1 Open the Dashboard

In any browser on **the same machine as the game**, go to:

```
http://localhost:8420
```

You should see a page with:
- Current track information
- Playback controls (play, pause, next, previous)
- Volume slider
- Settings panel
- Source selector (Local Files / YouTube Music)

### 6.2 Setting the Music Directory (GUI Method)

This is the easiest way to configure the mod:

1. **Open the dashboard** at `http://localhost:8420`
2. **Look for "Settings"** or a gear icon
3. **Find "Local Files"** section
4. **Click "Choose Directory"** or paste the path:
   - `~/Games/fh6-music` → resolved by your system to `/home/you/Games/fh6-music`
5. **Click "Save"** or "Apply"

The mod will immediately rescan the directory and show you how many tracks were found.

### 6.3 What You Should See

After setting the directory:

```
Local Files
============
Music directory: /home/yourusername/Games/fh6-music
Recursive:       Enabled ✓
Shuffle:         Enabled ✓
Tracks found:    42
Status:          Ready to play ✓
```

If it says "No tracks found":
- The path may be wrong (check it's absolute)
- The folder may be empty
- The files may have unsupported extensions (see [section 3](#3-supported-audio-formats))

---

## 7. Expected Behavior

Here's what should happen when everything is working:

### 7.1 First Time Setup

| Step | Expected Behavior |
|------|-------------------|
| Set music directory | Dashboard shows count of discovered tracks |
| Return to game | New radio station appears in the station list |
| Select station | Station plays without errors; one song plays fully |
| Song ends | Mod automatically plays next track (shuffled) |

### 7.2 During Playback

- **Audio quality**: Unchanged from the original files (no re-encoding)
- **Volume**: Responds to in-game volume controls
- **Menu fading**: Audio fades when you open menus (like other radio stations)
- **Seek position**: Shown in web dashboard; updates in real-time

### 7.3 Dashboard Behavior

| Action | Expected Result |
|--------|-----------------|
| Click "Play" | Song starts playing in-game within 1 second |
| Click "Next" | Skips to next track (shuffled list) |
| Click "Pause" | Audio stops; can resume with "Play" |
| Move volume slider | In-game radio volume changes immediately |
| Refresh page | Dashboard re-connects and shows current state |

---

## 8. Troubleshooting

### 8.1 "No Local Radio Station Appears in Game"

**Checklist:**

1. **Streamer Mode is ON?**
   - Go to Audio settings in-game
   - Set **Streamer Mode = On**
   - Restart the game

2. **DLLs are actually installed?**
   ```bash
   ls -lh ~/.local/share/Steam/steamapps/common/ForzaHorizon6/*.dll
   ```
   Should show 4 files. If not, copy them from the `build/` directory.

3. **Check the mod log:**
   ```bash
   cat ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log
   ```
   Look for error messages. If it says "webui index.html missing", reinstall the mod.

**Fix:** Reinstall the DLLs and media overlay, then restart the game.

---

### 8.2 "Dashboard Says 'No Tracks Found' or Wrong Count"

**Checklist:**

1. **Does the directory exist?**
   ```bash
   ls -la ~/Games/fh6-music/
   ```
   Should show files. If empty, add some music files (see [section 2.1](#21-where-to-put-your-music)).

2. **Are files in supported formats?**
   ```bash
   file ~/Games/fh6-music/*
   ```
   Should show MP3, FLAC, WAV, OGG, M4A, or Opus. If not, convert or copy different files.

3. **Are file extensions correct?**
   ```bash
   find ~/Games/fh6-music -type f | grep -v '\.\(mp3\|flac\|wav\|ogg\|m4a\|opus\)$'
   ```
   This lists files with wrong extensions. Rename or move them.

4. **Path is absolute?**
   If using the config file, the path must be absolute (`/home/you/...`, not `~/...`).

**Fix:** 
- Verify files exist: `ls ~/Games/fh6-music/ | head -10`
- Check formats: `file ~/Games/fh6-music/* | head -10`
- Restart the game to trigger a rescan

---

### 8.3 "Dashboard Shows Tracks but No Audio Plays"

**Checklist:**

1. **Is a track actually selected in the dashboard?**
   - The "current track" display should show a song name
   - If it's blank, click "Play" in the dashboard

2. **Is your speaker volume up?**
   - Check Linux volume: `alsamixer` or your system sound settings
   - Check in-game volume (not radio DJ volume, but master volume)

3. **Is the radio station actually selected in-game?**
   - Go back in-game and make sure the Universal Radio station is active
   - You should see the station name at the bottom of the screen

4. **Check the mod log for decode errors:**
   ```bash
   tail -50 ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log | grep -i error
   ```
   If you see "decoder failed" or similar, the file might be corrupted.

**Fix:**
- Ensure at least one track is playable: `mpv ~/Games/fh6-music/Song1.mp3` (replace filename)
- Try a different, known-good MP3 file
- Check in-game volume settings

---

### 8.4 "Dashboard Won't Load" or "Connection Refused"

**Checklist:**

1. **Is the game still running?**
   ```bash
   pgrep -f forzahorizon6
   ```
   Should show a process. If not, the game crashed; restart it.

2. **Is the dashboard port correct?**
   The default port is `8420`. Try: `http://localhost:8420`
   
   If you changed it in `config.toml`, use the new port: `http://localhost:8421` (if you set `port = 8421`).

3. **Is something else using port 8420?**
   ```bash
   netstat -tlnp | grep 8420
   ```
   If something else is listening, either change the mod's port in the config or stop the other service.

4. **Check the mod log:**
   ```bash
   tail -50 ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log
   ```
   If it says "bridge offline", the mod's web server didn't start. Restart the game.

**Fix:**
- Restart the game
- Try a different port in the config file: `port = 8421`
- Restart after editing the config

---

### 8.5 "Music Plays But Stutters or Cuts Out"

**Checklist:**

1. **Is the directory on a slow storage medium?**
   - If music is on a network mount or USB drive, it may be slow
   - Try copying files to your local SSD: `cp ~/Games/fh6-music/* ~/Music/`

2. **Are files very large or in exotic formats?**
   - WAV and FLAC files are larger; FLAC decoding may cause stutters
   - Try MP3 files: smaller and faster to decode

3. **Is the system under heavy load?**
   ```bash
   top -b -n 1 | head -15
   ```
   If CPU or disk I/O is very high, close other programs.

4. **Check for errors in the mod log:**
   ```bash
   tail -100 ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log | grep -i "warn\|error"
   ```

**Fix:**
- Use smaller/simpler formats (MP3 instead of FLAC)
- Copy music to a faster disk
- Close resource-heavy applications
- Restart the game

---

### 8.6 "Game Crashes on Startup"

**This suggests the mod's DLL failed to load.** See [FINAL_STATUS.md](FINAL_STATUS.md) for known issues.

**Checklist:**

1. **Are all 4 DLLs present?**
   ```bash
   ls -lh ~/.local/share/Steam/steamapps/common/ForzaHorizon6/*.dll
   ```
   Should show version.dll, libgcc_s_seh-1.dll, libstdc++-6.dll, libwinpthread-1.dll

2. **Is the media overlay installed?**
   ```bash
   ls -lh ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/
   ```
   Should show media/ folder with index.html

3. **Check the Proton log:**
   ```bash
   grep -i "version.dll\|error\|crash" ~/.local/share/Steam/steamapps/compatdata/2483190/pfx.log | tail -50
   ```

**Fix:**
- Verify all DLLs are copied (see [build/README_RUNTIME_DLLS.md](build/README_RUNTIME_DLLS.md))
- Delete the corrupted `version.dll` and copy a fresh one from `build/`
- Restart the game

**Known Issue:** There is a known Wine/Proton issue with MinGW-compiled DLLs that may cause crashes during initialization. See [PROTON_COMPATIBILITY_ANALYSIS.md](PROTON_COMPATIBILITY_ANALYSIS.md) for details and workarounds.

---

## 9. Checking Logs

Logs are your best friends when something goes wrong.

### 9.1 Mod Log

```bash
# View the entire mod log
cat ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log

# View only the last 50 lines
tail -50 ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log

# Watch the log in real-time (useful during debugging)
tail -f ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log
```

**Look for these keywords:**
- `INFO`: Normal operation
- `WARN`: Something is odd, but not breaking
- `ERROR`: Something failed
- `[local]`: Local files source-specific messages
- `[bridge]`: Web server messages
- `[http]`: HTTP API messages

### 9.2 Proton Log

If the game crashes during startup, check the Proton log:

```bash
# First, find the Proton log file
ls -lh ~/.local/share/Steam/steamapps/compatdata/*/pfx.log

# Then view errors related to version.dll
grep -i "version.dll" ~/.local/share/Steam/steamapps/compatdata/*/pfx.log | tail -50
```

This shows low-level Wine errors during DLL loading.

---

## 10. Testing Checklist

Use this checklist to verify everything is working:

### Before Testing
- [ ] Music files are in `~/Games/fh6-music/` (or your chosen directory)
- [ ] Music directory contains at least 5 supported format files (.mp3, .flac, etc.)
- [ ] All 4 DLLs are in the FH6 install directory
- [ ] Config file exists at `~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/config.toml`
- [ ] Game is fully closed (not running)

### Testing Steps
- [ ] **Launch the game** and wait for it to fully load (2-3 minutes)
- [ ] **Go to Settings > Audio** and verify:
  - [ ] Radio DJ = Off
  - [ ] Streamer Mode = On
- [ ] **Get in a car** and cycle through radio stations
- [ ] **Find the new station** (Universal Radio)
- [ ] **Select it** and verify music plays within 5 seconds
- [ ] **Open the web dashboard** at `http://localhost:8420`
- [ ] **Verify the dashboard shows:**
  - [ ] Current track name
  - [ ] Play/pause buttons work
  - [ ] Song counter and total count
  - [ ] Volume slider responds to input
- [ ] **Skip to next track** using the dashboard "Next" button
- [ ] **Listen for 30 seconds** of audio; verify no stuttering
- [ ] **Pause playback** and verify audio stops
- [ ] **Resume playback** and verify audio restarts from where it paused

### Success Criteria
You've passed testing if:
- ✓ Music plays in-game from the new radio station
- ✓ Web dashboard loads and controls respond
- ✓ At least 5 consecutive tracks play without errors
- ✓ No stuttering or audio dropouts during normal playback
- ✓ Volume controls work in-game and on the dashboard

---

## 11. Reporting Issues

If local files don't work, please include:

1. **Your system info:**
   ```bash
   uname -a
   proton --version  # or check Steam's Proton version
   ls -lh ~/.local/share/Steam/steamapps/common/ForzaHorizon6/*.dll
   ```

2. **Music directory info:**
   ```bash
   ls ~/Games/fh6-music/ | head -20
   find ~/Games/fh6-music -type f | wc -l
   ```

3. **The mod log (first and last 50 lines):**
   ```bash
   head -50 ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log
   echo "..."
   tail -50 ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log
   ```

4. **What you expected vs. what happened**
5. **Steps to reproduce the issue**

---

## 12. Next Steps

Once local files are working:

### If YouTube Music Interests You
- See [README.md](README.md) for YouTube Music setup requirements
- Install yt-dlp, ffmpeg, and deno on your Linux system
- Set up cookies.txt if your content is age-restricted (see dashboard)
- Test YouTube playback from the dashboard

### If You Want to Contribute
- Fork the project on GitHub
- Report bugs with detailed logs (see section 11)
- Suggest features or improvements
- Submit pull requests

### If You Want to Customize Further
- The web dashboard is at `ui/` in the source
- The config file format is TOML (see `config.example.toml`)
- Local file source code is at `src/sources/local_file_source.cpp`

---

## 13. Quick Reference

### Key Commands

```bash
# Check if the game is running
pgrep -f forzahorizon6

# View the mod log
tail -f ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log

# Open the web dashboard
firefox http://localhost:8420  # or any browser

# Check available music files
find ~/Games/fh6-music -type f | wc -l

# Test a single music file (requires mpv)
mpv ~/Games/fh6-music/Song1.mp3

# Edit the config file
nano ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/config.toml

# Kill the game if it's stuck
killall forzahorizon6.exe
```

### Default Configuration Values

| Setting | Default | Purpose |
|---------|---------|---------|
| `port` | `8420` | Web dashboard port |
| `ring_buffer_mb` | `4` | Audio buffer size |
| `default_source` | `local_files` | Source on startup |
| `fallback_source` | `local_files` | Fallback if primary fails |
| `music_dir` | (empty) | Path to music folder |
| `recursive` | `true` | Scan subfolders |
| `shuffle` | `true` | Randomize playback |

---

## 14. FAQ

**Q: Can I use a network share (NFS, SMB)?**  
A: Technically yes, but performance may suffer. Copy files to local storage for best results.

**Q: What if I want to disable shuffle?**  
A: Edit `config.toml` and set `shuffle = false`, or use the dashboard settings (if available).

**Q: Can the mod play from a USB drive?**  
A: Yes, but USB I/O may cause stuttering. Copy to your SSD for faster access.

**Q: How do I know if my MP3 files are corrupt?**  
A: Test them outside the game: `mpv ~/Games/fh6-music/Song.mp3`. If they play there, the mod should play them too.

**Q: What's the maximum number of tracks?**  
A: Theoretically unlimited, but performance degrades with very large directories (10,000+). Keep it under 5,000 for best performance.

**Q: Can I use the mod with other radio mods?**  
A: No, only one version.dll can be active. Uninstall other radio mods first.

**Q: Will this work on native Windows?**  
A: Yes! This guide is Linux-specific, but the mod works on Windows with the same configuration steps.

---

**Last updated:** 2026-05-26  
**Tested with:** Proton (latest), FH6 Universal Radio (local files feature)  
**Questions?** Check the [README.md](README.md) or open an issue on GitHub.
