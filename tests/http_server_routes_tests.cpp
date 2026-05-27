#include "fh6/audio_source_manager.hpp"
#include "fh6/config_store.hpp"
#include "fh6/fmod/dsp_bridge.hpp"
#include "fh6/http/http_server.hpp"
#include "fh6/sources/roon_source.hpp"

#include <winsock2.h>

#define CPPHTTPLIB_NO_EXCEPTIONS
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error{message};
}

struct Winsock {
    WSADATA data{};
    Winsock() {
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error{"WSAStartup failed"};
    }
    ~Winsock() { WSACleanup(); }
};

uint16_t find_free_port() {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) throw std::runtime_error{"socket failed"};

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(sock);
        throw std::runtime_error{"bind failed"};
    }

    int len = sizeof(addr);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        closesocket(sock);
        throw std::runtime_error{"getsockname failed"};
    }
    closesocket(sock);
    return ntohs(addr.sin_port);
}

httplib::Result wait_get(uint16_t port, const char* path) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline) {
        httplib::Client client{"127.0.0.1", port};
        if (auto res = client.Get(path)) return res;
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    return {};
}

httplib::Result wait_post(uint16_t port, const char* path, const char* body) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline) {
        httplib::Client client{"127.0.0.1", port};
        if (auto res = client.Post(path, body, "application/json")) return res;
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    return {};
}

void require_json_error(const httplib::Result& res, int status, const char* message) {
    require(res, message);
    require(res->status == status, message);
    require(res->get_header_value("Access-Control-Allow-Origin") == "*",
            "error responses should include CORS");
    auto body = nlohmann::json::parse(res->body);
    require(body.contains("error") && body["error"].is_string(),
            "error responses should return JSON error");
}

} // namespace

int main() {
    Winsock winsock;
    const auto root = std::filesystem::current_path() / "tmp" / "http-server-routes-tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    try {
        fh6::AudioSourceManager mgr{1U << 20U};
        fh6::fmod_bridge::FMODFns fns{};
        fh6::fmod_bridge::DSPBridge bridge{mgr, fns};
        fh6::ConfigStore store{root / "config.toml", fh6::Config{}};
        const auto port = find_free_port();
        fh6::http::HttpServer server{mgr, bridge, store, port, {}};

        auto res = wait_get(port, "/api/source/roon/loopback-endpoints");
        require(res, "loopback endpoints route should respond");
        require(res->status == 200, "loopback endpoints route should return HTTP 200");

        auto body = nlohmann::json::parse(res->body);
        require(body.contains("devices") && body["devices"].is_array(),
                "loopback endpoints route should return a devices array");
        for (const auto& device : body["devices"]) {
            require(device.contains("id") && device["id"].is_string(),
                    "loopback endpoints should include string id");
            require(device.contains("name") && device["name"].is_string(),
                    "loopback endpoints should include string name");
            require(device.contains("is_default") && device["is_default"].is_boolean(),
                    "loopback endpoints should include boolean is_default");
            require(device.contains("kind") && device["kind"].is_string(),
                    "loopback endpoints should include classification kind");
            require(device.contains("recommendation") && device["recommendation"].is_string(),
                    "loopback endpoints should include recommendation");
            require(device.contains("warning") && device["warning"].is_string(),
                    "loopback endpoints should include warning");
        }

        auto legacy_res = wait_get(port, "/api/source/roon/capture-devices");
        require(legacy_res, "legacy capture devices route should respond");
        require(legacy_res->status == 200, "legacy capture devices route should return HTTP 200");
        auto legacy_body = nlohmann::json::parse(legacy_res->body);
        require(legacy_body.contains("devices") && legacy_body["devices"].is_array(),
                "legacy capture devices route should still return devices");

        auto setup_res = wait_get(port, "/api/source/roon/setup");
        require(setup_res, "Roon setup diagnostics route should respond");
        require(setup_res->status == 200, "Roon setup diagnostics route should return HTTP 200");
        auto setup_body = nlohmann::json::parse(setup_res->body);
        require(setup_body.contains("roon_environment") &&
                    setup_body["roon_environment"].is_string(),
                "Roon setup diagnostics should include Roon environment");
        require(setup_body.contains("cable_environment") &&
                    setup_body["cable_environment"].is_string(),
                "Roon setup diagnostics should include cable environment");
        require(setup_body.contains("recommended_endpoint") &&
                    setup_body["recommended_endpoint"].is_object(),
                "Roon setup diagnostics should include recommended endpoint");
        require(setup_body.contains("actions") && setup_body["actions"].is_array(),
                "Roon setup diagnostics should include actions");

        for (const char* path : {"/api/source/roon/status", "/api/source/roon/zones",
                                 "/api/source/roon/outputs",
                                 "/api/source/roon/artwork/current"}) {
            require_json_error(wait_get(port, path), 404,
                               "unregistered Roon GET route should return JSON 404");
        }
        for (const char* path : {"/api/source/roon/select-zone",
                                 "/api/source/roon/select-capture-device",
                                 "/api/source/roon/test-capture", "/api/source/roon/volume",
                                 "/api/source/roon/reconnect"}) {
            require_json_error(wait_post(port, path, "{}"), 404,
                               "unregistered Roon POST route should return JSON 404");
        }

        fh6::RoonConfig roon_cfg;
        roon_cfg.enabled = true;
        roon_cfg.auto_start_bridge = false;
        auto roon = std::make_unique<fh6::sources::RoonSource>(roon_cfg);
        require(roon->initialize(), "test Roon source should initialize");
        mgr.register_source(std::move(roon));

        require_json_error(wait_post(port, "/api/source/roon/select-zone", "{}"), 400,
                           "select-zone should reject missing zone_id");
        require_json_error(wait_post(port, "/api/source/roon/select-capture-device", "{}"), 400,
                           "select-capture-device should reject missing device_id");
        require_json_error(wait_post(port, "/api/source/roon/test-capture", "{}"), 400,
                           "test-capture should reject missing device_id");
        require_json_error(wait_post(port, "/api/source/roon/volume", R"({"output_id":"out"})"),
                           400, "volume should reject missing value");

        std::filesystem::remove_all(root);
    } catch (const std::exception& e) {
        std::filesystem::remove_all(root);
        std::cerr << e.what() << '\n';
        return 1;
    } catch (...) {
        std::filesystem::remove_all(root);
        std::cerr << "unknown failure\n";
        return 1;
    }

    return 0;
}
