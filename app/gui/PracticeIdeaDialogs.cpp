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
        QStringLiteral("Diatonic / modal baseline"),
        QStringLiteral("Inversions, suspensions, neighbours"),
        QStringLiteral("Modal interchange"),
        QStringLiteral("Secondary dominants"),
        QStringLiteral("Chromatic passing movement"),
        QStringLiteral("Tonicisation and backdoor motion"),
        QStringLiteral("Substitutions and chromatic mediants"),
        QStringLiteral("Temporary modulation"),
    };
    for (int level = 1; level <= names.size(); ++level) {
        combo->addItem(QStringLiteral("%1 — %2").arg(level).arg(names.at(level - 1)), level);
    }
    combo->setCurrentIndex(1);
    combo->setToolTip(QStringLiteral(
        "Unlocks a broader theory palette and subtle groove variation; it does not simply make chords denser or drums busier."));
    return combo;
}

void refreshCharacterCombo(QComboBox* character)
{
    character->clear();
    character->addItem(QStringLiteral("Random"), QString());
    const QStringList names = characterNames();
    const QStringList ids = characterIds();
    for (int index = 0; index < names.size(); ++index) character->addItem(names.at(index), ids.at(index));
}

} // namespace

std::optional<ChordIdeaRequest> askForPracticeIdea(QWidget* parent, int beatsPerBar)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Generate Practice Idea"));
    auto* form = new QFormLayout();
    QComboBox* key = randomCombo(keyNames(), &dialog);
    QComboBox* style = randomCombo(chordStyleNames(), &dialog);
    QComboBox* character = new QComboBox(&dialog);
    QComboBox* bars = barsCombo(&dialog);
    QComboBox* complexity = complexityCombo(&dialog);
    refreshCharacterCombo(character);
    form->addRow(QStringLiteral("Key"), key);
    form->addRow(QStringLiteral("Style"), style);
    form->addRow(QStringLiteral("Character"), character);
    form->addRow(QStringLiteral("Bars"), bars);
    form->addRow(QStringLiteral("Complexity"), complexity);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Generate"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    auto* description = new QLabel(
        QStringLiteral(
            "Creates matching chord and drum sections. Style sets the musical language and BPM range; "
            "complexity independently changes the harmony, melody, and beat difficulty."),
        &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);
    layout->addLayout(form);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    ChordIdeaRequest request;
    request.key = key->currentData().toInt();
    const int styleIndex = style->currentData().toInt();
    request.styleId = styleIndex >= 0 ? styleIds().value(styleIndex) : QString();
    request.characterId = character->currentData().toString();
    request.bars = bars->currentData().toInt();
    request.beatsPerBar = beatsPerBar;
    request.harmonicComplexity = complexity->currentData().toInt();
    request.rhythmicComplexity = complexity->currentData().toInt();
    return request;
}

std::optional<ReferenceRenderSettings> askForReferenceRender(
    QWidget* parent,
    ReferenceRenderSettings defaults,
    int chordBeats,
    int beatBeats,
    int melodyBeats)
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
    auto* voicing = new QComboBox(&dialog);
    voicing->addItem(QStringLiteral("Style default"), static_cast<int>(ChordVoicing::StyleDefault));
    voicing->addItem(QStringLiteral("Close"), static_cast<int>(ChordVoicing::Close));
    voicing->addItem(QStringLiteral("Spread"), static_cast<int>(ChordVoicing::Spread));
    voicing->addItem(QStringLiteral("Voice-led"), static_cast<int>(ChordVoicing::VoiceLed));
    voicing->setCurrentIndex(voicing->findData(static_cast<int>(defaults.voicing)));
    int commonBeats = 0;
    for (int beats : {chordBeats, beatBeats, melodyBeats}) {
        if (beats <= 0) continue;
        commonBeats = commonBeats > 0 ? std::lcm(commonBeats, beats) : beats;
    }
    const double seconds = defaults.bpm > 0.0 ? commonBeats * 60.0 / defaults.bpm : 0.0;
    const qint64 frames = static_cast<qint64>(std::ceil(seconds * defaults.sampleRate));
    auto* summary = new QLabel(QStringLiteral("%1 BPM · %2 Hz · %3 beats · %4 s · %5 frames")
        .arg(defaults.bpm, 0, 'f', 1).arg(defaults.sampleRate).arg(commonBeats)
        .arg(seconds, 0, 'f', 2).arg(frames), &dialog);
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
    QDoubleSpinBox* attack = makeSpin(defaults.attackMs, 0.0, 100.0, 1);
    QDoubleSpinBox* release = makeSpin(defaults.releaseMs, 5.0, 1000.0, 1);
    advancedForm->addRow(QStringLiteral("Chord level"), chordLevel);
    advancedForm->addRow(QStringLiteral("Drum level"), drumLevel);
    advancedForm->addRow(QStringLiteral("Melody level"), melodyLevel);
    advancedForm->addRow(QStringLiteral("Attack (ms)"), attack);
    advancedForm->addRow(QStringLiteral("Release (ms)"), release);
    auto* form = new QFormLayout();
    form->addRow(QStringLiteral("Layers"), chords);
    form->addRow(QString(), drums);
    form->addRow(QString(), melody);
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
        (!chords->isChecked() && !drums->isChecked() && !melody->isChecked())) {
        return std::nullopt;
    }
    defaults.renderChords = chords->isChecked();
    defaults.renderDrums = drums->isChecked();
    defaults.renderMelody = melody->isChecked();
    defaults.voicing = static_cast<ChordVoicing>(voicing->currentData().toInt());
    defaults.chordLevel = chordLevel->value();
    defaults.drumLevel = drumLevel->value();
    defaults.melodyLevel = melodyLevel->value();
    defaults.attackMs = attack->value();
    defaults.releaseMs = release->value();
    return defaults;
}

void showIdeaDetails(QWidget* parent, const GenerationRecipe& recipe, bool contentChanged)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Idea Details"));
    dialog.resize(760, 660);
    auto* text = new QPlainTextEdit(&dialog);
    text->setReadOnly(true);
    text->setPlainText(generationRecipeDetails(recipe, contentChanged));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(text, 1);
    layout->addWidget(buttons);
    dialog.exec();
}

} // namespace jam2::practice
