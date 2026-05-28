#include "youtube_music_support.hpp"

#include <filesystem>

namespace fh6::sources {

namespace youtube_music_detail {

std::string drain_to_eof(HANDLE pipe) {
    std::string out;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(pipe, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
    return out;
}

bool is_playlist_url(std::string_view url) {
    return url.find("playlist?") != std::string_view::npos;
}

std::string watch_url_for_id(std::string_view id) {
    std::string s = "https://www.youtube.com/watch?v=";
    s.append(id);
    return s;
}

namespace {

bool tool_available(const std::filesystem::path& configured_path, const wchar_t* command_name) {
    if (!configured_path.empty()) {
        std::error_code ec;
        return std::filesystem::is_regular_file(configured_path, ec);
    }
    wchar_t resolved[MAX_PATH] = {};
    return SearchPathW(nullptr, command_name, L".exe", MAX_PATH, resolved, nullptr) != 0;
}

} // namespace

std::string youtube_tool_setup_message(const YouTubeMusicConfig& cfg) {
    const bool missing_yt = !tool_available(cfg.yt_dlp_path, L"yt-dlp");
    const bool missing_ff = !tool_available(cfg.ffmpeg_path, L"ffmpeg");
    if (!missing_yt && !missing_ff) return {};
    return "YouTube Music is enabled, but yt-dlp or ffmpeg could not be found. Run "
           "`winget install yt-dlp.yt-dlp`, `winget install Gyan.FFmpeg`, and "
           "`winget install DenoLand.Deno`, then keep yt-dlp/ffmpeg on PATH or set "
           "[youtube_music].yt_dlp_path and ffmpeg_path to the full .exe paths.";
}

} // namespace youtube_music_detail

YouTubeMusicSource::Pipe::~Pipe() {
    if (read_pipe) CloseHandle(read_pipe);
    if (title_pipe) CloseHandle(title_pipe);
    if (job) CloseHandle(job);
    if (proc_yt) CloseHandle(proc_yt);
    if (proc_ff) CloseHandle(proc_ff);
    if (proc_title) CloseHandle(proc_title);
}

} // namespace fh6::sources
