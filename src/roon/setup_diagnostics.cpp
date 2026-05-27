#include "fh6/roon/setup_diagnostics.hpp"

#include "fh6/audio/endpoint_classifier.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string_view>

namespace fh6::roon {
namespace {

constexpr const char* kRoonDownloadUrl = "https://roon.app/en/downloads";
constexpr const char* kRoonBridgeUrl = "https://help.roonlabs.com/portal/en/kb/articles/roonbridge";
constexpr const char* kVbCableUrl    = "https://vb-audio.com/Cable/";
constexpr const char* kNodeUrl       = "https://nodejs.org/";

std::string lower_ascii(std::string_view value) {
    std::string out{value};
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(),
                        needed, nullptr, nullptr);
    return out;
}

void add_issue(RoonSetupDiagnostics& out, std::string id, std::string severity,
               std::string message) {
    out.issues.push_back({std::move(id), std::move(severity), std::move(message)});
}

void add_action(RoonSetupDiagnostics& out, std::string id, std::string label,
                std::string url = {}) {
    out.actions.push_back({std::move(id), std::move(label), std::move(url)});
}

bool process_matches(std::string_view process, std::string_view name) {
    return lower_ascii(process) == name;
}

bool has_local_roon(const SetupProbeSnapshot& snapshot) {
    if (snapshot.roon_install_found) return true;
    return std::ranges::any_of(snapshot.processes, [](const std::string& process) {
        return process_matches(process, "roon.exe") || process_matches(process, "roonserver.exe") ||
               process_matches(process, "roonappliance.exe");
    });
}

bool has_bridge(const SetupProbeSnapshot& snapshot) {
    if (snapshot.bridge_install_found) return true;
    return std::ranges::any_of(snapshot.processes, [](const std::string& process) {
        return process_matches(process, "roonbridge.exe");
    });
}

bool file_exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

bool node_available(const RoonConfig& cfg) {
    if (!cfg.node_path.empty()) return file_exists(cfg.node_path);
    wchar_t resolved[MAX_PATH] = {};
    return SearchPathW(nullptr, L"node", L".exe", MAX_PATH, resolved, nullptr) != 0;
}

std::filesystem::path env_path(const wchar_t* key) {
    wchar_t* raw    = nullptr;
    std::size_t len = 0;
    if (_wdupenv_s(&raw, &len, key) != 0 || !raw) return {};
    std::unique_ptr<wchar_t, decltype(&std::free)> owned{raw, std::free};
    return std::filesystem::path{owned.get()};
}

bool any_file_exists(const std::vector<std::filesystem::path>& paths) {
    return std::ranges::any_of(paths, file_exists);
}

std::vector<std::string> running_processes() {
    std::vector<std::string> out;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return out;
    const auto close = std::unique_ptr<void, decltype(&CloseHandle)>{snapshot, CloseHandle};

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot, &entry)) return out;
    do {
        out.push_back(wide_to_utf8(entry.szExeFile));
    } while (Process32NextW(snapshot, &entry));
    return out;
}

} // namespace

RoonSetupDiagnostics build_setup_diagnostics(const SetupProbeSnapshot& snapshot) {
    RoonSetupDiagnostics out;
    out.node_available   = snapshot.node_available;
    const bool local     = has_local_roon(snapshot);
    const bool bridge    = has_bridge(snapshot);
    out.roon_environment = local ? "local_server" : (bridge ? "bridge_only" : "not_found");

    const audio::WasapiRenderDevice* preferred = nullptr;
    const audio::WasapiRenderDevice* fallback  = nullptr;
    std::size_t fallback_count                 = 0;
    std::size_t avoid_count                    = 0;
    for (const auto& endpoint : snapshot.render_endpoints) {
        const auto classification = audio::classify_endpoint(endpoint.name);
        if (classification.recommendation == "preferred" && !preferred) preferred = &endpoint;
        if (classification.recommendation == "fallback") {
            fallback = fallback ? fallback : &endpoint;
            ++fallback_count;
        }
        if (classification.recommendation == "avoid") ++avoid_count;
    }

    const auto* recommended = preferred ? preferred : fallback;
    if (recommended) {
        out.recommended_endpoint_id   = recommended->id;
        out.recommended_endpoint_name = recommended->name;
    }
    out.cable_environment = preferred           ? "hifi"
                          : fallback_count > 1  ? "multiple"
                          : fallback_count == 1 ? "vb_cable"
                          : avoid_count > 0     ? "partial"
                                                : "missing";

    add_action(out, "recheck", "Recheck");
    if (!out.node_available) {
        add_issue(out, "node_missing", "warn", "Node.js was not found for the Roon sidecar.");
        add_action(out, "open_node_download", "Open Node.js download", kNodeUrl);
    }
    if (out.roon_environment == "not_found") {
        add_issue(out, "roon_missing", "warn", "Roon Server or Roon for Windows was not detected.");
        add_action(out, "open_roon_download", "Open Roon download", kRoonDownloadUrl);
    } else if (out.roon_environment == "bridge_only") {
        add_issue(out, "roon_bridge_only", "warn",
                  "Roon Bridge was detected, but Roon Server was not detected on this PC.");
        add_action(out, "open_roon_bridge_help", "Open Roon Bridge help", kRoonBridgeUrl);
    }
    if (out.cable_environment == "missing" || out.cable_environment == "partial") {
        add_issue(out, "loopback_endpoint_missing", "warn",
                  "No recommended VB-Audio render endpoint was detected.");
        add_action(out, "open_vb_hifi_cable_download", "Open VB-Audio download", kVbCableUrl);
    }
    if (!out.recommended_endpoint_id.empty()) {
        add_action(out, "select_recommended_endpoint", "Use recommended device");
        add_action(out, "test_audio", "Test audio");
    }
    return out;
}

SetupProbeSnapshot collect_setup_probe(const RoonConfig& cfg) {
    const auto local_app_data = env_path(L"LOCALAPPDATA");
    const auto program_files  = env_path(L"ProgramFiles");
    SetupProbeSnapshot snapshot;
    snapshot.processes            = running_processes();
    snapshot.render_endpoints     = audio::enumerate_render_devices();
    snapshot.node_available       = node_available(cfg);
    snapshot.roon_install_found   = any_file_exists({
        local_app_data / "Roon" / "Application" / "Roon.exe",
        local_app_data / "RoonServer" / "Application" / "RoonServer.exe",
        program_files / "Roon" / "Application" / "Roon.exe",
        program_files / "RoonServer" / "Application" / "RoonServer.exe",
    });
    snapshot.bridge_install_found = any_file_exists({
        program_files / "RoonBridge" / "Application" / "RoonBridge.exe",
        local_app_data / "RoonBridge" / "Application" / "RoonBridge.exe",
    });
    return snapshot;
}

} // namespace fh6::roon
