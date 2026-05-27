#include "fh6/source_registration.hpp"
#include "fh6/audio_source_manager.hpp"
#include "fh6/sources/roon_source.hpp"

#include <memory>

namespace fh6 {

void sync_roon_source(AudioSourceManager& mgr, const RoonConfig& cfg) {
    if (cfg.enabled) {
        if (!mgr.find("roon")) {
            auto src = std::make_unique<sources::RoonSource>(cfg);
            if (src->initialize()) mgr.register_source(std::move(src));
        }
    } else if (mgr.find("roon")) {
        mgr.unregister_source("roon");
    }
}

} // namespace fh6
