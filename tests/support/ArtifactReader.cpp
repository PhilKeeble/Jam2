#include "ArtifactReader.hpp"

#include <QFile>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace jam2::test {
namespace {

QStringList splitCsvLine(const QString& line, bool& ok)
{
    QStringList fields;
    QString field;
    bool quoted = false;
    ok = true;
    for (qsizetype index = 0; index < line.size(); ++index) {
        const QChar character = line.at(index);
        if (quoted) {
            if (character == QLatin1Char('"')) {
                if (index + 1 < line.size() && line.at(index + 1) == QLatin1Char('"')) {
                    field += QLatin1Char('"');
                    ++index;
                } else {
                    quoted = false;
                }
            } else {
                field += character;
            }
            continue;
        }
        if (character == QLatin1Char('"') && field.isEmpty()) {
            quoted = true;
        } else if (character == QLatin1Char(',')) {
            fields.push_back(field);
            field.clear();
        } else if (character != QLatin1Char('\r') && character != QLatin1Char('\n')) {
            field += character;
        }
    }
    if (quoted) ok = false;
    fields.push_back(field);
    return fields;
}

std::uint16_t little16(const char* bytes)
{
    const auto* data = reinterpret_cast<const unsigned char*>(bytes);
    return static_cast<std::uint16_t>(data[0]) |
        static_cast<std::uint16_t>(data[1]) << 8U;
}

std::uint32_t little32(const char* bytes)
{
    const auto* data = reinterpret_cast<const unsigned char*>(bytes);
    return static_cast<std::uint32_t>(data[0]) |
        static_cast<std::uint32_t>(data[1]) << 8U |
        static_cast<std::uint32_t>(data[2]) << 16U |
        static_cast<std::uint32_t>(data[3]) << 24U;
}

std::uint64_t jsonUnsigned(const QJsonObject& object, const QString& name, std::uint64_t fallback = 0)
{
    const QJsonValue value = object.value(name);
    if (!value.isDouble()) return fallback;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0.0 ||
        number > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return fallback;
    }
    return static_cast<std::uint64_t>(number);
}

struct RawClickMatch {
    std::vector<std::int64_t> errors;
    std::size_t missing = 0;
    std::size_t extra = 0;
};

RawClickMatch matchClicks(
    const std::vector<std::int64_t>& expected,
    const std::vector<std::int64_t>& detected,
    std::int64_t tolerance)
{
    RawClickMatch result;
    std::vector<bool> used(detected.size(), false);
    for (const std::int64_t reference : expected) {
        std::size_t bestIndex = detected.size();
        std::int64_t bestError = std::numeric_limits<std::int64_t>::max();
        for (std::size_t index = 0; index < detected.size(); ++index) {
            if (used[index]) continue;
            const std::int64_t error = std::llabs(detected[index] - reference);
            if (error < bestError) {
                bestError = error;
                bestIndex = index;
            }
        }
        if (bestIndex == detected.size() || bestError > tolerance) {
            ++result.missing;
        } else {
            used[bestIndex] = true;
            result.errors.push_back(bestError);
        }
    }
    result.extra = static_cast<std::size_t>(std::count(used.begin(), used.end(), false));
    return result;
}

bool startupBoundaryMatch(
    const std::vector<std::int64_t>& expected,
    const std::vector<std::int64_t>& detected,
    std::size_t detectedStart,
    std::int64_t tolerance,
    std::int64_t& maximumError)
{
    if (expected.size() <= 1 || detectedStart > detected.size() ||
        expected.size() - 1 != detected.size() - detectedStart) {
        return false;
    }
    maximumError = 0;
    for (std::size_t index = 1; index < expected.size(); ++index) {
        const std::int64_t error = std::llabs(
            detected[detectedStart + index - 1] - expected[index]);
        if (error > tolerance) return false;
        maximumError = std::max(maximumError, error);
    }
    return true;
}

} // namespace

bool readJsonObject(const QString& path, QJsonObject& object, QString& error)
{
    error.clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("opening JSON failed: ") + path + QStringLiteral(": ") + file.errorString();
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = QStringLiteral("parsing JSON object failed: ") + path + QStringLiteral(": ") +
            parseError.errorString();
        return false;
    }
    object = document.object();
    return true;
}

