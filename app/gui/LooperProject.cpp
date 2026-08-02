#include "LooperProject.hpp"
#include "ContentLimits.hpp"
#include "metronome.hpp"

#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>
#include <QUuid>

#include <cmath>
#include <limits>

namespace {
constexpr int kBankCount = jam2::application::limits::kLooperBankCount;
constexpr int kMaxLanesPerBank = jam2::application::limits::kMaximumLooperLanesPerBank;
constexpr int kMaxIdCharacters = jam2::application::limits::kMaximumLooperIdCharacters;
constexpr int kMaxNameCharacters = jam2::application::limits::kMaximumLooperNameCharacters;
constexpr int kMaxPathCharacters = jam2::application::limits::kMaximumLooperPathCharacters;

QString laneId(const QString& value)
{
    return value.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : value;
}

qint64 jsonFrame(const QJsonObject& object, const char* key, qint64 fallback)
{
    const QJsonValue value = object.value(QString::fromLatin1(key));
    if (value.isString()) {
        bool ok = false;
        const qint64 parsed = value.toString().toLongLong(&ok);
        return ok ? parsed : fallback;
    }
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (std::isfinite(number) && std::floor(number) == number &&
            number >= static_cast<double>(std::numeric_limits<qint64>::min()) &&
            number <= static_cast<double>(std::numeric_limits<qint64>::max())) {
            return static_cast<qint64>(number);
        }
    }
    return fallback;
}

bool validBankId(const QString& id, int index)
{
    return id == QString(QChar('A' + index));
}

bool validAssetHash(const QString& hash)
{
    static const QRegularExpression expression(QStringLiteral("^[0-9a-f]{64}$"));
    return hash.isEmpty() || expression.match(hash).hasMatch();
}

bool validJsonFrameValue(const QJsonValue& value)
{
    if (value.isUndefined()) {
        return true;
    }
    if (value.isString()) {
        bool ok = false;
        (void)value.toString().toLongLong(&ok);
        return ok;
    }
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    return std::isfinite(number) && std::floor(number) == number &&
        number >= static_cast<double>(std::numeric_limits<qint64>::min()) &&
        number <= static_cast<double>(std::numeric_limits<qint64>::max());
}

std::uint64_t jsonMask(const QJsonObject& object, const char* key, std::uint64_t fallback)
{
    const QJsonValue value = object.value(QString::fromLatin1(key));
    if (!value.isString()) return fallback;
    bool ok = false;
    const qulonglong parsed = value.toString().toULongLong(&ok);
    return ok ? static_cast<std::uint64_t>(parsed) : fallback;
}

LooperBankTiming sanitizedTiming(LooperBankTiming timing, bool allowInheritance)
{
    jam2::metronome::PatternSnapshot pattern;
    pattern.bpm = timing.bpm;
    pattern.beats_per_bar = timing.beatsPerBar;
    pattern.beat_unit = timing.beatUnit;
    pattern.tempo_pulse_units = timing.tempoPulseUnits;
    pattern.division = timing.division;
    pattern.play_mask_low = timing.playMaskLow;
    pattern.play_mask_high = timing.playMaskHigh;
    pattern.accent_mask_low = timing.accentMaskLow;
    pattern.accent_mask_high = timing.accentMaskHigh;
    pattern = jam2::metronome::sanitize(pattern);
    timing.bpm = pattern.bpm;
    timing.beatsPerBar = pattern.beats_per_bar;
    timing.beatUnit = pattern.beat_unit;
    timing.tempoPulseUnits = pattern.tempo_pulse_units;
    timing.division = pattern.division;
    timing.playMaskLow = pattern.play_mask_low;
    timing.playMaskHigh = pattern.play_mask_high;
    timing.accentMaskLow = pattern.accent_mask_low;
    timing.accentMaskHigh = pattern.accent_mask_high;
    timing.inheritsBankA = allowInheritance && timing.inheritsBankA;
    return timing;
}
}

LooperProject::LooperProject()
{
    int index = 0;
    for (const QChar bank : {QLatin1Char('A'), QLatin1Char('B'), QLatin1Char('C'), QLatin1Char('D')}) {
        LooperBankTiming timing;
        timing.inheritsBankA = index > 0;
        banks_.append(LooperBank{QString(bank), {}, timing});
        ++index;
    }
}

