#include "fh6/roon/roon_control_client.hpp"
#include "fh6/log.hpp"

#define CPPHTTPLIB_NO_EXCEPTIONS
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

namespace fh6::roon {

namespace {

using json = nlohmann::json;

std::string get_string(const json& j, const char* key) {
    if (auto it = j.find(key); it != j.end() && it->is_string()) return it->get<std::string>();
    return {};
}

uint64_t seconds_to_ms(const json& j, const char* key) {
    if (auto it = j.find(key); it != j.end() && it->is_number()) {
        const auto seconds = std::max(0.0, it->get<double>());
        return static_cast<uint64_t>(std::lround(seconds * 1000.0));
    }
    return 0;
}

std::optional<RoonNowPlaying> parse_now_playing(const json& body) {
    auto it = body.find("now_playing");
    if (it == body.end() || !it->is_object()) return std::nullopt;

    RoonNowPlaying out;
    if (auto lines = it->find("three_line"); lines != it->end() && lines->is_object()) {
        out.title  = get_string(*lines, "line1");
        out.artist = get_string(*lines, "line2");
        out.album  = get_string(*lines, "line3");
    }
    if (out.title.empty()) out.title = get_string(*it, "title");
    if (out.artist.empty()) out.artist = get_string(*it, "artist");
    if (out.album.empty()) out.album = get_string(*it, "album");
    out.position_ms = seconds_to_ms(*it, "seek_position");
    out.duration_ms = seconds_to_ms(*it, "length");
    if (!get_string(*it, "image_key").empty()) {
        out.artwork_url = "/api/source/roon/artwork/current";
    }
    return out;
}

json parse_body(const httplib::Result& res) {
    if (!res || res->body.empty()) return json::object();
    return json::parse(res->body, nullptr, false);
}

std::string error_from_json(const json& j, std::string fallback) {
    if (j.is_object()) {
        if (auto it = j.find("error"); it != j.end() && it->is_string())
            return it->get<std::string>();
    }
    return fallback;
}

RoonCommandResult command_error(int status, std::string error) {
    return RoonCommandResult{false, status, std::move(error)};
}

} // namespace

RoonControlClient::RoonControlClient(RoonControlClientOptions options)
    : options_{std::move(options)} {}

namespace {

httplib::Client make_client(const RoonControlClientOptions& options) {
    httplib::Client client{options.host, options.port};
    client.set_connection_timeout(std::chrono::milliseconds{options.connect_timeout_ms});
    client.set_read_timeout(std::chrono::milliseconds{options.request_timeout_ms});
    client.set_write_timeout(std::chrono::milliseconds{options.request_timeout_ms});
    return client;
}

} // namespace

RoonHealth RoonControlClient::health() const {
    auto client = make_client(options_);
    auto res    = client.Get("/health");
    auto body   = parse_body(res);
    if (!res || res->status >= 400 || body.is_discarded()) {
        auto error = error_from_json(body, "sidecar health request failed");
        log::warn("[roon] sidecar health failed: {}", error);
        last_error_ = error;
        return RoonHealth{false, {}, std::move(error)};
    }
    last_error_.clear();
    return RoonHealth{body.value("ok", false), get_string(body, "service"), {}};
}

RoonStatus RoonControlClient::status() {
    auto client = make_client(options_);
    auto res    = client.Get("/status");
    auto body   = parse_body(res);
    if (!res || res->status >= 400 || body.is_discarded()) {
        auto error = error_from_json(body, "sidecar status request failed");
        log::warn("[roon] sidecar status failed: {}", error);
        last_error_ = error;
        RoonStatus out;
        out.error = std::move(error);
        return out;
    }

    RoonStatus out;
    out.ok            = true;
    out.pairing_state = get_string(body, "pairing_state");
    if (auto core = body.find("core"); core != body.end() && core->is_object()) {
        out.core_id   = get_string(*core, "id");
        out.core_name = get_string(*core, "name");
    }
    out.selected_zone_id   = get_string(body, "selected_zone_id");
    out.selected_zone_name = get_string(body, "selected_zone_name");
    out.error              = get_string(body, "error");
    out.now_playing        = parse_now_playing(body);
    last_status_           = out;
    last_error_            = out.error;
    return out;
}

std::vector<RoonZoneInfo> RoonControlClient::zones() const {
    auto client = make_client(options_);
    auto res    = client.Get("/zones");
    auto body   = parse_body(res);
    if (!res || res->status >= 400 || body.is_discarded()) {
        auto error = error_from_json(body, "sidecar zones request failed");
        log::warn("[roon] sidecar zones failed: {}", error);
        last_error_ = error;
        return {};
    }
    std::vector<RoonZoneInfo> out;
    if (auto zones_json = body.find("zones"); zones_json != body.end() && zones_json->is_array()) {
        out.reserve(zones_json->size());
        for (const auto& item : *zones_json) {
            out.push_back({get_string(item, "zone_id"), get_string(item, "display_name"),
                           get_string(item, "state")});
        }
    }
    last_error_.clear();
    return out;
}

std::vector<RoonOutputInfo> RoonControlClient::outputs() const {
    auto client = make_client(options_);
    auto res    = client.Get("/outputs");
    auto body   = parse_body(res);
    if (!res || res->status >= 400 || body.is_discarded()) {
        auto error = error_from_json(body, "sidecar outputs request failed");
        log::warn("[roon] sidecar outputs failed: {}", error);
        last_error_ = error;
        return {};
    }
    std::vector<RoonOutputInfo> out;
    if (auto outputs_json = body.find("outputs");
        outputs_json != body.end() && outputs_json->is_array()) {
        out.reserve(outputs_json->size());
        for (const auto& item : *outputs_json) {
            RoonOutputInfo info;
            info.id           = get_string(item, "output_id");
            info.zone_id      = get_string(item, "zone_id");
            info.display_name = get_string(item, "display_name");
            if (auto volume = item.find("volume"); volume != item.end() && volume->is_object()) {
                if (auto value = volume->find("value");
                    value != volume->end() && value->is_number()) {
                    info.has_volume   = true;
                    info.volume_value = value->get<double>();
                }
            }
            out.push_back(std::move(info));
        }
    }
    last_error_.clear();
    return out;
}

RoonCommandResult RoonControlClient::select_zone(std::string_view zone_id) const {
    json request{{"zone_id", std::string{zone_id}}};
    auto client = make_client(options_);
    auto res    = client.Post("/select-zone", request.dump(), "application/json");
    auto body   = parse_body(res);
    if (!res || res->status >= 400 || body.is_discarded()) {
        auto status = res ? res->status : 0;
        auto error  = error_from_json(body, "sidecar select-zone request failed");
        log::warn("[roon] sidecar select-zone failed: {}", error);
        last_error_ = error;
        return command_error(status, std::move(error));
    }
    last_error_.clear();
    return RoonCommandResult{true, res->status, {}};
}

RoonCommandResult RoonControlClient::transport(std::string_view control,
                                               std::string_view zone_id) const {
    json request{{"control", std::string{control}}};
    if (!zone_id.empty()) request["zone_id"] = std::string{zone_id};
    auto client = make_client(options_);
    auto res    = client.Post("/transport", request.dump(), "application/json");
    auto body   = parse_body(res);
    if (!res || res->status >= 400 || body.is_discarded()) {
        auto status = res ? res->status : 0;
        auto error  = error_from_json(body, "sidecar transport request failed");
        log::warn("[roon] sidecar transport failed: {}", error);
        last_error_ = error;
        return command_error(status, std::move(error));
    }
    last_error_.clear();
    return RoonCommandResult{true, res->status, {}};
}

RoonCommandResult RoonControlClient::set_volume(std::string_view output_id, double value,
                                                std::string_view how) const {
    json request{
        {"output_id", std::string{output_id}}, {"how", std::string{how}}, {"value", value}};
    auto client = make_client(options_);
    auto res    = client.Post("/volume", request.dump(), "application/json");
    auto body   = parse_body(res);
    if (!res || res->status >= 400 || body.is_discarded()) {
        auto status = res ? res->status : 0;
        auto error  = error_from_json(body, "sidecar volume request failed");
        log::warn("[roon] sidecar volume failed: {}", error);
        last_error_ = error;
        return command_error(status, std::move(error));
    }
    last_error_.clear();
    return RoonCommandResult{true, res->status, {}};
}

RoonCommandResult RoonControlClient::reconnect() const {
    auto client = make_client(options_);
    auto res    = client.Post("/reconnect", "{}", "application/json");
    auto body   = parse_body(res);
    if (!res || res->status >= 400 || body.is_discarded()) {
        auto status = res ? res->status : 0;
        auto error  = error_from_json(body, "sidecar reconnect request failed");
        log::warn("[roon] sidecar reconnect failed: {}", error);
        last_error_ = error;
        return command_error(status, std::move(error));
    }
    last_error_.clear();
    return RoonCommandResult{true, res->status, {}};
}

std::optional<RoonStatus> RoonControlClient::last_status() const { return last_status_; }

std::string RoonControlClient::last_error() const { return last_error_; }

} // namespace fh6::roon
