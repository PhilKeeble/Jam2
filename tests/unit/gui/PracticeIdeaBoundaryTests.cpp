#include "CuratedIdeaDialog.hpp"
#include "PracticeIdeaGenerator.hpp"
#include "PracticeIdeaController.hpp"
#include "PracticeIdeaDialogs.hpp"
#include "PracticeReferenceRenderer.hpp"
#include "ResearchDrumKit.hpp"
#include "StyleProfileCatalog.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace jam2::practice;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

bool allFinite(const QVector<float>& values)
{
    return std::all_of(values.cbegin(), values.cend(), [](float value) {
        return std::isfinite(value);
    });
}

double absoluteEnergy(const QVector<float>& values)
{
    double result = 0.0;
    for (float value : values) result += std::abs(static_cast<double>(value));
    return result;
}

struct DialogInteraction {
    bool seen = false;
    QString error;
};

template <typename Function>
void interactWithNextDialog(DialogInteraction& interaction, Function function)
{
    QTimer::singleShot(0, [&interaction, function = std::move(function)]() mutable {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            interaction.error = QStringLiteral("expected an active modal QDialog");
            return;
        }
        interaction.seen = true;
        try {
            function(*dialog);
        } catch (const std::exception& error) {
            interaction.error = QString::fromUtf8(error.what());
            dialog->reject();
        }
    });
}

QWidget* formField(QDialog& dialog, const QString& label)
{
    for (QFormLayout* form : dialog.findChildren<QFormLayout*>()) {
        for (int row = 0; row < form->rowCount(); ++row) {
            QLayoutItem* labelItem = form->itemAt(row, QFormLayout::LabelRole);
            auto* labelWidget = labelItem == nullptr
                ? nullptr : qobject_cast<QLabel*>(labelItem->widget());
            if (labelWidget == nullptr || labelWidget->text() != label) continue;
            QLayoutItem* fieldItem = form->itemAt(row, QFormLayout::FieldRole);
            if (fieldItem != nullptr && fieldItem->widget() != nullptr) {
                return fieldItem->widget();
            }
        }
    }
    throw std::runtime_error(
        QStringLiteral("missing form field: %1").arg(label).toStdString());
}

template <typename Widget>
Widget* textWidget(QDialog& dialog, const QString& text)
{
    for (Widget* widget : dialog.findChildren<Widget*>()) {
        if (widget->text() == text) return widget;
    }
    throw std::runtime_error(
        QStringLiteral("missing dialog widget: %1").arg(text).toStdString());
}

void clickStandard(QDialog& dialog, QDialogButtonBox::StandardButton button)
{
    auto* box = dialog.findChild<QDialogButtonBox*>();
    require(box != nullptr && box->button(button) != nullptr,
        "dialog must expose its expected standard button");
    box->button(button)->click();
}

void selectData(QComboBox& combo, const QVariant& value, const std::string& context)
{
    const int index = combo.findData(value);
    require(index >= 0, context);
    combo.setCurrentIndex(index);
}

void testCatalogSurface()
{
    const QStringList styles = styleIds();
    const QStringList chordNames = chordStyleNames();
    require(!styles.isEmpty() && chordNames.size() == styles.size() &&
            beatStyleNames() == chordNames,
        "chord and beat selectors expose the complete ordered style catalog");
    require(keyNames().size() == 12 && keyNames().front() == QStringLiteral("C") &&
            keyNames().back() == QStringLiteral("B"),
        "practice keys expose the twelve chromatic names");

    int visitedProfiles = 0;
    for (qsizetype styleIndex = 0; styleIndex < styles.size(); ++styleIndex) {
        const QString& style = styles.at(styleIndex);
        require(styleNameForId(style) == chordNames.at(styleIndex),
            "style identifiers and names retain one-to-one ordering");
        const QStringList profiles = profileIds(style);
        const QStringList names = profileNames(style);
        require(!profiles.isEmpty() && profiles.size() == names.size(),
            "every normal style exposes aligned profile identifiers and names");
        require(grooveFamilyIds(style).size() == grooveFamilyNames(style).size(),
            "groove family identifiers and names remain aligned");
        for (const QString& profileId : profiles) {
            ++visitedProfiles;
            const QStringList forms = nativeFormIds(profileId);
            const QStringList formNames = nativeFormNames(profileId);
            const QStringList meters = meterIds(profileId);
            const QStringList displayedMeters = meterNames(profileId);
            const QStringList productions = productionFamilyIds(profileId);
            const QStringList productionNames = productionFamilyNames(profileId);
            const QStringList modes = modeIds(profileId);
            const QStringList displayedModes = modeNames(profileId);
            require(!forms.isEmpty() && forms.size() == formNames.size() &&
                    !meters.isEmpty() && meters.size() == displayedMeters.size() &&
                    productions.size() == productionNames.size() &&
                    modes.size() == displayedModes.size(),
                "profile selectors expose aligned forms, meters, production, and mode labels");
            const QStringList compatibleMeters = compatibleMeterIds(style, profileId);
            const QStringList compatibleNames = compatibleMeterNames(style, profileId);
            require(compatibleMeters.size() == meters.size() &&
                    compatibleNames.size() == compatibleMeters.size() &&
                    std::all_of(compatibleMeters.cbegin(), compatibleMeters.cend(),
                        [&](const QString& meter) { return meters.contains(meter); }),
                "selected-profile compatible meters preserve exact profile membership");
            for (qsizetype meterIndex = 0;
                 meterIndex < compatibleMeters.size(); ++meterIndex) {
                const MeterDefinition* meter = findMeter(compatibleMeters.at(meterIndex));
                require(meter != nullptr && meter->name == compatibleNames.at(meterIndex),
                    "compatible meter labels follow global presentation order");
            }
            for (const QString& meter : meters) {
                const QVector<int> bars = compatibleBarCounts(style, profileId, meter);
                require(!bars.isEmpty() && std::is_sorted(bars.cbegin(), bars.cend()) &&
                        std::all_of(bars.cbegin(), bars.cend(), [](int value) {
                            return value > 0;
                        }),
                    "compatible bar counts are positive, sorted, and nonempty");
            }
        }
    }
    require(visitedProfiles >= styles.size(),
        "catalog walk reaches at least one profile per style");
    require(styleNameForId(QStringLiteral("metal-experimental")) ==
                QStringLiteral("Experimental Metal") &&
            styleNameForId(QStringLiteral("missing-style")).isEmpty(),
        "experimental and unknown style-name boundaries are explicit");
    require(nativeFormIds(QStringLiteral("missing-profile")).isEmpty() &&
            nativeFormNames(QStringLiteral("missing-profile")).isEmpty() &&
            meterIds(QStringLiteral("missing-profile")).isEmpty() &&
            meterNames(QStringLiteral("missing-profile")).isEmpty() &&
            productionFamilyIds(QStringLiteral("missing-profile")).isEmpty() &&
            productionFamilyNames(QStringLiteral("missing-profile")).isEmpty() &&
            modeIds(QStringLiteral("missing-profile")).isEmpty() &&
            modeNames(QStringLiteral("missing-profile")).isEmpty(),
        "unknown profiles expose no invented selector values");
    require(!compatibleMeterIds(
                QStringLiteral("missing-style"), QStringLiteral("missing-profile")).isEmpty() &&
            !compatibleMeterNames(
                QStringLiteral("missing-style"), QStringLiteral("missing-profile")).isEmpty() &&
            !compatibleBarCounts(QStringLiteral("missing-style"),
                QStringLiteral("missing-profile"), QStringLiteral("4-4")).isEmpty(),
        "unselected style/profile selectors fall back to the bounded normal catalog");

    const ProfileDefinition& firstProfile = profileCatalog().front();
    QString unsupportedMeter;
    for (const MeterDefinition& meter : meterCatalog()) {
        if (!firstProfile.meterIds.contains(meter.id)) {
            unsupportedMeter = meter.id;
            break;
        }
    }
    require(!unsupportedMeter.isEmpty() &&
            !compatibleBarCounts(firstProfile.styleId, firstProfile.id,
                unsupportedMeter).isEmpty(),
        "unsupported explicit meter falls back to the selected profile's native forms");
    require(findComplexityLevel(1) != nullptr &&
            findComplexityLevel(8) != nullptr &&
            findComplexityLevel(0) == nullptr &&
            findComplexityLevel(9) == nullptr,
        "complexity lookup retains its exact supported range");
}

