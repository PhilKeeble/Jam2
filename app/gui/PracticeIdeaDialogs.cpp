#include "PracticeIdeaDialogs.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
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

std::optional<ChordIdeaRequest> askForPracticeIdea(QWidget* parent, int beatsPerBar)
{
    (void)beatsPerBar;
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Generate Practice Idea"));
    auto* form = new QFormLayout();
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
    QComboBox* formChoice = new QComboBox(&dialog);
    QComboBox* meter = new QComboBox(&dialog);
    QComboBox* mode = new QComboBox(&dialog);
    QComboBox* production = new QComboBox(&dialog);
    QComboBox* complexity = complexityCombo(&dialog);
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
    const auto refreshProfileOptions = [profile, formChoice, meter, mode, production] {
        const QString selectedProfile = profile->currentData().toString();
        formChoice->clear();
        formChoice->addItem(QStringLiteral("Random profile-native form"), QString());
        const QStringList formIds = nativeFormIds(selectedProfile);
        const QStringList formNames = nativeFormNames(selectedProfile);
        for (int index = 0; index < formNames.size(); ++index) {
            formChoice->addItem(formNames.at(index), formIds.value(index));
        }
        meter->clear();
        meter->addItem(QStringLiteral("Use the form's native meter"), QString());
        const QStringList supportedMeterIds = meterIds(selectedProfile);
        const QStringList supportedMeterNames = meterNames(selectedProfile);
        for (int index = 0; index < supportedMeterNames.size(); ++index) {
            meter->addItem(supportedMeterNames.at(index), supportedMeterIds.value(index));
        }
        mode->clear();
        mode->addItem(QStringLiteral("Auto (profile-compatible)"), QString());
        const QStringList supportedModeIds = modeIds(selectedProfile);
        const QStringList supportedModeNames = modeNames(selectedProfile);
        for (int index = 0; index < supportedModeNames.size(); ++index) {
            mode->addItem(supportedModeNames.at(index), supportedModeIds.value(index));
        }
        production->clear();
        production->addItem(QStringLiteral("Profile default"), QString());
        const QStringList productionIds = productionFamilyIds(selectedProfile);
        const QStringList productionNames = productionFamilyNames(selectedProfile);
        for (int index = 0; index < productionNames.size(); ++index) {
            production->addItem(productionNames.at(index), productionIds.value(index));
        }
    };
    QObject::connect(style, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
        [refreshProfile, refreshProfileOptions](int) {
            refreshProfile();
            refreshProfileOptions();
        });
    QObject::connect(profile, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
        [refreshProfileOptions](int) { refreshProfileOptions(); });
    refreshProfile();
    refreshProfileOptions();
    form->addRow(QStringLiteral("Key"), key);
    form->addRow(QStringLiteral("Style"), style);
    form->addRow(QStringLiteral("Profile"), profile);
    form->addRow(QStringLiteral("Form"), formChoice);
    form->addRow(QStringLiteral("Meter"), meter);
    form->addRow(QStringLiteral("Scale / mode"), mode);
    form->addRow(QStringLiteral("Production"), production);
    form->addRow(QStringLiteral("Complexity"), complexity);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Generate"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    auto* description = new QLabel(
        QStringLiteral(
            "Creates matching chord and drum sections. Style sets the musical language and BPM range; "
            "the profile supplies native form, meter, bass, supporting-line, groove, and sound relationships. "
            "A seed-derived variation plan shapes energy, density, articulation, and production inside that profile. "
            "Complexity unlocks musical tools but does not force every tool into the result."),
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
    request.formId = formChoice->currentData().toString();
    request.meterId = meter->currentData().toString();
    request.modeId = mode->currentData().toString();
    request.productionFamilyId = production->currentData().toString();
    request.bars = 0;
    request.beatsPerBar = 0;
    request.harmonicComplexity = complexity->currentData().toInt();
    request.rhythmicComplexity = complexity->currentData().toInt();
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
            ? QStringLiteral("%1 sections -> Banks A-%2 | %3 BPM | %4 Hz")
                .arg(sectionCount)
                .arg(QChar(static_cast<ushort>('A' + qBound(1, sectionCount, 4) - 1)))
                .arg(defaults.bpm, 0, 'f', 1)
                .arg(defaults.sampleRate)
            : QStringLiteral("%1 BPM | %2 Hz | %3 beats | %4 s | %5 frames")
                .arg(defaults.bpm, 0, 'f', 1).arg(defaults.sampleRate).arg(commonBeats)
                .arg(seconds, 0, 'f', 2).arg(frames),
        &dialog);
    auto* advanced = new QGroupBox(QStringLiteral("Advanced"), &dialog);
    advanced->setCheckable(true);
    advanced->setChecked(false);
    auto* advancedForm = new QFormLayout(advanced);
    auto makeSpin = [advanced](double value, double minimum, double maximum, int decimals) {
        auto* spin = new QDoubleSpinBox(advanced);
        spin->setRange(minimum, maximum);
        spin->setDecimals(decimals);
        spin->setValue(value);
        return spin;
    };
    QDoubleSpinBox* chordLevel = makeSpin(defaults.chordLevel, 0.01, 1.0, 2);
    QDoubleSpinBox* drumLevel = makeSpin(defaults.drumLevel, 0.01, 1.0, 2);
    QDoubleSpinBox* melodyLevel = makeSpin(defaults.melodyLevel, 0.01, 1.0, 2);
    QDoubleSpinBox* bassLevel = makeSpin(defaults.bassLevel, 0.01, 1.0, 2);
    QDoubleSpinBox* supportLevel = makeSpin(defaults.supportLevel, 0.01, 1.0, 2);
    QDoubleSpinBox* attack = makeSpin(defaults.attackMs, 0.0, 100.0, 1);
    QDoubleSpinBox* release = makeSpin(defaults.releaseMs, 5.0, 1000.0, 1);
    advancedForm->addRow(QStringLiteral("Chord level"), chordLevel);
    advancedForm->addRow(QStringLiteral("Drum level"), drumLevel);
    advancedForm->addRow(QStringLiteral("Melody level"), melodyLevel);
    advancedForm->addRow(QStringLiteral("Bass level"), bassLevel);
    advancedForm->addRow(QStringLiteral("Support level"), supportLevel);
    advancedForm->addRow(QStringLiteral("Attack (ms)"), attack);
    advancedForm->addRow(QStringLiteral("Release (ms)"), release);
    auto* form = new QFormLayout();
    form->addRow(QStringLiteral("Layers"), chords);
    form->addRow(QString(), drums);
    form->addRow(QString(), melody);
    form->addRow(QString(), bass);
    form->addRow(QString(), support);
    form->addRow(QStringLiteral("Voicing"), voicing);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Render"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(summary);
    layout->addLayout(form);
    layout->addWidget(advanced);
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
    defaults.chordLevel = chordLevel->value();
    defaults.drumLevel = drumLevel->value();
    defaults.melodyLevel = melodyLevel->value();
    defaults.bassLevel = bassLevel->value();
    defaults.supportLevel = supportLevel->value();
    defaults.attackMs = attack->value();
    defaults.releaseMs = release->value();
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
