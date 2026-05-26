# FH6 Universal Radio: Proton Testing Documentation

**Last Updated:** 2026-05-26

This is a comprehensive collection of documentation for testing the FH6 Universal Radio mod running under Proton on Linux.

---

## 🎯 Choose Your Starting Point

### ⚡ Just Want to Test? (5 minutes)
**→ [PROTON_QUICK_START.md](PROTON_QUICK_START.md)**

Copy-paste quick start with:
- 5-minute setup checklist
- Minimal troubleshooting
- Direct commands to copy

**Best for:** Users who just want to get music playing quickly.

---

### 📖 Want Everything Explained? (30+ minutes)
**→ [PROTON_LOCAL_FILES_TESTING_GUIDE.md](PROTON_LOCAL_FILES_TESTING_GUIDE.md)

A detailed, step-by-step guide covering:
1. Prerequisites and verification
2. Setting up local music files
3. Supported audio formats
4. Configuring the mod (both GUI and file-based)
5. Launching and finding the radio station
6. Web dashboard usage
7. Expected behavior at each step
8. Comprehensive troubleshooting (6 categories with checklists)
9. Log inspection techniques
10. Complete testing checklist
11. Issue reporting template
12. Next steps (YouTube Music, contributing, customizing)
13. Quick reference (commands, config values, FAQ)

**Best for:** Users who need detailed explanations, prefer guided walkthroughs, or need comprehensive troubleshooting.

---

### 🔍 Understanding the Project (Technical Details)
**→ [FINAL_STATUS.md](FINAL_STATUS.md)**

Executive summary of the entire investigation:
- What works and what doesn't
- Why the Wine crash occurs
- Build artifacts
- Key findings and recommendations

**Then see:**
- [PROTON_COMPATIBILITY_ANALYSIS.md](PROTON_COMPATIBILITY_ANALYSIS.md) - Which Windows APIs work under Wine
- [PROTON_CRITICAL_APIS.md](PROTON_CRITICAL_APIS.md) - List of potentially problematic APIs
- [PROTON_STACK_OVERFLOW_FIX.md](PROTON_STACK_OVERFLOW_FIX.md) - Initial fix attempts
- [STACK_OVERFLOW_DIAGNOSIS.md](STACK_OVERFLOW_DIAGNOSIS.md) - Root cause analysis

**Best for:** Developers, system administrators, or those interested in why Proton compatibility is challenging.

---

### 🛠️ Building or Troubleshooting Build Issues?
**→ [BUILD_ON_LINUX.md](BUILD_ON_LINUX.md)**

How to compile the mod on Linux using MinGW-w64.

**Then see:**
- [LINUX_BUILD_SUCCESS.md](LINUX_BUILD_SUCCESS.md) - Confirmation that builds work
- [MINGW_RUNTIME_FIX.md](MINGW_RUNTIME_FIX.md) - Runtime dependency notes
- [build/README_RUNTIME_DLLS.md](build/README_RUNTIME_DLLS.md) - How to install the built DLLs

**Best for:** Users who want to modify the mod or understand the build process.

---

## 📋 Documentation Map

```
Start Here
├── PROTON_QUICK_START.md ...................... 5-min quick start
└── PROTON_LOCAL_FILES_TESTING_GUIDE.md ...... Full detailed guide
    ├── Sections 1-6: Setup & Configuration
    ├── Section 7: Expected Behavior
    ├── Section 8: Troubleshooting (6 categories)
    ├── Section 9: Log Inspection
    ├── Sections 10-14: Testing, Reporting, FAQ

Technical Details
├── FINAL_STATUS.md ............................ Executive summary
├── PROTON_COMPATIBILITY_ANALYSIS.md ......... API compatibility
├── PROTON_CRITICAL_APIS.md .................. Problematic APIs
├── PROTON_STACK_OVERFLOW_FIX.md ............ Fix attempts
└── STACK_OVERFLOW_DIAGNOSIS.md .............. Root cause analysis

Building from Source
├── BUILD_ON_LINUX.md ........................ How to build
├── LINUX_BUILD_SUCCESS.md .................. Build confirmation
├── MINGW_RUNTIME_FIX.md ................... Runtime dependencies
└── build/README_RUNTIME_DLLS.md ........... DLL installation

Mod Features
└── README.md ................................ Main README (features, install, troubleshooting)
```