const QVector<LooperBank>& LooperProject::banks() const { return banks_; }
QVector<LooperBank>& LooperProject::banks() { return banks_; }
int LooperProject::activeBankIndex() const { return activeBankIndex_; }
void LooperProject::setActiveBankIndex(int index) { activeBankIndex_ = qBound(0, index, banks_.size() - 1); }
bool LooperProject::gridLockEnabled() const { return gridLockEnabled_; }
void LooperProject::setGridLockEnabled(bool enabled) { gridLockEnabled_ = enabled; }
bool LooperProject::trackSyncEnabled() const { return trackSyncEnabled_; }
void LooperProject::setTrackSyncEnabled(bool enabled) { trackSyncEnabled_ = enabled; }
LooperBankTiming LooperProject::resolvedTiming(int bankIndex) const
{
    const int bounded = qBound(0, bankIndex, banks_.size() - 1);
    if (bounded > 0 && banks_.at(bounded).timing.inheritsBankA) {
        LooperBankTiming timing = banks_.front().timing;
        timing.inheritsBankA = true;
        return timing;
    }
    return banks_.at(bounded).timing;
}
bool LooperProject::setTiming(int bankIndex, LooperBankTiming timing)
{
    if (bankIndex < 0 || bankIndex >= banks_.size()) return false;
    banks_[bankIndex].timing = sanitizedTiming(std::move(timing), bankIndex > 0);
    hasSerializedTiming_ = true;
    return true;
}
bool LooperProject::hasSerializedTiming() const { return hasSerializedTiming_; }
const ArrangementDefinition& LooperProject::arrangement() const { return arrangement_; }
bool LooperProject::setArrangement(ArrangementDefinition arrangement)
{
    if (arrangement.steps.size() > 64) return false;
    for (const ArrangementStep& step : arrangement.steps) {
        if (step.bankIndex < 0 || step.bankIndex >= kBankCount ||
            step.repeats < 1 || step.repeats > 64) return false;
    }
    arrangement_ = std::move(arrangement);
    return true;
}
bool LooperProject::appendLane(int bankIndex, LooperLane lane)
{
    if (bankIndex < 0 || bankIndex >= banks_.size() || banks_[bankIndex].lanes.size() >= kMaxLanesPerBank ||
        lane.id.size() > kMaxIdCharacters || lane.name.size() > kMaxNameCharacters ||
        lane.assetPath.size() > kMaxPathCharacters || !validAssetHash(lane.assetHash) ||
        lane.referenceKind.size() > 16 || !validAssetHash(lane.referenceSourceSignature) ||
        !std::isfinite(lane.referenceBpm) || lane.referenceBpm < 0.0 || lane.referenceBpm > 400.0) {
        return false;
    }
    lane.id = laneId(lane.id);
    if (lane.name.trimmed().isEmpty()) {
        if (lane.assetPath.trimmed().isEmpty()) {
            int number = 1;
            while (true) {
                const QString candidate = QStringLiteral("Empty Track %1").arg(number);
                bool used = false;
                for (const LooperLane& existing : banks_[bankIndex].lanes) {
                    if (existing.name.compare(candidate, Qt::CaseInsensitive) == 0) {
                        used = true;
                        break;
                    }
                }
                if (!used) {
                    lane.name = candidate;
                    break;
                }
                ++number;
            }
        } else {
            lane.name = lane.assetPath.section(QLatin1Char('/'), -1).section(QLatin1Char('\\'), -1);
        }
    }
    banks_[bankIndex].lanes.append(std::move(lane));
    return true;
}

bool LooperProject::removeLane(int bankIndex, int laneIndex)
{
    if (bankIndex < 0 || bankIndex >= banks_.size() || laneIndex < 0 || laneIndex >= banks_[bankIndex].lanes.size()) {
        return false;
    }
    banks_[bankIndex].lanes.removeAt(laneIndex);
    return true;
}

