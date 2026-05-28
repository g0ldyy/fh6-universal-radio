#pragma once

#include "fh6/audio/wasapi_loopback_capture.hpp"
#include "fh6/config.hpp"

#include <string>
#include <vector>

namespace fh6::roon {

struct SetupIssue {
    std::string id;
    std::string severity;
    std::string message;
};

struct SetupAction {
    std::string id;
    std::string label;
    std::string url;
};

struct SetupProbeSnapshot {
    std::vector<std::string> processes;
    std::vector<audio::WasapiRenderDevice> render_endpoints;
    bool node_available       = false;
    bool roon_install_found   = false;
    bool bridge_install_found = false;
};

struct RoonSetupDiagnostics {
    std::string roon_environment;
    std::string cable_environment;
    std::string recommended_endpoint_id;
    std::string recommended_endpoint_name;
    bool node_available = false;
    std::vector<SetupIssue> issues;
    std::vector<SetupAction> actions;
};

RoonSetupDiagnostics build_setup_diagnostics(const SetupProbeSnapshot& snapshot);
SetupProbeSnapshot collect_setup_probe(const RoonConfig& cfg);

} // namespace fh6::roon
