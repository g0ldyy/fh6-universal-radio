// version.dll proxy: forwards every export to the real system DLL via PE
// forwarders, and spawns the bridge on DLL_PROCESS_ATTACH so the loader is
// never blocked on FMOD discovery or HTTP startup.

// Disable exceptions for DllMain to avoid C++ runtime initialization issues under Wine/Proton.
// The mod spawns a separate thread (bridge_thread) which will have exceptions enabled.
#if defined(__GNUC__) && !defined(_MSC_VER)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC optimize("-fno-exceptions")
#endif

#include <windows.h>

// Forwarding to "version.dll" without a path resolves back to us; the
// absolute system32 path is what breaks the basename collision.
//
// MSVC: __pragma(comment(linker, "/EXPORT:..."))
// MinGW: Use .def file or asm directives. We use .def file approach via
// CMakeLists.txt which generates version.def from this list.
#ifdef _MSC_VER
#define FWD(name) \
    __pragma(comment(linker, "/EXPORT:" #name "=C:\\Windows\\System32\\version." #name))
FWD(GetFileVersionInfoA)
FWD(GetFileVersionInfoByHandle)
FWD(GetFileVersionInfoExA)
FWD(GetFileVersionInfoExW)
FWD(GetFileVersionInfoSizeA)
FWD(GetFileVersionInfoSizeExA)
FWD(GetFileVersionInfoSizeExW)
FWD(GetFileVersionInfoSizeW)
FWD(GetFileVersionInfoW)
FWD(VerFindFileA)
FWD(VerFindFileW)
FWD(VerInstallFileA)
FWD(VerInstallFileW)
FWD(VerLanguageNameA)
FWD(VerLanguageNameW)
FWD(VerQueryValueA)
FWD(VerQueryValueW)
#undef FWD
#endif

namespace fh6 {
void run_bridge(HMODULE self) noexcept;
} // namespace fh6

namespace {
// Stub exports for version.dll forwarders - these are minimal and don't do anything.
// The real implementation is in System32\version.dll, but we provide stubs
// so code that imports these functions can at least load.
extern "C" {
    __declspec(dllexport) void* GetFileVersionInfoA(void) { return nullptr; }
    __declspec(dllexport) void* GetFileVersionInfoByHandle(void) { return nullptr; }
    __declspec(dllexport) void* GetFileVersionInfoExA(void) { return nullptr; }
    __declspec(dllexport) void* GetFileVersionInfoExW(void) { return nullptr; }
    __declspec(dllexport) void* GetFileVersionInfoSizeA(void) { return nullptr; }
    __declspec(dllexport) void* GetFileVersionInfoSizeExA(void) { return nullptr; }
    __declspec(dllexport) void* GetFileVersionInfoSizeExW(void) { return nullptr; }
    __declspec(dllexport) void* GetFileVersionInfoSizeW(void) { return nullptr; }
    __declspec(dllexport) void* GetFileVersionInfoW(void) { return nullptr; }
    __declspec(dllexport) void* VerFindFileA(void) { return nullptr; }
    __declspec(dllexport) void* VerFindFileW(void) { return nullptr; }
    __declspec(dllexport) void* VerInstallFileA(void) { return nullptr; }
    __declspec(dllexport) void* VerInstallFileW(void) { return nullptr; }
    __declspec(dllexport) void* VerLanguageNameA(void) { return nullptr; }
    __declspec(dllexport) void* VerLanguageNameW(void) { return nullptr; }
    __declspec(dllexport) void* VerQueryValueA(void) { return nullptr; }
    __declspec(dllexport) void* VerQueryValueW(void) { return nullptr; }
}
} // namespace

namespace {
DWORD WINAPI bridge_thread(LPVOID self) {
    fh6::run_bridge(static_cast<HMODULE>(self));
    return 0;
}
} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) noexcept {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        if (HANDLE t = CreateThread(nullptr, 0, bridge_thread, hModule, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}

#if defined(__GNUC__) && !defined(_MSC_VER)
#pragma GCC diagnostic pop
#endif