bool CsvTable::read(const QString& path, QString& error)
{
    error.clear();
    columns_.clear();
    rows_.clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error = QStringLiteral("opening CSV failed: ") + path + QStringLiteral(": ") + file.errorString();
        return false;
    }
    bool splitOk = false;
    const QStringList header = splitCsvLine(QString::fromUtf8(file.readLine()), splitOk);
    if (!splitOk || header.isEmpty()) {
        error = QStringLiteral("CSV header is malformed: ") + path;
        return false;
    }
    for (qsizetype index = 0; index < header.size(); ++index) {
        if (header.at(index).isEmpty() || columns_.contains(header.at(index))) {
            error = QStringLiteral("CSV header contains an empty or duplicate field: ") + path;
            return false;
        }
        columns_.insert(header.at(index), index);
    }
    while (!file.atEnd()) {
        const QByteArray bytes = file.readLine();
        if (bytes.trimmed().isEmpty()) continue;
        QStringList row = splitCsvLine(QString::fromUtf8(bytes), splitOk);
        if (!splitOk || row.size() != header.size()) {
            error = QStringLiteral("CSV row has the wrong shape: ") + path;
            return false;
        }
        rows_.push_back(std::move(row));
    }
    if (rows_.isEmpty()) {
        error = QStringLiteral("CSV contains no data rows: ") + path;
        return false;
    }
    return true;
}

QString CsvTable::value(qsizetype row, const QString& name) const
{
    const auto column = columns_.constFind(name);
    if (row < 0 || row >= rows_.size() || column == columns_.cend()) return {};
    return rows_.at(row).value(*column);
}

double CsvTable::number(qsizetype row, const QString& name, double fallback) const
{
    bool ok = false;
    const double result = value(row, name).toDouble(&ok);
    return ok && std::isfinite(result) ? result : fallback;
}

std::int64_t CsvTable::integer(qsizetype row, const QString& name, std::int64_t fallback) const
{
    bool ok = false;
    const qlonglong result = value(row, name).toLongLong(&ok);
    return ok ? static_cast<std::int64_t>(result) : fallback;
}

bool CsvTable::yes(qsizetype row, const QString& name) const
{
    const QString text = value(row, name).trimmed().toLower();
    return text == QStringLiteral("yes") || text == QStringLiteral("true") ||
        text == QStringLiteral("on") || text == QStringLiteral("1");
}

QVector<qsizetype> CsvTable::rowsOfType(const QString& type) const
{
    QVector<qsizetype> result;
    for (qsizetype row = 0; row < rows_.size(); ++row) {
        if (value(row, QStringLiteral("row_type")) == type) result.push_back(row);
    }
    return result;
}

