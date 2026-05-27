#include "roon_routes.hpp"

#include "fh6/audio/wasapi_loopback_capture.hpp"
#include "fh6/audio_source_manager.hpp"
#include "fh6/config_store.hpp"
#include "fh6/roon/roon_control_client.hpp"

#define CPPHTTPLIB_NO_EXCEPTIONS
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <string_view>

namespace fh6::http {
namespace {

using json = nlohmann::json;

void cors(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
}

void ok(httplib::Response& res, const json& body = json::object()) {
    res.set_content(body.empty() ? std::string{R"({"ok":true})"} : body.dump(), "application/json");
}

void fail(httplib::Response& res, int status, std::string_view msg) {
    res.status = status;
    res.set_content(json{{"error", std::string{msg}}}.dump(), "application/json");
}

bool roon_registered(AudioSourceManager& mgr, httplib::Response& res) {
    if (mgr.find("roon")) return true;
    fail(res, 404, "roon not registered");
    return false;
}

void command_response(httplib::Response& res, const roon::RoonCommandResult& result) {
    if (result.ok) {
        ok(res);
    } else {
        fail(res, result.http_status > 0 ? result.http_status : 502, result.error);
    }
}

std::string required_string(const json& body, const char* key) {
    if (auto it = body.find(key); it != body.end() && it->is_string()) {
        auto value = it->get<std::string>();
        if (!value.empty()) return value;
    }
    return {};
}

json capture_devices_to_json() {
    json devices = json::array();
    for (const auto& device : audio::enumerate_render_devices()) {
        devices.push_back(json{
            {"id", device.id},
            {"name", device.name},
            {"is_default", device.is_default},
        });
    }
    return json{{"devices", std::move(devices)}};
}

json roon_status_to_json(const roon::RoonStatus& s) {
    json out{{"ok", s.ok},
             {"pairing_state", s.pairing_state},
             {"core", json{{"id", s.core_id}, {"name", s.core_name}}},
             {"selected_zone_id", s.selected_zone_id},
             {"selected_zone_name", s.selected_zone_name},
             {"error", s.error}};
    if (s.now_playing) {
        out["now_playing"] = json{{"title", s.now_playing->title},
                                  {"artist", s.now_playing->artist},
                                  {"album", s.now_playing->album},
                                  {"artwork_url", s.now_playing->artwork_url},
                                  {"duration_ms", s.now_playing->duration_ms},
                                  {"position_ms", s.now_playing->position_ms}};
    } else {
        out["now_playing"] = nullptr;
    }
    return out;
}

json roon_zones_to_json(const std::vector<roon::RoonZoneInfo>& zones) {
    json out = json::array();
    for (const auto& z : zones)
        out.push_back(json{{"id", z.id}, {"display_name", z.display_name}, {"state", z.state}});
    return json{{"zones", std::move(out)}};
}

json roon_outputs_to_json(const std::vector<roon::RoonOutputInfo>& outputs) {
    json out = json::array();
    for (const auto& o : outputs) {
        json item{{"id", o.id}, {"zone_id", o.zone_id}, {"display_name", o.display_name}};
        if (o.has_volume) item["volume"] = json{{"value", o.volume_value}};
        out.push_back(std::move(item));
    }
    return json{{"outputs", std::move(out)}};
}

void proxy_artwork(httplib::Response& res) {
    httplib::Client client{"127.0.0.1", 47821};
    client.set_connection_timeout(std::chrono::milliseconds{500});
    client.set_read_timeout(std::chrono::milliseconds{1000});
    auto sidecar = client.Get("/artwork/current");
    if (!sidecar || sidecar->status >= 400) {
        fail(res, sidecar ? sidecar->status : 502, "current artwork is unavailable");
        return;
    }
    res.set_content(sidecar->body, sidecar->get_header_value("content-type"));
}

} // namespace

void wire_roon_routes(httplib::Server& svr, AudioSourceManager& mgr, ConfigStore& store) {
    svr.Get("/api/source/roon/capture-devices",
            [](const httplib::Request&, httplib::Response& res) {
                cors(res);
                ok(res, capture_devices_to_json());
            });
    svr.Get("/api/source/roon/status", [&](const httplib::Request&, httplib::Response& res) {
        cors(res);
        if (!roon_registered(mgr, res)) return;
        roon::RoonControlClient client;
        ok(res, roon_status_to_json(client.status()));
    });
    svr.Get("/api/source/roon/zones", [&](const httplib::Request&, httplib::Response& res) {
        cors(res);
        if (!roon_registered(mgr, res)) return;
        roon::RoonControlClient client;
        auto zones = client.zones();
        if (zones.empty() && !client.last_error().empty()) {
            fail(res, 502, client.last_error());
        } else {
            ok(res, roon_zones_to_json(zones));
        }
    });
    svr.Get("/api/source/roon/outputs", [&](const httplib::Request&, httplib::Response& res) {
        cors(res);
        if (!roon_registered(mgr, res)) return;
        roon::RoonControlClient client;
        auto outputs = client.outputs();
        if (outputs.empty() && !client.last_error().empty()) {
            fail(res, 502, client.last_error());
        } else {
            ok(res, roon_outputs_to_json(outputs));
        }
    });
    svr.Post("/api/source/roon/select-zone",
             [&](const httplib::Request& req, httplib::Response& res) {
                 cors(res);
                 if (!roon_registered(mgr, res)) return;
                 auto body    = json::parse(req.body, nullptr, false);
                 auto zone_id = body.is_object() ? required_string(body, "zone_id") : std::string{};
                 if (zone_id.empty()) {
                     fail(res, 400, "zone_id is required");
                     return;
                 }
                 roon::RoonControlClient client;
                 auto result = client.select_zone(zone_id);
                 if (!result.ok) {
                     command_response(res, result);
                     return;
                 }
                 store.patch([&](Config& c) { c.roon.selected_zone_id = zone_id; });
                 ok(res);
             });
    svr.Post("/api/source/roon/select-capture-device", [&](const httplib::Request& req,
                                                           httplib::Response& res) {
        cors(res);
        if (!roon_registered(mgr, res)) return;
        auto body      = json::parse(req.body, nullptr, false);
        auto device_id = body.is_object() ? required_string(body, "device_id") : std::string{};
        if (device_id.empty()) {
            fail(res, 400, "device_id is required");
            return;
        }
        std::string name;
        if (auto it = body.find("name"); it != body.end() && it->is_string())
            name = it->get<std::string>();
        store.patch([&](Config& c) {
            c.roon.capture_device_id   = device_id;
            c.roon.capture_device_name = name;
        });
        ok(res);
    });
    svr.Post("/api/source/roon/test-capture", [&](const httplib::Request& req,
                                                  httplib::Response& res) {
        cors(res);
        if (!roon_registered(mgr, res)) return;
        auto body      = json::parse(req.body, nullptr, false);
        auto device_id = body.is_object() ? required_string(body, "device_id") : std::string{};
        if (device_id.empty()) {
            fail(res, 400, "device_id is required");
            return;
        }
        audio::WasapiLoopbackCapture capture;
        audio::WasapiLoopbackCaptureConfig cfg;
        cfg.device_id  = device_id;
        cfg.latency_ms = store.snapshot().roon.latency_ms;
        cfg.queue_ms   = 250;
        if (!capture.start(cfg)) {
            fail(res, 502, capture.status().error);
            return;
        }
        auto status = capture.status();
        capture.stop();
        ok(res, json{{"ok", true}, {"peak", status.peak}, {"queued_bytes", status.queued_bytes}});
    });
    svr.Post("/api/source/roon/volume", [&](const httplib::Request& req, httplib::Response& res) {
        cors(res);
        if (!roon_registered(mgr, res)) return;
        auto body      = json::parse(req.body, nullptr, false);
        auto output_id = body.is_object() ? required_string(body, "output_id") : std::string{};
        if (output_id.empty() || !body.contains("value") || !body["value"].is_number()) {
            fail(res, 400, "output_id and numeric value are required");
            return;
        }
        std::string how = "absolute";
        if (auto it = body.find("how"); it != body.end() && it->is_string())
            how = it->get<std::string>();
        roon::RoonControlClient client;
        command_response(res, client.set_volume(output_id, body["value"].get<double>(), how));
    });
    svr.Post("/api/source/roon/reconnect", [&](const httplib::Request&, httplib::Response& res) {
        cors(res);
        if (!roon_registered(mgr, res)) return;
        roon::RoonControlClient client;
        command_response(res, client.reconnect());
    });
    svr.Get("/api/source/roon/artwork/current",
            [&](const httplib::Request&, httplib::Response& res) {
                cors(res);
                if (!roon_registered(mgr, res)) return;
                proxy_artwork(res);
            });
}

} // namespace fh6::http
