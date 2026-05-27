#include "fh6/roon/roon_control_client.hpp"

#define CPPHTTPLIB_NO_EXCEPTIONS
#include <httplib.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using json = nlohmann::json;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error{message};
}

class FakeSidecar {
public:
    FakeSidecar() {
        server_.Get("/health", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(json{{"ok", true}, {"service", "fh6-roon-bridge"}}.dump(),
                            "application/json");
        });
        server_.Get("/status", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(json{{"core", {{"id", "core-1"}, {"name", "Roon Server"}}},
                                 {"pairing_state", "authorized"},
                                 {"selected_zone_id", "zone-1"},
                                 {"selected_zone_name", "Main Room"},
                                 {"now_playing",
                                  {{"three_line",
                                    {{"line1", "Road Song"},
                                     {"line2", "The Drivers"},
                                     {"line3", "Horizon Radio"}}},
                                   {"seek_position", 67.0},
                                   {"length", 245.0},
                                   {"image_key", "image-1"}}},
                                 {"error", ""}}
                                .dump(),
                            "application/json");
        });
        server_.Get("/zones", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(json{{"zones",
                                  {{{"zone_id", "zone-1"},
                                    {"display_name", "Main Room"},
                                    {"state", "paused"}}}}}
                                .dump(),
                            "application/json");
        });
        server_.Get("/outputs", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(json{{"outputs",
                                  {{{"output_id", "output-1"},
                                    {"zone_id", "zone-1"},
                                    {"display_name", "Speakers"},
                                    {"volume", {{"value", 35.0}}}}}}}
                                .dump(),
                            "application/json");
        });
        server_.Post("/select-zone", [this](const httplib::Request& req, httplib::Response& res) {
            last_select_zone_body = req.body;
            res.set_content(json{{"selected_zone_id", "zone-1"}}.dump(), "application/json");
        });
        server_.Post("/transport", [this](const httplib::Request& req, httplib::Response& res) {
            last_transport_body = req.body;
            res.set_content(json{{"ok", true}}.dump(), "application/json");
        });
        server_.Post("/volume", [this](const httplib::Request& req, httplib::Response& res) {
            last_volume_body = req.body;
            res.set_content(json{{"ok", true}}.dump(), "application/json");
        });
        server_.Post("/reconnect", [this](const httplib::Request&, httplib::Response& res) {
            reconnect_count += 1;
            res.set_content(json{{"ok", true}}.dump(), "application/json");
        });
        server_.Get("/broken", [](const httplib::Request&, httplib::Response& res) {
            res.status = 500;
            res.set_content(json{{"error", "sidecar failed"}}.dump(), "application/json");
        });

        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) throw std::runtime_error{"failed to bind fake sidecar"};
        thread_ = std::thread([this] { server_.listen_after_bind(); });
    }

    ~FakeSidecar() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    int port() const noexcept { return port_; }

    std::string last_select_zone_body;
    std::string last_transport_body;
    std::string last_volume_body;
    int reconnect_count = 0;

private:
    httplib::Server server_;
    int port_ = 0;
    std::thread thread_;
};

} // namespace

int main() {
    FakeSidecar sidecar;

    fh6::roon::RoonControlClientOptions options;
    options.host = "127.0.0.1";
    options.port = static_cast<uint16_t>(sidecar.port());

    require(options.connect_timeout_ms <= 500, "default connect timeout should be short");
    require(options.request_timeout_ms <= 1000, "default request timeout should be short");

    fh6::roon::RoonControlClient client{options};

    const auto health = client.health();
    require(health.ok, "health should parse ok response");
    require(health.service == "fh6-roon-bridge", "health should parse service name");

    const auto status = client.status();
    require(status.ok, "status should parse ok response");
    require(status.core_id == "core-1", "status should parse core id");
    require(status.selected_zone_id == "zone-1", "status should parse selected zone");
    require(status.now_playing.has_value(), "status should parse now playing");
    require(status.now_playing->title == "Road Song", "status should parse now playing title");
    require(status.now_playing->artist == "The Drivers", "status should parse now playing artist");
    require(status.now_playing->album == "Horizon Radio", "status should parse now playing album");
    require(status.now_playing->position_ms == 67000, "status should convert seek seconds to ms");
    require(status.now_playing->duration_ms == 245000, "status should convert length seconds to ms");
    require(status.now_playing->artwork_url == "/api/source/roon/artwork/current",
            "status should expose dashboard artwork URL when image is available");

    const auto cached = client.last_status();
    require(cached.has_value(), "status should cache the last successful status");
    require(cached->selected_zone_name == "Main Room", "cached status should keep selected zone name");

    const auto zones = client.zones();
    require(zones.size() == 1, "zones should parse one zone");
    require(zones[0].id == "zone-1", "zones should parse zone id");
    require(zones[0].display_name == "Main Room", "zones should parse display name");

    const auto outputs = client.outputs();
    require(outputs.size() == 1, "outputs should parse one output");
    require(outputs[0].id == "output-1", "outputs should parse output id");
    require(outputs[0].volume_value == 35.0, "outputs should parse volume value");

    require(client.select_zone("zone-1").ok, "select_zone should return ok");
    require(json::parse(sidecar.last_select_zone_body)["zone_id"] == "zone-1",
            "select_zone should post zone_id");

    require(client.transport("playpause", "zone-1").ok, "transport should return ok");
    auto transport_body = json::parse(sidecar.last_transport_body);
    require(transport_body["control"] == "playpause", "transport should post control");
    require(transport_body["zone_id"] == "zone-1", "transport should post zone_id");

    require(client.set_volume("output-1", 42.0).ok, "set_volume should return ok");
    auto volume_body = json::parse(sidecar.last_volume_body);
    require(volume_body["output_id"] == "output-1", "set_volume should post output_id");
    require(volume_body["how"] == "absolute", "set_volume should default to absolute volume");
    require(volume_body["value"] == 42.0, "set_volume should post value");

    require(client.reconnect().ok, "reconnect should return ok");
    require(sidecar.reconnect_count == 1, "reconnect should call sidecar once");

    fh6::roon::RoonControlClientOptions bad_options;
    bad_options.host = "127.0.0.1";
    bad_options.port = 9;
    fh6::roon::RoonControlClient bad_client{bad_options};
    require(!bad_client.health().ok, "connection failures should return a typed error");

    return 0;
}
