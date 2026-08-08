#include "PracticeIdeaDialogs.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <numeric>
#include <cmath>

namespace jam2::practice {
namespace {

QComboBox* randomCombo(const QStringList& values, QWidget* parent)
{
    auto* combo = new QComboBox(parent);
    combo->addItem(QStringLiteral("Random"), -1);
    for (int index = 0; index < values.size(); ++index) combo->addItem(values.at(index), index);
    return combo;
}

QComboBox* barsCombo(QWidget* parent)
{
    auto* combo = new QComboBox(parent);
    combo->addItem(QStringLiteral("Random"), 0);
    for (int bars : {4, 8, 12, 16}) combo->addItem(QString::number(bars), bars);
    combo->setCurrentIndex(combo->findData(16));
    return combo;
}

QComboBox* complexityCombo(QWidget* parent)
{
    auto* combo = new QComboBox(parent);
    const QStringList names{
        QStringLiteral("Core style grammar"),
        QStringLiteral("Voicing and connection"),
        QStringLiteral("Directed colour"),
        QStringLiteral("Rhythmic development"),
        QStringLiteral("Expanded tonal or riff vocabulary"),
        QStringLiteral("Independent dialogue and form"),
        QStringLiteral("Large-scale tonal or metric tools"),
        QStringLiteral("Integrated mastery"),
    };
    for (int level = 1; level <= names.size(); ++level) {
        combo->addItem(QStringLiteral("%1 — %2").arg(level).arg(names.at(level - 1)), level);
    }
    combo->setCurrentIndex(1);
    combo->setToolTip(QStringLiteral(
        "Unlocks a broader theory palette and subtle groove variation; it does not simply make chords denser or drums busier."));
    return combo;
}

} // namespace

std::optional<ChordIdeaRequest> askForPracticeIdea(
    QWidget* parent,
    const PracticeIdeaDialogDefaults& defaults)
{
    QDialog dialog(parent);
    PracticeIdeaDialogDefaults selectedDefaults = defaults;
    dialog.setWindowTitle(QStringLiteral("Generate Practice Idea"));
    auto* form = new QFormLayout();
    auto* targetBank = new QComboBox(&dialog);
    for (int bank = 0; bank < 4; ++bank) {
        targetBank->addItem(
            QStringLiteral("Section %1").arg(QChar(QLatin1Char('A').unicode() + bank)),
            bank);
    }
    targetBank->setCurrentIndex(qBound(0, defaults.targetSectionIndex, 3));
    auto* parts = new QComboBox(&dialog);
    parts->addItem(
        QStringLiteral("Full arrangement"),
        static_cast<int>(PracticeIdeaParts::FullArrangement));
    parts->addItem(
        QStringLiteral("Chords, Bass & Melody Only"),
        static_cast<int>(PracticeIdeaParts::PitchedPartsOnly));
    parts->addItem(
        QStringLiteral("Drums Only"),
        static_cast<int>(PracticeIdeaParts::DrumsOnly));
    QComboBox* key = randomCombo(keyNames(), &dialog);
    QComboBox* style = new QComboBox(&dialog);
    style->addItem(QStringLiteral("Random"), QString());
    const QStringList publicStyleNames = chordStyleNames();
    const QStringList publicStyleIds = styleIds();
    for (int index = 0; index < publicStyleNames.size(); ++index) {
        style->addItem(publicStyleNames.at(index), publicStyleIds.value(index));
    }
    style->insertSeparator(style->count());
    style->addItem(
        QStringLiteral("Experimental - Modern Progressive Metalcore"),
        QStringLiteral("metal-experimental"));
    QComboBox* profile = new QComboBox(&dialog);
    QComboBox* meter = new QComboBox(&dialog);
    QComboBox* length = new QComboBox(&dialog);
    auto* exactBpm = new QCheckBox(QStringLiteral("Use exact BPM"), &dialog);
    auto* bpm = new QSpinBox(&dialog);
    bpm->setRange(20, 400);
    bpm->setValue(qBound(20, defaults.bpm, 400));
    exactBpm->setChecked(false);
    bpm->setEnabled(false);
    bpm->setSuffix(QStringLiteral(" BPM"));
    bpm->setToolTip(QStringLiteral(
        "Overrides the selected style's normal tempo range."));
    auto* bpmControls = new QWidget(&dialog);
    auto* bpmLayout = new QHBoxLayout(bpmControls);
    bpmLayout->setContentsMargins(0, 0, 0, 0);
    bpmLayout->addWidget(exactBpm);
    bpmLayout->addWidget(bpm);
    bpmLayout->addStretch();
    QObject::connect(exactBpm, &QCheckBox::toggled, bpm, &QSpinBox::setEnabled);
    QComboBox* complexity = complexityCombo(&dialog);
    const auto partialGeneration = [parts] {
        return static_cast<PracticeIdeaParts>(parts->currentData().toInt()) !=
            PracticeIdeaParts::FullArrangement;
    };
    const auto refreshProfile = [style, profile] {
        profile->clear();
        profile->addItem(QStringLiteral("Random profile"), QString());
        const QString selectedStyle = style->currentData().toString();
        if (selectedStyle == QStringLiteral("metal-experimental")) {
            profile->addItem(
                QStringLiteral("Modern Progressive Metalcore (sound test)"),
                QStringLiteral("metal_modern_progressive"));
            profile->setCurrentIndex(1);
            return;
        }
        const QStringList ids = profileIds(selectedStyle);
        const QStringList names = profileNames(selectedStyle);
        for (int index = 0; index < names.size(); ++index) {
            profile->addItem(names.at(index), ids.value(index));
        }
    };
    const auto refreshLength = [style, profile, meter, length, partialGeneration, &selectedDefaults] {
        const int previousBars = length->currentData().toInt();
        const QString selectedStyle = style->currentData().toString();
        const QString selectedProfile = profile->currentData().toString();
        const QVector<int> supportedBars = compatibleBarCounts(
            selectedStyle,
            selectedProfile,
            meter->currentData().toString());
        const QSignalBlocker blocker(length);
        length->clear();
        length->addItem(QStringLiteral("Random compatible length"), 0);
        for (const int bars : supportedBars) {
            length->addItem(QStringLiteral("%1 bars").arg(bars), bars);
        }
        if (partialGeneration() && selectedDefaults.bars > 0) {
            int currentIndex = length->findData(selectedDefaults.bars);
            if (currentIndex < 0) {
                length->addItem(
                    QStringLiteral("%1 bars (current section)").arg(selectedDefaults.bars),
                    selectedDefaults.bars);
                currentIndex = length->count() - 1;
            } else {
                length->setItemText(
                    currentIndex,
                    QStringLiteral("%1 bars (current section)").arg(selectedDefaults.bars));
            }
        }
        const int preferredBars = previousBars > 0
            ? previousBars
            : partialGeneration() ? selectedDefaults.bars : 0;
        const int preferredIndex = length->findData(preferredBars);
        length->setCurrentIndex(preferredIndex >= 0 ? preferredIndex : 0);
    };
    const auto refreshProfileOptions = [
        style, profile, meter, refreshLength, partialGeneration, &selectedDefaults] {
        const QString previousMeter = meter->currentData().toString();
        const QString selectedStyle = style->currentData().toString();
        const QString selectedProfile = profile->currentData().toString();
        const QSignalBlocker blocker(meter);
        meter->clear();
        meter->addItem(QStringLiteral("Random compatible meter (default)"), QString());
        const QStringList supportedMeterIds =
            compatibleMeterIds(selectedStyle, selectedProfile);
        const QStringList supportedMeterNames =
            compatibleMeterNames(selectedStyle, selectedProfile);
        for (int index = 0; index < supportedMeterNames.size(); ++index) {
            meter->addItem(supportedMeterNames.at(index), supportedMeterIds.value(index));
        }
        if (!selectedDefaults.meterId.isEmpty()) {
            int projectIndex = meter->findData(selectedDefaults.meterId);
            QString projectName = selectedDefaults.meterId;
            const QStringList allIds = compatibleMeterIds(QString(), QString());
            const QStringList allNames = compatibleMeterNames(QString(), QString());
            const int catalogIndex = allIds.indexOf(selectedDefaults.meterId);
            if (catalogIndex >= 0) projectName = allNames.value(catalogIndex, projectName);
            if (projectIndex < 0) {
                meter->addItem(
                    projectName + QStringLiteral(" (current project)"),
                    selectedDefaults.meterId);
                projectIndex = meter->count() - 1;
            } else {
                meter->setItemText(
                    projectIndex,
                    projectName + QStringLiteral(" (current project)"));
            }
        }
        // Random remains the default. The target bank's current meter is
        // labelled above so it remains a one-click explicit choice without
        // silently constraining every new idea.
        const QString preferredMeter = previousMeter;
        const int preferredIndex = meter->findData(preferredMeter);
        meter->setCurrentIndex(preferredIndex >= 0 ? preferredIndex : 0);
        refreshLength();
    };
    QObject::connect(style, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
        [refreshProfile, refreshProfileOptions](int) {
            refreshProfile();
            refreshProfileOptions();
        });
    QObject::connect(profile, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
        [refreshProfileOptions](int) { refreshProfileOptions(); });
    QObject::connect(meter, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
        [refreshLength](int) { refreshLength(); });
    QObject::connect(parts, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
        [parts, refreshProfileOptions, exactBpm, bpm, &selectedDefaults](int) {
            const bool partial =
                static_cast<PracticeIdeaParts>(parts->currentData().toInt()) !=
                PracticeIdeaParts::FullArrangement;
            if (partial) {
                bpm->setValue(qBound(20, selectedDefaults.bpm, 400));
                exactBpm->setChecked(true);
            }
            refreshProfileOptions();
        });
    QObject::connect(targetBank, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
        [targetBank, parts, bpm, exactBpm, refreshProfileOptions, &selectedDefaults, &defaults](int) {
            const int bank = targetBank->currentData().toInt();
            selectedDefaults.targetSectionIndex = bank;
            selectedDefaults.bpm = defaults.bankBpms.value(bank, defaults.bpm);
            selectedDefaults.meterId = defaults.bankMeterIds.value(bank, defaults.meterId);
            selectedDefaults.bars = defaults.bankBars.value(bank, defaults.bars);
            const bool partial =
                static_cast<PracticeIdeaParts>(parts->currentData().toInt()) !=
                PracticeIdeaParts::FullArrangement;
            if (partial || !exactBpm->isChecked()) {
                bpm->setValue(qBound(20, selectedDefaults.bpm, 400));
            }
            refreshProfileOptions();
        });
    refreshProfile();
    refreshProfileOptions();
    form->addRow(QStringLiteral("Section"), targetBank);
    form->addRow(QStringLiteral("Parts"), parts);
    form->addRow(QStringLiteral("Key"), key);
    form->addRow(QStringLiteral("Style"), style);
    form->addRow(QStringLiteral("Profile"), profile);
    form->addRow(QStringLiteral("Meter"), meter);
    form->addRow(QStringLiteral("Length"), length);
    form->addRow(QStringLiteral("Tempo"), bpmControls);
    form->addRow(QStringLiteral("Complexity"), complexity);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Generate"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    auto* description = new QLabel(
        QStringLiteral(
            "Creates a full arrangement or replaces only its pitched or drum parts. Generation starts "
            "with a random compatible meter. The target section's current meter is marked in the list when you want to keep it explicitly. "
            "Partial generation starts with the target section's current tempo as an exact override; untick Use exact BPM to choose from the style's tempo range. "
            "Untouched sections inherit Section A's timing. Partial generation also starts with the current section length. "
            "Choose a meter while leaving Style random to generate from any style that supports it. "
            "Form, scale, production, and other relationships are chosen automatically from the compatible profile. "
            "Complexity unlocks musical tools without forcing every tool into the result."),
        &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);
    layout->addLayout(form);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    ChordIdeaRequest request;
    request.key = key->currentData().toInt();
    request.styleId = style->currentData().toString();
    request.profileId = profile->currentData().toString();
    request.parts = static_cast<PracticeIdeaParts>(parts->currentData().toInt());
    request.targetSectionIndex = targetBank->currentData().toInt();
    request.meterId = meter->currentData().toString();
    request.allowMeterOverride = !request.meterId.isEmpty() &&
        !compatibleMeterIds(request.styleId, request.profileId).contains(request.meterId);
    request.bpm = exactBpm->isChecked() ? bpm->value() : 0;
    request.bars = length->currentData().toInt();
    request.beatsPerBar = 0;
    request.harmonicComplexity = complexity->currentData().toInt();
    request.rhythmicComplexity = complexity->currentData().toInt();
    return request;
}

