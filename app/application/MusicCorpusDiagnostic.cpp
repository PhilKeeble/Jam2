#include "MusicCorpusDiagnostic.hpp"

#include "BeatGridModel.hpp"
#include "GenerationRecipe.hpp"
#include "PracticeIdeaController.hpp"
#include "PracticeIdeaGenerator.hpp"
#include "PracticeReferenceRenderer.hpp"
#include "StyleProfileCatalog.hpp"
#include "pcm16_wav.hpp"

#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTextStream>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kTicksPerBeat = 12;

std::uint32_t stableSeed(const QString& text)
{
    std::uint32_t value = 2166136261U;
    for (const QChar character : text) {
        value ^= character.unicode();
        value *= 16777619U;
    }
    return value ^ 0x4a326b1dU;
}

QJsonArray roleEvents(
    const jam2::practice::GenerationRecipe& recipe)
{
    QJsonArray result;
    for (const jam2::practice::MelodyRecipeEvent& event : recipe.melodyEvents) {
        result.append(QJsonObject{
            {QStringLiteral("role"), QStringLiteral("melody")},
            {QStringLiteral("start_beat"),
             static_cast<double>(event.tick) / kTicksPerBeat},
            {QStringLiteral("duration_beats"),
             static_cast<double>(event.durationTicks) / kTicksPerBeat},
            {QStringLiteral("midi"), event.midi},
            {QStringLiteral("note"), event.note},
            {QStringLiteral("velocity"), event.velocity},
            {QStringLiteral("articulation"), QString()},
        });
    }
    const auto appendSupporting =
        [&result](const QVector<jam2::practice::RoleRecipeEvent>& events,
                  const QString& fallbackRole) {
            for (const jam2::practice::RoleRecipeEvent& event : events) {
                result.append(QJsonObject{
                    {QStringLiteral("role"),
                     event.role.isEmpty() ? fallbackRole : event.role},
                    {QStringLiteral("start_beat"),
                     static_cast<double>(event.tick) / kTicksPerBeat},
                    {QStringLiteral("duration_beats"),
                     static_cast<double>(event.durationTicks) / kTicksPerBeat},
                    {QStringLiteral("midi"), event.midi},
                    {QStringLiteral("note"), event.note},
                    {QStringLiteral("velocity"), event.velocity},
                    {QStringLiteral("articulation"), event.articulation},
                });
            }
        };
    appendSupporting(recipe.bassEvents, QStringLiteral("bass"));
    appendSupporting(recipe.supportingEvents, QStringLiteral("support"));
    return result;
}

QJsonArray drumHits(const jam2::practice::GeneratedPracticeIdea& idea)
{
    QJsonArray result;
    const QStringList laneNames = BeatGridModel::beatLaneNames();
    for (int beat = 0; beat < idea.beatSection.beatPatterns.size(); ++beat) {
        const BeatPattern& pattern = idea.beatSection.beatPatterns.at(beat);
        if (pattern.division <= 0) continue;
        for (int lane = 0;
             lane < pattern.lanes.size() && lane < laneNames.size();
             ++lane) {
            const QString& cells = pattern.lanes.at(lane);
            for (int step = 0; step < cells.size(); ++step) {
                if (cells.at(step) == QLatin1Char('.')) continue;
                const int tick = beat * kTicksPerBeat +
                    step * kTicksPerBeat / pattern.division;
                result.append(QJsonObject{
                    {QStringLiteral("lane"), laneNames.at(lane)},
                    {QStringLiteral("state"), QString(cells.at(step))},
                    {QStringLiteral("tick"), tick},
                    {QStringLiteral("beat"),
                     static_cast<double>(tick) / kTicksPerBeat},
                });
            }
        }
    }
    return result;
}

