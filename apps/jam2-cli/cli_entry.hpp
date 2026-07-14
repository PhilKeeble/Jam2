#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

struct Jam2CliHost {
    std::function<void(std::string_view)> output;
    std::function<void(std::string_view)> error;
    std::function<void(std::uint16_t, const std::vector<std::uint8_t>&)> control_frame;
};

struct Jam2EmbeddedEngineStats {
    std::uint64_t starts = 0;
    std::uint64_t restarts = 0;
    std::uint64_t reuses = 0;
    std::uint64_t engine_frame = 0;
};

int jam2_cli_main(int argc, char** argv);

// The embedded GUI installs one host before starting its worker. Ordinary CLI
// and Python launches leave this unset and retain normal stdout/stderr output.
void jam2_cli_set_host(Jam2CliHost* host) noexcept;
bool jam2_cli_has_host() noexcept;
bool jam2_cli_submit_control(std::span<const std::uint8_t> payload) noexcept;
bool jam2_cli_update_peers(const std::vector<std::string>& peers) noexcept;
void jam2_cli_request_stop() noexcept;
Jam2EmbeddedEngineStats jam2_cli_embedded_engine_stats() noexcept;
void jam2_cli_shutdown_embedded_engine() noexcept;
