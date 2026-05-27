#include "fh6/roon/roon_sidecar_process.hpp"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error{message};
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::filesystem::path find_node() {
    wchar_t resolved[MAX_PATH] = {};
    const DWORD len = SearchPathW(nullptr, L"node", L".exe", MAX_PATH, resolved, nullptr);
    if (len == 0 || len >= MAX_PATH) throw std::runtime_error{"node.exe not found on PATH"};
    return std::filesystem::path{resolved};
}

void write_script(const std::filesystem::path& path) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out << "console.log('sidecar stdout ready');\n"
           "console.error('sidecar stderr ready');\n"
           "setInterval(() => {}, 1000);\n";
}

bool wait_for_text(const std::filesystem::path& path, std::string_view needle) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(path) && read_text(path).find(needle) != std::string::npos)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    return false;
}

} // namespace

int main() {
    const auto root = std::filesystem::current_path() / "tmp" / "roon-sidecar-process-tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "tools" / "roon-bridge");

    try {
        fh6::RoonConfig missing_node;
        missing_node.enabled = true;
        missing_node.node_path = root / "missing-node.exe";
        missing_node.bridge_path = root / "missing-bridge.mjs";

        fh6::roon::RoonSidecarProcess missing{missing_node, root};
        require(!missing.start(), "missing configured node should not start");
        require(!missing.running(), "missing configured node should not report running");
        require(missing.error().find("Node") != std::string::npos,
                "missing configured node should report actionable Node error");

        const auto script = root / "tools" / "roon-bridge" / "index.mjs";
        write_script(script);

        fh6::RoonConfig cfg;
        cfg.enabled = true;
        cfg.node_path = find_node();
        cfg.bridge_path = std::filesystem::path{"tools"} / "roon-bridge" / "index.mjs";
        cfg.selected_zone_id = "zone-1";

        fh6::roon::RoonSidecarProcess sidecar{cfg, root};
        require(sidecar.resolved_bridge_path() == script,
                "relative bridge_path should resolve against data_dir");
        require(sidecar.start(), "valid node and bridge script should start");
        require(sidecar.started_process_id() != 0, "started sidecar should expose its process id");
        require(sidecar.running(), "started sidecar should report running");

        require(wait_for_text(root / "roon-sidecar.out.log", "sidecar stdout ready"),
                "sidecar stdout should be redirected to log");
        require(wait_for_text(root / "roon-sidecar.err.log", "sidecar stderr ready"),
                "sidecar stderr should be redirected to log");

        sidecar.stop();
        require(!sidecar.running(), "stop should terminate the process this sidecar started");
        sidecar.stop();
        require(!sidecar.running(), "stop should be idempotent");

        std::filesystem::remove_all(root);
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }

    return 0;
}