std::optional<ContinueIdeaRequest> askForIdeaContinuation(
    QWidget* parent,
    const ContinueIdeaDialogDefaults& defaults)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Continue Idea"));
    auto* source = new QComboBox(&dialog);
    auto* target = new QComboBox(&dialog);
    for (int bank = 0; bank < 4; ++bank) {
        const QString bankName = defaults.bankNames.value(bank).trimmed();
        const QString state = defaults.bankHasContent.value(bank)
            ? bankName.isEmpty() ? QStringLiteral("has material") : bankName
            : QStringLiteral("empty");
        const QString label = QStringLiteral("Section %1 — %2")
            .arg(QChar(QLatin1Char('A').unicode() + bank), state);
        source->addItem(label, bank);
        target->addItem(label, bank);
    }
    source->setCurrentIndex(qBound(0, defaults.sourceSectionIndex, 3));
    target->setCurrentIndex(qBound(0, defaults.targetSectionIndex, 3));

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Continue"));
    const auto updateState = [source, target, buttons, &defaults] {
        const int sourceBank = source->currentData().toInt();
        const int targetBank = target->currentData().toInt();
        buttons->button(QDialogButtonBox::Ok)->setEnabled(
            sourceBank != targetBank && defaults.bankHasContent.value(sourceBank));
    };
    QObject::connect(source, qOverload<int>(&QComboBox::currentIndexChanged),
        &dialog, [updateState](int) { updateState(); });
    QObject::connect(target, qOverload<int>(&QComboBox::currentIndexChanged),
        &dialog, [updateState](int) { updateState(); });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    updateState();

    auto* description = new QLabel(QStringLiteral(
        "Analyses the source section's harmony, timing, groove, and melodic rhythm, then creates a related but contrasting section. The source section is never changed."),
        &dialog);
    description->setWordWrap(true);
    auto* form = new QFormLayout();
    form->addRow(QStringLiteral("Source Section"), source);
    form->addRow(QStringLiteral("Target Section"), target);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(description);
    layout->addLayout(form);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;

    ContinueIdeaRequest request;
    request.sourceSectionIndex = source->currentData().toInt();
    request.targetSectionIndex = target->currentData().toInt();
    return request;
}

