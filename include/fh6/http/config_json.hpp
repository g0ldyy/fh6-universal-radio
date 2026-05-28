#pragma once

#include "fh6/config.hpp"

#include <nlohmann/json.hpp>

namespace fh6::http {

nlohmann::json config_to_json(const Config& c);
void apply_config_patch(Config& c, const nlohmann::json& j);

} // namespace fh6::http
