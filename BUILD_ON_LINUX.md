# Cross-Compiling FH6 Universal Radio on Linux

This guide explains how to cross-compile the FH6 Universal Radio mod from Linux using MinGW-w64 for use under Proton.

## Prerequisites

### Fedora/RHEL-based Systems
```bash
sudo dnf install -y mingw64-gcc mingw64-gcc-c++ mingw64-cmake mingw64-binutils
```

### Ubuntu/Debian-based Systems
```bash
sudo apt install -y mingw-w64 cmake build-essential
```

### Arch-based Systems
```bash
sudo pacman -S mingw-w64 cmake
```

## Building

### 1. Download Dependencies (One-time)

The project has four single-header library dependencies. Download them:

```bash
cd /path/to/fh6-universal-radio
mkdir -p third_party/{cpp-httplib,nlohmann,toml11,miniaudio}

curl -sS https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h \
  -o third_party/cpp-httplib/httplib.h
  
curl -sS https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp \
  -o third_party/nlohmann/json.hpp
  
curl -sS https://raw.githubusercontent.com/ToruNiina/toml11/main/single_include/toml.hpp \
  -o third_party/toml11/toml.hpp
  
curl -sS https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h \
  -o third_party/miniaudio/miniaudio.h

# Fix nlohmann directory structure
mkdir -p third_party/nlohmann/nlohmann
mv third_party/nlohmann/json.hpp third_party/nlohmann/nlohmann/
```

### 2. Configure and Build

```bash
# Create build directory
mkdir -p build
cd build

# Configure with MinGW toolchain
cmake -DCMAKE_TOOLCHAIN_FILE=../mingw64-toolchain.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      ..

# Build (uses parallel LTO compilation)
cmake --build . --config Release
```

The build uses `-flto` (Link-Time Optimization) for maximum size/performance optimization.

### 3. Locate Output

The compiled DLL is at:
```
build/version.dll (2.9 MB)
```

## Installation for Proton

1. **Obtain Forza Horizon 6 radio station media overlay**
   - This comes from any existing FH6 radio mod ZIP on Nexus Mods
   - Extract the `media/` folder from the ZIP

2. **Set up mod structure**
   ```
   <Forza Horizon 6 install directory>/
   ├── version.dll (from build/version.dll)
   └── fh6-radio/
       ├── media/ (from radio mod ZIP)
       └── ui/ (from radio mod ZIP if available)
   ```

3. **Enable DLL Override in Proton**
   
   Edit or create `user_settings.py` in your Proton prefix:
   ```
   PROTON_DLL_OVERRIDES={
       "version": "native"
   }
   ```
   
   Or use Protontricks:
   ```bash
   protontricks <app_id> PROTON_DLL_OVERRIDES="version=native"
   ```

4. **Configure in-game**
   - Launch Forza Horizon 6
   - Go to **Audio settings**
   - Set **Radio DJ = Off**
   - Set **Streamer Mode = On**
   - Cycle through radio stations

5. **Access dashboard**
   - Open browser to `http://localhost:8420`
   - Configure local files or YouTube Music sources

## What Changed for MinGW Compatibility

### 1. **DLL Export Syntax** (`src/proxy/dll_main.cpp`)
- MSVC: Uses `__pragma(comment(linker, "/EXPORT:..."))` 
- MinGW: Uses `.def` file (`src/proxy/version.def`) with linker flags

### 2. **SEH Exception Handling** (`include/fh6/safe_mem.hpp`)
- MSVC: Uses `__try/__except` for structured exceptions
- MinGW: Falls back to standard C++ `try/catch` (less robust but safer)
- The `is_readable()` pre-checks provide similar safety guarantees

### 3. **Build Configuration** (`CMakeLists.txt`)
- Added MinGW compiler flags: `-Wall -Wextra -Wpedantic -fexceptions`
- Conditional compilation: `#ifdef _MSC_VER` guards MSVC-specific code
- Enables `-flto` for size optimization

## Known Differences Under Proton

| Feature | Windows | Proton/Wine | Status |
|---------|---------|-------------|--------|
| DLL injection | Native | Via `WINEDLLOVERRIDE` | ✅ Works |
| SEH exceptions | `__try/__except` | C++ try/catch fallback | ⚠️ Limited |
| Memory APIs | Full | Well-emulated | ✅ Works |
| Job objects | Full | Emulated (may leak processes) | ⚠️ Best effort |
| Process spawning | Full | Fully supported | ✅ Works |

### Potential Issues Under Proton

1. **Signature scanning may fail** if game PE layout differs
   - Graceful fallback: logs warning, FMOD patching skipped
   
2. **std::string layout assumptions** may not hold
   - Metadata injection fails silently if layout doesn't match
   
3. **Job object cleanup** may not terminate subprocesses reliably
   - YouTube Music might hang or zombie processes pile up

All failures have graceful fallbacks - the mod won't crash, it'll just lose features.

## Troubleshooting

### Build Errors

**"toml.hpp: No such file or directory"**
- Ensure you ran the dependency download step

**"unrecognized command-line option '-EHsc'"**
- This is expected on MinGW (uses `-fexceptions` instead)
- The CMakeLists.txt should handle this automatically

### Runtime Issues Under Proton

**Check logs:**
```bash
# DLL init log
~/.proton/[pfx]/drive_c/fh6-radio/bridge.log

# YouTube Music stderr
~/.proton/[pfx]/drive_c/temp/fh6-yt-stderr.log
```

**Radio station doesn't appear:**
- Verify `version.dll` is in game directory
- Check Audio > Streamer Mode is ON
- Restart the game

**Dashboard says "bridge offline":**
- Media overlay not installed
- Check `fh6-radio/media/` and `fh6-radio/ui/` exist

**YouTube Music doesn't work:**
- `yt-dlp`, `ffmpeg`, `deno` must be on Windows PATH (visible to Proton)
- Check `fh6-yt-stderr.log` for details

## Compilation Options

### Disable LTO (if slow)
```cmake
# In CMakeLists.txt, comment out:
# add_link_options("$<$<CONFIG:Release>:-flto>")
```

### Debug Build
```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
```

## Architecture

The compiled DLL:
- **Size**: ~2.9 MB (with LTO)
- **Architecture**: x86-64 (Windows PE format)
- **Exports**: 18 version.dll functions (forwarded to Windows System32)
- **Platform**: Any Windows x64 or Proton x64 prefix

The mod is a DLL proxy that:
1. Intercepts `version.dll` loads (Windows redirects to our DLL)
2. Spawns a background thread
3. Discovers the game's radio stream objects
4. Injects a custom audio DSP into FMOD's mixer chain
5. Runs an HTTP dashboard for configuration
6. Spawns external tools for YouTube Music support

All core logic is cross-platform C++20; only the DLL entry point and Windows API calls are platform-specific.
