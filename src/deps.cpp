#include "fh6/deps.hpp"
#include "fh6/log.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <array>
#include <cctype>
#include <string_view>
#include <system_error>
#include <utility>

namespace fh6 {

namespace {

struct ToolSpec {
    const char* name;
    const wchar_t* out_name; // final name under bin/
    const wchar_t* url;
    const char* sha256; // lowercase hex, "" = skip verification
};

constexpr std::array<ToolSpec, kToolCount> kSpecs{{
    {"ffmpeg", L"ffmpeg.exe",
     L"https://github.com/g0ldyy/fh6-universal-radio/releases/download/deps/ffmpeg.exe", ""},
    {"yt-dlp", L"yt-dlp.exe",
     L"https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe", ""},
    {"librespot", L"librespot.exe",
     L"https://github.com/g0ldyy/fh6-universal-radio/releases/download/deps/librespot.exe", ""},
    {"texconv", L"texconv.exe",
     L"https://github.com/microsoft/DirectXTex/releases/latest/download/texconv.exe", ""},
}};

const ToolSpec& spec(Tool t) { return kSpecs[static_cast<std::size_t>(t)]; }

struct WinHttp {
    HINTERNET h = nullptr;
    ~WinHttp() {
        if (h) WinHttpCloseHandle(h);
    }
    operator HINTERNET() const { return h; }
};

std::string to_hex(const unsigned char* p, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (std::size_t i = 0; i < n; ++i) {
        s[i * 2]     = d[p[i] >> 4];
        s[i * 2 + 1] = d[p[i] & 0xF];
    }
    return s;
}

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

// Streams url to out, hashing as it goes. Reports byte progress through the
// atomics so the dashboard can render a bar.
bool download(const std::wstring& url, const std::filesystem::path& out,
              std::atomic<std::uint64_t>& got, std::atomic<std::uint64_t>& total,
              std::string& sha_hex, const std::atomic<bool>& stop, std::string& err) {
    URL_COMPONENTS uc{};
    uc.dwStructSize      = sizeof(uc);
    uc.dwHostNameLength  = (DWORD)-1;
    uc.dwUrlPathLength   = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        err = "bad url";
        return false;
    }
    std::wstring host{uc.lpszHostName, uc.dwHostNameLength};
    std::wstring path{uc.lpszUrlPath, uc.dwUrlPathLength};
    path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

    WinHttp ses{WinHttpOpen(L"fh6-universal-radio", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!ses) {
        err = "WinHttpOpen failed";
        return false;
    }
    WinHttp con{WinHttpConnect(ses, host.c_str(), uc.nPort, 0)};
    if (!con) {
        err = "connect failed";
        return false;
    }

    const DWORD secure = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    WinHttp req{WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, secure)};
    if (!req) {
        err = "open request failed";
        return false;
    }

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
                            0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        err = "request failed";
        return false;
    }

    DWORD code = 0, len = sizeof(code);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code,
                        &len, nullptr);
    if (code != 200) {
        err = "http " + std::to_string(code);
        return false;
    }

    DWORD content = 0;
    len           = sizeof(content);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, nullptr,
                            &content, &len, nullptr))
        total.store(content, std::memory_order_relaxed);

    BCRYPT_ALG_HANDLE alg   = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);

    HANDLE f = CreateFileW(out.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        if (hash) BCryptDestroyHash(hash);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        err = "cannot write " + out.string();
        return false;
    }

    bool ok = true;
    std::array<unsigned char, 64 * 1024> buf{};
    for (;;) {
        if (stop.load(std::memory_order_acquire)) {
            ok  = false;
            err = "aborted";
            break;
        }
        DWORD avail = 0;
        if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &avail)) {
            ok  = false;
            err = "read error";
            break;
        }
        if (avail == 0) break;
        BCryptHashData(hash, buf.data(), avail, 0);
        DWORD wrote = 0;
        if (!WriteFile(f, buf.data(), avail, &wrote, nullptr) || wrote != avail) {
            ok  = false;
            err = "disk write error";
            break;
        }
        got.fetch_add(avail, std::memory_order_relaxed);
    }
    CloseHandle(f);

    if (ok) {
        unsigned char digest[32];
        BCryptFinishHash(hash, digest, sizeof(digest), 0);
        sha_hex = to_hex(digest, sizeof(digest));
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

} // namespace

