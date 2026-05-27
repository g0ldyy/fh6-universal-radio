#include "fh6/roon/setup_diagnostics.hpp"

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error{message};
}

bool has_action(const fh6::roon::RoonSetupDiagnostics& report, std::string_view id) {
    for (const auto& action : report.actions)
        if (action.id == id) return true;
    return false;
}

} // namespace

int main() {
    try {
        fh6::roon::SetupProbeSnapshot local;
        local.processes = {"RoonServer.exe"};
        local.render_endpoints.push_back({"id-hifi", "Hi-Fi Cable Input", false});
        local.node_available = true;

        auto report = fh6::roon::build_setup_diagnostics(local);
        require(report.roon_environment == "local_server",
                "local Roon Server should be detected");
        require(report.cable_environment == "hifi",
                "Hi-Fi Cable render endpoint should be detected");
        require(report.recommended_endpoint_id == "id-hifi",
                "preferred endpoint should be selected");
        require(has_action(report, "select_recommended_endpoint"),
                "diagnostics should offer recommended endpoint selection");
        require(has_action(report, "test_audio"), "diagnostics should offer audio test");

        fh6::roon::SetupProbeSnapshot bridge_only;
        bridge_only.processes = {"RoonBridge.exe"};
        bridge_only.render_endpoints.push_back({"id-vb", "CABLE Input", false});
        bridge_only.node_available = false;
        auto bridge_report = fh6::roon::build_setup_diagnostics(bridge_only);
        require(bridge_report.roon_environment == "bridge_only",
                "Roon Bridge without local server should be classified");
        require(bridge_report.cable_environment == "vb_cable",
                "VB-CABLE render endpoint should be detected");
        require(has_action(bridge_report, "open_node_download"),
                "missing Node should offer official Node download");
        require(has_action(bridge_report, "open_roon_bridge_help"),
                "Bridge-only setup should offer Roon Bridge guidance");

        fh6::roon::SetupProbeSnapshot missing;
        missing.render_endpoints.push_back({"id-output", "Hi-Fi Cable Output", false});
        auto missing_report = fh6::roon::build_setup_diagnostics(missing);
        require(missing_report.roon_environment == "not_found",
                "missing Roon should be classified");
        require(missing_report.cable_environment == "partial",
                "recording-side-only cable should be partial");
        require(has_action(missing_report, "open_roon_download"),
                "missing Roon should offer official Roon download");
        require(has_action(missing_report, "open_vb_hifi_cable_download"),
                "missing render endpoint should offer VB-Audio download");
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }

    std::cout << "roon_setup_diagnostics_tests passed\n";
    return 0;
}