ChordIdeaRequest representativeRequest()
{
    for (const StyleDefinition& style : styleCatalog()) {
        for (const QString& profileId : style.profileIds) {
            const ProfileDefinition* profile = findProfile(profileId);
            if (profile == nullptr || profile->forms.isEmpty() ||
                profile->meterIds.isEmpty()) {
                continue;
            }
            ChordIdeaRequest request;
            request.key = 5;
            request.styleId = style.id;
            request.profileId = profile->id;
            request.formId = profile->forms.front().id;
            request.meterId = profile->meterIds.front();
            request.productionFamilyId =
                compatibleProductionFamilyIds(*profile).value(0);
            request.modeId = profile->tonalCollections.value(0);
            request.bpm = (profile->minimumBpm + profile->maximumBpm) / 2;
            request.bars = profile->forms.front().bars;
            if (const MeterDefinition* meter = findMeter(request.meterId)) {
                request.beatsPerBar = meter->numerator;
            }
            request.harmonicComplexity = 4;
            request.rhythmicComplexity = 4;
            return request;
        }
    }
    throw std::runtime_error("normal catalog has no generatable profile");
}

void requireGenerated(const GeneratedPracticeIdea& idea, const std::string& context)
{
    require(idea.bpm >= 20 && idea.bpm <= 400 && idea.meterNumerator >= 2 &&
            idea.meterDenominator > 0 && idea.recipe.bars > 0 &&
            !idea.recipe.formSections.isEmpty() &&
            (idea.chordSection.generatedKind == QStringLiteral("chord") ||
             idea.chordSection.generatedKind == QStringLiteral("practice")) &&
            (idea.beatSection.generatedKind == QStringLiteral("beat") ||
             idea.beatSection.generatedKind == QStringLiteral("practice")) &&
            !generatedChordFingerprint(idea.chordSection).isEmpty() &&
            !generatedBeatFingerprint(idea.beatSection).isEmpty(),
        context);
}

