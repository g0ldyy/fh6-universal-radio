#include "fh6/melody/melody_scanner.hpp"

#include <algorithm>
#include <string>

namespace fh6::melody {

std::vector<std::string> scan_melody_files(const std::filesystem::path& folder) {
    std::vector<std::string> result;
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) return result;

    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;

        const auto& p = entry.path();
        if (!p.has_extension()) continue;

        // Case-insensitive extension match
        auto ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });

        for (const char* supported : kSupportedExtensions) {
            if (ext == supported) {
                result.push_back(p.filename().string());
                break;
            }
        }
    }

    std::sort(result.begin(), result.end());
    return result;
}

} // namespace fh6::melody
