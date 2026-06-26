#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fh6::melody {

/// Audio formats supported by miniaudio (used for playback).
/// All comparisons are case-insensitive.
inline constexpr const char* kSupportedExtensions[] = {
    ".wav", ".mp3", ".flac", ".ogg", ".opus", ".m4a"
};

/// Returns bare filenames (e.g. "startup.mp3") for every supported audio file
/// found directly inside `folder`. Returns an empty vector if the folder does
/// not exist or cannot be read. Results are sorted alphabetically.
std::vector<std::string> scan_melody_files(const std::filesystem::path& folder);

} // namespace fh6::melody
