#pragma once

#include <filesystem>
#include <array>
#include <map>
#include <string>
#include <vector>

namespace jamtaster::native {

struct TimedLabel {
    double start = 0.0;
    double end = 0.0;
    std::string label;
    double confidence = 0.0;
};

struct NoteEvent {
    double start = 0.0;
    double end = 0.0;
    int midi = 0;
    int velocity = 88;
    double confidence = 0.0;
};

struct DrumHit {
    double time = 0.0;
    std::string lane;
    int velocity = 100;
    double confidence = 0.0;
    double energyRatio = 0.0;
    std::string provenance = "detected";
};

struct SectionChoice {
    std::string role;
    std::string sourceLabel;
    double start = 0.0;
    double end = 0.0;
    int firstBeat = 0;
    int beats = 0;
};

struct ChordEvidence {
    double start = 0.0;
    double end = 0.0;
    std::string label;
    double confidence = 0.0;
    std::array<double, 12> profile{};
    std::vector<std::pair<std::string, double>> candidates;
};

struct Analysis {
    std::vector<double> beats;
    std::vector<double> downbeats;
    double detectedBpm = 0.0;
    double bpm = 0.0;
    int beatsPerBar = 4;
    std::vector<TimedLabel> structures;
    std::vector<TimedLabel> chords;
    std::vector<ChordEvidence> chordEvidence;
    std::vector<DrumHit> drums;
    std::vector<DrumHit> drumCandidates;
    std::vector<NoteEvent> bass;
    int drumDivision = 4;
    std::vector<std::string> warnings;
};

struct PipelineOptions {
    std::filesystem::path input;
    std::filesystem::path projectRoot;
    std::filesystem::path modelsRoot;
    std::string name;
    int threads = 1;
    int seed = 0;
    int drumDivision = 4;
    int minimumBassMidi = 28;
    int maximumBassMidi = 60;
    int requestedMeter = 0;
    double requestedBpm = 0.0;
    bool force = false;
    bool reuseStems = false;
    bool timeStretch = true;
    bool arrangementLoop = true;
    bool exportJamJar = true;
};

struct PipelineResult {
    std::filesystem::path analysisRoot;
    std::filesystem::path analysisReport;
    std::filesystem::path songRoot;
    std::filesystem::path jamjar;
    std::map<std::string, double> timings;
    Analysis analysis;
    std::vector<SectionChoice> sections;
    bool cached = false;
};

} // namespace jamtaster::native