bool LooperProject::moveLane(int bankIndex, int from, int to)
{
    if (bankIndex < 0 || bankIndex >= banks_.size() || from < 0 || to < 0 ||
        from >= banks_[bankIndex].lanes.size() || to >= banks_[bankIndex].lanes.size()) {
        return false;
    }
    banks_[bankIndex].lanes.move(from, to);
    return true;
}

bool LooperProject::renameLane(int bankIndex, int laneIndex, const QString& name)
{
    if (bankIndex < 0 || bankIndex >= banks_.size() || laneIndex < 0 || laneIndex >= banks_[bankIndex].lanes.size()) {
        return false;
    }
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    banks_[bankIndex].lanes[laneIndex].name = trimmed;
    return true;
}

QJsonObject LooperProject::toJson(bool syncCompatibleOnly) const
{
    QJsonArray banks;
    for (const LooperBank& bank : banks_) {
        QJsonArray lanes;
        for (const LooperLane& lane : bank.lanes) {
            if (syncCompatibleOnly && (!lane.sampleRateCompatible || lane.localOnly)) {
                continue;
            }
            QJsonObject laneObject{{"id", lane.id}, {"asset_path", lane.assetPath}, {"asset_hash", lane.assetHash},
                {"name", lane.name}, {"sample_rate", lane.sampleRate},
                {"start_frame", QString::number(lane.startFrame)}, {"stop_frame", QString::number(lane.stopFrame)},
                {"loop_start_frame", QString::number(lane.loopStartFrame)}, {"loop_end_frame", QString::number(lane.loopEndFrame)},
                {"gain_db", lane.gainDb}, {"muted", lane.muted}, {"solo", lane.solo}, {"loop_enabled", lane.loopEnabled},
                {"reference_kind", lane.referenceKind}, {"reference_source_signature", lane.referenceSourceSignature},
                {"reference_bpm", lane.referenceBpm},
                {"reference_stale", lane.referenceStale}};
            if (!syncCompatibleOnly) {
                laneObject.insert(QStringLiteral("local_only"), lane.localOnly);
            }
            lanes.append(std::move(laneObject));
        }
        const LooperBankTiming timing = bank.timing;
        banks.append(QJsonObject{
            {"id", bank.id},
            {"lanes", lanes},
            {"timing", QJsonObject{
                {"version", 1},
                {"inherits_bank_a", timing.inheritsBankA},
                {"bpm", timing.bpm},
                {"beats_per_bar", timing.beatsPerBar},
                {"beat_unit", timing.beatUnit},
                {"tempo_pulse_units", timing.tempoPulseUnits},
                {"division", timing.division},
                {"play_mask_low", QString::number(timing.playMaskLow)},
                {"play_mask_high", QString::number(timing.playMaskHigh)},
                {"accent_mask_low", QString::number(timing.accentMaskLow)},
                {"accent_mask_high", QString::number(timing.accentMaskHigh)},
            }},
        });
    }
    QJsonArray arrangementSteps;
    for (const ArrangementStep& step : arrangement_.steps) {
        arrangementSteps.append(QJsonObject{
            {QStringLiteral("bank"), step.bankIndex},
            {QStringLiteral("repeats"), step.repeats},
        });
    }
    return QJsonObject{{"active_bank", activeBankIndex_}, {"grid_lock", gridLockEnabled_},
        {"banks", banks},
        {"arrangement", QJsonObject{
            {QStringLiteral("version"), 1},
            {QStringLiteral("loop"), arrangement_.loop},
            {QStringLiteral("steps"), arrangementSteps},
        }}};
}

