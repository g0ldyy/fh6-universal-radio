#include "fh6/melody/car_change_detector.hpp"
#include "fh6/log.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>
#include <cstdint>

namespace fh6::melody {

// ---------------------------------------------------------------------------
// FH6 / FH5 / FM8 "Sled + Dash" Data Out packet layout.
// The game sends 232-byte or 311-byte packets depending on the format selected.
// CarOrdinal sits at byte offset 212 in both formats (it's in the Sled block).
// ---------------------------------------------------------------------------
namespace {
    constexpr std::size_t kCarOrdinalOffset = 212;
    constexpr std::size_t kMinPacketSize    = 216; // must have bytes 212-215
}

CarChangeDetector::CarChangeDetector(int port)
    : thread_{[this, port](std::stop_token /*tok*/) {
          stop_.store(false, std::memory_order_relaxed);
          run(port);
      }} {}

CarChangeDetector::~CarChangeDetector() {
    stop_.store(true, std::memory_order_release);
    thread_.request_stop();
}

bool CarChangeDetector::poll_car_changed() noexcept {
    return changed_.exchange(false, std::memory_order_acq_rel);
}

void CarChangeDetector::restart(int new_port) {
    stop_.store(true, std::memory_order_release);
    thread_.request_stop();
    if (thread_.joinable()) {
        thread_.join();
    }
    stop_.store(false, std::memory_order_relaxed);
    thread_ = std::jthread{[this, new_port](std::stop_token) { run(new_port); }};
}

void CarChangeDetector::run(int port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log::warn("[car_det] WSAStartup failed; car-change detection unavailable");
        return;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        log::warn("[car_det] socket() failed");
        WSACleanup();
        return;
    }

    // Non-blocking via a 200ms select timeout so we can check stop_ each loop.
    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // only accept from localhost

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        log::warn("[car_det] bind() on port {} failed; is FH6 Data Out configured?", port);
        closesocket(sock);
        WSACleanup();
        return;
    }

    log::info("[car_det] listening for FH6 telemetry on UDP port {}", port);

    constexpr int kBufSize = 512;
    char buf[kBufSize];
    int32_t prev_ordinal = -1;

    while (!stop_.load(std::memory_order_acquire)) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        timeval tv{0, 200'000}; // 200 ms
        const int sel = select(0, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        const int n = recv(sock, buf, kBufSize, 0);
        if (n < static_cast<int>(kMinPacketSize)) continue;

        // Read CarOrdinal as little-endian int32 at offset 212
        int32_t ordinal = 0;
        std::memcpy(&ordinal, buf + kCarOrdinalOffset, sizeof(ordinal));

        if (ordinal == 0) continue; // 0 = no car / in menu

        if (prev_ordinal != -1 && ordinal != prev_ordinal) {
            log::info("[car_det] car changed: ordinal {} -> {}", prev_ordinal, ordinal);
            changed_.store(true, std::memory_order_release);
        }
        prev_ordinal = ordinal;
    }

    closesocket(sock);
    WSACleanup();
    log::info("[car_det] listener stopped");
}

} // namespace fh6::melody
