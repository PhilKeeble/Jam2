#pragma once

#include "PipelineTypes.hpp"
#include "Wav.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace jamtaster::native {

double estimateBpm(const std::vector<double>& beats);
int inferMeter(const std::vector<double>& beats, const std::vector<double>& downbeats, int fallback = 4);
std::string normalizeChord(const std::string& raw);
std::pair<std::vector<TimedLabel>, std::vector<ChordEvidence>> analyzeChromaChords(
    const AudioBuffer& audio, const std::vector<double>& beats);
std::vector<TimedLabel> fuseChordsWithBass(const std::vector<TimedLabel>& chords,
    const std::vector<NoteEvent>& bass);
std::vector<TimedLabel> stabilizeChords(std::vector<TimedLabel> chords, double duration);
std::vector<TimedLabel> contextualizeChords(const std::vector<TimedLabel>& primary,
    const std::vector<ChordEvidence>& chroma, const std::vector<NoteEvent>& bass,
    const std::vector<double>& beats, double duration);
std::vector<NoteEvent> contextualizeBass(std::vector<NoteEvent> notes,
    const std::vector<TimedLabel>& chords);
std::vector<DrumHit> repairDrums(const std::vector<DrumHit>& detected,
    const std::vector<DrumHit>& candidates, const std::vector<double>& beats,
    double bpm, int beatsPerBar, int division, int minimumRepeats = 3,
    int neighborhoodBars = 8, bool rimMode = false);
std::vector<DrumHit> shapeDrumDynamics(const std::vector<DrumHit>& hits,
    const std::vector<double>& beats, const std::vector<double>& downbeats,
    double bpm, int beatsPerBar, int division);
std::vector<DrumHit> classifyCymbals(const std::vector<DrumHit>& drums,
    const std::vector<double>& beats, const std::vector<double>& downbeats,
    const std::vector<TimedLabel>& structures);
std::vector<TimedLabel> inferSongSections(const Analysis& analysis, double duration,
    const std::map<std::string, AudioBuffer>& stems, int minimumInternalBars = 4,
    int maximumSections = 11);
std::vector<SectionChoice> chooseSections(const Analysis& analysis, double duration);

} // namespace jamtaster::native
