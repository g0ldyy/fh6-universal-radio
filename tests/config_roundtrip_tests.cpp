#include "fh6/config.hpp"
#include "fh6/http/config_json.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error{message};
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) throw std::runtime_error{"failed to open test file for write"};
    out << text;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) throw std::runtime_error{"failed to open test file for read"};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

void require_roon_defaults(const fh6::RoonConfig& roon) {
    require(!roon.enabled, "roon defaults to disabled");
    require(roon.node_path.empty(), "node_path defaults empty");
    require(roon.bridge_path == (std::filesystem::path{"tools"} / "roon-bridge" / "index.mjs"),
            "bridge_path defaults to sidecar path");
    require(roon.selected_core_id.empty(), "selected_core_id defaults empty");
    require(roon.selected_zone_id.empty(), "selected_zone_id defaults empty");
    require(roon.selected_output_id.empty(), "selected_output_id defaults empty");
    require(roon.render_loopback_endpoint_id.empty(), "render loopback endpoint id defaults empty");
    require(roon.render_loopback_endpoint_name.empty(),
            "render loopback endpoint name defaults empty");
    require(roon.control_volume, "control_volume defaults true");
    require(roon.auto_start_bridge, "auto_start_bridge defaults true");
    require(roon.auto_reconnect, "auto_reconnect defaults true");
    require(roon.latency_ms == 250, "latency_ms defaults 250");
    require(roon.metadata_poll_ms == 750, "metadata_poll_ms defaults 750");
}

void require_audio_defaults(const fh6::AudioConfig& audio) {
    require(audio.output_gain == 1.0f, "output_gain defaults 1.0");
    require(!audio.allow_volume_over_100, "boosted volume defaults disabled");
}

} // namespace

