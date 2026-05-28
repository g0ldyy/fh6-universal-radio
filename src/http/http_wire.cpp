#include "http_wire.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>

namespace fh6::http {
namespace {

constexpr std::size_t kMaxBodyBytes = 1024U * 1024U;

constexpr std::string_view status_text(int code) noexcept {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 502: return "Bad Gateway";
        default: return "Internal Server Error";
    }
}

constexpr std::string_view mime_for(std::string_view path) noexcept {
    auto ends = [&](std::string_view ext) { return path.ends_with(ext); };
    if (ends(".html")) return "text/html";
    if (ends(".css")) return "text/css";
    if (ends(".js")) return "application/javascript";
    if (ends(".svg")) return "image/svg+xml";
    if (ends(".png")) return "image/png";
    if (ends(".json")) return "application/json";
    return "text/plain";
}

std::size_t header_size_t(std::string_view headers, std::string_view name_lower) {
    for (std::size_t i = 0; i + name_lower.size() < headers.size(); ++i) {
        bool match = true;
        for (std::size_t k = 0; k < name_lower.size(); ++k) {
            auto ch = static_cast<unsigned char>(headers[i + k]);
            if (static_cast<char>(std::tolower(ch)) != name_lower[k]) {
                match = false;
                break;
            }
        }
        if (!match) continue;
        std::size_t p = i + name_lower.size();
        while (p < headers.size() && (headers[p] == ':' || headers[p] == ' ' || headers[p] == '\t'))
            ++p;
        std::size_t v = 0;
        while (p < headers.size() && std::isdigit(static_cast<unsigned char>(headers[p])))
            v = v * 10 + static_cast<std::size_t>(headers[p++] - '0');
        return v;
    }
    return 0;
}

} // namespace

bool read_request(SOCKET client, Request& req) {
    std::string raw;
    raw.reserve(1024);
    std::array<char, 4096> buf{};

    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        int r = recv(client, buf.data(), static_cast<int>(buf.size()), 0);
        if (r <= 0) return false;
        raw.append(buf.data(), static_cast<std::size_t>(r));
        header_end = raw.find("\r\n\r\n");
        if (raw.size() > 64 * 1024) return false;
    }

    std::istringstream first{raw.substr(0, raw.find("\r\n"))};
    if (!(first >> req.method >> req.path)) return false;

    const std::string_view headers{raw.data(), header_end};
    const std::size_t content_length = header_size_t(headers, "content-length");
    if (content_length > kMaxBodyBytes) {
        req.error_status  = 413;
        req.error_message = "request body too large";
        return false;
    }

    req.body.assign(raw, header_end + 4, std::string::npos);
    if (req.body.size() > kMaxBodyBytes) {
        req.error_status  = 413;
        req.error_message = "request body too large";
        return false;
    }
    while (req.body.size() < content_length) {
        const std::size_t need =
            std::min<std::size_t>(buf.size(), content_length - req.body.size());
        int r = recv(client, buf.data(), static_cast<int>(need), 0);
        if (r <= 0) break;
        req.body.append(buf.data(), static_cast<std::size_t>(r));
    }
    return true;
}

void send_all(SOCKET client, std::string_view data) {
    while (!data.empty()) {
        int n = send(client, data.data(), static_cast<int>(data.size()), 0);
        if (n <= 0) return;
        data.remove_prefix(static_cast<std::size_t>(n));
    }
}

void send_response(SOCKET client, int code, std::string_view body, std::string_view content_type) {
    std::ostringstream resp;
    resp << "HTTP/1.1 " << code << ' ' << status_text(code) << "\r\n"
         << "Content-Type: " << content_type << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Access-Control-Allow-Methods: GET, POST, PUT, OPTIONS\r\n"
         << "Access-Control-Allow-Headers: Content-Type\r\n"
         << "Connection: close\r\n\r\n";
    auto headers = resp.str();
    send_all(client, headers);
    send_all(client, body);
}

bool serve_file(SOCKET client, const std::filesystem::path& file) {
    std::ifstream f{file, std::ios::binary};
    if (!f) return false;
    std::ostringstream buf;
    buf << f.rdbuf();
    send_response(client, 200, buf.str(), mime_for(file.string()));
    return true;
}

bool static_file_path(const std::filesystem::path& root, std::string_view request_path,
                      std::filesystem::path& out) {
    if (request_path.empty() || request_path.front() != '/') return false;
    const std::string rel =
        request_path == "/" ? "index.html" : std::string{request_path.substr(1)};
    const std::filesystem::path rel_path{rel};
    if (rel_path.empty() || rel_path.is_absolute() || rel_path.has_root_name()) return false;
    for (const auto& part : rel_path) {
        if (part == "..") return false;
    }
    out = root / rel_path;
    return true;
}

} // namespace fh6::http
