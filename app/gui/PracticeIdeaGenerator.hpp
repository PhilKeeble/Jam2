#pragma once

#include "BeatGridModel.hpp"

#include <QStringList>

#include <cstdint>

namespace jam2::practice {

enum class ChordStyle {
    FunctionalPop,
    BluesForm,
    ModalVamp,
    JazzTurnaround,
    ModernMetal,
};

enum class BeatStyle {
    StraightRock,
    HalfTime,
    Funk,
    FourOnTheFloor,
    BluesShuffle,
    ModernMetal,
};

struct ChordIdeaRequest {
    int key = -1;
    int style = -1;
    QString character;
    int bars = 0;
    int beatsPerBar = 4;
};

struct BeatIdeaRequest {
    int style = -1;
    QString character;
    int bars = 0;
    int beatsPerBar = 4;
};

struct GeneratedPracticeIdea {
    SongSection chordSection;
    SongSection beatSection;
    int bpm = 120;
    int clickDivision = 1;
    QVector<bool> clickEnabled;
    QVector<bool> clickAccents;
};

QStringList chordStyleNames();
QStringList beatStyleNames();
QStringList chordCharacters(int style);
QStringList beatCharacters(int style);
QStringList keyNames();

GeneratedPracticeIdea generateCoupledPracticeIdea(const ChordIdeaRequest& request);

// Explicit seeds are intentionally test-only inputs. They are never stored in
// the song or exposed by the practice UI.
SongSection generateChordIdeaForTest(const ChordIdeaRequest& request, std::uint32_t seed);
SongSection generateBeatIdeaForTest(const BeatIdeaRequest& request, std::uint32_t seed);
GeneratedPracticeIdea generateCoupledPracticeIdeaForTest(
    const ChordIdeaRequest& request,
    std::uint32_t seed);

} // namespace jam2::practice