---

## 🎯 Task-Based Navigation

### "I want to test local files on Proton"
1. Start: [PROTON_QUICK_START.md](PROTON_QUICK_START.md)
2. If issues: [PROTON_LOCAL_FILES_TESTING_GUIDE.md](PROTON_LOCAL_FILES_TESTING_GUIDE.md#8-troubleshooting)

### "Local files work! What about YouTube?"
1. Read: [README.md](README.md) (YouTube Music section)
2. Install: yt-dlp, ffmpeg, deno on Linux
3. Test from: Dashboard at http://localhost:8420

### "I want to understand why Proton compatibility is hard"
1. Read: [FINAL_STATUS.md](FINAL_STATUS.md)
2. Technical: [PROTON_COMPATIBILITY_ANALYSIS.md](PROTON_COMPATIBILITY_ANALYSIS.md)
3. Debug details: [STACK_OVERFLOW_DIAGNOSIS.md](STACK_OVERFLOW_DIAGNOSIS.md)

### "I want to modify the mod"
1. Build: [BUILD_ON_LINUX.md](BUILD_ON_LINUX.md)
2. Code: `src/sources/local_file_source.cpp` (local files logic)
3. Config: `src/config.cpp` (configuration handling)
4. UI: `ui/` folder (web dashboard)

### "Something is broken, and I need to debug"
1. Quick check: [PROTON_QUICK_START.md](PROTON_QUICK_START.md#logs-for-debugging)
2. Detailed: [PROTON_LOCAL_FILES_TESTING_GUIDE.md](PROTON_LOCAL_FILES_TESTING_GUIDE.md#9-checking-logs)
3. Report: [PROTON_LOCAL_FILES_TESTING_GUIDE.md](PROTON_LOCAL_FILES_TESTING_GUIDE.md#11-reporting-issues)

### "The game crashes on startup"
1. Check: [FINAL_STATUS.md](FINAL_STATUS.md) (known issues)
2. Read: [STACK_OVERFLOW_DIAGNOSIS.md](STACK_OVERFLOW_DIAGNOSIS.md) (root cause)
3. Try: [PROTON_LOCAL_FILES_TESTING_GUIDE.md](PROTON_LOCAL_FILES_TESTING_GUIDE.md#86-game-crashes-on-startup)

---

## 📊 Guide Comparison

| Guide | Length | Pace | Audience | Best For |
|-------|--------|------|----------|----------|
| **PROTON_QUICK_START.md** | ~125 lines | ⚡ Fast | Everyone | Getting started quickly |
| **PROTON_LOCAL_FILES_TESTING_GUIDE.md** | ~700 lines | 📖 Thorough | Detailed learners | Complete understanding |
| **FINAL_STATUS.md** | ~200 lines | 📊 Executive | Developers | Understanding constraints |
| **PROTON_COMPATIBILITY_ANALYSIS.md** | ~330 lines | 🔬 Technical | Architects | Design decisions |

---

## 🔑 Key Information at a Glance

### Supported Audio Formats
- **MP3** ✓ (recommended)
- **FLAC** ✓ (lossless)
- **WAV** ✓ (uncompressed)
- **OGG** ✓ (Vorbis)
- **M4A** ✓ (AAC)
- **Opus** ✓ (modern)

### Default Configuration
- Dashboard: http://localhost:8420
- Music directory: Set from dashboard
- Default source: Local files
- Recursive scanning: Enabled
- Shuffle: Enabled

### In-Game Setup Required
- **Radio DJ:** Off
- **Streamer Mode:** On
- Restart game after enabling

### Files You Need
- 4 DLLs: `version.dll`, `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`
- Media overlay: In `fh6-radio/media/` folder
- Config file: Auto-created in `fh6-radio/config.toml`

### Troubleshooting Quick Links
- [No radio station appears](PROTON_LOCAL_FILES_TESTING_GUIDE.md#81-no-local-radio-station-appears-in-game)
- [Dashboard shows "0 tracks"](PROTON_LOCAL_FILES_TESTING_GUIDE.md#82-dashboard-says-no-tracks-found-or-wrong-count)
- [No audio plays](PROTON_LOCAL_FILES_TESTING_GUIDE.md#83-dashboard-shows-tracks-but-no-audio-plays)
- [Dashboard won't load](PROTON_LOCAL_FILES_TESTING_GUIDE.md#84-dashboard-wont-load-or-connection-refused)
- [Audio stutters](PROTON_LOCAL_FILES_TESTING_GUIDE.md#85-music-plays-but-stutters-or-cuts-out)
- [Game crashes](PROTON_LOCAL_FILES_TESTING_GUIDE.md#86-game-crashes-on-startup)

---

## 🚀 Quick Commands Reference

```bash
# Test music folder setup
mkdir -p ~/Games/fh6-music
cp ~/Music/*.mp3 ~/Games/fh6-music/

# Verify DLLs installed
ls -lh ~/.local/share/Steam/steamapps/common/ForzaHorizon6/*.dll

# Watch mod log in real-time
tail -f ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log

# Check if game is running
pgrep -f forzahorizon6

# Access dashboard
firefox http://localhost:8420  # or any browser

# Test a music file
mpv ~/Games/fh6-music/Song.mp3
```

---

## 📞 Getting Help

### If Something Doesn't Work

1. **Check logs first:**
   ```bash
   tail -50 ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log
   ```

2. **Follow the troubleshooting guide:**
   - [PROTON_LOCAL_FILES_TESTING_GUIDE.md#8-troubleshooting](PROTON_LOCAL_FILES_TESTING_GUIDE.md#8-troubleshooting)

3. **Report with full context:**
   - System info (uname -a, Proton version)
   - Music directory info (file count, formats)
   - Mod log (first + last 50 lines)
   - What you expected vs. what happened

See [full reporting guide](PROTON_LOCAL_FILES_TESTING_GUIDE.md#11-reporting-issues)

---

## 📚 Documentation Statistics

| Document | Lines | Topics | Sections |
|----------|-------|--------|----------|
| PROTON_QUICK_START.md | 125 | 5 | 8 |
| PROTON_LOCAL_FILES_TESTING_GUIDE.md | 698 | 14 | 14 |
| FINAL_STATUS.md | ~200 | 8 | Multiple |
| PROTON_COMPATIBILITY_ANALYSIS.md | 331 | Windows APIs | By category |
| PROTON_CRITICAL_APIS.md | 136 | Problematic APIs | By risk level |

**Total Documentation:** ~1,400 lines covering setup, troubleshooting, technical details, and FAQ

---

## ✅ Success Checklist

You know you're ready when:
- [ ] Music files are in a folder (e.g., `~/Games/fh6-music/`)
- [ ] All 4 DLLs are copied to the FH6 directory
- [ ] Game can launch (no immediate crash)
- [ ] Streamer Mode is enabled in Audio settings
- [ ] Web dashboard loads at http://localhost:8420
- [ ] Dashboard shows track count
- [ ] Radio station appears in-game
- [ ] Music plays without errors
- [ ] At least 5 consecutive tracks play smoothly

---

## 🔗 Related Resources

- **Mod GitHub:** [github.com/g0ldyy/fh6-universal-radio](https://github.com/g0ldyy/fh6-universal-radio)
- **Nexus Mods:** [nexusmods.com/forzahorizon6/mods/215](https://www.nexusmods.com/forzahorizon6/mods/215)
- **Discord:** [discord.gg/NyZUcATqWZ](https://discord.gg/NyZUcATqWZ)
- **Main README:** [README.md](README.md)

---

## 📝 Version History

| Date | Changes |
|------|---------|
| 2026-05-26 | Created comprehensive Proton testing documentation set |

---

**Last Updated:** 2026-05-26  
**For questions:** Check [PROTON_LOCAL_FILES_TESTING_GUIDE.md#14-faq](PROTON_LOCAL_FILES_TESTING_GUIDE.md#14-faq)
