#pragma once

#include <string>
#include <vector>

namespace fh6::audio {

struct WasapiRenderDevice {
    std::string id;
    std::string name;
    bool is_default = false;
};

std::vector<WasapiRenderDevice> enumerate_render_devices();

} // namespace fh6::audio
