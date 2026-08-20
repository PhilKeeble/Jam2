#include "PluginSharedMemory.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <new>
#include <stdexcept>
#include <utility>

namespace jam2::pluginhost {
namespace {
std::string mapping_name(const std::string& token)
{
    if (token.empty() || token.size() > 128) throw std::invalid_argument("Invalid plugin transport token");
    // Darwin's POSIX shared-memory namespace has a short implementation limit.
    // Hash the complete random service token so distinct tokens cannot collapse
    // merely because their differing characters occur after the name limit.
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char ch : token) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'))
            throw std::invalid_argument("Invalid plugin transport token");
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    constexpr char hexadecimal[] = "0123456789abcdef";
    std::string result = "/j2p_";
    result.reserve(21U);
    for (int shift = 60; shift >= 0; shift -= 4) {
        result.push_back(hexadecimal[(hash >> shift) & 0x0fU]);
    }
    return result;
}

std::runtime_error system_error(const char* operation, const std::string& name)
{
    const int code = errno;
    return std::runtime_error(
        std::string(operation) + " '" + name + "': " + std::strerror(code) +
        " (errno " + std::to_string(code) + ")");
}
}

class PluginSharedMemory::Impl final {
public:
    ~Impl()
    {
        if (owner && state) std::destroy_at(state);
        if (state) munmap(state, sizeof(SharedState));
        if (descriptor >= 0) close(descriptor);
        if (owner && !name.empty()) shm_unlink(name.c_str());
    }
    int descriptor = -1;
    SharedState* state = nullptr;
    std::string name;
    bool owner = false;
};

PluginSharedMemory::PluginSharedMemory() = default;
PluginSharedMemory::~PluginSharedMemory() = default;
PluginSharedMemory::PluginSharedMemory(PluginSharedMemory&&) noexcept = default;
PluginSharedMemory& PluginSharedMemory::operator=(PluginSharedMemory&&) noexcept = default;
PluginSharedMemory::PluginSharedMemory(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

PluginSharedMemory PluginSharedMemory::create(const std::string& token)
{
    auto impl = std::make_unique<Impl>();
    impl->name = mapping_name(token);
    impl->descriptor = shm_open(impl->name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (impl->descriptor < 0)
        throw system_error("Could not create plugin shared memory", impl->name);
    impl->owner = true;
    if (ftruncate(impl->descriptor, sizeof(SharedState)) != 0)
        throw std::runtime_error("Could not size plugin shared memory");
    impl->state = static_cast<SharedState*>(mmap(nullptr, sizeof(SharedState),
        PROT_READ | PROT_WRITE, MAP_SHARED, impl->descriptor, 0));
    if (impl->state == MAP_FAILED) { impl->state = nullptr; throw std::runtime_error("Could not map plugin shared memory"); }
    std::construct_at(impl->state);
    return PluginSharedMemory(std::move(impl));
}

PluginSharedMemory PluginSharedMemory::open(const std::string& token)
{
    auto impl = std::make_unique<Impl>();
    impl->name = mapping_name(token);
    impl->descriptor = shm_open(impl->name.c_str(), O_RDWR, 0600);
    if (impl->descriptor < 0)
        throw system_error("Could not open plugin shared memory", impl->name);
    impl->state = static_cast<SharedState*>(mmap(nullptr, sizeof(SharedState),
        PROT_READ | PROT_WRITE, MAP_SHARED, impl->descriptor, 0));
    if (impl->state == MAP_FAILED) { impl->state = nullptr; throw std::runtime_error("Could not map plugin shared memory"); }
    if (impl->state->magic != kProtocolMagic || impl->state->version != kProtocolVersion ||
        impl->state->bytes != sizeof(SharedState)) throw std::runtime_error("Plugin shared-memory protocol mismatch");
    return PluginSharedMemory(std::move(impl));
}

SharedState* PluginSharedMemory::get() noexcept { return impl_ ? impl_->state : nullptr; }
const SharedState* PluginSharedMemory::get() const noexcept { return impl_ ? impl_->state : nullptr; }
} // namespace jam2::pluginhost
