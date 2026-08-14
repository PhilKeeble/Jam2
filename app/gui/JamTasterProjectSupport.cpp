#include "JamTasterProjectSupport.hpp"

#include "ContentLimits.hpp"

#include <QJsonArray>
#include <QUuid>
#include <QtGlobal>

#include <cmath>

namespace jam2::gui {

bool appendJamTasterReferenceSection(
    QJsonObject& song,
    QJsonObject lane,
    QString& error)
{
    QJsonArray sections = song.value(QStringLiteral("sections")).toArray();
    QJsonObject looper = song.value(QStringLiteral("looper")).toObject();
    QJsonArray banks = looper.value(QStringLiteral("banks")).toArray();
    if (sections.isEmpty() || banks.isEmpty() ||
        sections.size() >= jam2::application::limits::kMaximumSongSections) {
        error = QStringLiteral(
            "There is no free section for the original reference WAV. Remove a section "
            "or choose to delete the source instead.");
        return false;
    }

    const QJsonObject firstBank = banks.first().toObject();
    QJsonObject timing = firstBank.value(QStringLiteral("timing")).toObject();
    const int bpm = qBound(20, timing.value(QStringLiteral("bpm")).toInt(120), 400);
    const int meter = qBound(
        2, timing.value(QStringLiteral("beats_per_bar")).toInt(4), 12);
    const qint64 frames = lane.value(QStringLiteral("source_frames"))
        .toVariant().toLongLong();
    const int sampleRate = lane.value(QStringLiteral("sample_rate")).toInt(48000);
    const double duration = sampleRate > 0
        ? frames / static_cast<double>(sampleRate)
        : 0.0;
    const int rawBeats = qMax(
        jam2::application::limits::kMinimumBeatsPerSection,
        static_cast<int>(std::ceil(duration * bpm / 60.0)));
    const int beats = ((rawBeats + meter - 1) / meter) * meter;
    if (beats > jam2::application::limits::kMaximumBeatsPerSection) {
        error = QStringLiteral(
            "The original recording is longer than Jam2's 512-beat section limit.");
        return false;
    }

    QJsonArray strings;
    QJsonArray beatPatterns;
    QJsonArray musicalPatterns;
    QJsonArray emptyDrumLanes;
    for (int drumLane = 0; drumLane < 10; ++drumLane) {
        emptyDrumLanes.append(QString());
    }
    const auto restSteps = [] {
        QJsonArray result;
        for (int index = 0; index < 4; ++index) {
            result.append(QJsonObject{
                {QStringLiteral("state"), QStringLiteral("rest")},
                {QStringLiteral("value"), QString()},
                {QStringLiteral("velocity"), 96},
            });
        }
        return result;
    };
    for (int beat = 0; beat < beats; ++beat) {
        strings.append(QString());
        beatPatterns.append(QJsonObject{
            {QStringLiteral("division"), 4},
            {QStringLiteral("lanes"), emptyDrumLanes},
        });
        musicalPatterns.append(QJsonObject{
            {QStringLiteral("division"), 4},
            {QStringLiteral("chords"), restSteps()},
            {QStringLiteral("bass"), restSteps()},
            {QStringLiteral("melody"), restSteps()},
            {QStringLiteral("support"), restSteps()},
        });
    }

    const int index = sections.size();
    const QString label(QChar(QLatin1Char('A').unicode() + index));
    sections.append(QJsonObject{
        {QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("label"), label},
        {QStringLiteral("name"), QStringLiteral("Original Reference")},
        {QStringLiteral("beats"), beats},
        {QStringLiteral("targets"), strings},
        {QStringLiteral("beat_notes"), strings},
        {QStringLiteral("lyrics"), strings},
        {QStringLiteral("chords"), strings},
        {QStringLiteral("beat_patterns"), beatPatterns},
        {QStringLiteral("musical_patterns"), musicalPatterns},
        {QStringLiteral("drum_kit"), QStringLiteral("acoustic")},
        {QStringLiteral("generated_kind"), QString()},
    });

    lane.insert(QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    lane.insert(QStringLiteral("name"), QStringLiteral("Original Source (muted)"));
    lane.insert(QStringLiteral("muted"), true);
    lane.insert(QStringLiteral("solo"), false);
    lane.insert(QStringLiteral("loop_enabled"), false);
    lane.insert(QStringLiteral("local_only"), false);
    lane.insert(QStringLiteral("origin_kind"), QStringLiteral("imported"));
    timing.insert(QStringLiteral("bpm"), bpm);
    timing.insert(QStringLiteral("beats_per_bar"), meter);
    timing.insert(QStringLiteral("inherits_bank_a"), true);
    banks.append(QJsonObject{
        {QStringLiteral("id"), label},
        {QStringLiteral("lanes"), QJsonArray{lane}},
        {QStringLiteral("timing"), timing},
    });
    looper.insert(QStringLiteral("banks"), banks);
    song.insert(QStringLiteral("sections"), sections);
    song.insert(QStringLiteral("looper"), looper);
    error.clear();
    return true;
}

} // namespace jam2::gui
