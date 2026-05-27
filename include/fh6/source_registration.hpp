#pragma once

#include "fh6/config.hpp"

namespace fh6 {

class AudioSourceManager;

void sync_roon_source(AudioSourceManager& mgr, const RoonConfig& cfg);

} // namespace fh6