std::optional<ReferenceRenderSettings> askForReferenceRender(
    QWidget* parent,
    ReferenceRenderSettings defaults,
    int chordBeats,
    int beatBeats,
    int melodyBeats,
    int bassBeats,
    int supportBeats,
    int sectionCount)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Generate Reference WAVs"));
    auto* chords = new QCheckBox(QStringLiteral("Chord reference"), &dialog);
    chords->setChecked(defaults.renderChords && chordBeats > 0);
    chords->setEnabled(chordBeats > 0);
    auto* drums = new QCheckBox(QStringLiteral("Drum reference"), &dialog);
    drums->setChecked(defaults.renderDrums && beatBeats > 0);
    drums->setEnabled(beatBeats > 0);
    auto* melody = new QCheckBox(QStringLiteral("Melody reference"), &dialog);
    melody->setChecked(defaults.renderMelody && melodyBeats > 0);
    melody->setEnabled(melodyBeats > 0);
    auto* bass = new QCheckBox(QStringLiteral("Bass reference"), &dialog);
    bass->setChecked(defaults.renderBass && bassBeats > 0);
    bass->setEnabled(bassBeats > 0);
    auto* support = new QCheckBox(QStringLiteral("Supporting-line reference"), &dialog);
    support->setChecked(defaults.renderSupport && supportBeats > 0);
    support->setEnabled(supportBeats > 0);
    auto* voicing = new QComboBox(&dialog);
    voicing->addItem(QStringLiteral("Style default"), static_cast<int>(ChordVoicing::StyleDefault));
    voicing->addItem(QStringLiteral("Close"), static_cast<int>(ChordVoicing::Close));
    voicing->addItem(QStringLiteral("Spread"), static_cast<int>(ChordVoicing::Spread));
    voicing->addItem(QStringLiteral("Voice-led"), static_cast<int>(ChordVoicing::VoiceLed));
    voicing->setCurrentIndex(voicing->findData(static_cast<int>(defaults.voicing)));
    auto* drumKit = new QComboBox(&dialog);
    drumKit->addItem(
        QStringLiteral("Style default"),
        static_cast<int>(ReferenceDrumKit::StyleDefault));
    drumKit->addItem(
        QStringLiteral("Acoustic Kit"),
        static_cast<int>(ReferenceDrumKit::Acoustic));
    drumKit->addItem(
        QStringLiteral("Electronic Kit"),
        static_cast<int>(ReferenceDrumKit::Electronic));
    drumKit->setCurrentIndex(
        drumKit->findData(static_cast<int>(defaults.drumKit)));
    int commonBeats = 0;
    for (int beats : {chordBeats, beatBeats, melodyBeats, bassBeats, supportBeats}) {
        if (beats <= 0) continue;
        commonBeats = commonBeats > 0 ? std::lcm(commonBeats, beats) : beats;
    }
    const double seconds = defaults.bpm > 0.0
        ? commonBeats * 60.0 / defaults.bpm *
            (1.0 / qMax(1, defaults.tempoPulseUnits))
        : 0.0;
    const qint64 frames = static_cast<qint64>(std::ceil(seconds * defaults.sampleRate));
    auto* summary = new QLabel(
        sectionCount > 1
            ? QStringLiteral("%1 sections -> Sections A-%2 | %3 BPM | %4 Hz")
                .arg(sectionCount)
                .arg(QChar(static_cast<ushort>('A' + qBound(1, sectionCount, 4) - 1)))
                .arg(defaults.bpm, 0, 'f', 1)
                .arg(defaults.sampleRate)
            : QStringLiteral("%1 BPM | %2 Hz | %3 beats | %4 s | %5 frames")
                .arg(defaults.bpm, 0, 'f', 1).arg(defaults.sampleRate).arg(commonBeats)
                .arg(seconds, 0, 'f', 2).arg(frames),
        &dialog);
    auto* form = new QFormLayout();
    form->addRow(QStringLiteral("Layers"), chords);
    form->addRow(QString(), drums);
    form->addRow(QString(), melody);
    form->addRow(QString(), bass);
    form->addRow(QString(), support);
    form->addRow(QStringLiteral("Voicing"), voicing);
    form->addRow(QStringLiteral("Drum Kit"), drumKit);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Render"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(summary);
    layout->addLayout(form);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted ||
        (!chords->isChecked() && !drums->isChecked() && !melody->isChecked() &&
         !bass->isChecked() && !support->isChecked())) {
        return std::nullopt;
    }
    defaults.renderChords = chords->isChecked();
    defaults.renderDrums = drums->isChecked();
    defaults.renderMelody = melody->isChecked();
    defaults.renderBass = bass->isChecked();
    defaults.renderSupport = support->isChecked();
    defaults.voicing = static_cast<ChordVoicing>(voicing->currentData().toInt());
    defaults.drumKit = static_cast<ReferenceDrumKit>(
        drumKit->currentData().toInt());
    return defaults;
}

void showIdeaDetails(QWidget* parent, const GenerationRecipe& recipe, bool contentChanged)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Idea Details"));
    dialog.resize(760, 700);
    auto* teaching = new QPlainTextEdit(&dialog);
    teaching->setReadOnly(true);
    teaching->setPlainText(generationRecipeTeaching(recipe, contentChanged));
    auto* technical = new QPlainTextEdit(&dialog);
    technical->setReadOnly(true);
    technical->setPlainText(generationRecipeDetails(recipe, contentChanged));
    technical->setVisible(false);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QPushButton* details = buttons->addButton(
        QStringLiteral("Detailed Analysis"), QDialogButtonBox::ActionRole);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(teaching, 1);
    layout->addWidget(technical, 1);
    layout->addWidget(buttons);
    QObject::connect(details, &QPushButton::clicked, &dialog,
        [teaching, technical, details] {
            const bool showTechnical = !technical->isVisible();
            technical->setVisible(showTechnical);
            teaching->setVisible(!showTechnical);
            details->setText(showTechnical
                ? QStringLiteral("Teaching View") : QStringLiteral("Detailed Analysis"));
        });
    dialog.exec();
}

} // namespace jam2::practice