std::vector<float> readMonoPcm16(const QString& path)
{
    const auto inspected = jam2::wav::inspect_pcm16_file(
        std::filesystem::path(path.toStdWString()));
    if (!inspected || inspected.info.channels != 1) {
        throw std::runtime_error(
            QStringLiteral("Corpus renderer produced an invalid mono PCM16 WAV: %1 (%2)")
                .arg(path, QString::fromStdString(inspected.error))
                .toStdString());
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) ||
        !file.seek(static_cast<qint64>(inspected.info.data_offset))) {
        throw std::runtime_error("Cannot read a rendered corpus stem.");
    }
    const QByteArray pcm =
        file.read(static_cast<qint64>(inspected.info.data_bytes));
    if (pcm.size() != static_cast<qsizetype>(inspected.info.data_bytes)) {
        throw std::runtime_error("A rendered corpus stem is truncated.");
    }
    std::vector<float> result(inspected.info.frames);
    const auto* bytes = reinterpret_cast<const uchar*>(pcm.constData());
    for (std::size_t frame = 0; frame < result.size(); ++frame) {
        result[frame] =
            static_cast<float>(qFromLittleEndian<qint16>(bytes + frame * 2)) /
            32768.0f;
    }
    return result;
}

bool writeMonoPcm16(const QString& path, const std::vector<float>& audio)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    const quint32 dataBytes = static_cast<quint32>(audio.size() * 2);
    stream.writeRawData("RIFF", 4);
    stream << static_cast<quint32>(36 + dataBytes);
    stream.writeRawData("WAVEfmt ", 8);
    stream << static_cast<quint32>(16)
           << static_cast<quint16>(1)
           << static_cast<quint16>(1)
           << static_cast<quint32>(kSampleRate)
           << static_cast<quint32>(kSampleRate * 2)
           << static_cast<quint16>(2)
           << static_cast<quint16>(16);
    stream.writeRawData("data", 4);
    stream << dataBytes;
    for (float value : audio) {
        stream << static_cast<qint16>(std::lround(
            std::clamp(value, -0.98f, 0.98f) * 32767.0f));
    }
    return file.commit();
}

std::vector<float> auditionMix(
    const std::array<std::vector<float>, 5>& stems)
{
    std::size_t frames = 0;
    for (const auto& stem : stems) frames = std::max(frames, stem.size());
    std::vector<float> output(frames, 0.0f);
    // The production drum stem already contains its post-bus makeup. Mirror
    // the unity managed-lane default so the corpus measures the Jam2 balance.
    constexpr std::array<double, 5> gains{
        0.46, 0.56, 0.54, 0.34, 0.58};
    for (std::size_t frame = 0; frame < frames; ++frame) {
        double value = 0.0;
        for (std::size_t lane = 0; lane < stems.size(); ++lane) {
            if (frame < stems[lane].size()) {
                value += gains[lane] * stems[lane][frame];
            }
        }
        output[frame] = static_cast<float>(std::tanh(0.72 * value));
    }
    return output;
}

struct CorpusAudioRender {
    QString mixRelativePath;
    QString drumRelativePath;
    QJsonObject diagnostics;
};

