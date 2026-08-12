#include "PluginSharedMemory.hpp"

#include <windows.h>

#include <new>
#include <stdexcept>
#include <utility>

namespace jam2::pluginhost {
namespace {

std::wstring mapping_name(const std::string& token)
{
    if (token.empty() || token.size() > 128) throw std::invalid_argument("Invalid plugin transport token");
    std::wstring result = L"Local\\Jam2Plugin_";
    result.reserve(result.size() + token.size());
    for (const unsigned char ch : token) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'))
            throw std::invalid_argument("Invalid plugin transport token");
        result.push_back(static_cast<wchar_t>(ch));
    }
    return result;
}

} // namespace

class PluginSharedMemory::Impl final {
public:
    ~Impl()
    {
        if (owner && state) std::destroy_at(state);
        if (state) UnmapViewOfFile(state);
        if (mapping) CloseHandle(mapping);
    }
    HANDLE mapping = nullptr;
    SharedState* state = nullptr;
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
    const auto name = mapping_name(token);
    impl->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(sizeof(SharedState)), name.c_str());
    if (!impl->mapping || GetLastError() == ERROR_ALREADY_EXISTS)
        throw std::runtime_error("Could not create unique plugin shared memory");
    impl->state = static_cast<SharedState*>(MapViewOfFile(
        impl->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!impl->state) throw std::runtime_error("Could not map plugin shared memory");
    std::construct_at(impl->state);
    impl->owner = true;
    return PluginSharedMemory(std::move(impl));
}

PluginSharedMemory PluginSharedMemory::open(const std::string& token)
{
    auto impl = std::make_unique<Impl>();
    const auto name = mapping_name(token);
    impl->mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
    if (!impl->mapping) throw std::runtime_error("Could not open plugin shared memory");
    impl->state = static_cast<SharedState*>(MapViewOfFile(
        impl->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!impl->state || impl->state->magic != kProtocolMagic ||
        impl->state->version != kProtocolVersion || impl->state->bytes != sizeof(SharedState))
        throw std::runtime_error("Plugin shared-memory protocol mismatch");
    return PluginSharedMemory(std::move(impl));
}

SharedState* PluginSharedMemory::get() noexcept { return impl_ ? impl_->state : nullptr; }
const SharedState* PluginSharedMemory::get() const noexcept { return impl_ ? impl_->state : nullptr; }

} // namespace jam2::pluginhost
