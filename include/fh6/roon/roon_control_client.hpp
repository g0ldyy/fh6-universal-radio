#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fh6::roon {

struct RoonControlClientOptions {
    std::string host            = "127.0.0.1";
    uint16_t port               = 47821;
    uint32_t connect_timeout_ms = 500;
    uint32_t request_timeout_ms = 1000;
};

struct RoonCommandResult {
    bool ok         = false;
    int http_status = 0;
    std::string error;
};

struct RoonHealth {
    bool ok = false;
    std::string service;
    std::string error;
};

struct RoonNowPlaying {
    std::string title;
    std::string artist;
    std::string album;
    std::string artwork_url;
    uint64_t duration_ms = 0;
    uint64_t position_ms = 0;
};

struct RoonStatus {
    bool ok = false;
    std::string pairing_state;
    std::string core_id;
    std::string core_name;
    std::string selected_zone_id;
    std::string selected_zone_name;
    bool zone_available = false;
    std::string error;
    std::optional<RoonNowPlaying> now_playing;
};

struct RoonZoneInfo {
    std::string id;
    std::string display_name;
    std::string state;
};

struct RoonOutputInfo {
    std::string id;
    std::string zone_id;
    std::string display_name;
    bool has_volume     = false;
    double volume_value = 0.0;
};

class RoonControlClient {
public:
    explicit RoonControlClient(RoonControlClientOptions options = {});

    RoonHealth health() const;
    RoonStatus status();
    std::vector<RoonZoneInfo> zones() const;
    std::vector<RoonOutputInfo> outputs() const;

    RoonCommandResult select_zone(std::string_view zone_id) const;
    RoonCommandResult transport(std::string_view control, std::string_view zone_id = {}) const;
    RoonCommandResult set_volume(std::string_view output_id, double value,
                                 std::string_view how = "absolute") const;
    RoonCommandResult reconnect() const;

    std::optional<RoonStatus> last_status() const;
    std::string last_error() const;

private:
    struct Impl;

    RoonControlClientOptions options_;
    mutable std::mutex state_mutex_;
    mutable std::optional<RoonStatus> last_status_;
    mutable std::string last_error_;
};

} // namespace fh6::roon