CorpusAudioRender renderFullMix(
    const jam2::practice::GeneratedPracticeIdea& idea,
    const QString& sampleId,
    const QDir& artifacts)
{
    jam2::practice::ReferenceRenderSettings settings;
    settings.renderChords = true;
    settings.renderDrums = true;
    settings.renderMelody = !idea.recipe.melodyEvents.isEmpty();
    settings.renderBass = !idea.recipe.bassEvents.isEmpty();
    settings.renderSupport = !idea.recipe.supportingEvents.isEmpty();
    settings.voicing = jam2::practice::ChordVoicing::StyleDefault;
    settings.sampleRate = kSampleRate;
    settings.bpm = idea.bpm;
    settings.meterNumerator = idea.meterNumerator;
    settings.meterDenominator = idea.meterDenominator;
    settings.tempoPulseUnits = idea.tempoPulseUnits;
    const auto rendered = jam2::practice::renderPracticeReferences(
        &idea.chordSection, &idea.beatSection, settings,
        artifacts.absolutePath());
    if (!rendered.error.isEmpty()) {
        throw std::runtime_error(
            QStringLiteral("Jam2 corpus render failed: %1")
                .arg(rendered.error).toStdString());
    }
    const std::array<std::vector<float>, 5> stems{
        readMonoPcm16(rendered.chords.path),
        settings.renderMelody
            ? readMonoPcm16(rendered.melody.path) : std::vector<float>{},
        settings.renderBass
            ? readMonoPcm16(rendered.bass.path) : std::vector<float>{},
        settings.renderSupport
            ? readMonoPcm16(rendered.support.path) : std::vector<float>{},
        readMonoPcm16(rendered.drums.path),
    };
    const QString drumRelative =
        QStringLiteral("audio/drum-stems/") + sampleId +
        QStringLiteral(".wav");
    const QString drumAbsolute =
        artifacts.absoluteFilePath(drumRelative);
    QDir().mkpath(QFileInfo(drumAbsolute).absolutePath());
    if (QFile::exists(drumAbsolute) &&
        !QFile::remove(drumAbsolute)) {
        throw std::runtime_error(
            "Cannot replace a previous corpus drum stem.");
    }
    if (!QFile::rename(rendered.drums.path, drumAbsolute)) {
        throw std::runtime_error(
            "Cannot retain the corpus drum stem.");
    }
    for (const QString& path : {
             rendered.chords.path,
             rendered.melody.path,
             rendered.bass.path,
             rendered.support.path}) {
        if (!path.isEmpty()) QFile::remove(path);
    }
    const QString relative =
        QStringLiteral("audio/full-form-corpus/") + sampleId +
        QStringLiteral(".wav");
    if (!writeMonoPcm16(
            artifacts.absoluteFilePath(relative), auditionMix(stems))) {
        throw std::runtime_error("Cannot write a full-form corpus mix.");
    }
    const auto stemJson = [](const jam2::practice::ReferenceWav& stem) {
        return QJsonObject{
            {QStringLiteral("peak"), stem.peak},
            {QStringLiteral("rms"), stem.rms},
            {QStringLiteral("rms_dbfs"),
             stem.rms > 0.0f
                 ? 20.0 * std::log10(stem.rms)
                 : -120.0},
            {QStringLiteral("events"), stem.eventCount},
            {QStringLiteral("frames"),
             static_cast<double>(stem.frames)},
            {QStringLiteral("pre_makeup_peak"),
             stem.preMakeupPeak},
            {QStringLiteral("makeup_gain_db"),
             stem.makeupGainDb},
            {QStringLiteral("limited_samples"),
             static_cast<double>(stem.limitedSamples)},
        };
    };
    return {
        relative,
        drumRelative,
        QJsonObject{
            {QStringLiteral("chords"), stemJson(rendered.chords)},
            {QStringLiteral("melody"), stemJson(rendered.melody)},
            {QStringLiteral("bass"), stemJson(rendered.bass)},
            {QStringLiteral("support"), stemJson(rendered.support)},
            {QStringLiteral("drums"), stemJson(rendered.drums)},
            {QStringLiteral("drum_mix_gain_db"),
             idea.recipe.drumMixGainDb},
            {QStringLiteral("generated_drum_lane_gain_db"),
             jam2::practice::kGeneratedDrumLaneGainDb},
            {QStringLiteral("generated_drum_stem_makeup_db"),
             jam2::practice::kGeneratedDrumStemMakeupDb},
            {QStringLiteral("renderer"), rendered.diagnostics},
        },
    };
}

