# Critical Windows APIs Used by FH6 Radio - Wine Support Status

## Direct Comparison to Spotify Mod Crash

The Spotify mod crashed because:
```
wine: Call from 0x7bc24670 to unimplemented function ntdll.dll.RtlVirtualUnwind
```

**RtlVirtualUnwind** is a stub in Wine - not implemented.

---

## FH6 Radio's Windows APIs - Wine Support Status

| API | Location | Wine Support | Risk |
|-----|----------|--------------|------|
| `GetModuleFileNameW` | `src/bridge.cpp` | ✅ Implemented | Low |
| `GetModuleHandleW` | `src/bridge.cpp` | ✅ Implemented | Low |
| `VirtualAlloc` | `src/fmod/metadata_injector.cpp` | ✅ Implemented | Low |
| `VirtualQuery` | `src/fmod/radio_discovery.cpp` | ✅ Implemented | Low |
| `CreateThread` | `src/proxy/dll_main.cpp` | ✅ Implemented | Low |
| `DisableThreadLibraryCalls` | `src/proxy/dll_main.cpp` | ✅ Implemented | Low |
| `CreateFileW` | `src/sources/youtube_music_source.cpp` | ✅ Implemented | Low |
| `CreateJobObjectW` | `src/sources/youtube_music_source.cpp` | ✅ Implemented | Medium |
| `SetInformationJobObject` | `src/sources/youtube_music_source.cpp` | ✅ Implemented | Medium |
| `CreateProcessW` | `src/sources/youtube_music_source.cpp` | ✅ Implemented | Low |
| `CreatePipe` | `src/sources/youtube_music_source.cpp` | ✅ Implemented | Low |
| `SetHandleInformation` | `src/sources/youtube_music_source.cpp` | ✅ Implemented | Low |
| `AssignProcessToJobObject` | `src/sources/youtube_music_source.cpp` | ✅ Implemented | Medium |
| `CloseHandle` | Multiple files | ✅ Implemented | Low |
| `ResumeThread` | `src/sources/youtube_music_source.cpp` | ✅ Implemented | Low |
| `TerminateProcess` | `src/sources/youtube_music_source.cpp` | ✅ Implemented | Low |
| `ReadFile` | `src/sources/youtube_music_source.cpp` | ✅ Implemented | Low |

---

## The Critical Difference from Spotify Mod

**Spotify mod crashed on:**
- `RtlVirtualUnwind` (Rust stack unwinding) - NOT IMPLEMENTED in Wine

**FH6 radio uses:**
- All standard Windows APIs that ARE implemented in Wine
- No low-level RTL (Runtime Library) functions
- No Rust runtime with unwinding

---

## Potential Failure Points (Not unimplemented APIs, but behavioral differences)

### 1. `CreateJobObjectW` + `SetInformationJobObject` + Job Cleanup

**Code**:
```cpp
HANDLE job = CreateJobObjectW(nullptr, nullptr);
JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
    CloseHandle(job);
    return nullptr;
}
```

**Wine Support**: Job objects ARE implemented, but...

**Known Issue**: Wine's job object implementation has **limitations**:
- `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` is supposed to kill all child processes when the job object is closed
- Under Wine, this may not work reliably in all scenarios
- Child processes (yt-dlp, ffmpeg, deno) might not get terminated properly

**Impact**: If job cleanup fails, orphaned processes pile up. YouTube Music playback might hang or stall.

**Graceful fallback**: The code checks `if (!SetInformationJobObject(...))` and returns nullptr on failure. The mod logs a warning and YouTube Music simply won't work for that particular request.

---

### 2. `CreatePipe` with Large Buffers

**Code**:
```cpp
if (!CreatePipe(&yt_out_r, &yt_out_w, &sa, 1 << 20)) { bail(); return; }  // 1MB buffer
```

**Wine Support**: Pipes work, but...

**Known Limitation**: Wine's named/unnamed pipe implementation may have issues with:
- Very large pipe buffers (1MB here)
- Multiple simultaneous pipes
- Async I/O on pipes

**Impact**: If pipe creation fails, YouTube Music won't work. Graceful fallback with warning log.

---

### 3. Memory Protection Flags Reporting

**Code** in `src/fmod/radio_discovery.cpp`:
```cpp
const bool readable = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
                      (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_WRITECOPY ||
                       mbi.Protect == PAGE_READONLY) &&
                      (mbi.Protect & PAGE_GUARD) == 0;
```

**Wine Support**: `VirtualQuery` works, but...

**Known Issue**: Wine may report different page protection flags than Windows in some cases. The game's memory layout under Wine might have slightly different protection flags.

**Impact**: Heap scanning might skip valid memory regions, causing radio instance discovery to fail. Graceful fallback: logs "no heap candidates yet" and retries in 5 seconds.

---

## The Real Risk: Not Unimplemented APIs, But Behavioral Differences

Unlike the Spotify mod which hit an **unimplemented function stub**, FH6 radio uses only **implemented APIs**. But those APIs might behave slightly differently under Wine:

1. ✅ **Won't crash like Spotify** - No missing API stubs
2. ⚠️ **Might silently fail to detect things** - Signature scanning, heap scanning, job object cleanup
3. ✅ **Has fallbacks** - Most failures just disable features, not crash

---

## Verdict

**You won't get `wine: Call to unimplemented function`** - that's not a risk here.

**Instead, you might get:**
- `[discovery] no heap candidates yet` - Radio instance not found, waiting...
- `[yt] CreateJobObject failed` - YouTube Music disabled
- `[sigscan] anchor not found` - FMOD patching skipped
- `[dsp] retargeting failed` - Audio playback stops

All logged, all non-fatal.

**Key difference from Spotify mod**: The Spotify mod *actively panicked* when it hit an unimplemented API. FH6 radio has defensive checks everywhere - it just logs and continues or retries.
