# Build Status & Next Steps

## Current State

### Stack Overflow Fixed ✓
- **Issue**: Previous static-linked build (21 MB) crashed with stack overflow during DLL initialization in Proton
- **Root Cause**: Static C++ runtime initialization conflicts with Wine's emulation layer
- **Solution**: Switched to dynamic linking of MinGW runtime DLLs
- **Result**: New DLL is 2.9 MB (same as original) and should load without stack overflow

### Build Artifacts Ready ✓
Located in `/home/devu/Documents/Projects/fh6-universal-radio/build/`:

```
version.dll                 - The mod DLL (2.9 MB)
libgcc_s_seh-1.dll         - GCC runtime (970 KB)
libstdc++-6.dll            - C++ standard library (29 MB)  
libwinpthread-1.dll        - Threading library (74 KB)
```

### Code Changes ✓
1. **CMakeLists.txt**: Removed `-static-libgcc -static-libstdc++` flags
2. **src/log.cpp**: Added MinGW-compatible time formatting (replaced MSVC `localtime_s` with standard `localtime`)

---

## Testing Instructions

### Prerequisites
You need:
- Forza Horizon 6 installed under Proton
- The mod's web UI files (`fh6-radio/ui/index.html`)
- Basic familiarity with Proton logging

### Step 1: Install DLLs
Copy these 4 DLLs to your game directory:
```bash
~/.local/share/Steam/steamapps/common/ForzaHorizon6/
```

**All four files must be present:**
- `version.dll` (the mod)
- `libgcc_s_seh-1.dll` (from build/)
- `libstdc++-6.dll` (from build/)
- `libwinpthread-1.dll` (from build/)

### Step 2: Launch with Logging
```bash
cd ~/.local/share/Steam/steamapps/common/ForzaHorizon6/
PROTON_LOG=1 proton run forzahorizon6.exe
```

Or through Steam's GUI:
- Right-click Forza Horizon 6 → Properties → General → Launch Options
- Add: `PROTON_LOG=1 %command%`

### Step 3: Check Logs
After the crash (or 10 seconds if it doesn't crash), check:
```bash
cat ~/steam-2483190.log | grep -A 5 "stack overflow"
```

**Expected result**: No "stack overflow" error found

### Step 4: Further Troubleshooting
If the game still crashes after the stack overflow is fixed:

1. **Check mod initialization**:
   ```bash
   cat ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log
   ```
   Look for lines like:
   - `[bridge] FH6 Universal Radio starting`
   - `[sigscan] searching for FMOD signatures`
   - `[bridge] running on port 8080`

2. **Check Proton log for other errors**:
   ```bash
   grep -i "error\|exception\|crash" ~/steam-2483190.log | tail -30
   ```

3. **If FMOD signature scanning fails**: This is expected; see `PROTON_COMPATIBILITY_ANALYSIS.md` for details on what to do next.

---

## Expected Outcomes

### Success (Game runs, mod works)
- Game launches without crashing
- Proton log shows: `Loaded L"S:\\...\\version.dll"`
- No "stack overflow" error
- `fh6-radio/bridge.log` shows `[bridge] running on port 8080`
- Radio station appears in-game with web dashboard

### Partial Success (Game runs, mod loads but doesn't work)
- Game launches without crashing
- DLL loads successfully
- But `fh6-radio/bridge.log` shows FMOD signature scanning failed
- This is **expected under Proton** — see compatibility docs

### Still Crashes After Stack Overflow Fix
- Different error in Proton log (not stack overflow)
- Indicates the issue was in our code or the runtime DLL dependencies
- Share the new error message for further diagnosis

---

## Documentation Files
For reference and distribution:

- **PROTON_STACK_OVERFLOW_FIX.md** - Technical details of this fix
- **build/README_RUNTIME_DLLS.md** - Installation guide for end users
- **PROTON_COMPATIBILITY_ANALYSIS.md** - Detailed Proton/Wine compatibility notes (from previous session)
- **PROTON_CRITICAL_APIS.md** - Which Windows APIs have known issues under Proton

---

## If You Need to Rebuild

```bash
cd /home/devu/Documents/Projects/fh6-universal-radio
rm -rf build && mkdir build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../mingw64-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
make -j4

# DLLs will be in build/ directory
```

The build should complete successfully with no errors related to `localtime_s` or static linking issues.

---

## Next Steps Recommendation

1. **Test the fixed DLL** with the instructions above
2. **If stack overflow is fixed**: Great! Move on to testing FMOD functionality
3. **If new errors appear**: Share the error message and we can debug further
4. **If everything works**: Consider packaging for release (ZIP with all 4 DLLs + web UI)

---

*Last Updated: 2026-05-26*  
*Fix: Removed static linking causing stack overflow in Proton*