QJsonObject corpusSample(
    const jam2::practice::ProfileDefinition& profile,
    const jam2::practice::NativeFormDefinition& form,
    int complexity,
    int sampleIndex,
    const QString& seedNamespace,
    const QDir& artifacts,
    bool renderAudio,
    bool matchedComplexitySeeds)
{
    const QString sampleId = QStringLiteral("%1__%2__c%3__s%4")
        .arg(profile.id, form.id)
        .arg(complexity)
        .arg(sampleIndex + 1);
    const QString seedGroupId = QStringLiteral("%1__%2__s%3")
        .arg(profile.id, form.id)
        .arg(sampleIndex + 1);
    const std::uint32_t seed =
        stableSeed(
            seedNamespace + QLatin1Char('/') +
            (matchedComplexitySeeds ? seedGroupId : sampleId));
    jam2::practice::ChordIdeaRequest request;
    request.styleId = profile.styleId;
    request.profileId = profile.id;
    request.formId = form.id;
    request.harmonicComplexity = complexity;
    request.rhythmicComplexity = complexity;
    const auto idea =
        jam2::practice::generateCoupledPracticeIdeaForTest(request, seed);
    if (!idea.recipe.isValid() ||
        !idea.chordSection.generatedRecipe.isValid() ||
        !idea.beatSection.generatedRecipe.isValid()) {
        throw std::runtime_error(
            QStringLiteral("Generated corpus sample %1 has an invalid recipe.")
                .arg(sampleId)
                .toStdString());
    }
    QString voicingError;
    const QJsonArray voicings =
        jam2::practice::practiceChordVoicingDiagnostics(
            idea.chordSection,
            jam2::practice::ChordVoicing::StyleDefault,
            voicingError);
    if (!voicingError.isEmpty()) {
        throw std::runtime_error(voicingError.toStdString());
    }
    const CorpusAudioRender audio = renderAudio
        ? renderFullMix(idea, sampleId, artifacts)
        : CorpusAudioRender{};
    return {
        {QStringLiteral("id"), sampleId},
        {QStringLiteral("style_id"), profile.styleId},
        {QStringLiteral("profile_id"), profile.id},
        {QStringLiteral("profile_name"), profile.name},
        {QStringLiteral("form_id"), form.id},
        {QStringLiteral("form_name"), form.name},
        {QStringLiteral("requested_bars"), form.bars},
        {QStringLiteral("requested_meter"), form.meterId},
        {QStringLiteral("requested_phrase_bars"), form.phraseBars},
        {QStringLiteral("requested_complexity"), complexity},
        {QStringLiteral("sample_index"), sampleIndex},
        {QStringLiteral("seed_group_id"), seedGroupId},
        {QStringLiteral("seed"), QString::number(seed)},
        {QStringLiteral("audio_mix"),
         audio.mixRelativePath},
        {QStringLiteral("audio_drums"),
         audio.drumRelativePath},
        {QStringLiteral("reference_render"),
         audio.diagnostics},
        {QStringLiteral("research_constraints"), QJsonObject{
             {QStringLiteral("minimum_bpm"), profile.minimumBpm},
             {QStringLiteral("maximum_bpm"), profile.maximumBpm},
             {QStringLiteral("tonal_collections"),
              QJsonArray::fromStringList(profile.tonalCollections)},
             {QStringLiteral("progression_families"),
              QJsonArray::fromStringList(profile.progressionFamilies)},
             {QStringLiteral("groove_families"),
              QJsonArray::fromStringList(profile.grooveFamilies)},
             {QStringLiteral("meter_ids"),
              QJsonArray::fromStringList(profile.meterIds)},
             {QStringLiteral("bass_grammar"), profile.bassGrammar},
             {QStringLiteral("motif_grammar"), profile.motifGrammar},
         }},
        {QStringLiteral("recipe"),
         jam2::practice::generationRecipeToJson(idea.recipe)},
        {QStringLiteral("chord_voicings"), voicings},
        {QStringLiteral("lane_events"), roleEvents(idea.recipe)},
        {QStringLiteral("drum_hits"), drumHits(idea)},
    };
}

} // namespace

