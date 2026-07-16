#include "tuning_profile.hpp"

#include <array>

namespace jam2 {
namespace {

constexpr std::array<JoinProfile, 3> kJoinProfiles{{
    {
        "fast", "Fast", 32, 256, 4096, 1536, 4096,
        true, 0.02, 25, 500, true, 256, 512, 1024,
        true, 256, 256, 1536, 5000, 250,
    },
    {
        "moderate", "Moderate", 64, 512, 8192, 4096, 4096,
        true, 0.02, 25, 500, true, 512, 2048, 3072,
        true, 512, 512, 4096, 5000, 250,
    },
    {
        "safe", "Safe", 64, 1024, 8192, 7168, 4096,
        true, 0.02, 25, 500, true, 1024, 2048, 6144,
        true, 1024, 1024, 7168, 5000, 250,
    },
}};

constexpr std::array<CreateProfile, 3> kCreateProfiles{{
    {"fast", "Fast", 48000, 64, &kJoinProfiles[0]},
    {"moderate", "Moderate", 48000, 128, &kJoinProfiles[1]},
    {"safe", "Safe", 48000, 256, &kJoinProfiles[2]},
}};

} // namespace

std::span<const JoinProfile> join_profiles() { return kJoinProfiles; }
std::span<const CreateProfile> create_profiles() { return kCreateProfiles; }

const JoinProfile* find_join_profile(std::string_view name)
{
    for (const auto& profile : kJoinProfiles) {
        if (profile.name == name) return &profile;
    }
    return nullptr;
}

const CreateProfile* find_create_profile(std::string_view name)
{
    for (const auto& profile : kCreateProfiles) {
        if (profile.name == name) return &profile;
    }
    return nullptr;
}

const JoinProfile& default_join_profile() { return kJoinProfiles.front(); }
const CreateProfile& default_create_profile() { return kCreateProfiles.front(); }

std::string tuning_profile_names()
{
    std::string names;
    for (const auto& profile : kJoinProfiles) {
        if (!names.empty()) names += ", ";
        names.append(profile.name.data(), profile.name.size());
    }
    return names;
}

} // namespace jam2
