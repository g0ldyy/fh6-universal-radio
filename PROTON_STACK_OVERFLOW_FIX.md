# Stack Overflow Fix for Proton Compatibility

## Issue
The previous build used `-static-libgcc -static-libstdc++ -static` flags to fully static-link the MinGW runtime into the DLL. This caused a **stack overflow during DLL initialization** when the DLL was loaded under Proton/Wine, likely due to CRT startup code initialization ordering issues with the static C++ runtime.

## Solution
Removed the static linking flags and reverted to **dynamic linking** of the MinGW runtime DLLs. The DLL is now 2.9 MB instead of 21 MB.

### Runtime Dependencies
The mod now requires the following MinGW DLLs to be present in the game directory or Windows system directory:

- `libgcc_s_seh-1.dll` - GCC runtime (SEH exception handling)
- `libstdc++-6.dll` - C++ standard library
- `libwinpthread-1.dll` - POSIX threading library (for std::mutex)

### Installation Instructions for Proton

When packaging the mod for distribution, include these DLLs alongside `version.dll`:

```
ForzaHorizon6/
├── version.dll            (the mod)
├── libgcc_s_seh-1.dll     (MinGW GCC runtime)
├── libstdc++-6.dll        (MinGW C++ runtime)
└── libwinpthread-1.dll    (MinGW threading runtime)
```

These DLLs can be extracted from any MinGW-w64 installation, typically found at:
- `/usr/x86_64-w64-mingw32/lib/` on Fedora/RHEL
- Similar paths on other Linux distributions

Alternatively, when running under Proton, you can:
1. Copy the DLLs to the Proton prefix's `drive_c/windows/system32/` directory
2. Use Winetricks to install the required libraries
3. Use a bottle/prefix manager to pre-stage the dependencies

## Why Not Static Linking?
While static linking avoids runtime dependency issues on Windows, it creates problems in Proton:
1. CRT initialization code conflicts with Wine's emulation layer
2. Stack/heap management differs between native Windows and Wine
3. Exception handling (SEH) initialization can corrupt the stack

Dynamic linking is more compatible with Wine/Proton's emulation layer and is the recommended approach for cross-platform DLL development.

## Code Changes
- Modified `CMakeLists.txt`: Removed static linking flags
- Fixed `src/log.cpp`: Added platform-specific handling for `localtime_s` (MSVC) vs `std::localtime` (MinGW)

## Testing
After rebuilding:
1. Copy `build/version.dll` to the Forza game directory
2. Ensure the three MinGW runtime DLLs are available to the game
3. Launch the game under Proton with debug enabled (e.g., `PROTON_LOG=1`)
4. Check for the stack overflow error in the log

If the game still crashes, the log should show different error messages that are more actionable.
