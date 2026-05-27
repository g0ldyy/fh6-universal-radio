#pragma once

#include "fh6/config.hpp"

#include <filesystem>

namespace fh6 {

class AudioSourceManager;

void sync_roon_source(AudioSourceManager& mgr, const RoonConfig& cfg,
                      const std::filesystem::path& data_dir = {});

} // namespace fh6
