#pragma once

#include "RuntimeContracts.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace jam2::cli {

extern const std::string_view kUsage;
extern const std::string_view kNetworkUsage;

bool is_help_argument(std::string_view argument) noexcept;
bool has_help_argument(int argc, char** argv, int start) noexcept;
void print_device_help(std::string_view command);
void print_audio_options_help();
void print_local_help();
void print_network_options_help();
void print_network_create_help();
void print_network_join_help();

std::string_view require_value(int argc, char** argv, int& index, std::string_view name);
std::string_view metronome_mode_text(Jam2MetronomeMode mode);
std::string_view metronome_mode_text(int mode);
int metronome_mode_id(Jam2MetronomeMode mode);
std::string_view test_input_mode_text(Jam2TestInputMode mode);
int test_input_mode_id(Jam2TestInputMode mode);
std::string channel_selection_text(const jam2::audio::ChannelSelection& channels);
std::string mono_mix_mode_text(std::size_t channel_count);
std::string_view os_priority_text(Jam2OsPriorityMode mode);
Jam2RuntimeOptions parse_options(
    int argc,
    char** argv,
    int start,
    Jam2ProfileApplication profile_application = Jam2ProfileApplication::Create);

} // namespace jam2::cli
