#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <vector>

namespace jam2::test {

bool readJsonObject(const QString& path, QJsonObject& object, QString& error);

class CsvTable final {
public:
    bool read(const QString& path, QString& error);

    qsizetype rowCount() const noexcept { return rows_.size(); }
    bool hasColumn(const QString& name) const { return columns_.contains(name); }
    QString value(qsizetype row, const QString& name) const;
    double number(qsizetype row, const QString& name, double fallback = 0.0) const;
    std::int64_t integer(qsizetype row, const QString& name, std::int64_t fallback = 0) const;
    bool yes(qsizetype row, const QString& name) const;
    QVector<qsizetype> rowsOfType(const QString& type) const;

private:
    QHash<QString, qsizetype> columns_;
    QVector<QStringList> rows_;
};

struct Pcm16MonoWav {
    int sampleRate = 0;
    std::vector<std::int16_t> samples;
};

bool readPcm16MonoWav(const QString& path, Pcm16MonoWav& wav, QString& error);

std::vector<std::int64_t> detectEvents(
    const std::vector<std::int16_t>& samples,
    int threshold,
    std::int64_t refractoryFrames);

std::vector<std::int64_t> expectedMetronomeFrames(
    const QJsonObject& sidecar,
    std::int64_t sampleCount);

struct ExpectedClickAnalysis {
    bool ok = false;
    std::size_t expectedClicks = 0;
    std::size_t detectedClicks = 0;
    std::size_t steadyMissingClicks = 0;
    std::size_t steadyExtraClicks = 0;
    std::int64_t steadyMaximumErrorFrames = 0;
    bool startupBoundaryMismatch = false;
};

ExpectedClickAnalysis analyzeExpectedClicks(
    const QJsonObject& sidecar,
    const std::vector<std::int16_t>& samples,
    int threshold = 1800,
    std::int64_t refractoryFrames = 900,
    std::int64_t toleranceFrames = 96);

struct IntervalGridAnalysis {
    std::size_t clicks = 0;
    std::size_t intervals = 0;
    std::int64_t maximumPhaseErrorFrames = 0;
    std::int64_t maximumIntervalMultiple = 0;
};

IntervalGridAnalysis analyzeIntervalGrid(
    const std::vector<std::int16_t>& samples,
    int threshold,
    std::int64_t refractoryFrames,
    std::int64_t intervalFrames);

struct NearestEventAnalysis {
    std::vector<std::int64_t> signedErrors;
    std::size_t missingReferences = 0;
    std::size_t extraMeasured = 0;
};

NearestEventAnalysis nearestEvents(
    const std::vector<std::int64_t>& references,
    const std::vector<std::int64_t>& measured,
    std::int64_t maximumAbsoluteError);

double estimateToneHz(
    const std::vector<std::int16_t>& samples,
    int sampleRate,
    std::int64_t startFrame,
    std::int64_t stopFrame);

double rootMeanSquare(
    const std::vector<std::int16_t>& samples,
    std::int64_t startFrame,
    std::int64_t stopFrame);

} // namespace jam2::test