int main() {
    const auto root = std::filesystem::current_path() / "tmp" / "config-roundtrip-tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    try {
        fh6::Config defaults;
        require_roon_defaults(defaults.roon);
        require_audio_defaults(defaults.audio);

        const auto old_config_path = root / "old-config.toml";
        write_text(old_config_path, R"toml(
[general]
port = 8420

[local_files]
enabled = true
music_dir = 'C:\Music'
)toml");
        auto old_cfg = fh6::load_config(old_config_path);
        require_roon_defaults(old_cfg.roon);
        require_audio_defaults(old_cfg.audio);

        const auto roon_config_path = root / "roon-config.toml";
        write_text(roon_config_path, R"toml(
[roon]
enabled = true
node_path = 'C:\Program Files\nodejs\node.exe'
bridge_path = 'tools\roon-bridge\index.mjs'
selected_core_id = 'core-1'
selected_zone_id = 'zone-1'
selected_output_id = 'output-1'
render_loopback_endpoint_id = 'new-render-id'
render_loopback_endpoint_name = 'Hi-Fi Cable Input'
capture_device_id = 'legacy-device'
capture_device_name = 'Legacy Speakers'
control_volume = false
auto_start_bridge = false
auto_reconnect = false
latency_ms = 125
metadata_poll_ms = 900
)toml");
        auto roon_cfg = fh6::load_config(roon_config_path);
        require(roon_cfg.roon.enabled, "roon enabled parses");
        require(roon_cfg.roon.node_path == "C:\\Program Files\\nodejs\\node.exe",
                "node_path parses");
        require(roon_cfg.roon.selected_zone_id == "zone-1", "selected_zone_id parses");
        require(roon_cfg.roon.render_loopback_endpoint_id == "new-render-id",
                "new render loopback endpoint id parses");
        require(roon_cfg.roon.render_loopback_endpoint_name == "Hi-Fi Cable Input",
                "new render loopback endpoint name parses");
        require(!roon_cfg.roon.control_volume, "control_volume parses");
        require(!roon_cfg.roon.auto_start_bridge, "auto_start_bridge parses");
        require(!roon_cfg.roon.auto_reconnect, "auto_reconnect parses");
        require(roon_cfg.roon.latency_ms == 125, "latency_ms parses");
        require(roon_cfg.roon.metadata_poll_ms == 900, "metadata_poll_ms parses");

        const auto legacy_roon_config_path = root / "legacy-roon-config.toml";
        write_text(legacy_roon_config_path, R"toml(
[roon]
enabled = true
capture_device_id = 'legacy-render-id'
capture_device_name = 'Legacy Render'
)toml");
        auto legacy_roon_cfg = fh6::load_config(legacy_roon_config_path);
        require(legacy_roon_cfg.roon.render_loopback_endpoint_id == "legacy-render-id",
                "legacy capture_device_id migrates to render loopback endpoint id");
        require(legacy_roon_cfg.roon.render_loopback_endpoint_name == "Legacy Render",
                "legacy capture_device_name migrates to render loopback endpoint name");

        const auto invalid_config_path = root / "invalid-config.toml";
        write_text(invalid_config_path, R"toml(
[roon]
enabled = 'yes'
latency_ms = -5
metadata_poll_ms = 'fast'
)toml");
        auto invalid_cfg = fh6::load_config(invalid_config_path);
        require_roon_defaults(invalid_cfg.roon);

        const auto saved_path = root / "saved.toml";
        fh6::save_config(saved_path, roon_cfg);
        auto saved_text = read_text(saved_path);
        require(saved_text.find("[roon]") != std::string::npos, "save_config emits roon section");
        require(saved_text.find("render_loopback_endpoint_id") != std::string::npos,
                "save_config emits render loopback endpoint id");
        require(saved_text.find("render_loopback_endpoint_name") != std::string::npos,
                "save_config emits render loopback endpoint name");
        require(saved_text.find("capture_device_id") == std::string::npos,
                "save_config does not emit legacy capture device id");
        auto saved_cfg = fh6::load_config(saved_path);
        require(saved_cfg.roon.enabled, "saved roon enabled round-trips");
        require(saved_cfg.roon.render_loopback_endpoint_id == "new-render-id",
                "saved render loopback endpoint id round-trips");
        require(saved_cfg.roon.latency_ms == 125, "saved latency round-trips");
        require(saved_cfg.roon.metadata_poll_ms == 900, "saved metadata poll round-trips");

        const auto audio_config_path = root / "audio-config.toml";
        write_text(audio_config_path, R"toml(
[audio]
output_gain = 1.75
allow_volume_over_100 = true
)toml");
        auto audio_cfg = fh6::load_config(audio_config_path);
        require(audio_cfg.audio.output_gain == 1.75f, "boosted output_gain parses");
        require(audio_cfg.audio.allow_volume_over_100, "boosted volume setting parses");
        fh6::save_config(saved_path, audio_cfg);
        saved_text = read_text(saved_path);
        require(saved_text.find("allow_volume_over_100 = true") != std::string::npos,
                "save_config emits boosted volume setting");

        auto as_json = fh6::http::config_to_json(roon_cfg);
        require(as_json["roon"]["enabled"] == true, "config_to_json emits roon enabled");
        require(as_json["roon"]["selected_zone_id"] == "zone-1",
                "config_to_json emits selected_zone_id");
        require(as_json["roon"]["render_loopback_endpoint_id"] == "new-render-id",
                "config_to_json emits render loopback endpoint id");
        require(as_json["roon"]["latency_ms"] == 125, "config_to_json emits latency");
        auto audio_json = fh6::http::config_to_json(audio_cfg);
        require(audio_json["audio"]["allow_volume_over_100"] == true,
                "config_to_json emits boosted volume setting");

        fh6::Config patched;
        fh6::http::apply_config_patch(
            patched, nlohmann::json{{"roon",
                                     {{"enabled", true},
                                      {"selected_zone_id", "zone-2"},
                                      {"render_loopback_endpoint_id", "device-2"},
                                      {"render_loopback_endpoint_name", "Hi-Fi Cable Input"},
                                      {"latency_ms", 333}}}});
        require(patched.roon.enabled, "apply_config_patch updates roon enabled");
        require(patched.roon.selected_zone_id == "zone-2",
                "apply_config_patch updates selected zone");
        require(patched.roon.render_loopback_endpoint_id == "device-2",
                "apply_config_patch updates render loopback endpoint");
        require(patched.roon.render_loopback_endpoint_name == "Hi-Fi Cable Input",
                "apply_config_patch updates render loopback endpoint name");
        require(patched.roon.latency_ms == 333, "apply_config_patch updates latency");
        require(patched.roon.metadata_poll_ms == 750, "partial roon patch keeps fallback");

        patched.roon.node_path   = "C:\\Program Files\\nodejs\\node.exe";
        patched.roon.bridge_path = std::filesystem::path{"tools"} / "roon-bridge" / "index.mjs";
        patched.roon.auto_start_bridge = false;
        fh6::http::apply_config_patch(
            patched, nlohmann::json{{"roon",
                                     {{"node_path", "C:\\Temp\\not-node.exe"},
                                      {"bridge_path", "\\\\attacker\\share\\sidecar.mjs"},
                                      {"auto_start_bridge", true}}}});
        require(patched.roon.node_path == "C:\\Program Files\\nodejs\\node.exe",
                "api patch must not update Roon node executable path");
        require(patched.roon.bridge_path ==
                    (std::filesystem::path{"tools"} / "roon-bridge" / "index.mjs"),
                "api patch must not update Roon sidecar script path");
        require(!patched.roon.auto_start_bridge,
                "api patch must not update Roon sidecar auto-start");

        fh6::http::apply_config_patch(
            patched,
            nlohmann::json{{"audio", {{"output_gain", 1.8}, {"allow_volume_over_100", true}}}});
        require(patched.audio.output_gain == 1.8f, "apply_config_patch updates boosted gain");
        require(patched.audio.allow_volume_over_100,
                "apply_config_patch updates boosted volume setting");

        fh6::http::apply_config_patch(
            patched, nlohmann::json{{"roon",
                                     {{"capture_device_id", "legacy-device-2"},
                                      {"capture_device_name", "Legacy Device 2"}}}});
        require(patched.roon.render_loopback_endpoint_id == "legacy-device-2",
                "legacy JSON capture_device_id updates render loopback endpoint");
        require(patched.roon.render_loopback_endpoint_name == "Legacy Device 2",
                "legacy JSON capture_device_name updates render loopback endpoint name");

        fh6::http::apply_config_patch(
            patched, nlohmann::json{{"roon", {{"latency_ms", -1}, {"metadata_poll_ms", "fast"}}}});
        require(patched.roon.latency_ms == 333, "invalid patch latency keeps previous value");
        require(patched.roon.metadata_poll_ms == 750,
                "invalid patch metadata poll keeps previous value");

        std::filesystem::remove_all(root);
    } catch (const std::exception& e) {
        std::filesystem::remove_all(root);
        std::cerr << e.what() << '\n';
        return 1;
    } catch (...) {
        std::filesystem::remove_all(root);
        std::cerr << "unknown config_roundtrip_tests failure\n";
        return 1;
    }

    std::cout << "config_roundtrip_tests passed\n";
    return 0;
}
