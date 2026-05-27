#include "fh6/audio/endpoint_classifier.hpp"

#include <algorithm>
#include <cctype>

namespace fh6::audio {
namespace {

std::string lower_ascii(std::string_view value) {
    std::string out{value};
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

bool is_vb_cable_input(std::string_view name) {
    if (contains(name, "cable input")) return true;
    for (char suffix : {'a', 'b', 'c', 'd'}) {
        std::string pattern  = "cable-";
        pattern             += suffix;
        pattern             += " input";
        if (contains(name, pattern)) return true;
    }
    return false;
}

bool is_vb_cable_output(std::string_view name) {
    if (contains(name, "cable output")) return true;
    for (char suffix : {'a', 'b', 'c', 'd'}) {
        std::string pattern  = "cable-";
        pattern             += suffix;
        pattern             += " output";
        if (contains(name, pattern)) return true;
    }
    return false;
}

EndpointClassification recording_side_warning() {
    return EndpointClassification{
        "probable_recording_side",
        "avoid",
        "This looks like the recording side of a virtual cable. Select a playback/render endpoint "
        "such as Hi-Fi Cable Input or CABLE Input for WASAPI loopback.",
    };
}

} // namespace

EndpointClassification classify_endpoint(std::string_view name) {
    const auto lower = lower_ascii(name);
    if (contains(lower, "hi-fi cable output")) return recording_side_warning();
    if (is_vb_cable_output(lower)) return recording_side_warning();
    if (contains(lower, "hi-fi cable input")) return {"render_loopback", "preferred", {}};
    if (is_vb_cable_input(lower)) return {"render_loopback", "fallback", {}};
    return {"unknown", "manual", {}};
}

} // namespace fh6::audio
