# Fixed MinGW Runtime Dependency Issue

## Problem
The compiled DLL had dependencies on MinGW runtime libraries that weren't available:
- `libgcc_s_seh-1.dll` (exception handling)
- `libstdc++-6.dll` (C++ standard library)
- `libwinpthread-1.dll` (threading)

This caused the game to crash on startup when Wine couldn't find these DLLs.

## Solution
Rebuilt with static linking of MinGW runtime libraries via:
```cmake
add_link_options(-static-libgcc -static-libstdc++ -static)
```

## Result
**New DLL**: `build/version.dll` (21 MB, fully self-contained)
- Size increased from 2.9 MB to 21 MB
- Now contains all necessary runtime libraries embedded
- No external dependencies needed

## Installation
1. Replace your current `version.dll` in the Forza Horizon 6 directory with the new one from `build/version.dll`
2. Restart the game
3. The DLL should now load without dependency errors

## What Changed
Modified `CMakeLists.txt` to add static linking flags for MinGW builds:
```cmake
# Statically link MinGW runtime to avoid runtime library dependencies
add_link_options(-static-libgcc -static-libstdc++ -static)
```

This ensures the compiled DLL is fully standalone and doesn't require external runtime libraries to be present on the system.
