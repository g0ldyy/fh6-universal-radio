#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace fh6::http {

struct Request {
    std::string method;
    std::string path;
    std::string body;
    int error_status = 0;
    std::string error_message;
};

constexpr DWORD kSocketTimeoutMs = 2000;

constexpr SOCKET invalid_socket() noexcept {
    return static_cast<SOCKET>(~static_cast<UINT_PTR>(0));
}

bool read_request(SOCKET client, Request& req);
void send_all(SOCKET client, std::string_view data);
void send_response(SOCKET client, int code, std::string_view body,
                   std::string_view content_type = "application/json");
bool serve_file(SOCKET client, const std::filesystem::path& file);
bool static_file_path(const std::filesystem::path& root, std::string_view request_path,
                      std::filesystem::path& out);

} // namespace fh6::http