bool readPcm16MonoWav(const QString& path, Pcm16MonoWav& wav, QString& error)
{
    error.clear();
    wav = {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("opening WAV failed: ") + path + QStringLiteral(": ") + file.errorString();
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.size() < 44 || std::memcmp(bytes.constData(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.constData() + 8, "WAVE", 4) != 0) {
        error = QStringLiteral("WAV RIFF header is invalid: ") + path;
        return false;
    }
    bool formatFound = false;
    bool dataFound = false;
    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits = 0;
    std::uint32_t sampleRate = 0;
    qsizetype dataOffset = 0;
    std::uint32_t dataBytes = 0;
    qsizetype offset = 12;
    while (offset + 8 <= bytes.size()) {
        const char* header = bytes.constData() + offset;
        const std::uint32_t chunkBytes = little32(header + 4);
        const qsizetype payload = offset + 8;
        if (chunkBytes > static_cast<std::uint32_t>(bytes.size()) ||
            payload > bytes.size() - static_cast<qsizetype>(chunkBytes)) {
            error = QStringLiteral("WAV chunk exceeds file bounds: ") + path;
            return false;
        }
        if (std::memcmp(header, "fmt ", 4) == 0 && chunkBytes >= 16) {
            format = little16(bytes.constData() + payload);
            channels = little16(bytes.constData() + payload + 2);
            sampleRate = little32(bytes.constData() + payload + 4);
            bits = little16(bytes.constData() + payload + 14);
            formatFound = true;
        } else if (std::memcmp(header, "data", 4) == 0) {
            dataOffset = payload;
            dataBytes = chunkBytes;
            dataFound = true;
        }
        offset = payload + static_cast<qsizetype>(chunkBytes) + (chunkBytes & 1U);
    }
    if (!formatFound || !dataFound || format != 1 || channels != 1 || bits != 16 ||
        sampleRate == 0 || (dataBytes & 1U) != 0) {
        error = QStringLiteral("WAV must be mono PCM16 with a valid sample rate: ") + path;
        return false;
    }
    wav.sampleRate = static_cast<int>(sampleRate);
    wav.samples.resize(dataBytes / 2U);
    for (std::size_t index = 0; index < wav.samples.size(); ++index) {
        const std::uint16_t raw = little16(bytes.constData() + dataOffset +
            static_cast<qsizetype>(index * 2U));
        wav.samples[index] = static_cast<std::int16_t>(raw);
    }
    return true;
}

std::vector<std::int64_t> detectEvents(
    const std::vector<std::int16_t>& samples,
    int threshold,
    std::int64_t refractoryFrames)
{
    std::vector<std::int64_t> result;
    const int boundedThreshold = std::max(0, threshold);
    std::int64_t holdoff = 0;
    for (std::size_t index = 0; index < samples.size(); ++index) {
        if (holdoff > 0) {
            --holdoff;
            continue;
        }
        if (std::abs(static_cast<int>(samples[index])) >= boundedThreshold) {
            result.push_back(static_cast<std::int64_t>(index));
            holdoff = std::max<std::int64_t>(0, refractoryFrames);
        }
    }
    return result;
}

std::vector<std::int64_t> expectedMetronomeFrames(
    const QJsonObject& sidecar,
    std::int64_t sampleCount)
{
    std::vector<std::int64_t> expected;
    const int sampleRate = sidecar.value(QStringLiteral("sample_rate")).toInt(48000);
    const int bpm = sidecar.value(QStringLiteral("bpm")).toInt(120);
    const int division = sidecar.value(QStringLiteral("metronome_division")).toInt(1);
    const int stepCount = std::max(1,
        sidecar.value(QStringLiteral("metronome_step_count")).toInt(4));
    const std::uint64_t playLow = jsonUnsigned(
        sidecar, QStringLiteral("metronome_play_mask_low"), 0x0f);
    const std::uint64_t playHigh = jsonUnsigned(
        sidecar, QStringLiteral("metronome_play_mask_high"), 0);
    const std::uint64_t start = jsonUnsigned(sidecar, QStringLiteral("start_audio_frame"));
    const std::uint64_t epoch = jsonUnsigned(
        sidecar, QStringLiteral("metronome_epoch_sample_time"));
    if (!sidecar.value(QStringLiteral("metronome_epoch_valid")).toBool(false) ||
        sampleRate <= 0 || bpm <= 0 || division <= 0 || sampleCount <= 0) {
        return expected;
    }
    const std::uint64_t interval = static_cast<std::uint64_t>(std::max<long long>(
        1, std::llround(60.0 * sampleRate / static_cast<double>(bpm * division))));
    const std::uint64_t firstAbsolute = std::max(start, epoch);
    std::uint64_t step = firstAbsolute > epoch
        ? (firstAbsolute - epoch) / interval : 0;
    if (step > 0) --step;
    const std::uint64_t stop = start + static_cast<std::uint64_t>(sampleCount);
    for (;; ++step) {
        if (step > (std::numeric_limits<std::uint64_t>::max() - epoch) / interval) break;
        const std::uint64_t absolute = epoch + step * interval;
        if (absolute >= stop) break;
        if (absolute < start) continue;
        const int patternStep = static_cast<int>(step % static_cast<std::uint64_t>(stepCount));
        const std::uint64_t mask = patternStep < 64 ? playLow : playHigh;
        const int bit = patternStep < 64 ? patternStep : patternStep - 64;
        if (bit >= 0 && bit < 64 && ((mask >> bit) & 1ULL) != 0) {
            expected.push_back(static_cast<std::int64_t>(absolute - start));
        }
    }
    return expected;
}

ExpectedClickAnalysis analyzeExpectedClicks(
    const QJsonObject& sidecar,
    const std::vector<std::int16_t>& samples,
    int threshold,
    std::int64_t refractoryFrames,
    std::int64_t toleranceFrames)
{
    const auto expected = expectedMetronomeFrames(
        sidecar, static_cast<std::int64_t>(samples.size()));
    const auto detected = detectEvents(samples, threshold, refractoryFrames);
    const RawClickMatch raw = matchClicks(expected, detected, toleranceFrames);
    ExpectedClickAnalysis result;
    result.expectedClicks = expected.size();
    result.detectedClicks = detected.size();
    result.steadyMissingClicks = raw.missing;
    result.steadyExtraClicks = raw.extra;
    result.steadyMaximumErrorFrames = raw.errors.empty()
        ? 0 : *std::max_element(raw.errors.begin(), raw.errors.end());
    if (raw.missing != 0 || raw.extra != 0) {
        const std::size_t maximumSkip = std::min<std::size_t>(3, detected.size());
        for (std::size_t skip = 0; skip <= maximumSkip; ++skip) {
            std::int64_t maximumError = 0;
            if (startupBoundaryMatch(
                    expected, detected, skip, toleranceFrames, maximumError)) {
                result.startupBoundaryMismatch = true;
                result.steadyMissingClicks = 0;
                result.steadyExtraClicks = 0;
                result.steadyMaximumErrorFrames = maximumError;
                break;
            }
        }
    }
    result.ok = !expected.empty() &&
        result.steadyMissingClicks == 0 && result.steadyExtraClicks == 0 &&
        result.steadyMaximumErrorFrames <= toleranceFrames;
    return result;
}

IntervalGridAnalysis analyzeIntervalGrid(
    const std::vector<std::int16_t>& samples,
    int threshold,
    std::int64_t refractoryFrames,
    std::int64_t intervalFrames)
{
    IntervalGridAnalysis result;
    const auto clicks = detectEvents(samples, threshold, refractoryFrames);
    result.clicks = clicks.size();
    if (intervalFrames <= 0) return result;
    for (std::size_t index = 1; index < clicks.size(); ++index) {
        const std::int64_t delta = clicks[index] - clicks[index - 1];
        const std::int64_t multiple = std::max<std::int64_t>(
            1, std::llround(static_cast<double>(delta) / intervalFrames));
        const std::int64_t error = std::llabs(delta - multiple * intervalFrames);
        result.maximumPhaseErrorFrames = std::max(result.maximumPhaseErrorFrames, error);
        result.maximumIntervalMultiple = std::max(result.maximumIntervalMultiple, multiple);
        ++result.intervals;
    }
    return result;
}

NearestEventAnalysis nearestEvents(
    const std::vector<std::int64_t>& references,
    const std::vector<std::int64_t>& measured,
    std::int64_t maximumAbsoluteError)
{
    NearestEventAnalysis result;
    std::vector<bool> used(measured.size(), false);
    for (const std::int64_t reference : references) {
        std::size_t bestIndex = measured.size();
        std::int64_t bestAbsolute = std::numeric_limits<std::int64_t>::max();
        std::int64_t bestSigned = 0;
        for (std::size_t index = 0; index < measured.size(); ++index) {
            if (used[index]) continue;
            const std::int64_t signedError = measured[index] - reference;
            const std::int64_t absolute = std::llabs(signedError);
            if (absolute < bestAbsolute) {
                bestIndex = index;
                bestAbsolute = absolute;
                bestSigned = signedError;
            }
        }
        if (bestIndex == measured.size() || bestAbsolute > maximumAbsoluteError) {
            ++result.missingReferences;
        } else {
            used[bestIndex] = true;
            result.signedErrors.push_back(bestSigned);
        }
    }
    result.extraMeasured = static_cast<std::size_t>(
        std::count(used.begin(), used.end(), false));
    return result;
}

double estimateToneHz(
    const std::vector<std::int16_t>& samples,
    int sampleRate,
    std::int64_t startFrame,
    std::int64_t stopFrame)
{
    if (sampleRate <= 0 || samples.empty()) return 0.0;
    const std::int64_t start = std::clamp<std::int64_t>(
        startFrame, 0, static_cast<std::int64_t>(samples.size()));
    const std::int64_t stop = std::clamp<std::int64_t>(
        stopFrame, start, static_cast<std::int64_t>(samples.size()));
    std::vector<std::int64_t> crossings;
    for (std::int64_t frame = start + 1; frame < stop; ++frame) {
        if (samples[static_cast<std::size_t>(frame - 1)] <= 0 &&
            samples[static_cast<std::size_t>(frame)] > 0) {
            crossings.push_back(frame);
        }
    }
    std::vector<std::int64_t> periods;
    const std::int64_t minimumPeriod = std::max<std::int64_t>(1, sampleRate / 600);
    const std::int64_t maximumPeriod = std::max<std::int64_t>(minimumPeriod, sampleRate / 300);
    for (std::size_t index = 1; index < crossings.size(); ++index) {
        const std::int64_t period = crossings[index] - crossings[index - 1];
        if (period >= minimumPeriod && period <= maximumPeriod) periods.push_back(period);
    }
    if (periods.size() < 20) return 0.0;
    const auto middle = periods.begin() + static_cast<std::ptrdiff_t>(periods.size() / 2);
    std::nth_element(periods.begin(), middle, periods.end());
    return static_cast<double>(sampleRate) / static_cast<double>(*middle);
}

double rootMeanSquare(
    const std::vector<std::int16_t>& samples,
    std::int64_t startFrame,
    std::int64_t stopFrame)
{
    const std::int64_t start = std::clamp<std::int64_t>(
        startFrame, 0, static_cast<std::int64_t>(samples.size()));
    const std::int64_t stop = std::clamp<std::int64_t>(
        stopFrame, start, static_cast<std::int64_t>(samples.size()));
    if (stop <= start) return 0.0;
    long double sum = 0.0;
    for (std::int64_t frame = start; frame < stop; ++frame) {
        const long double sample = samples[static_cast<std::size_t>(frame)];
        sum += sample * sample;
    }
    return std::sqrt(static_cast<double>(sum / static_cast<long double>(stop - start)));
}

} // namespace jam2::test
