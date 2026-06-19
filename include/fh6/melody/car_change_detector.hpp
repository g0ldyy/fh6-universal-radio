#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace fh6::melody {

/// Listens on a UDP port for Forza Horizon 6 Data Out telemetry packets and
/// detects when the player switches cars (CarOrdinal changes).
///
/// Setup in FH6:
///   Settings → HUD and Gameplay → Data Out → On
///   Data Out IP Address : 127.0.0.1
///   Data Out Port       : (must match data_out_port in config.toml, default 7777)
///   Data Out Packet Format: Car Dash (or Sled – both contain CarOrdinal)
class CarChangeDetector {
public:
    /// Starts the background UDP listener thread on the given port.
    explicit CarChangeDetector(int port);
    ~CarChangeDetector();

    CarChangeDetector(const CarChangeDetector&)            = delete;
    CarChangeDetector& operator=(const CarChangeDetector&) = delete;

    /// Returns true (exactly once) when a car change has been detected since
    /// the last call. Thread-safe; safe to call every control-loop tick.
    bool poll_car_changed() noexcept;

    /// Restart the listener on a new port (called when config changes).
    void restart(int new_port);

private:
    void run(int port);

    std::atomic<bool>     changed_{false};
    std::atomic<bool>     stop_{false};
    std::jthread          thread_;
};

} // namespace fh6::melody
