#include "JamTasterProjectSupport.hpp"

#include "ContentLimits.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

QJsonObject songFixture(int sectionCount = 1, int bpm = 120, int meter = 4)
{
    QJsonArray sections;
    QJsonArray banks;
    for (int index = 0; index < sectionCount; ++index) {
        sections.append(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("section-%1").arg(index)},
            {QStringLiteral("label"), QString(QChar(u'A' + index))},
        });
        banks.append(QJsonObject{
            {QStringLiteral("id"), QString(QChar(u'A' + index))},
            {QStringLiteral("lanes"), QJsonArray{}},
            {QStringLiteral("timing"), QJsonObject{
                {QStringLiteral("bpm"), bpm},
                {QStringLiteral("beats_per_bar"), meter},
            }},
        });
    }
    return QJsonObject{
        {QStringLiteral("sections"), sections},
        {QStringLiteral("looper"), QJsonObject{
            {QStringLiteral("banks"), banks},
        }},
    };
}

QJsonObject laneFixture(qint64 frames, int sampleRate = 48000)
{
    return QJsonObject{
        {QStringLiteral("asset_path"), QStringLiteral("imported/reference.wav")},
        {QStringLiteral("asset_hash"), QString(64, QLatin1Char('a'))},
        {QStringLiteral("source_frames"), QString::number(frames)},
        {QStringLiteral("sample_rate"), sampleRate},
    };
}

void testRejectedInputsAreTransactional()
{
    for (QJsonObject invalid : {
             QJsonObject{},
             QJsonObject{{QStringLiteral("sections"), QJsonArray{QJsonObject{}}}},
             songFixture(jam2::application::limits::kMaximumSongSections)}) {
        const QJsonObject before = invalid;
        QString error;
        require(!jam2::gui::appendJamTasterReferenceSection(
                    invalid, laneFixture(48000), error) &&
                !error.isEmpty() && invalid == before,
            "missing/full project structure must reject without mutation");
    }

    QJsonObject tooLong = songFixture();
    const QJsonObject before = tooLong;
    QString error;
    const qint64 frames = 48000LL * 60LL * 513LL / 120LL;
    require(!jam2::gui::appendJamTasterReferenceSection(
                tooLong, laneFixture(frames), error) &&
            error.contains(QStringLiteral("512-beat")) && tooLong == before,
        "an over-limit reference must reject transactionally");
}

void testReferenceSectionShape()
{
    QJsonObject song = songFixture(1, 120, 4);
    QString error = QStringLiteral("old error");
    require(jam2::gui::appendJamTasterReferenceSection(
                song, laneFixture(48000LL * 3LL), error) && error.isEmpty(),
        "a bounded source WAV must append a reference section");

    const QJsonArray sections = song.value(QStringLiteral("sections")).toArray();
    const QJsonObject section = sections.at(1).toObject();
    require(sections.size() == 2 &&
            section.value(QStringLiteral("label")).toString() == QStringLiteral("B") &&
            section.value(QStringLiteral("name")).toString() ==
                QStringLiteral("Original Reference") &&
            section.value(QStringLiteral("beats")).toInt() == 8 &&
            section.value(QStringLiteral("targets")).toArray().size() == 8 &&
            section.value(QStringLiteral("beat_patterns")).toArray().size() == 8 &&
            section.value(QStringLiteral("musical_patterns")).toArray().size() == 8 &&
            !section.value(QStringLiteral("id")).toString().isEmpty(),
        "reference duration must round up to a complete meter with aligned grids");

    const QJsonArray banks = song.value(QStringLiteral("looper"))
        .toObject().value(QStringLiteral("banks")).toArray();
    const QJsonObject appended = banks.at(1).toObject();
    const QJsonObject timing = appended.value(QStringLiteral("timing")).toObject();
    const QJsonObject lane = appended.value(QStringLiteral("lanes"))
        .toArray().first().toObject();
    require(banks.size() == 2 &&
            appended.value(QStringLiteral("id")).toString() == QStringLiteral("B") &&
            timing.value(QStringLiteral("bpm")).toInt() == 120 &&
            timing.value(QStringLiteral("beats_per_bar")).toInt() == 4 &&
            timing.value(QStringLiteral("inherits_bank_a")).toBool() &&
            lane.value(QStringLiteral("muted")).toBool() &&
            !lane.value(QStringLiteral("solo")).toBool() &&
            !lane.value(QStringLiteral("loop_enabled")).toBool() &&
            !lane.value(QStringLiteral("local_only")).toBool() &&
            lane.value(QStringLiteral("origin_kind")).toString() ==
                QStringLiteral("imported") &&
            lane.value(QStringLiteral("name")).toString() ==
                QStringLiteral("Original Source (muted)") &&
            !lane.value(QStringLiteral("id")).toString().isEmpty(),
        "the reference bank must preserve timing and create a safe muted shared lane");
}

void testBoundsAndDefaults()
{
    QJsonObject song = songFixture(1, 999, 99);
    QString error;
    require(jam2::gui::appendJamTasterReferenceSection(
                song, laneFixture(0, 0), error),
        "zero-duration metadata must still create a minimum reference section");
    const QJsonObject appended = song.value(QStringLiteral("looper"))
        .toObject().value(QStringLiteral("banks")).toArray().at(1).toObject();
    const QJsonObject timing = appended.value(QStringLiteral("timing")).toObject();
    require(timing.value(QStringLiteral("bpm")).toInt() == 400 &&
            timing.value(QStringLiteral("beats_per_bar")).toInt() == 12 &&
            song.value(QStringLiteral("sections")).toArray().at(1).toObject()
                .value(QStringLiteral("beats")).toInt() == 12,
        "project timing must be bounded and the minimum section aligned to its meter");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        testRejectedInputsAreTransactional();
        testReferenceSectionShape();
        testBoundsAndDefaults();
        std::cout << "JamTaster project support tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "JamTaster project support tests failed: " << error.what() << '\n';
        return 1;
    }
}