DependencyManager::DependencyManager(std::filesystem::path bin_dir) : bin_dir_{std::move(bin_dir)} {
    std::error_code ec;
    std::filesystem::create_directories(bin_dir_, ec);
    for (std::size_t i = 0; i < kToolCount; ++i) {
        const auto p = bin_dir_ / spec(static_cast<Tool>(i)).out_name;
        slots_[i].present.store(std::filesystem::exists(p, ec), std::memory_order_relaxed);
    }
}

DependencyManager::~DependencyManager() {
    stop_.store(true, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
}

void DependencyManager::start(std::function<void()> on_tool_ready) {
    on_ready_ = std::move(on_tool_ready);
    retry();
}

void DependencyManager::retry() {
    if (worker_.joinable()) worker_.join();
    if (stop_.load(std::memory_order_acquire)) return;
    worker_ = std::thread{[this] { run_worker(); }};
}

void DependencyManager::run_worker() {
    std::error_code ec;
    for (std::size_t i = 0; i < kToolCount && !stop_.load(std::memory_order_acquire); ++i) {
        Slot& s             = slots_[i];
        const ToolSpec& sp  = spec(static_cast<Tool>(i));
        const auto out_path = bin_dir_ / sp.out_name;
        if (std::filesystem::exists(out_path, ec)) {
            s.present.store(true, std::memory_order_release);
            continue;
        }

        s.downloaded.store(0, std::memory_order_relaxed);
        s.total.store(0, std::memory_order_relaxed);
        s.downloading.store(true, std::memory_order_release);
        {
            std::scoped_lock lk{s.err_mu};
            s.error.clear();
        }
        log::info("[deps] downloading {}", sp.name);

        const auto dl = bin_dir_ / (std::wstring{sp.out_name} + L".part");
        std::string sha, err;
        bool ok = download(sp.url, dl, s.downloaded, s.total, sha, stop_, err);

        if (ok && sp.sha256[0] && !ieq(sha, sp.sha256)) {
            ok  = false;
            err = "checksum mismatch";
        }
        if (ok) {
            std::filesystem::rename(dl, out_path, ec);
            if (ec) {
                ok  = false;
                err = ec.message();
            }
        }
        if (!ok) std::filesystem::remove(dl, ec);

        s.downloading.store(false, std::memory_order_release);
        if (ok) {
            s.present.store(true, std::memory_order_release);
            log::info("[deps] {} ready", sp.name);
            if (on_ready_) on_ready_();
        } else {
            std::scoped_lock lk{s.err_mu};
            s.error = err;
            log::warn("[deps] {} failed: {}", sp.name, err);
        }
    }
}

std::filesystem::path DependencyManager::resolve(Tool tool,
                                                 const std::filesystem::path& user_override) const {
    if (!user_override.empty()) return user_override;
    const auto p = bin_dir_ / spec(tool).out_name;
    std::error_code ec;
    return std::filesystem::exists(p, ec) ? p : std::filesystem::path{};
}

std::vector<DependencyStatus> DependencyManager::snapshot() const {
    std::vector<DependencyStatus> out;
    out.reserve(kToolCount);
    for (std::size_t i = 0; i < kToolCount; ++i) {
        const Slot& s = slots_[i];
        DependencyStatus st;
        st.name             = spec(static_cast<Tool>(i)).name;
        st.managed_present  = s.present.load(std::memory_order_acquire);
        st.downloading      = s.downloading.load(std::memory_order_acquire);
        st.downloaded_bytes = s.downloaded.load(std::memory_order_relaxed);
        st.total_bytes      = s.total.load(std::memory_order_relaxed);
        {
            std::scoped_lock lk{s.err_mu};
            st.error = s.error;
        }
        out.push_back(std::move(st));
    }
    return out;
}

} // namespace fh6
