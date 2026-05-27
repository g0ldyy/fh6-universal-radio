// version.dll proxy: forwards every export to the real system DLL via PE
// forwarders, and spawns the bridge on DLL_PROCESS_ATTACH so the loader is
// never blocked on FMOD discovery or HTTP startup.

#include <windows.h>

// Forwarding to "version.dll" without a path resolves back to us; the
// absolute system32 path is what breaks the basename collision.
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

namespace fh6 {
void run_bridge(HMODULE self) noexcept;
} // namespace fh6

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
