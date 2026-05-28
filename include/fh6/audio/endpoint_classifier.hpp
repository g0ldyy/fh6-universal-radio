#pragma once

#include <string>
#include <string_view>

namespace fh6::audio {

struct EndpointClassification {
    std::string kind;
    std::string recommendation;
    std::string warning;
};

EndpointClassification classify_endpoint(std::string_view name);

} // namespace fh6::audio
