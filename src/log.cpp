#include "fh6/log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace fh6::log {

namespace {
std::FILE* g_file = nullptr;
std::mutex g_mu;

constexpr std::string_view level_name(Level l) noexcept {
    switch (l) {
        case Level::trace: return "TRACE";
        case Level::info: return "INFO";
        case Level::warn: return "WARN";
        case Level::error: return "ERROR";
    }
    return "?";
}
} // namespace

void init(const std::filesystem::path& log_file) noexcept {
    std::scoped_lock lk{g_mu};
    if (g_file) std::fclose(g_file);
    g_file = _wfopen(log_file.c_str(), L"a");
}

void shutdown() noexcept {
    std::scoped_lock lk{g_mu};
    if (g_file) {
        std::fflush(g_file);
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void emit(Level level, std::string_view message) noexcept {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t   = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _MSC_VER
    localtime_s(&tm, &t);
#else
    // MinGW: localtime is thread-safe enough for our purposes (we lock before use)
    auto* ptm = std::localtime(&t);
    if (ptm) tm = *ptm;
#endif
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    char ts[32];
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d.%03lld", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (long long)ms);

    std::scoped_lock lk{g_mu};
    if (g_file) {
        std::fprintf(g_file, "%s %-5.*s %.*s\n", ts, (int)level_name(level).size(),
                     level_name(level).data(), (int)message.size(), message.data());
        std::fflush(g_file);
    }
}

} // namespace fh6::log
