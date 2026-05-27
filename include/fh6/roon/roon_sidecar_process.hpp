#pragma once

#include "fh6/config.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace fh6::roon {

class RoonSidecarProcess {
public:
    RoonSidecarProcess(RoonConfig cfg, std::filesystem::path data_dir);
    ~RoonSidecarProcess();

    RoonSidecarProcess(const RoonSidecarProcess&)            = delete;
    RoonSidecarProcess& operator=(const RoonSidecarProcess&) = delete;

    bool start();
    void stop() noexcept;
    bool running() const noexcept;

    const std::string& error() const noexcept { return error_; }
    std::filesystem::path resolved_bridge_path() const;
    uint32_t started_process_id() const noexcept { return process_id_; }

private:
    struct Handle;

    std::filesystem::path resolve_node_path() const;
    void set_error(std::string message);

    RoonConfig cfg_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Handle> job_;
    std::unique_ptr<Handle> process_;
    uint32_t process_id_ = 0;
    std::string error_;
};

} // namespace fh6::roon
