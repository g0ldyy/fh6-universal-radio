#include "fh6/audio/wasapi_loopback_capture.hpp"

#include <stdexcept>
#include <string>
#include <unordered_set>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error{message};
}

} // namespace

int main() {
    const auto devices = fh6::audio::enumerate_render_devices();

    std::unordered_set<std::string> seen_ids;
    int default_count = 0;
    for (const auto& device : devices) {
        require(!device.id.empty(), "render device id should be stable and non-empty");
        require(!device.name.empty(), "render device name should be friendly and non-empty");
        require(seen_ids.insert(device.id).second, "render device ids should be unique");
        if (device.is_default) ++default_count;
    }

    require(default_count <= 1, "at most one render device should be marked default");
    return 0;
}