void testGenerationWrappers()
{
    const ChordIdeaRequest request = representativeRequest();
    constexpr std::uint32_t seed = 0x5049434bU;
    const GeneratedPracticeIdea first = generateCoupledPracticeIdeaForTest(request, seed);
    const GeneratedPracticeIdea second = generateCoupledPracticeIdeaSeeded(request, seed);
    requireGenerated(first, "seeded coupled generator emits complete bounded content");
    require(generatedChordFingerprint(first.chordSection) ==
                generatedChordFingerprint(second.chordSection) &&
            generatedBeatFingerprint(first.beatSection) ==
                generatedBeatFingerprint(second.beatSection),
        "test and product seeded wrappers are byte-shape deterministic");
    const SongSection chord = generateChordIdeaForTest(request, seed);
    require(generatedChordFingerprint(chord) ==
                generatedChordFingerprint(first.chordSection),
        "chord-only test wrapper owns the coupled chord result");

    BeatIdeaRequest beatRequest;
    beatRequest.styleId = request.styleId;
    beatRequest.profileId = request.profileId;
    beatRequest.formId = request.formId;
    beatRequest.meterId = request.meterId;
    beatRequest.productionFamilyId = request.productionFamilyId;
    beatRequest.bpm = request.bpm;
    beatRequest.bars = request.bars;
    beatRequest.beatsPerBar = request.beatsPerBar;
    beatRequest.rhythmicComplexity = request.rhythmicComplexity;
    const SongSection beat = generateBeatIdeaForTest(beatRequest, seed);
    require(generatedBeatFingerprint(beat) ==
                generatedBeatFingerprint(first.beatSection),
        "beat-only test wrapper owns the coupled beat result");

    ChordIdeaRequest randomRequest;
    randomRequest.key = -1;
    randomRequest.styleId = QStringLiteral("missing-style");
    randomRequest.profileId = QStringLiteral("missing-profile");
    randomRequest.formId = QStringLiteral("missing-form");
    randomRequest.meterId = QStringLiteral("missing-meter");
    randomRequest.productionFamilyId = QStringLiteral("missing-production");
    randomRequest.modeId = QStringLiteral("missing-mode");
    randomRequest.bpm = -1;
    randomRequest.bars = 7;
    randomRequest.harmonicComplexity = 99;
    randomRequest.rhythmicComplexity = -10;
    const GeneratedPracticeIdea randomFirst =
        generateCoupledPracticeIdeaForTest(randomRequest, 71);
    const GeneratedPracticeIdea randomSecond =
        generateCoupledPracticeIdeaForTest(randomRequest, 71);
    requireGenerated(randomFirst,
        "invalid optional selections resolve through bounded catalog choices");
    require(generatedChordFingerprint(randomFirst.chordSection) ==
                generatedChordFingerprint(randomSecond.chordSection) &&
            generatedBeatFingerprint(randomFirst.beatSection) ==
                generatedBeatFingerprint(randomSecond.beatSection),
        "random-choice fallback remains deterministic under an explicit test seed");

    const GeneratedPracticeIdea unseeded = generateCoupledPracticeIdea(request);
    requireGenerated(unseeded, "public random generator emits complete bounded content");

    SongSection combined = first.chordSection;
    combined.beatNotes = first.beatSection.beatNotes;
    combined.beatPatterns = first.beatSection.beatPatterns;
    ContinueIdeaRequest continuationRequest;
    continuationRequest.sourceSectionIndex = 0;
    continuationRequest.targetSectionIndex = 3;
    continuationRequest.bpm = first.bpm;
    continuationRequest.meterId = first.recipe.meterId;
    continuationRequest.beatsPerBar = first.meterNumerator;
    continuationRequest.beatUnit = first.meterDenominator;
    continuationRequest.tempoPulseUnits = first.tempoPulseUnits;
    const GeneratedContinuationIdea continued =
        generateContinuationPracticeIdeaForTest(combined, continuationRequest, 91);
    requireGenerated(continued.idea, "seeded continuation emits a complete B section");
    require(continued.analysis.candidateCount > 0 &&
            !continued.analysis.relationshipId.isEmpty() &&
            !continued.idea.recipe.formSections.isEmpty() &&
            continued.idea.chordSection.label == QStringLiteral("D") &&
            continued.idea.beatSection.label == QStringLiteral("D"),
        "continuation reports selection evidence and owns its requested target section");
    requireGenerated(generateContinuationPracticeIdea(combined, continuationRequest).idea,
        "public random continuation wrapper emits complete bounded content");
}

ResearchDrumPiece basePiece(const QString& source)
{
    ResearchDrumPiece piece;
    piece.source = source;
    piece.secondSource = QStringLiteral("off");
    piece.frequencyHz = 160.0f;
    piece.decay = 0.35f;
    piece.tone = 0.6f;
    piece.colour = 0.7f;
    piece.fmAmount = 0.25f;
    piece.level = 0.8f;
    piece.voiceDrive = 1.2f;
    piece.transientType = QStringLiteral("off");
    piece.textureType = QStringLiteral("off");
    piece.synthSource = QStringLiteral("off");
    return piece;
}

