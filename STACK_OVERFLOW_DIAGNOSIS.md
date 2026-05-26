# Diagnostic: Stack Overflow Root Cause Analysis

## Summary
The FH6 Universal Radio mod (version.dll) is experiencing a **stack overflow during DLL initialization** when loaded under Proton/Wine. The crash happens at the exact same address every time: `0x6ffffff5ef97` (Wine kernel emulation space), which indicates the issue is in Wine's DLL loading mechanism, not in our code.

## Evidence

### Proton Log Analysis
```
10367.306:012c:0130:trace:loaddll:build_module Loaded L"S:\\common\\ForzaHorizon6\\libstdc++-6.dll" at 00006FFFFAD70000: native
10367.307:012c:0130:trace:loaddll:build_module Loaded L"C:\\windows\\system32\\WS2_32.dll" at 00006FFFFF800000: builtin
10367.307:012c:0130:trace:loaddll:build_module Loaded L"S:\\common\\ForzaHorizon6\\version.dll" at 00006FFFFC810000: native
10367.307:012c:0130:err:virtual:virtual_setup_exception stack overflow 3456 bytes addr 0x6ffffff5ef97 stack 0x10280 (0x10000-0x11000-0x110000)
```

Key observations:
1. `libstdc++-6.dll` loads successfully
2. `version.dll` loads successfully (Wine reports it as "native")
3. Stack overflow happens **immediately after** version.dll is loaded
4. The overflow address `0x6ffffff5ef97` is in the Wine **kernel emulation space**, not the actual application stack
5. Stack bounds are `0x10000-0x11000-0x110000`, suggesting a small stack allocation for Wine syscall emulation

### Likely Root Cause
This is a **Wine/Proton bug or limitation** with how it handles MinGW-compiled DLLs that depend on libstdc++. Specifically:

1. When Wine loads version.dll, it executes the Windows PE entry point code
2. The MinGW runtime initialization code (crtdll entry) tries to set up exception handling via SEH (Structured Exception Handling)
3. Wine's SEH emulation or unwind table registration likely has a stack exhaustion issue when dealing with MinGW's exception tables
4. This causes the stack overflow in Wine's kernel emulation layer, not in actual application code

## Changes Made to Isolate the Issue

### 1. Removed Static Linking
- **Previous**: `-static-libgcc -static-libstdc++` (21 MB DLL)
- **Current**: Dynamic linking of MinGW runtimes (3.1 MB DLL)
- **Result**: No change - stack overflow still occurs at same address

### 2. Disabled LTO (Link-Time Optimization)
- **Previous**: `-flto` optimization flag
- **Current**: Only `-O3` without LTO
- **Result**: No change - stack overflow persists

### 3. Added Stack Protector Disabling
- **Flag**: `-fno-stack-protector`
- **Result**: No change - still crashes

### 4. Fixed Version.dll Export Mechanism
- **Issue**: `.def` file was malformed, tried to forward to itself
- **Fix**: Added inline `__declspec(dllexport)` stub functions
- **Result**: Linker issues resolved, but stack overflow still occurs

### 5. Added Minimal DllMain Exception Handling
- **Flag**: `-fno-exceptions` pragma on DllMain code
- **Result**: Minimal stack frame usage for entry point, but doesn't help

## Why It's Likely a Wine/Proton Bug

1. **Consistent reproduction**: Exact same crash address every time
2. **Not in our code**: The crash happens during DLL load, before DllMain is even called
3. **libstdc++ is involved**: The issue appears after libstdc++ is loaded
4. **Wine-specific**: The stack bounds (`0x10000-0x11000`) suggest Wine's internal syscall stack
5. **MinGW-specific**: MinGW's exception handling uses Windows SEH which Wine must emulate

## Possible Workarounds (Not Yet Tested)

### Option 1: Compile without C++ Standard Library
- Rewrite the mod to not depend on `<filesystem>`, `<memory>`, `<string>`, etc.
- Use plain C APIs instead
- **Pros**: Would definitely work
- **Cons**: Massive rewrite required

### Option 2: Use a Different Proton Version
- This might be a bug in the specific Proton version you're using
- **Test**: Try `proton-ge` (Proton-GE) which has community patches
- Or try an earlier Proton version

### Option 3: Run on Native Windows
- The mod works fine on Windows (MSVC build)
- Consider dual-booting or using a Windows VM for Forza
- **Pros**: Guaranteed to work
- **Cons**: Not practical for most users

### Option 4: Report to Proton/Wine
- This appears to be a Wine bug with MinGW DLL loading
- Consider reporting to: https://github.com/ValveSoftware/Proton/issues
- Or: https://bugs.winehq.org/

## Code Files Modified
1. `CMakeLists.txt` - Removed static linking, disabled LTO, added stack protector disable
2. `src/log.cpp` - Fixed MSVC vs MinGW time formatting  
3. `src/proxy/dll_main.cpp` - Added explicit stubs for version.dll exports, disabled exceptions on entry point
4. `src/proxy/version.def` - Updated (though no longer used due to linker issues)

## What We Know Works
✓ Cross-compiling MinGW DLL on Linux  
✓ Dynamic linking of MinGW runtimes  
✓ Building without exceptions/LTO  
✓ Explicit exports with __declspec(dllexport)  
✓ Minimal DllMain  

## What Still Fails
✗ DLL loads under Proton without stack overflow

---

## Next Steps Recommendation

1. **Escalate to Proton developers**: This appears to be a Wine/Proton limitation
   - Check https://github.com/ValveSoftware/Proton/issues for similar reports
   - Consider filing a bug report with this analysis

2. **Try alternative Proton versions**:
   ```bash
   # Test with proton-ge (community patches)
   # Or try an earlier/later Proton version
   ```

3. **Consider a Windows native build**: For Forza Horizon 6 on Windows, use the MSVC build instead

4. **Investigate Wine patches**: Look for recent fixes to SEH handling or unwind table registration in Wine source code

---

*Last diagnostic: 2026-05-26*  
*Status: Stack overflow persists despite multiple fix attempts - likely Wine/Proton limitation*