bool LooperProject::loadJson(const QJsonObject& object)
{
    const QJsonArray savedBanks = object.value(QStringLiteral("banks")).toArray();
    if (savedBanks.size() != kBankCount) return false;
    QVector<LooperBank> loaded;
    bool loadedTiming = true;
    for (int i = 0; i < savedBanks.size(); ++i) {
        const QJsonObject bankObject = savedBanks.at(i).toObject();
        const QString bankId = bankObject.value(QStringLiteral("id")).toString(QString(QChar('A' + i)));
        if (!validBankId(bankId, i)) {
            return false;
        }
        LooperBankTiming bankTiming;
        bankTiming.inheritsBankA = i > 0;
        const QJsonValue timingValue = bankObject.value(QStringLiteral("timing"));
        if (timingValue.isUndefined()) {
            loadedTiming = false;
        } else {
            if (!timingValue.isObject()) return false;
            const QJsonObject timing = timingValue.toObject();
            if (timing.value(QStringLiteral("version")).toInt() != 1) return false;
            bankTiming.bpm = timing.value(QStringLiteral("bpm")).toInt(120);
            bankTiming.beatsPerBar = timing.value(QStringLiteral("beats_per_bar")).toInt(4);
            bankTiming.beatUnit = timing.value(QStringLiteral("beat_unit")).toInt(4);
            bankTiming.tempoPulseUnits = timing.value(QStringLiteral("tempo_pulse_units")).toInt(1);
            bankTiming.division = timing.value(QStringLiteral("division")).toInt(1);
            bankTiming.playMaskLow = jsonMask(timing, "play_mask_low", 0x0fULL);
            bankTiming.playMaskHigh = jsonMask(timing, "play_mask_high", 0);
            bankTiming.accentMaskLow = jsonMask(timing, "accent_mask_low", 0x01ULL);
            bankTiming.accentMaskHigh = jsonMask(timing, "accent_mask_high", 0);
            bankTiming.inheritsBankA = i > 0 &&
                timing.value(QStringLiteral("inherits_bank_a")).toBool(false);
            bankTiming = sanitizedTiming(std::move(bankTiming), i > 0);
        }
        LooperBank bank{bankId, {}, bankTiming};
        const QJsonArray savedLanes = bankObject.value(QStringLiteral("lanes")).toArray();
        if (savedLanes.size() > kMaxLanesPerBank) {
            return false;
        }
        for (const QJsonValue& value : savedLanes) {
            if (!value.isObject()) {
                return false;
            }
            const QJsonObject laneObject = value.toObject();
            for (const QString& key : {
                    QStringLiteral("id"), QStringLiteral("asset_path"),
                    QStringLiteral("asset_hash"), QStringLiteral("name"),
                    QStringLiteral("reference_kind"), QStringLiteral("reference_source_signature")}) {
                const QJsonValue text = laneObject.value(key);
                if (!text.isUndefined() && !text.isString()) {
                    return false;
                }
            }
            for (const QString& key : {
                    QStringLiteral("start_frame"), QStringLiteral("stop_frame"),
                    QStringLiteral("loop_start_frame"), QStringLiteral("loop_end_frame")}) {
                if (!validJsonFrameValue(laneObject.value(key))) {
                    return false;
                }
            }
            for (const QString& key : {
                    QStringLiteral("muted"), QStringLiteral("solo"), QStringLiteral("loop_enabled"),
                    QStringLiteral("reference_stale"), QStringLiteral("local_only")}) {
                const QJsonValue flag = laneObject.value(key);
                if (!flag.isUndefined() && !flag.isBool()) {
                    return false;
                }
            }
            const QJsonValue gain = laneObject.value(QStringLiteral("gain_db"));
            if (!gain.isUndefined() && (!gain.isDouble() || !std::isfinite(gain.toDouble()))) {
                return false;
            }
            const QJsonValue referenceBpm = laneObject.value(QStringLiteral("reference_bpm"));
            if (!referenceBpm.isUndefined() &&
                (!referenceBpm.isDouble() || !std::isfinite(referenceBpm.toDouble()) ||
                 referenceBpm.toDouble() < 0.0 || referenceBpm.toDouble() > 400.0)) {
                return false;
            }
            const QJsonValue sampleRate = laneObject.value(QStringLiteral("sample_rate"));
            if (!sampleRate.isUndefined() &&
                (!sampleRate.isDouble() || sampleRate.toDouble() < 0 ||
                 sampleRate.toDouble() > jam2::application::limits::kMaximumSampleRate ||
                 std::floor(sampleRate.toDouble()) != sampleRate.toDouble())) {
                return false;
            }
            LooperLane lane;
            lane.id = laneId(laneObject.value(QStringLiteral("id")).toString());
            lane.assetPath = laneObject.value(QStringLiteral("asset_path")).toString();
            lane.assetHash = laneObject.value(QStringLiteral("asset_hash")).toString();
            lane.name = laneObject.value(QStringLiteral("name")).toString();
            lane.sampleRate = laneObject.value(QStringLiteral("sample_rate")).toInt();
            lane.startFrame = jsonFrame(laneObject, "start_frame", 0);
            lane.stopFrame = jsonFrame(laneObject, "stop_frame", -1);
            lane.loopStartFrame = jsonFrame(laneObject, "loop_start_frame", -1);
            lane.loopEndFrame = jsonFrame(laneObject, "loop_end_frame", -1);
            lane.gainDb = laneObject.value(QStringLiteral("gain_db")).toDouble();
            lane.muted = laneObject.value(QStringLiteral("muted")).toBool();
            lane.solo = laneObject.value(QStringLiteral("solo")).toBool();
            lane.loopEnabled = laneObject.value(QStringLiteral("loop_enabled")).toBool();
            lane.referenceKind = laneObject.value(QStringLiteral("reference_kind")).toString();
            lane.referenceSourceSignature = laneObject.value(QStringLiteral("reference_source_signature")).toString();
            lane.referenceBpm = laneObject.value(QStringLiteral("reference_bpm")).toDouble();
            lane.referenceStale = laneObject.value(QStringLiteral("reference_stale")).toBool();
            lane.localOnly = laneObject.value(QStringLiteral("local_only")).toBool();
            if (lane.id.size() > kMaxIdCharacters || lane.name.size() > kMaxNameCharacters ||
                lane.assetPath.size() > kMaxPathCharacters || !validAssetHash(lane.assetHash) ||
                lane.referenceKind.size() > 16 || !validAssetHash(lane.referenceSourceSignature) ||
                !std::isfinite(lane.gainDb) || lane.gainDb < -120.0 || lane.gainDb > 24.0 ||
                !std::isfinite(lane.referenceBpm) || lane.referenceBpm < 0.0 || lane.referenceBpm > 400.0 ||
                lane.startFrame < 0 ||
                (lane.stopFrame >= 0 && lane.stopFrame < lane.startFrame) ||
                (lane.loopStartFrame >= 0 && lane.loopEndFrame >= 0 && lane.loopEndFrame <= lane.loopStartFrame)) {
                return false;
            }
            bank.lanes.append(std::move(lane));
        }
        loaded.append(std::move(bank));
    }
    ArrangementDefinition arrangement;
    const QJsonValue arrangementValue = object.value(QStringLiteral("arrangement"));
    if (!arrangementValue.isUndefined()) {
        if (!arrangementValue.isObject()) return false;
        const QJsonObject savedArrangement = arrangementValue.toObject();
        const QJsonValue version = savedArrangement.value(QStringLiteral("version"));
        const QJsonValue loop = savedArrangement.value(QStringLiteral("loop"));
        const QJsonValue steps = savedArrangement.value(QStringLiteral("steps"));
        if (!version.isDouble() || version.toInt() != 1 ||
            !loop.isBool() || !steps.isArray() || steps.toArray().size() > 64) return false;
        arrangement.loop = loop.toBool(true);
        for (const QJsonValue& stepValue : steps.toArray()) {
            if (!stepValue.isObject()) return false;
            const QJsonObject step = stepValue.toObject();
            const QJsonValue bank = step.value(QStringLiteral("bank"));
            const QJsonValue repeats = step.value(QStringLiteral("repeats"));
            if (!bank.isDouble() || !repeats.isDouble() ||
                bank.toDouble() != std::floor(bank.toDouble()) ||
                repeats.toDouble() != std::floor(repeats.toDouble()) ||
                bank.toInt() < 0 || bank.toInt() >= kBankCount ||
                repeats.toInt() < 1 || repeats.toInt() > 64) return false;
            arrangement.steps.push_back(ArrangementStep{bank.toInt(), repeats.toInt()});
        }
    }
    banks_ = std::move(loaded);
    hasSerializedTiming_ = loadedTiming;
    arrangement_ = std::move(arrangement);
    setActiveBankIndex(object.value(QStringLiteral("active_bank")).toInt());
    gridLockEnabled_ = object.value(QStringLiteral("grid_lock")).toBool(true);
    return true;
}
