#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string_view>

namespace fh6 {
class AudioSourceManager;
class ConfigStore;
} // namespace fh6

namespace fh6::http {

using JsonResponder  = std::function<void(const nlohmann::json&)>;
using ErrorResponder = std::function<void(int, std::string_view)>;
using BodyResponder  = std::function<void(int, std::string_view, std::string_view)>;

bool dispatch_roon_route(std::string_view method, std::string_view path, std::string_view body,
                         AudioSourceManager& mgr, ConfigStore& store, const JsonResponder& ok,
                         const ErrorResponder& fail, const BodyResponder& send_body);

} // namespace fh6::http
