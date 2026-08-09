#include "CuratedIdeaCatalog.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <cmath>

namespace jam2::practice {
namespace {

constexpr int kCatalogVersion = 1;
constexpr int kMaximumIdeas = 128;
constexpr int kMaximumIdLength = 96;
constexpr int kMaximumNameLength = 160;

bool boundedString(
    const QJsonObject& object,
    const QString& key,
    int maximum,
    QString& output,
    bool required = true)
{
    const QJsonValue value = object.value(key);
    if ((!required && value.isUndefined()) || value.isNull()) {
        output.clear();
        return !required;
    }
    if (!value.isString() || value.toString().trimmed().isEmpty() ||
        value.toString().size() > maximum) {
        return false;
    }
    output = value.toString();
    return true;
}

bool exactInteger(
    const QJsonObject& object,
    const QString& key,
    int minimum,
    int maximum,
    int& output)
{
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) return false;
    const double raw = value.toDouble();
    if (!std::isfinite(raw) || raw != std::floor(raw) ||
        raw < minimum || raw > maximum) {
        return false;
    }
    output = static_cast<int>(raw);
    return true;
}

} // namespace

ChordIdeaRequest CuratedIdeaEntry::generationRequest(int targetSectionIndex) const
{
    ChordIdeaRequest request;
    request.styleId = styleId;
    request.profileId = profileId;
    // Groove-library sources are always complete 32-bar performances. The
    // catalog form id is descriptive metadata rather than a native-form
    // override, because native forms can be shorter than the library source.
    request.bars = 32;
    request.meterId = meterId;
    request.targetSectionIndex = targetSectionIndex;
    request.harmonicComplexity = complexity;
    request.rhythmicComplexity = complexity;
    return request;
}

QVector<CuratedIdeaEntry> loadCuratedIdeaCatalog(QString& error)
{
    error.clear();
    QFile file(QStringLiteral(":/jam2/ideas/catalog.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("The embedded idea catalog is unavailable.");
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = QStringLiteral("The embedded idea catalog is invalid: %1")
            .arg(parseError.errorString());
        return {};
    }
    const QJsonObject root = document.object();
    int version = 0;
    if (!exactInteger(root, QStringLiteral("version"), kCatalogVersion,
            kCatalogVersion, version) ||
        !root.value(QStringLiteral("ideas")).isArray()) {
        error = QStringLiteral("The embedded idea catalog has an unsupported schema.");
        return {};
    }
    const QJsonArray items = root.value(QStringLiteral("ideas")).toArray();
    if (items.isEmpty() || items.size() > kMaximumIdeas) {
        error = QStringLiteral("The embedded idea catalog must contain 1 to %1 ideas.")
            .arg(kMaximumIdeas);
        return {};
    }

    QVector<CuratedIdeaEntry> result;
    result.reserve(items.size());
    QSet<QString> ids;
    for (int index = 0; index < items.size(); ++index) {
        if (!items.at(index).isObject()) {
            error = QStringLiteral("Idea catalog entry %1 is not an object.").arg(index + 1);
            return {};
        }
        const QJsonObject item = items.at(index).toObject();
        CuratedIdeaEntry entry;
        QString seedText;
        if (!boundedString(item, QStringLiteral("id"), kMaximumIdLength, entry.id) ||
            !boundedString(item, QStringLiteral("name"), kMaximumNameLength, entry.name) ||
            !boundedString(item, QStringLiteral("style_id"), kMaximumIdLength, entry.styleId) ||
            !boundedString(item, QStringLiteral("style_name"), kMaximumNameLength, entry.styleName) ||
            !boundedString(item, QStringLiteral("profile_id"), kMaximumIdLength, entry.profileId) ||
            !boundedString(item, QStringLiteral("profile_name"), kMaximumNameLength, entry.profileName) ||
            !boundedString(item, QStringLiteral("form_id"), kMaximumIdLength, entry.formId) ||
            !boundedString(item, QStringLiteral("form_name"), kMaximumNameLength, entry.formName) ||
            !boundedString(item, QStringLiteral("preview_resource"), 256, entry.previewResource) ||
            !boundedString(item, QStringLiteral("preview_sha256"), 64, entry.previewSha256) ||
            !boundedString(item, QStringLiteral("chord_fingerprint"), 64, entry.chordFingerprint) ||
            !boundedString(item, QStringLiteral("beat_fingerprint"), 64, entry.beatFingerprint) ||
            !boundedString(item, QStringLiteral("tonic"), 32, entry.tonic) ||
            !boundedString(item, QStringLiteral("mode"), 64, entry.mode) ||
            !boundedString(item, QStringLiteral("meter_id"), 32, entry.meterId) ||
            !boundedString(item, QStringLiteral("seed"), 16, seedText) ||
            !exactInteger(item, QStringLiteral("generator_version"), 1, 1000, entry.generatorVersion) ||
            !exactInteger(item, QStringLiteral("complexity"), 1, 8, entry.complexity) ||
            !exactInteger(item, QStringLiteral("bpm"), 20, 400, entry.bpm) ||
            !exactInteger(item, QStringLiteral("meter_numerator"), 1, 32, entry.meterNumerator) ||
            !exactInteger(item, QStringLiteral("meter_denominator"), 1, 32, entry.meterDenominator) ||
            !exactInteger(item, QStringLiteral("bars"), 1, 64, entry.bars) ||
            !exactInteger(item, QStringLiteral("preview_bars"), 1, 4, entry.previewBars)) {
            error = QStringLiteral("Idea catalog entry %1 has invalid or missing fields.")
                .arg(index + 1);
            return {};
        }
        bool seedOk = false;
        const qulonglong seed = seedText.toULongLong(&seedOk);
        if (!seedOk || seed > 0xffffffffULL || ids.contains(entry.id) ||
            entry.bars != 32 || entry.previewBars != 4 ||
            entry.previewSha256.size() != 64 || entry.chordFingerprint.size() != 64 ||
            entry.beatFingerprint.size() != 64 ||
            !entry.previewResource.startsWith(QStringLiteral(":/jam2/ideas/previews/")) ||
            !QFile::exists(entry.previewResource)) {
            error = QStringLiteral("Idea catalog entry %1 failed identity or resource validation.")
                .arg(index + 1);
            return {};
        }
        entry.seed = static_cast<std::uint32_t>(seed);
        ids.insert(entry.id);
        result.push_back(std::move(entry));
    }
    return result;
}

} // namespace jam2::practice
