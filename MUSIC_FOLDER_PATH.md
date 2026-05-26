# Quick Path Reference for Forza Horizon 6 Music Folder

## Your Setup
- **Linux path**: `~/.local/share/Steam/steamapps/common/ForzaHorizon6/Music`
- **Wine sees it as**: `S:\common\ForzaHorizon6\Music`

## Config Path Options

### Option 1: Relative Windows Path (EASIEST) ✅ RECOMMENDED
```toml
[local_files]
music_dir = "Music"
```

**Why this works**: 
- The mod runs from the game folder
- `Music` is relative to the game folder
- Wine automatically resolves it correctly

### Option 2: Full Wine Path
```toml
[local_files]
music_dir = "S:\\common\\ForzaHorizon6\\Music"
```

**Note**: Must escape backslashes with `\\`

### Option 3: Absolute Linux Path (May work)
```toml
[local_files]
music_dir = "/home/devu/.local/share/Steam/steamapps/common/ForzaHorizon6/Music"
```

**Note**: This depends on how the mod reads the path. Relative path is safer.

---

## Step-by-Step Instructions

### 1. Edit the Config File
```bash
nano ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/config.toml
```

### 2. Find the `[local_files]` Section
Look for:
```toml
[local_files]
enabled = true
music_dir = "..."
recursive = true
shuffle = true
```

### 3. Update the Path
**Change this**:
```toml
music_dir = ""
```

**To this**:
```toml
music_dir = "Music"
```

### 4. Save the File
- Press `Ctrl+O` then `Enter` (to save)
- Press `Ctrl+X` (to exit nano)

### 5. Verify Your Music Files
```bash
ls -la ~/.local/share/Steam/steamapps/common/ForzaHorizon6/Music/
```

You should see your music files listed.

---

## Testing After Configuration

### Before Starting Game
```bash
# Clear old logs
rm ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log
```

### Launch Game
Wait 15-20 seconds for full load.

### Check if it Found Your Music
```bash
cat ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log
```

**Look for lines like**:
```
INFO  [bridge] FH6 Universal Radio starting...
INFO  [bridge] webui index.html loaded successfully
INFO  [bridge] running on port 8080
```

**If there are errors about the path**, try adjusting it:
- Try `"Music"` (relative)
- Try `"S:\\common\\ForzaHorizon6\\Music"` (full path)
- Try `/home/devu/.local/share/Steam/steamapps/common/ForzaHorizon6/Music` (Linux path)

### Access the Dashboard
Open in web browser:
```
http://127.0.0.1:8080
```

You should see your music files listed!

---

## Troubleshooting

### "Music files not found"
1. Check that `Music` folder exists: 
   ```bash
   ls ~/.local/share/Steam/steamapps/common/ForzaHorizon6/Music/
   ```

2. Check that files are readable:
   ```bash
   ls -la ~/.local/share/Steam/steamapps/common/ForzaHorizon6/Music/
   ```

3. Make sure at least one file has a supported format (MP3, FLAC, WAV, OGG, AAC, M4A)

### Path Shows in Dashboard but No Songs Listed
1. Try `recursive = false` in config
2. Check file permissions
3. Verify file format is supported

### Different Folder Location
If you put Music somewhere else (like `~/Music/forza`), use one of these paths:
```toml
# Absolute Linux path
music_dir = "/home/devu/Music/forza"

# Or if it's on a mounted Windows drive
music_dir = "/mnt/d/Music/Forza"  # adjust d: to your drive letter
```

---

**Most likely**, just use:
```toml
music_dir = "Music"
```

And it will work! 🎵