double sampleEnergy(
    const ResearchDrumPiece& piece,
    const QString& articulation,
    std::uint32_t seed = 17)
{
    double energy = 0.0;
    for (qint64 age = 0; age < 512; ++age) {
        const double sample = researchDrumSample(
            piece, articulation, 104, seed, age, 48'000);
        require(std::isfinite(sample), "legacy research drum sample must remain finite");
        energy += std::abs(sample);
    }
    return energy;
}

void testResearchDrumSamples()
{
    const std::array<std::pair<const char*, const char*>, 11> sources{{
        {"daisy-analog-kick", "normal"},
        {"jam2-shell-snare", "normal"},
        {"jam2-shell-tom", "normal"},
        {"jam2-cross-stick", "normal"},
        {"jam2-hand-clap", "normal"},
        {"jam2-crash-cymbal", "normal"},
        {"jam2-ride-cymbal", "bell"},
        {"jam2-ride-cymbal", "edge"},
        {"jam2-ride-cymbal", "bow"},
        {"daisy-ring-metal", "normal"},
        {"future-skin", "normal"},
    }};
    for (const auto& [source, articulation] : sources) {
        require(sampleEnergy(basePiece(QString::fromLatin1(source)),
                    QString::fromLatin1(articulation)) > 0.0,
            std::string("research source must produce energy: ") + source);
    }
    require(sampleEnergy(basePiece(QStringLiteral("off")),
                QStringLiteral("normal")) == 0.0,
        "disabled source produces exact silence");

    for (const QString& transient : {
             QStringLiteral("soft-beater"), QStringLiteral("hard-beater"),
             QStringLiteral("rim"), QStringLiteral("head-strike"),
             QStringLiteral("future-transient")}) {
        ResearchDrumPiece piece = basePiece(QStringLiteral("off"));
        piece.transientType = transient;
        piece.transientLevel = 0.8f;
        piece.transientTone = 0.6f;
        require(sampleEnergy(piece, QStringLiteral("normal")) > 0.0,
            "enabled transient family produces finite energy");
    }
    for (const QString& texture : {
             QStringLiteral("wire"), QStringLiteral("dust"),
             QStringLiteral("metal-wash"), QStringLiteral("future-texture")}) {
        ResearchDrumPiece piece = basePiece(QStringLiteral("off"));
        piece.textureType = texture;
        piece.textureLevel = 0.8f;
        piece.textureDensity = 0.75f;
        require(sampleEnergy(piece, QStringLiteral("normal")) > 0.0,
            "enabled texture family produces finite energy");
    }
    ResearchDrumPiece blended = basePiece(QStringLiteral("daisy-analog-kick"));
    blended.secondSource = QStringLiteral("jam2-shell-snare");
    blended.blend = 0.4f;
    require(sampleEnergy(blended, QStringLiteral("normal")) > 0.0,
        "equal-power second-source blend produces energy");
    ResearchDrumPiece synthetic = basePiece(QStringLiteral("off"));
    synthetic.synthSource = QStringLiteral("sine-fundamental");
    synthetic.synthLevel = 0.8f;
    synthetic.synthMidiNote = 48;
    require(sampleEnergy(synthetic, QStringLiteral("normal")) > 0.0,
        "legacy sine layer produces energy");
    const double deterministic = researchDrumSample(
        blended, QStringLiteral("normal"), 100, 9, 37, 48'000);
    require(deterministic == researchDrumSample(
                blended, QStringLiteral("normal"),
                100, 9, 37, 48'000),
        "legacy sample is exactly deterministic for recipe seed and age");

    ResearchDrumPiece generic = basePiece(QStringLiteral("future-skin"));
    ResearchDrumPiece kick = basePiece(QStringLiteral("daisy-analog-kick"));
    ResearchDrumPiece crash = basePiece(QStringLiteral("jam2-crash-cymbal"));
    ResearchDrumPiece ride = basePiece(QStringLiteral("jam2-ride-cymbal"));
    ResearchDrumPiece metal = basePiece(QStringLiteral("daisy-metal"));
    ResearchDrumPiece wood = basePiece(QStringLiteral("jam2-wood-block"));
    ResearchDrumPiece tom = basePiece(QStringLiteral("jam2-shell-tom"));
    require(researchDrumTailSeconds(kick, QStringLiteral("normal")) >
                researchDrumTailSeconds(generic, QStringLiteral("normal")) &&
            researchDrumTailSeconds(crash, QStringLiteral("normal")) >
                researchDrumTailSeconds(kick, QStringLiteral("normal")) &&
            researchDrumTailSeconds(ride, QStringLiteral("bell")) >
                researchDrumTailSeconds(ride, QStringLiteral("edge")) &&
            researchDrumTailSeconds(metal, QStringLiteral("normal")) > 0.0 &&
            researchDrumTailSeconds(wood, QStringLiteral("normal")) > 0.0 &&
            researchDrumTailSeconds(tom, QStringLiteral("normal")) > 0.0,
        "tail estimator distinguishes kick, crash, ride, metal, wood, tom, and generic sources");
    generic.decay = 100.0f;
    generic.transientDecaySeconds = 100.0f;
    generic.textureDecaySeconds = 100.0f;
    generic.synthSource = QStringLiteral("sine-fundamental");
    generic.synthDecaySeconds = 100.0f;
    generic.synthReleaseSeconds = 100.0f;
    require(researchDrumTailSeconds(generic, QStringLiteral("normal")) == 6.0,
        "tail estimator applies its six-second safety cap");
}

void testResearchDrumCatalogAndEngine()
{
    const ResearchDrumKit* acoustic = researchDrumKitForBase(QStringLiteral("acoustic"));
    const ResearchDrumKit* electronic = researchDrumKitForBase(QStringLiteral("electronic"));
    require(acoustic != nullptr && electronic != nullptr && acoustic->pieces.size() == 10 &&
            electronic->pieces.size() == 10 &&
            researchDrumKitForBase(QStringLiteral("missing")) == nullptr &&
            researchDrumKitById(QStringLiteral("missing")) == nullptr,
        "embedded research catalog exposes two complete bases and rejects unknown IDs");
    require(researchDrumKitById(acoustic->id) == acoustic &&
            researchDrumPiece(*acoustic, QStringLiteral("closed_hat")) != nullptr &&
            researchDrumPiece(*acoustic, QStringLiteral("missing")) == nullptr,
        "kit ID and underscore-normalized lane lookup preserve stable ownership");
    const ResearchDrumKit* profileKit = nullptr;
    for (const ProfileDefinition& profile : profileCatalog()) {
        if ((profileKit = researchDrumKitForProfile(profile.id)) != nullptr) break;
    }
    require(profileKit != nullptr &&
            researchDrumKitForProfile(QStringLiteral("missing")) == nullptr,
        "at least one normal profile resolves its embedded researched kit");

    require(!researchDrumSourceSupportsLane(QStringLiteral("kick"), {}) &&
            researchDrumSourceSupportsLane(
                QStringLiteral("kick"), QStringLiteral("daisy-analog-kick")) &&
            researchDrumSourceSupportsLane(
                QStringLiteral("snare"), QStringLiteral("jam2-hand-clap")) &&
            researchDrumSourceSupportsLane(
                QStringLiteral("closed_hat"), QStringLiteral("daisy-metal")) &&
            researchDrumSourceSupportsLane(
                QStringLiteral("floor_tom"), QStringLiteral("jam2-shell-tom")) &&
            researchDrumSourceSupportsLane(
                QStringLiteral("crash"), QStringLiteral("jam2-crash-cymbal")) &&
            researchDrumSourceSupportsLane(
                QStringLiteral("ride"), QStringLiteral("jam2-wood-block")) &&
            researchDrumSourceSupportsLane(
                QStringLiteral("cross_stick"), QStringLiteral("jam2-cross-stick")) &&
            !researchDrumSourceSupportsLane(
                QStringLiteral("cowbell"), QStringLiteral("daisy-metal")),
        "lane/source validation covers every supported lane family and unknown rejection");

    ResearchDrumKit synthKit;
    synthKit.id = QStringLiteral("test-synth-kit");
    ResearchDrumPiece synth = basePiece(QStringLiteral("off"));
    synth.synthSource = QStringLiteral("sine-fundamental");
    synth.synthLevel = 0.9f;
    synth.synthMidiNote = 48;
    synth.synthGateSeconds = 0.02f;
    synth.synthAttackSeconds = 0.001f;
    synth.synthDecaySeconds = 0.03f;
    synth.synthSustain = 0.1f;
    synth.synthReleaseSeconds = 0.04f;
    synth.roomSend = 0.4f;
    synth.modalBands.push_back({120.0f, 0.0f, 0.15f, 0.04f});
    synth.noiseBands.push_back({3200.0f, 1.2f, 0.08f, 0.025f});
    synthKit.pieces.insert(QStringLiteral("kick"), synth);
    QVector<ResearchDrumRenderEvent> events;
    for (int index = 0; index < 40; ++index) {
        events.push_back({0, QStringLiteral("kick"), QStringLiteral("normal"),
            80 + index % 40, 0, static_cast<std::uint32_t>(index + 1)});
    }
    events.push_back({16, QStringLiteral("unknown"), {}, 100, 0, 99});
    require(renderResearchDrumVoices(synthKit, events, 0, 48'000).dry.isEmpty() &&
            renderResearchDrumVoices(synthKit, events, 100, 0).dry.isEmpty(),
        "research renderer rejects nonpositive frame and sample-rate shapes");
    const ResearchDrumRenderResult first =
        renderResearchDrumVoices(synthKit, events, 4096, 48'000, true);
    const ResearchDrumRenderResult second =
        renderResearchDrumVoices(synthKit, events, 4096, 48'000, true);
    require(first.dry.size() == 4096 && first.roomSend.size() == 4096 &&
            allFinite(first.dry) && allFinite(first.roomSend) &&
            absoluteEnergy(first.dry) > 0.0 && absoluteEnergy(first.roomSend) > 0.0 &&
            first.dry == second.dry && first.roomSend == second.roomSend &&
            first.setupUs >= 0 && first.voicesUs >= 0 && first.detailBanksUs >= 0 &&
            first.modalDetailSamples > 0 && first.noiseDetailSamples > 0,
        "synth/detail renderer is finite, nonzero, timed, and exactly deterministic");

    QVector<float> empty;
    applyResearchDrumBus(empty, {}, synthKit.bus, 48'000);
    QVector<float> invalidRate{0.5f, -0.25f};
    const QVector<float> invalidBefore = invalidRate;
    applyResearchDrumBus(invalidRate, {}, synthKit.bus, 0);
    require(invalidRate == invalidBefore, "research bus rejects an invalid sample rate");
    QVector<float> mixed = first.dry;
    ResearchDrumBus bus;
    bus.drive = 1.8f;
    bus.lowpassHz = 9000.0f;
    bus.compressorThreshold = 0.03f;
    bus.compressorRatio = 4.0f;
    bus.compressorReleaseMs = 35.0f;
    bus.roomMix = 0.35f;
    bus.roomSizeMs = 19.0f;
    bus.roomDamping = 0.45f;
    applyResearchDrumBus(mixed, first.roomSend.mid(0, 3000), bus, 48'000);
    require(mixed.size() == first.dry.size() && allFinite(mixed) &&
            mixed != first.dry && absoluteEnergy(mixed) > 0.0,
        "research bus applies bounded room, drive, low-pass, and compression in place");
}

void testPracticeIdeaDialog()
{
    PracticeIdeaDialogDefaults defaults;
    defaults.bpm = 112;
    defaults.meterId = QStringLiteral("4-4");
    defaults.bars = 8;
    defaults.targetSectionIndex = 1;
    defaults.bankBpms = {92, 112, 155};
    defaults.bankMeterIds = {
        QStringLiteral("4-4"), QStringLiteral("4-4"), QStringLiteral("13-8")};
    defaults.bankBars = {4, 8, 7};
    defaults.exactBpm = false;
    defaults.complexity = 3;
    const ChordIdeaRequest representative = representativeRequest();
    defaults.styleId = representative.styleId;
    defaults.profileId = representative.profileId;
    defaults.preferredMeterId = representative.meterId;
    defaults.preferredBars = representative.bars;

    DialogInteraction cancelled;
    interactWithNextDialog(cancelled, [](QDialog& dialog) {
        require(dialog.windowTitle() == QStringLiteral("Generate Practice Idea"),
            "practice dialog exposes its exact title");
        clickStandard(dialog, QDialogButtonBox::Cancel);
    });
    require(!askForPracticeIdea(nullptr, defaults).has_value() &&
            cancelled.seen && cancelled.error.isEmpty(),
        "practice dialog cancellation returns no request");

    DialogInteraction accepted;
    interactWithNextDialog(accepted,
        [representative](QDialog& dialog) {
            auto* section = qobject_cast<QComboBox*>(formField(dialog, QStringLiteral("Section")));
            auto* parts = qobject_cast<QComboBox*>(formField(dialog, QStringLiteral("Parts")));
            auto* key = qobject_cast<QComboBox*>(formField(dialog, QStringLiteral("Key")));
            auto* style = qobject_cast<QComboBox*>(formField(dialog, QStringLiteral("Style")));
            auto* profile = qobject_cast<QComboBox*>(formField(dialog, QStringLiteral("Profile")));
            auto* meter = qobject_cast<QComboBox*>(formField(dialog, QStringLiteral("Meter")));
            auto* length = qobject_cast<QComboBox*>(formField(dialog, QStringLiteral("Length")));
            auto* complexity = qobject_cast<QComboBox*>(
                formField(dialog, QStringLiteral("Complexity")));
            QWidget* tempo = formField(dialog, QStringLiteral("Tempo"));
            auto* exact = tempo->findChild<QCheckBox*>();
            auto* bpm = tempo->findChild<QSpinBox*>();
            require(section != nullptr && parts != nullptr && key != nullptr &&
                    style != nullptr && profile != nullptr && meter != nullptr &&
                    length != nullptr && complexity != nullptr && exact != nullptr &&
                    bpm != nullptr,
                "practice dialog exposes every requested form control");

            selectData(*style, QStringLiteral("metal-experimental"),
                "experimental style option must exist");
            require(profile->currentData().toString() ==
                    QStringLiteral("metal_modern_progressive"),
                "experimental style selects its explicit sound-test profile");
            selectData(*style, representative.styleId,
                "representative style must remain selectable after experimental refresh");
            selectData(*profile, representative.profileId,
                "representative profile must repopulate after style refresh");
            selectData(*section, 2, "third target section must exist");
            selectData(*parts, static_cast<int>(PracticeIdeaParts::PitchedPartsOnly),
                "pitched-only option must exist");
            require(exact->isChecked() && bpm->isEnabled() && bpm->value() == 155,
                "partial generation adopts and locks the selected bank BPM explicitly");
            selectData(*meter, QStringLiteral("13-8"),
                "current project meter must remain explicitly selectable");
            selectData(*length, 7, "current section length must remain selectable");
            selectData(*key, 9, "explicit key must remain selectable");
            selectData(*complexity, 8, "maximum complexity must remain selectable");
            auto* box = dialog.findChild<QDialogButtonBox*>();
            require(box != nullptr && box->button(QDialogButtonBox::Ok)->text() ==
                    QStringLiteral("Generate"),
                "practice dialog labels its acceptance action Generate");
            box->button(QDialogButtonBox::Ok)->click();
        });
    const std::optional<ChordIdeaRequest> request = askForPracticeIdea(nullptr, defaults);
    require(accepted.seen && accepted.error.isEmpty() && request.has_value() &&
            request->targetSectionIndex == 2 &&
            request->parts == PracticeIdeaParts::PitchedPartsOnly &&
            request->key == 9 && request->styleId == representative.styleId &&
            request->profileId == representative.profileId &&
            request->meterId == QStringLiteral("13-8") && request->allowMeterOverride &&
            request->bpm == 155 && request->bars == 7 &&
            request->harmonicComplexity == 8 && request->rhythmicComplexity == 8,
        "accepted practice dialog returns exact bank, parts, key, profile, override, tempo, length, and complexity");
}

void testContinuationDialog()
{
    ContinueIdeaDialogDefaults defaults;
    defaults.sourceSectionIndex = 1;
    defaults.targetSectionIndex = 1;
    defaults.bankHasContent = {true, false, true};
    defaults.bankNames = {
        QStringLiteral("Verse"), QStringLiteral(""), QStringLiteral("Chorus")};
    DialogInteraction cancelled;
    interactWithNextDialog(cancelled, [](QDialog& dialog) {
        clickStandard(dialog, QDialogButtonBox::Cancel);
    });
    require(!askForIdeaContinuation(nullptr, defaults).has_value() &&
            cancelled.seen && cancelled.error.isEmpty(),
        "continuation dialog cancellation returns no request");

    DialogInteraction accepted;
    interactWithNextDialog(accepted, [](QDialog& dialog) {
        require(dialog.windowTitle() == QStringLiteral("Continue Idea"),
            "continuation dialog exposes its exact title");
        auto* source = qobject_cast<QComboBox*>(
            formField(dialog, QStringLiteral("Source Section")));
        auto* target = qobject_cast<QComboBox*>(
            formField(dialog, QStringLiteral("Target Section")));
        auto* box = dialog.findChild<QDialogButtonBox*>();
        require(source != nullptr && target != nullptr && box != nullptr &&
                !box->button(QDialogButtonBox::Ok)->isEnabled(),
            "same or empty source initially disables continuation");
        selectData(*source, 2, "content-bearing source section must exist");
        selectData(*target, 0, "different target section must exist");
        require(box->button(QDialogButtonBox::Ok)->isEnabled() &&
                box->button(QDialogButtonBox::Ok)->text() == QStringLiteral("Continue"),
            "different content source enables the labelled continuation action");
        box->button(QDialogButtonBox::Ok)->click();
    });
    const std::optional<ContinueIdeaRequest> request =
        askForIdeaContinuation(nullptr, defaults);
    require(accepted.seen && accepted.error.isEmpty() && request.has_value() &&
            request->sourceSectionIndex == 2 && request->targetSectionIndex == 0,
        "accepted continuation returns exact distinct source and target ownership");
}

void testReferenceDialog()
{
    ReferenceRenderSettings defaults;
    defaults.renderChords = true;
    defaults.renderDrums = true;
    defaults.renderMelody = true;
    defaults.renderBass = true;
    defaults.renderSupport = true;
    defaults.bpm = 123.5;
    defaults.sampleRate = 48'000;
    defaults.tempoPulseUnits = 1;

    DialogInteraction cancelled;
    interactWithNextDialog(cancelled, [](QDialog& dialog) {
        const QList<QLabel*> labels = dialog.findChildren<QLabel*>();
        require(std::any_of(labels.cbegin(), labels.cend(), [](const QLabel* label) {
                    return label->text().contains(QStringLiteral("beats")) &&
                        label->text().contains(QStringLiteral("frames"));
                }),
            "single-section reference summary exposes beats, seconds, and frames");
        clickStandard(dialog, QDialogButtonBox::Cancel);
    });
    require(!askForReferenceRender(nullptr, defaults, 8, 8, 4, 4, 4).has_value() &&
            cancelled.seen && cancelled.error.isEmpty(),
        "reference dialog cancellation returns no settings");

    DialogInteraction emptyAccepted;
    interactWithNextDialog(emptyAccepted, [](QDialog& dialog) {
        for (QCheckBox* check : dialog.findChildren<QCheckBox*>()) {
            require(!check->isEnabled() && !check->isChecked(),
                "zero-beat reference layer is disabled and clear");
        }
        clickStandard(dialog, QDialogButtonBox::Ok);
    });
    require(!askForReferenceRender(nullptr, defaults, 0, 0, 0, 0, 0).has_value() &&
            emptyAccepted.seen && emptyAccepted.error.isEmpty(),
        "accepted reference dialog with no selected available layer returns no settings");

    DialogInteraction accepted;
    interactWithNextDialog(accepted, [](QDialog& dialog) {
        const QList<QLabel*> labels = dialog.findChildren<QLabel*>();
        require(std::any_of(labels.cbegin(), labels.cend(), [](const QLabel* label) {
                    return label->text().contains(QStringLiteral("4 sections")) &&
                        label->text().contains(QStringLiteral("Sections A-D"));
                }),
            "multi-section reference summary exposes its A-D scope");
        auto* drums = textWidget<QCheckBox>(dialog, QStringLiteral("Drum reference"));
        auto* chords = textWidget<QCheckBox>(dialog, QStringLiteral("Chord reference"));
        auto* melody = textWidget<QCheckBox>(dialog, QStringLiteral("Melody reference"));
        auto* bass = textWidget<QCheckBox>(dialog, QStringLiteral("Bass reference"));
        auto* support = textWidget<QCheckBox>(
            dialog, QStringLiteral("Supporting line reference"));
        require(drums->isEnabled() && chords->isEnabled() && melody->isEnabled() &&
                !bass->isEnabled() && support->isEnabled(),
            "layer availability follows each source beat count independently");
        drums->setChecked(false);
        chords->setChecked(true);
        melody->setChecked(true);
        support->setChecked(true);
        auto* voicing = qobject_cast<QComboBox*>(formField(dialog, QStringLiteral("Voicing")));
        auto* kit = qobject_cast<QComboBox*>(formField(dialog, QStringLiteral("Drum Kit")));
        require(voicing != nullptr && kit != nullptr,
            "reference dialog exposes voicing and drum-kit selectors");
        selectData(*voicing, static_cast<int>(ChordVoicing::Spread),
            "spread voicing must be selectable");
        selectData(*kit, static_cast<int>(ReferenceDrumKit::Electronic),
            "electronic reference kit must be selectable");
        auto* box = dialog.findChild<QDialogButtonBox*>();
        require(box->button(QDialogButtonBox::Ok)->text() == QStringLiteral("Render"),
            "reference dialog labels its acceptance action Render");
        box->button(QDialogButtonBox::Ok)->click();
    });
    const std::optional<ReferenceRenderSettings> result =
        askForReferenceRender(nullptr, defaults, 8, 12, 4, 0, 16, 4);
    require(accepted.seen && accepted.error.isEmpty() && result.has_value() &&
            result->renderChords && !result->renderDrums && result->renderMelody &&
            !result->renderBass && result->renderSupport &&
            result->voicing == ChordVoicing::Spread &&
            result->drumKit == ReferenceDrumKit::Electronic &&
            result->sampleRate == defaults.sampleRate && result->bpm == defaults.bpm,
        "accepted reference dialog preserves exact layers, voicing, kit, and render shape");
}

void testCuratedIdeaDialog()
{
    CuratedIdeaDialogDefaults defaults;
    defaults.targetSectionIndex = 1;
    defaults.timing = CuratedIdeaTimingPolicy::UseIdeaTiming;
    defaults.importBars = 12;
    defaults.bankBpms = {90, 123};
    defaults.bankMeterNumerators = {4, 7};
    defaults.bankMeterDenominators = {4, 8};
    defaults.bankBeats = {16, 28};

    CuratedIdeaPreviewCallbacks unavailable;
    unavailable.unavailableReason = QStringLiteral("preview unavailable in this fixture");
    DialogInteraction cancelled;
    interactWithNextDialog(cancelled, [](QDialog& dialog) {
        require(dialog.windowTitle() == QStringLiteral("Groove Library"),
            "curated dialog exposes its exact title");
        auto* preview = textWidget<QPushButton>(dialog, QStringLiteral("PREVIEW 4 BARS"));
        require(!preview->isEnabled(),
            "unavailable curated preview remains explicitly disabled");
        clickStandard(dialog, QDialogButtonBox::Cancel);
    });
    require(!askForCuratedIdea(nullptr, defaults, unavailable).has_value() &&
            cancelled.seen && cancelled.error.isEmpty(),
        "curated dialog cancellation returns no selection");

    int previewStarts = 0;
    int previewStops = 0;
    CuratedIdeaPreviewCallbacks preview;
    preview.available = true;
    preview.play = [&previewStarts](const CuratedIdeaEntry&, QString&) {
        ++previewStarts;
        return true;
    };
    preview.stop = [&previewStops] { ++previewStops; };
    DialogInteraction accepted;
    interactWithNextDialog(accepted, [](QDialog& dialog) {
        auto* style = qobject_cast<QComboBox*>(formField(dialog, QStringLiteral("Style")));
        auto* profile = qobject_cast<QComboBox*>(formField(dialog, QStringLiteral("Profile")));
        auto* target = qobject_cast<QComboBox*>(formField(dialog, QStringLiteral("Target")));
        auto* length = qobject_cast<QComboBox*>(
            formField(dialog, QStringLiteral("Groove length")));
        auto* list = dialog.findChild<QListWidget*>();
        require(style != nullptr && profile != nullptr && target != nullptr &&
                length != nullptr && list != nullptr && list->count() > 0,
            "curated dialog exposes filters, target, length, and catalog");
        if (style->count() > 1) style->setCurrentIndex(1);
        if (profile->count() > 1) profile->setCurrentIndex(1);
        require(list->count() > 0,
            "curated style/profile filtering preserves matching entries");
        list->setCurrentRow(0);
        target->setCurrentIndex(0);
        selectData(*length, 12, "curated 12-bar import option must exist");
        for (QRadioButton* radio : dialog.findChildren<QRadioButton*>()) {
            if (radio->text().startsWith(QStringLiteral("Keep Section"))) {
                radio->setChecked(true);
            }
        }
        auto* previewButton = textWidget<QPushButton>(
            dialog, QStringLiteral("PREVIEW 4 BARS"));
        previewButton->click();
        require(previewButton->text() == QStringLiteral("STOP PREVIEW"),
            "curated preview enters its playing state");
        previewButton->click();
        auto* box = dialog.findChild<QDialogButtonBox*>();
        require(box != nullptr && box->button(QDialogButtonBox::Ok)->isEnabled() &&
                box->button(QDialogButtonBox::Ok)->text() == QStringLiteral("Use Groove"),
            "curated selection enables its labelled acceptance action");
        box->button(QDialogButtonBox::Ok)->click();
    });
    const std::optional<CuratedIdeaSelection> selection =
        askForCuratedIdea(nullptr, defaults, preview);
    require(accepted.seen && accepted.error.isEmpty() && selection.has_value() &&
            selection->targetSectionIndex == 0 && selection->importBars == 12 &&
            selection->timing == CuratedIdeaTimingPolicy::KeepSectionTiming &&
            previewStarts == 1 && previewStops == 1,
        "curated dialog returns exact selection and balances preview lifecycle");
}

void testCuratedIdeaSeedReproducibility()
{
    QString error;
    const QVector<CuratedIdeaEntry> catalog = loadCuratedIdeaCatalog(error);
    require(error.isEmpty() && !catalog.isEmpty() &&
            catalog.front().id == QStringLiteral("pop_loop__groove-1"),
        "curated seed regression resolves the maintained first catalog entry");
    for (const CuratedIdeaEntry& entry : catalog) {
        const GeneratedPracticeIdea idea = generateCoupledPracticeIdeaSeeded(
            entry.generationRequest(), entry.seed);
        require(generatedChordFingerprint(idea.chordSection) == entry.chordFingerprint &&
                generatedBeatFingerprint(idea.beatSection) == entry.beatFingerprint,
            QStringLiteral(
                "curated seeded generation reproduces the cross-platform stored "
                "fingerprints for %1")
                .arg(entry.id)
                .toStdString());
    }
}

void testReferenceSignaturesAndKeyAwareKit()
{
    const GeneratedPracticeIdea idea = generateCoupledPracticeIdeaForTest(
        representativeRequest(), 905);
    ReferenceRenderSettings settings;
    settings.renderChords = true;
    settings.renderDrums = true;
    settings.renderBass = true;
    settings.bpm = idea.bpm;
    settings.meterNumerator = idea.meterNumerator;
    settings.meterDenominator = idea.meterDenominator;
    settings.sampleRate = 48'000;
    const QString first = practiceReferenceSignature(
        &idea.chordSection, &idea.beatSection, settings);
    settings.bassLevel -= 0.5;
    const QString second = practiceReferenceSignature(
        &idea.chordSection, &idea.beatSection, settings);
    require(first.size() == 64 && second.size() == 64 && first != second,
        "reference signature is stable-width and includes render settings");

    const ResearchDrumKit* source = researchDrumKitForBase(QStringLiteral("acoustic"));
    require(source != nullptr, "key-aware drum test requires the acoustic research kit");
    const ResearchDrumKit adjusted = keyAwareResearchDrumKit(*source, idea.beatSection);
    require(adjusted.id == source->id && adjusted.pieces.size() == source->pieces.size(),
        "key-aware drum adaptation preserves kit identity and piece ownership");
}

void testIdeaDetailsAndGeneratedSection()
{
    const GenerationRecipe recipe = generateCoupledPracticeIdeaForTest(
        representativeRequest(), 404).recipe;
    DialogInteraction details;
    interactWithNextDialog(details, [](QDialog& dialog) {
        require(dialog.windowTitle() == QStringLiteral("Idea Details"),
            "idea details exposes its exact title");
        const QList<QPlainTextEdit*> edits = dialog.findChildren<QPlainTextEdit*>();
        require(edits.size() == 2 && edits.at(0)->isReadOnly() && edits.at(1)->isReadOnly() &&
                !edits.at(0)->toPlainText().isEmpty() &&
                !edits.at(1)->toPlainText().isEmpty(),
            "idea details owns nonempty read-only teaching and technical views");
        QPlainTextEdit* teaching = edits.at(0)->isVisible() ? edits.at(0) : edits.at(1);
        QPlainTextEdit* technical = teaching == edits.at(0) ? edits.at(1) : edits.at(0);
        auto* toggle = textWidget<QPushButton>(dialog, QStringLiteral("Detailed Analysis"));
        toggle->click();
        require(!teaching->isVisible() && technical->isVisible() &&
                toggle->text() == QStringLiteral("Teaching View"),
            "details toggle switches from teaching to technical analysis");
        toggle->click();
        require(teaching->isVisible() && !technical->isVisible() &&
                toggle->text() == QStringLiteral("Detailed Analysis"),
            "details toggle returns to the teaching view");
        clickStandard(dialog, QDialogButtonBox::Close);
    });
    showIdeaDetails(nullptr, recipe, true);
    require(details.seen && details.error.isEmpty(),
        "idea details closes safely after both views are exercised");

    BeatGridModel model;
    SongSection chord = model.section(0);
    chord.generatedKind = QStringLiteral("chord");
    chord.name = QStringLiteral("Generated chord section");
    require(model.replaceSection(0, chord), "generated chord fixture must apply");
    SongSection practice = model.section(1);
    practice.generatedKind = QStringLiteral("practice");
    practice.name = QStringLiteral("Combined practice section");
    require(model.replaceSection(1, practice), "combined practice fixture must apply");
    const std::optional<SongSection> foundChord =
        PracticeIdeaController::generatedSection(model, QStringLiteral("chord"));
    const std::optional<SongSection> foundBeat =
        PracticeIdeaController::generatedSection(model, QStringLiteral("beat"));
    require(foundChord.has_value() &&
            foundChord->name == QStringLiteral("Generated chord section") &&
            foundBeat.has_value() &&
            foundBeat->name == QStringLiteral("Combined practice section") &&
            !PracticeIdeaController::generatedSection(
                model, QStringLiteral("missing")).has_value(),
        "controller selects exact generated kind, combined fallback, and missing result");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        QApplication application(argc, argv);
        testCatalogSurface();
        testGenerationWrappers();
        testResearchDrumSamples();
        testResearchDrumCatalogAndEngine();
        testPracticeIdeaDialog();
        testContinuationDialog();
        testReferenceDialog();
        testCuratedIdeaDialog();
        testCuratedIdeaSeedReproducibility();
        testReferenceSignaturesAndKeyAwareKit();
        testIdeaDetailsAndGeneratedSection();
        std::cout << "Practice idea and research drum boundary tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Practice idea boundary test failed: " << error.what() << '\n';
        return 1;
    }
}
