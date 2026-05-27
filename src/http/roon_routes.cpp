#include "roon_routes.hpp"

#include "fh6/audio/endpoint_classifier.hpp"
#include "fh6/audio/wasapi_loopback_capture.hpp"
#include "fh6/audio_source_manager.hpp"
#include "fh6/config_store.hpp"
#include "fh6/log.hpp"
#include "fh6/roon/roon_control_client.hpp"
#include "fh6/roon/setup_diagnostics.hpp"

#define CPPHTTPLIB_NO_EXCEPTIONS
#include <httplib.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace fh6::http {
namespace {

using json = nlohmann::json;

bool is_route(std::string_view method, std::string_view path, std::string_view want_method,
              std::string_view want_path) {
    return method == want_method && path == want_path;
}

bool roon_registered(AudioSourceManager& mgr, const ErrorResponder& fail) {
    if (mgr.find("roon")) return true;
    fail(404, "roon not registered");
    return false;
}

void command_response(const JsonResponder& ok, const ErrorResponder& fail,
                      const roon::RoonCommandResult& result) {
    if (result.ok) {
        ok(json::object());
    } else {
        fail(result.http_status > 0 ? result.http_status : 502, result.error);
    }
}

json parse_body(std::string_view body) {
    if (body.empty()) return json::object();
    return json::parse(body.begin(), body.end(), nullptr, false);
}

std::string required_string(const json& body, const char* key) {
    if (auto it = body.find(key); it != body.end() && it->is_string()) {
        auto value = it->get<std::string>();
        if (!value.empty()) return value;
    }
    return {};
}

json loopback_devices_to_json() {
    json devices = json::array();
    for (const auto& device : audio::enumerate_render_devices()) {
        const auto classification = audio::classify_endpoint(device.name);
        devices.push_back(json{
            {"id", device.id},
            {"name", device.name},
            {"is_default", device.is_default},
            {"kind", classification.kind},
            {"recommendation", classification.recommendation},
            {"warning", classification.warning},
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

json setup_diagnostics_to_json(const roon::RoonSetupDiagnostics& d) {
    json issues = json::array();
    for (const auto& issue : d.issues) {
        issues.push_back(
            json{{"id", issue.id}, {"severity", issue.severity}, {"message", issue.message}});
    }
    json actions = json::array();
    for (const auto& action : d.actions) {
        actions.push_back(json{{"id", action.id}, {"label", action.label}, {"url", action.url}});
    }
    return json{
        {"roon_environment", d.roon_environment},
        {"cable_environment", d.cable_environment},
        {"node_available", d.node_available},
        {"recommended_endpoint",
         json{{"id", d.recommended_endpoint_id}, {"name", d.recommended_endpoint_name}}},
        {"issues", std::move(issues)},
        {"actions", std::move(actions)},
        {"official_urls",
         json{{"roon", "https://roon.app/en/downloads"},
              {"roon_bridge", "https://help.roonlabs.com/portal/en/kb/articles/roonbridge"},
              {"vb_hifi_cable", "https://vb-audio.com/Cable/"},
              {"node", "https://nodejs.org/"}}},
    };
}

audio::WasapiLoopbackCaptureStatus wait_for_capture_signal(audio::WasapiLoopbackCapture& capture) {
    audio::WasapiLoopbackCaptureStatus status;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{1500};
    do {
        status = capture.status();
        if (status.peak >= 0.01f || !status.running) return status;
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    } while (std::chrono::steady_clock::now() < deadline);
    return capture.status();
}

void proxy_artwork(const ErrorResponder& fail, const BodyResponder& send_body) {
    httplib::Client client{"127.0.0.1", 47821};
    client.set_connection_timeout(std::chrono::milliseconds{500});
    client.set_read_timeout(std::chrono::milliseconds{1000});
    auto sidecar = client.Get("/artwork/current");
    if (!sidecar || sidecar->status >= 400) {
        fail(sidecar ? sidecar->status : 502, "current artwork is unavailable");
        return;
    }
    auto type = sidecar->get_header_value("content-type");
    if (type.empty()) type = "application/octet-stream";
    send_body(sidecar->status, sidecar->body, type);
}

bool handle_select_zone(std::string_view body, ConfigStore& store, const JsonResponder& ok,
                        const ErrorResponder& fail) {
    auto parsed  = parse_body(body);
    auto zone_id = parsed.is_object() ? required_string(parsed, "zone_id") : std::string{};
    if (zone_id.empty()) {
        fail(400, "zone_id is required");
        return true;
    }
    roon::RoonControlClient client;
    auto result = client.select_zone(zone_id);
    if (!result.ok) {
        command_response(ok, fail, result);
        return true;
    }
    log::info("[roon] selected zone {}", zone_id);
    store.patch([&](Config& c) { c.roon.selected_zone_id = zone_id; });
    ok(json::object());
    return true;
}

bool handle_select_loopback(std::string_view body, ConfigStore& store, const JsonResponder& ok,
                            const ErrorResponder& fail, const char* id_key) {
    auto parsed      = parse_body(body);
    auto endpoint_id = parsed.is_object() ? required_string(parsed, id_key) : std::string{};
    if (endpoint_id.empty()) {
        fail(400, std::string_view{id_key} == "endpoint_id" ? "endpoint_id is required"
                                                            : "device_id is required");
        return true;
    }
    std::string name;
    if (auto it = parsed.find("name"); it != parsed.end() && it->is_string())
        name = it->get<std::string>();
    log::info("[roon] selected loopback endpoint id={} name={}", endpoint_id, name);
    store.patch([&](Config& c) {
        c.roon.render_loopback_endpoint_id   = endpoint_id;
        c.roon.render_loopback_endpoint_name = name;
    });
    ok(json::object());
    return true;
}

bool handle_test_capture(std::string_view body, ConfigStore& store, const JsonResponder& ok,
                         const ErrorResponder& fail) {
    auto parsed    = parse_body(body);
    auto device_id = parsed.is_object() ? required_string(parsed, "device_id") : std::string{};
    if (device_id.empty()) {
        fail(400, "device_id is required");
        return true;
    }
    audio::WasapiLoopbackCapture capture;
    audio::WasapiLoopbackCaptureConfig cfg;
    const auto roon_cfg = store.snapshot().roon;
    cfg.device_id       = device_id;
    cfg.latency_ms      = roon_cfg.latency_ms;
    cfg.queue_ms        = 250;
    auto endpoint_name  = roon_cfg.render_loopback_endpoint_id == device_id
                            ? roon_cfg.render_loopback_endpoint_name
                            : std::string{};
    if (endpoint_name.empty()) endpoint_name = "selected render endpoint";
    log::info("[roon] test capture requested device_id={}", device_id);
    if (!capture.start(cfg)) {
        fail(502, capture.status().error);
        return true;
    }
    auto status = wait_for_capture_signal(capture);
    capture.stop();
    log::info("[roon] test capture result peak={} queued_bytes={}", status.peak,
              status.queued_bytes);
    if (status.peak < 0.01f) {
        log::warn("[roon] capture device is silent during test-capture");
        const auto message = "Roon appears to be playing, but FH6 is receiving silence from the "
                             "selected render endpoint. Check that Roon is playing to " +
                             endpoint_name + " and that exclusive mode is disabled.";
        fail(409, message);
        return true;
    }
    ok(json{{"ok", true},
            {"peak", status.peak},
            {"queued_bytes", status.queued_bytes},
            {"message", "Audio detected on " + endpoint_name + " through WASAPI loopback."}});
    return true;
}

bool handle_volume(std::string_view body, const JsonResponder& ok, const ErrorResponder& fail) {
    auto parsed    = parse_body(body);
    auto output_id = parsed.is_object() ? required_string(parsed, "output_id") : std::string{};
    if (output_id.empty() || !parsed.contains("value") || !parsed["value"].is_number()) {
        fail(400, "output_id and numeric value are required");
        return true;
    }
    std::string how = "absolute";
    if (auto it = parsed.find("how"); it != parsed.end() && it->is_string())
        how = it->get<std::string>();
    roon::RoonControlClient client;
    command_response(ok, fail, client.set_volume(output_id, parsed["value"].get<double>(), how));
    return true;
}

} // namespace

bool dispatch_roon_route(std::string_view method, std::string_view path, std::string_view body,
                         AudioSourceManager& mgr, ConfigStore& store, const JsonResponder& ok,
                         const ErrorResponder& fail, const BodyResponder& send_body) {
    if (is_route(method, path, "GET", "/api/source/roon/loopback-endpoints") ||
        is_route(method, path, "GET", "/api/source/roon/capture-devices")) {
        ok(loopback_devices_to_json());
        return true;
    }
    if (is_route(method, path, "GET", "/api/source/roon/setup")) {
        ok(setup_diagnostics_to_json(
            roon::build_setup_diagnostics(roon::collect_setup_probe(store.snapshot().roon))));
        return true;
    }

    if (is_route(method, path, "GET", "/api/source/roon/status")) {
        if (!roon_registered(mgr, fail)) return true;
        roon::RoonControlClient client;
        ok(roon_status_to_json(client.status()));
        return true;
    }
    if (is_route(method, path, "GET", "/api/source/roon/zones")) {
        if (!roon_registered(mgr, fail)) return true;
        roon::RoonControlClient client;
        auto zones = client.zones();
        zones.empty() && !client.last_error().empty() ? fail(502, client.last_error())
                                                      : ok(roon_zones_to_json(zones));
        return true;
    }
    if (is_route(method, path, "GET", "/api/source/roon/outputs")) {
        if (!roon_registered(mgr, fail)) return true;
        roon::RoonControlClient client;
        auto outputs = client.outputs();
        outputs.empty() && !client.last_error().empty() ? fail(502, client.last_error())
                                                        : ok(roon_outputs_to_json(outputs));
        return true;
    }
    const bool known_post =
        is_route(method, path, "POST", "/api/source/roon/select-zone") ||
        is_route(method, path, "POST", "/api/source/roon/select-loopback-endpoint") ||
        is_route(method, path, "POST", "/api/source/roon/select-capture-device") ||
        is_route(method, path, "POST", "/api/source/roon/test-capture") ||
        is_route(method, path, "POST", "/api/source/roon/volume") ||
        is_route(method, path, "POST", "/api/source/roon/reconnect");
    const bool known_get = is_route(method, path, "GET", "/api/source/roon/artwork/current");
    if (!known_post && !known_get) return false;
    if (!roon_registered(mgr, fail)) return true;

    if (is_route(method, path, "POST", "/api/source/roon/select-zone"))
        return handle_select_zone(body, store, ok, fail);
    if (is_route(method, path, "POST", "/api/source/roon/select-loopback-endpoint"))
        return handle_select_loopback(body, store, ok, fail, "endpoint_id");
    if (is_route(method, path, "POST", "/api/source/roon/select-capture-device"))
        return handle_select_loopback(body, store, ok, fail, "device_id");
    if (is_route(method, path, "POST", "/api/source/roon/test-capture"))
        return handle_test_capture(body, store, ok, fail);
    if (is_route(method, path, "POST", "/api/source/roon/volume"))
        return handle_volume(body, ok, fail);
    if (is_route(method, path, "POST", "/api/source/roon/reconnect")) {
        roon::RoonControlClient client;
        log::info("[roon] reconnect requested");
        command_response(ok, fail, client.reconnect());
        return true;
    }
    if (is_route(method, path, "GET", "/api/source/roon/artwork/current")) {
        proxy_artwork(fail, send_body);
        return true;
    }
    return false;
}

} // namespace fh6::http
