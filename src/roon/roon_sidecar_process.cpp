#include "fh6/roon/roon_sidecar_process.hpp"
#include "fh6/log.hpp"

#include <windows.h>

#include <string>
#include <system_error>
#include <utility>

namespace fh6::roon {

struct RoonSidecarProcess::Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value{h} {}
    ~Handle() {
        if (value) CloseHandle(value);
    }
};

namespace {

std::wstring quote_arg(const std::wstring& value) {
    if (value.empty()) return L"\"\"";
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring out{L"\""};
    for (wchar_t ch : value) {
        if (ch == L'"') out += L'\\';
        out += ch;
    }
    out += L'"';
    return out;
}

HANDLE create_job() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

HANDLE open_log(const std::filesystem::path& path) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    return CreateFileW(path.wstring().c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

std::string wide_to_utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(),
                        needed, nullptr, nullptr);
    return out;
}

} // namespace

RoonSidecarProcess::RoonSidecarProcess(RoonConfig cfg, std::filesystem::path data_dir)
    : cfg_{std::move(cfg)}, data_dir_{std::move(data_dir)} {}

RoonSidecarProcess::~RoonSidecarProcess() { stop(); }

std::filesystem::path RoonSidecarProcess::resolve_node_path() const {
    if (!cfg_.node_path.empty()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(cfg_.node_path, ec)) return cfg_.node_path;
        return {};
    }

    wchar_t resolved[MAX_PATH] = {};
    const DWORD len = SearchPathW(nullptr, L"node", L".exe", MAX_PATH, resolved, nullptr);
    if (len == 0 || len >= MAX_PATH) return {};
    return std::filesystem::path{resolved};
}

std::filesystem::path RoonSidecarProcess::resolved_bridge_path() const {
    if (cfg_.bridge_path.empty()) return {};
    if (cfg_.bridge_path.is_absolute()) return cfg_.bridge_path;
    const auto base = data_dir_.empty() ? std::filesystem::current_path() : data_dir_;
    return base / cfg_.bridge_path;
}

void RoonSidecarProcess::set_error(std::string message) {
    error_ = std::move(message);
    log::warn("[roon-sidecar] {}", error_);
}

bool RoonSidecarProcess::start() {
    if (running()) return true;
    error_.clear();

    const auto node = resolve_node_path();
    if (node.empty()) {
        set_error(
            "Node.js executable was not found. Install Node.js, keep node.exe on PATH, or set "
            "[roon].node_path to the full node.exe path.");
        return false;
    }

    const auto bridge = resolved_bridge_path();
    std::error_code ec;
    if (bridge.empty() || !std::filesystem::is_regular_file(bridge, ec)) {
        set_error("Roon sidecar script was not found at " + bridge.string() +
                  ". Set [roon].bridge_path to tools\\roon-bridge\\index.mjs or an absolute path.");
        return false;
    }

    const auto dir = data_dir_.empty() ? std::filesystem::current_path() : data_dir_;
    std::filesystem::create_directories(dir, ec);
    auto stdout_handle = std::make_unique<Handle>(open_log(dir / "roon-sidecar.out.log"));
    auto stderr_handle = std::make_unique<Handle>(open_log(dir / "roon-sidecar.err.log"));
    if (!stdout_handle->value || !stderr_handle->value) {
        set_error("Failed to open Roon sidecar stdout/stderr log files in " + dir.string());
        return false;
    }
    log::info("[roon-sidecar] log files stdout={} stderr={}",
              (dir / "roon-sidecar.out.log").string(), (dir / "roon-sidecar.err.log").string());

    HANDLE job_handle = create_job();
    if (!job_handle) {
        set_error("Failed to create Roon sidecar job object.");
        return false;
    }
    auto job = std::make_unique<Handle>(job_handle);

    std::wstring command = quote_arg(node.wstring()) + L" " + quote_arg(bridge.wstring()) +
                           L" --host 127.0.0.1 --port 47821";
    if (!cfg_.selected_zone_id.empty()) {
        command +=
            L" --zone-id " + quote_arg(std::filesystem::path{cfg_.selected_zone_id}.wstring());
    }
    log::info("[roon-sidecar] start command={}", wide_to_utf8(command));

    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = stdout_handle->value;
    si.hStdError  = stderr_handle->value;

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                        bridge.parent_path().wstring().c_str(), &si, &pi)) {
        set_error("Failed to start Roon sidecar with Node.js: Win32 error " +
                  std::to_string(GetLastError()));
        return false;
    }

    if (!AssignProcessToJobObject(job->value, pi.hProcess)) {
        const DWORD assign_error = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        set_error("Failed to attach Roon sidecar to its job object: Win32 error " +
                  std::to_string(assign_error));
        return false;
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    job_        = std::move(job);
    process_    = std::make_unique<Handle>(pi.hProcess);
    process_id_ = pi.dwProcessId;
    log::info("[roon-sidecar] started pid={} script={}", process_id_, bridge.string());
    return true;
}

bool RoonSidecarProcess::running() const noexcept {
    if (!process_ || !process_->value) return false;
    DWORD code = 0;
    return GetExitCodeProcess(process_->value, &code) && code == STILL_ACTIVE;
}

void RoonSidecarProcess::stop() noexcept {
    if (process_ && process_->value && running()) {
        TerminateProcess(process_->value, 0);
        WaitForSingleObject(process_->value, 2000);
        log::info("[roon-sidecar] stopped pid={}", process_id_);
    }
    process_.reset();
    job_.reset();
    process_id_ = 0;
}

} // namespace fh6::roon
