#pragma once

#include <filesystem>
#include <string>

struct MacLoopbackOptions {
    std::string source = "default";
    int duration_ms = 10000;
    std::filesystem::path output;
    bool trigger = false;
    double trigger_threshold_db = -45.0;
    int trigger_hold_ms = 50;
    int pre_roll_ms = 250;
    double tail_silence_db = -50.0;
    int tail_silence_ms = 1000;
    bool trim_leading_silence = true;
    bool trim_trailing_silence = true;
    bool summary_json = true;
};

#ifdef __APPLE__
int list_loopback_sources_macos();
int record_loopback_macos(const MacLoopbackOptions& options);
#endif
