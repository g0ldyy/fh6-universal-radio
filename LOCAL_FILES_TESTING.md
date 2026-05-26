# FH6 Universal Radio - Local Files Testing Guide

## ✅ FIXED: Mod is Now Initializing!

The latest build has been updated to handle minified web UI files. The mod should now initialize properly without aborting on missing attribution markers.

---

## Testing with Local Music Files

Since YouTube/yt-dlp may not work properly under Proton/Wine, the local files feature is the best way to test the mod's core functionality.

### Step 1: Create Music Directory

```bash
# Create a directory for your music
mkdir -p ~/.local/share/Steam/steamapps/common/ForzaHorizon6/Music

# Or create it in a convenient location and configure it later
mkdir -p ~/Music/forza-radio
```

### Step 2: Add Music Files

Copy your music files to the directory. **Supported formats**:
- MP3
- WAV
- FLAC
- OGG
- AAC
- M4A

**Example**:
```bash
cp /path/to/your/music/*.mp3 ~/Music/forza-radio/
# or
cp /path/to/your/music/*.flac ~/Music/forza-radio/
```

### Step 3: Configure the Mod

After the game launches once, a config file will be created at:
```
~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/config.toml
```

**Edit the config file** to enable local files and point to your music:

```toml
# General settings
[general]
default_source = "local_files"  # Set this to "local_files"
fallback_source = "local_files"
port = 8080

# Local files source
[local_files]
enabled = true
music_dir = "/home/devu/Music/forza-radio"  # Change this to your music directory
recursive = true  # Search subdirectories
shuffle = true    # Shuffle songs
```

**Important**: Replace `/home/devu/Music/forza-radio` with your actual music directory path!

### Step 4: Quick Test Before Game Launch

Check that the mod at least tries to initialize:

```bash
# Clear old logs
rm ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log

# Launch game with debug logging
PROTON_LOG=1 steam steam://rungameid/2483190 &
sleep 10

# Check if mod initialized
cat ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log
```

**Expected output**:
```
2026-05-26 HH:MM:SS.SSS INFO  [bridge] FH6 Universal Radio starting; data_dir=...
2026-05-26 HH:MM:SS.SSS INFO  [bridge] webui index.html loaded successfully
2026-05-26 HH:MM:SS.SSS INFO  [bridge] running on port 8080
```

