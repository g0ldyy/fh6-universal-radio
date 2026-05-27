#include "fh6/audio/endpoint_classifier.hpp"

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error{message};
}

void require_classification(std::string_view name, std::string_view kind,
                            std::string_view recommendation, bool warning_expected) {
    const auto result = fh6::audio::classify_endpoint(name);
    require(result.kind == kind, "endpoint kind should match expected value");
    require(result.recommendation == recommendation,
            "endpoint recommendation should match expected value");
    require(result.warning.empty() != warning_expected, "endpoint warning presence should match");
}

} // namespace

int main() {
    try {
        require_classification("Hi-Fi Cable Input", "render_loopback", "preferred", false);
        require_classification("CABLE Input", "render_loopback", "fallback", false);
        require_classification("CABLE-A Input", "render_loopback", "fallback", false);
        require_classification("Hi-Fi Cable Output", "probable_recording_side", "avoid", true);
        require_classification("CABLE Output", "probable_recording_side", "avoid", true);
        require_classification("Speakers (Realtek Audio)", "unknown", "manual", false);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }

    std::cout << "audio_endpoint_classifier_tests passed\n";
    return 0;
}
