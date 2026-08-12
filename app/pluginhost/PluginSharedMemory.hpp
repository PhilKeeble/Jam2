#pragma once

#include "PluginProtocol.hpp"

#include <memory>
#include <string>

namespace jam2::pluginhost {

class PluginSharedMemory final {
public:
    PluginSharedMemory();
    ~PluginSharedMemory();
    PluginSharedMemory(PluginSharedMemory&&) noexcept;
    PluginSharedMemory& operator=(PluginSharedMemory&&) noexcept;
    PluginSharedMemory(const PluginSharedMemory&) = delete;
    PluginSharedMemory& operator=(const PluginSharedMemory&) = delete;

    static PluginSharedMemory create(const std::string& token);
    static PluginSharedMemory open(const std::string& token);

    SharedState* get() noexcept;
    const SharedState* get() const noexcept;
    explicit operator bool() const noexcept { return get() != nullptr; }

private:
    class Impl;
    explicit PluginSharedMemory(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace jam2::pluginhost