QJsonObject jam2WriteFullFormMusicCorpus(
    const QString& artifactRoot,
    const QString& seedNamespace,
    const Jam2MusicCorpusOptions& options)
{
    constexpr std::array<int, 3> complexities{1, 4, 8};
    const int samplesPerCell = std::clamp(options.samplesPerCell, 2, 16);
    const QDir artifacts(artifactRoot);
    const QString corpusPath =
        artifacts.absoluteFilePath(QStringLiteral("full-form-corpus.json"));
    QSaveFile file(corpusPath);
    if (!file.open(QIODevice::WriteOnly)) {
        throw std::runtime_error(
            "Cannot open the full-form corpus JSON for publication.");
    }
    const auto writeChunk = [&file](const QByteArray& chunk) {
        qint64 written = 0;
        while (written < chunk.size()) {
            const qint64 amount =
                file.write(chunk.constData() + written, chunk.size() - written);
            if (amount <= 0) return false;
            written += amount;
        }
        return true;
    };
    QJsonObject corpusHeader{
        {QStringLiteral("version"), 3},
        {QStringLiteral("generated_at"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {QStringLiteral("generator"),
         QStringLiteral("release/jam2.exe production generator and reference renderer")},
        {QStringLiteral("seed_namespace"), seedNamespace},
        {QStringLiteral("complexity_levels"), QJsonArray{1, 4, 8}},
        {QStringLiteral("samples_per_profile_form_complexity"),
         samplesPerCell},
        {QStringLiteral("matched_complexity_seeds"),
         options.matchedComplexitySeeds},
        {QStringLiteral("style_filter"), options.styleId},
        {QStringLiteral("profile_filter"), options.profileId},
        {QStringLiteral("full_audio_policy"),
          options.includeAudio
            ? QStringLiteral(
                  "One complete complexity-4 Jam2 audition mix and drum stem per native form")
            : QStringLiteral(
                  "Structure-only corpus; no reference audio rendered")},
    };
    QByteArray prefix =
        QJsonDocument(corpusHeader).toJson(QJsonDocument::Compact);
    if (prefix.isEmpty() || prefix.back() != '}') {
        throw std::runtime_error("Cannot encode the full-form corpus header.");
    }
    prefix.chop(1);
    prefix.append(",\"samples\":[");
    if (!writeChunk(prefix)) {
        throw std::runtime_error("Cannot write the full-form corpus header.");
    }
    bool firstSample = true;
    int sampleCount = 0;
    int audioCount = 0;
    int profileCount = 0;
    QTextStream console(stdout);
    for (const auto& profile :
         jam2::practice::profileCatalog(true)) {
        if (!options.styleId.isEmpty() &&
            profile.styleId != options.styleId) {
            continue;
        }
        if (!options.profileId.isEmpty() &&
            profile.id != options.profileId) {
            continue;
        }
        ++profileCount;
        console << "Generating full forms for "
                << profile.name << "...\n";
        console.flush();
        for (const auto& form : profile.forms) {
            for (int complexity : complexities) {
                for (int sampleIndex = 0;
                     sampleIndex < samplesPerCell;
                     ++sampleIndex) {
                    const bool renderAudio =
                        options.includeAudio &&
                        complexity == 4 &&
                        sampleIndex == 0;
                    const QJsonObject sample = corpusSample(
                        profile, form, complexity, sampleIndex,
                        seedNamespace, artifacts, renderAudio,
                        options.matchedComplexitySeeds);
                    QByteArray encoded =
                        QJsonDocument(sample).toJson(QJsonDocument::Compact);
                    if (!firstSample && !writeChunk(QByteArrayLiteral(","))) {
                        throw std::runtime_error(
                            "Cannot write a full-form corpus sample separator.");
                    }
                    if (!writeChunk(encoded)) {
                        throw std::runtime_error(
                            QStringLiteral("Cannot write full-form corpus sample %1.")
                                .arg(sample.value(QStringLiteral("id")).toString())
                                .toStdString());
                    }
                    firstSample = false;
                    ++sampleCount;
                    if (renderAudio) ++audioCount;
                }
            }
            console << "  Completed " << form.name << ".\n";
            console.flush();
        }
    }
    if (profileCount == 0) {
        throw std::runtime_error(
            "Music corpus filters did not select a profile.");
    }
    if (!writeChunk(QByteArrayLiteral("]}")) || !file.commit()) {
        throw std::runtime_error(
            "Cannot publish the full-form corpus JSON.");
    }
    return {
        {QStringLiteral("event"),
         QStringLiteral("music_full_form_corpus_result")},
        {QStringLiteral("ok"), true},
        {QStringLiteral("corpus"), corpusPath},
        {QStringLiteral("samples"), sampleCount},
        {QStringLiteral("audio_mixes"), audioCount},
        {QStringLiteral("profiles"),
          profileCount},
    };
}