**If you see errors**, check the [Troubleshooting](#troubleshooting) section below.

### Step 5: In-Game Testing

1. **Launch the game** and wait for it to fully load (30-60 seconds)
2. **Check for the radio station** in your garage/radio UI
3. **Select the radio station** - does it appear?
4. **Try to play a song** - does audio come through?
5. **Check the dashboard**:
   - Open web browser to `http://127.0.0.1:8080`
   - You should see the FH6 Radio dashboard
   - Can you see your local files listed?
   - Can you select and play a song?

### Step 6: Monitor for Stuttering

During your test:
- **Is there stuttering?** (freezes every second)
- **Does the audio play smoothly?**
- **Can you navigate the dashboard without lag?**

---

## Important Paths on Proton/Wine

⚠️ **Note**: File paths in the config must use **forward slashes** `/` or **backslashes** `\\`, NOT mixed slashes!

**Examples that work**:
```toml
music_dir = "/home/devu/Music/forza-radio"      # ✓ Linux path
music_dir = "C:\\Users\\Music\\forza"            # ✓ Windows path format
music_dir = "/mnt/d/Music/Forza"                # ✓ Mounted Windows drive
```

**Examples that DON'T work**:
```toml
music_dir = "C:/Users/Music/forza"              # ✗ Mixed slashes
music_dir = "\\Users\\Music"                    # ✗ UNC path without proper escaping
```

---

## Web Dashboard Access

The mod runs a web server on `http://127.0.0.1:8080`

### Features:
- **Browse music** - See all detected local files
- **Play/Pause** - Control playback
- **Volume control** - Adjust mod volume (separate from game volume)
- **Shuffle toggle** - Enable/disable random playback
- **Current song** - Display of now-playing track

### If Dashboard Doesn't Load:

1. **Check if port is in use**:
   ```bash
   netstat -tlnp | grep 8080
   lsof -i :8080
   ```

2. **Try a different port** - Edit config.toml:
   ```toml
   [general]
   port = 8081  # Try 8081 or 8082 instead
   ```

3. **Check firewall** - Make sure localhost (127.0.0.1) is not blocked

---

## Troubleshooting

### Issue: "webui index.html missing"
**Solution**: The UI files weren't packaged with the mod. This should now be fixed in the latest build.

### Issue: "neither default nor fallback source was registered"
**Meaning**: Neither local_files nor youtube_music could be initialized

**Check**:
1. Is `music_dir` path correct?
2. Do the music files exist?
3. Are the permissions correct? (`ls -la ~/Music/forza-radio/`)

### Issue: Music files not found
**Check**:
1. Path in config is correct
2. Files are readable: `ls ~/Music/forza-radio/`
3. File format is supported (MP3, FLAC, WAV, etc.)
4. Try with `recursive = false` first

### Issue: Still stuttering every second
**Possible causes**:
1. **Mod is crashing repeatedly** - Check bridge.log for errors
2. **FMOD signature scanning failing** - Mod can't inject audio DSP
3. **Proton issue** - Try different Proton version

**Check Proton log**:
```bash
grep -i "error\|exception\|segv" /home/devu/steam-2483190.log | tail -20
```

### Issue: No audio from mod
**Possible causes**:
1. **DSP injection failed** - Bridge log will show FMOD warnings
2. **Audio not routed through mod** - Check game audio settings
3. **Volume set to 0** - Check dashboard or config

---

## Expected Behavior

### If Everything Works:
- ✓ Game launches without stuttering
- ✓ Radio station appears in UI
- ✓ Songs play through the mod
- ✓ Dashboard loads at http://127.0.0.1:8080
- ✓ You can see/select songs in the dashboard
- ✓ Song metadata displays in-game (if metadata injection works)

### Partial Success (Expected under Proton):
- ✓ Game launches without stuttering
- ✓ Radio station appears
- ✗ No audio plays (FMOD signature scanning failed)
- ✓ Dashboard works and shows songs

If you're in the "Partial Success" state, the mod is working but audio DSP injection isn't. This is a known limitation under Proton - the game's audio still plays normally, but through the standard game audio instead of through the mod.

### What Definitely Won't Work on Proton:
- ✗ YouTube/Spotify source (requires yt-dlp and ffmpeg)
- ✗ Metadata injection might not work (memory write issues under Wine)
- ✗ Some rare Windows API calls might fail

---

## Next Steps

1. **Test with local files** using this guide
2. **Report back** with:
   - bridge.log output
   - Whether stuttering occurs
   - Whether audio plays
   - Whether dashboard loads
3. **If working**: Celebrate! The mod is functional on Proton
4. **If issues**: Share the logs and we can debug further

---

## Creating Test Music

Don't have music handy? You can create test files:

### Create silent MP3 (Linux):
```bash
# Using ffmpeg (if installed)
ffmpeg -f lavfi -i anullsrc=r=44100:cl=mono -t 10 -q:a 9 -acodec libmp3lame test.mp3

# Or download a royalty-free track
# https://freepd.com/
# https://incompetech.com/
```

### Create a list of test songs:
```bash
cd ~/Music/forza-radio/
for i in {1..5}; do
  ffmpeg -f lavfi -i anullsrc=r=44100:cl=mono -t 30 -q:a 9 -acodec libmp3lame "Test_Track_$i.mp3" 2>/dev/null
done
ls -lh
```

---

**Good luck testing! Let me know what you find!** 🎮🎵
