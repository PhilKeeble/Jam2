#include "tuning_profile.hpp"

#include <array>

namespace jam2 {
namespace {

constexpr std::array<TuningProfile, 3> kProfiles{{
    {
        "fast",
        "Fast",
        48000,
        32,
        64,
        256,
        4096,
        1536,
        4096,
        true,
        0.02,
        25,
        500,
        true,
        256,
        512,
        1024,
        true,
        256,
        256,
        1536,
        5000,
        250,
    },
    {
        "moderate",
        "Moderate",
        48000,
        64,
        128,
        512,
        8192,
        4096,
        4096,
        true,
        0.02,
        25,
        500,
        true,
        512,
        2048,
        3072,
        true,
        512,
        512,
        4096,
        5000,
        250,
    },
    {
        "safe",
        "Safe",
        48000,
        64,
        256,
        1024,
        8192,
        7168,
        4096,
        true,
        0.02,
        25,
        500,
        true,
        1024,
        2048,
        6144,
        true,
        1024,
        1024,
        7168,
        5000,
        250,
    },
}};

} // namespace

std::span<const TuningProfile> tuning_profiles()
{
    return kProfiles;
}

const TuningProfile* find_tuning_profile(std::string_view name)
{
    for (const auto& profile : kProfiles) {
        if (profile.name == name) {
            return &profile;
        }
    }
    return nullptr;
}

const TuningProfile& default_tuning_profile()
{
    return kProfiles.front();
}

std::string tuning_profile_names()
{
    std::string names;
    for (const auto& profile : kProfiles) {
        if (!names.empty()) {
            names += ", ";
        }
        names.append(profile.name.data(), profile.name.size());
    }
    return names;
}

} // namespace jam2
