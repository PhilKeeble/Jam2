#include "PreparedMixRenderer.hpp"

#include "signalsmith-stretch/signalsmith-stretch.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {
quint16 le16(const QByteArray& b, qsizetype o) { return quint16(uchar(b[o])) | quint16(uchar(b[o + 1]) << 8); }
quint32 le32(const QByteArray& b, qsizetype o) { return quint32(uchar(b[o])) | (quint32(uchar(b[o + 1])) << 8) | (quint32(uchar(b[o + 2])) << 16) | (quint32(uchar(b[o + 3])) << 24); }
void put16(QByteArray& b, quint16 v) { b.append(char(v)); b.append(char(v >> 8)); }
void put32(QByteArray& b, quint32 v) { b.append(char(v)); b.append(char(v >> 8)); b.append(char(v >> 16)); b.append(char(v >> 24)); }
QString absoluteAsset(const QString& folder, const QString& path) { return QFileInfo(path).isAbsolute() ? path : QDir(folder).absoluteFilePath(path); }

struct FocusState {
    double x1 = 0.0;
    double x2 = 0.0;
    double y1 = 0.0;
    double y2 = 0.0;
};

void applyFocus(std::vector<float>& samples, int sampleRate, const SharedTrackModel& track)
{
    if (!track.focusEnabled || std::abs(track.focusGainDb) < 0.001 || samples.empty() || sampleRate <= 0) {
        return;
    }
    const double frequency = qBound(20.0, track.focusFrequencyHz, static_cast<double>(sampleRate) * 0.45);
    const double q = qBound(0.1, track.focusQ, 20.0);
    const double omega = 2.0 * 3.14159265358979323846 * frequency / static_cast<double>(sampleRate);
    const double sinOmega = std::sin(omega);
    const double cosOmega = std::cos(omega);
    const double alpha = sinOmega / (2.0 * q);
    const double makeup = std::pow(10.0, qBound(-24.0, track.focusGainDb, 24.0) / 20.0);

    const double b0 = alpha;
    const double b1 = 0.0;
    const double b2 = -alpha;
    const double a0 = 1.0 + alpha;
    const double a1 = -2.0 * cosOmega;
    const double a2 = 1.0 - alpha;
    const double focusA0 = makeup * b0 / a0;
    const double focusA1 = makeup * b1 / a0;
    const double focusA2 = makeup * b2 / a0;
    const double focusB1 = a1 / a0;
    const double focusB2 = a2 / a0;

    FocusState state;
    for (float& sample : samples) {
        const double input = sample;
        const double output = focusA0 * input + focusA1 * state.x1 + focusA2 * state.x2 - focusB1 * state.y1 - focusB2 * state.y2;
        state.x2 = state.x1;
        state.x1 = input;
        state.y2 = state.y1;
        state.y1 = output;
        sample = static_cast<float>(qBound(-1.0, output, 1.0));
    }
}

std::vector<float> processTrack(std::vector<float> input, int sampleRate, const SharedTrackModel& track, QString& error)
{
    const double speed = qBound(0.10, track.speed, 2.0);
    const int pitchCents = qBound(-1200, track.pitchCents, 1200);
    const qint64 outputFrames = static_cast<qint64>(std::ceil(static_cast<double>(input.size()) / speed));
    const qint64 maxFrames = static_cast<qint64>(std::max(1, sampleRate)) * 60LL * 5LL;
    if (outputFrames > maxFrames) {
        error = QStringLiteral("prepared mix exceeds five-minute limit after speed processing");
        return {};
    }
    if (outputFrames <= 0) {
        return {};
    }

    std::vector<float> output(static_cast<std::size_t>(outputFrames), 0.0f);
    if (pitchCents == 0) {
        for (qint64 frame = 0; frame < outputFrames; ++frame) {
            const double source = static_cast<double>(frame) * speed;
            const qint64 left = static_cast<qint64>(std::floor(source));
            const qint64 right = left + 1;
            const double frac = source - static_cast<double>(left);
            const double a = left >= 0 && left < static_cast<qint64>(input.size()) ? input[static_cast<std::size_t>(left)] : 0.0;
            const double b = right >= 0 && right < static_cast<qint64>(input.size()) ? input[static_cast<std::size_t>(right)] : a;
            output[static_cast<std::size_t>(frame)] = static_cast<float>(a + (b - a) * frac);
        }
    } else {
        signalsmith::stretch::SignalsmithStretch<float> stretch;
        stretch.presetDefault(1, static_cast<float>(std::max(1, sampleRate)), true);
        stretch.setTransposeSemitones(static_cast<float>(pitchCents) / 100.0f);
        std::vector<std::vector<float>> inputBlock(1);
        std::vector<std::vector<float>> outputBlock(1);
        inputBlock[0] = std::move(input);
        outputBlock[0].assign(static_cast<std::size_t>(outputFrames), 0.0f);
        stretch.process(inputBlock, static_cast<int>(inputBlock[0].size()), outputBlock, static_cast<int>(outputFrames));
        output = std::move(outputBlock[0]);
    }
    applyFocus(output, sampleRate, track);
    return output;
}
}

PreparedMixResult PreparedMixRenderer::render(const LooperProject& project, const QString& projectFolder, int sampleRate, const QString& outputPath, const SharedTrackModel& track)
{
    PreparedMixResult result;
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    if (sampleRate <= 0) { result.error = QStringLiteral("prepared mix sample rate must be positive"); return result; }
    const LooperBank& bank = project.banks().at(project.activeBankIndex());
    bool anySolo = false;
    for (const LooperLane& lane : bank.lanes) anySolo |= lane.solo && !lane.muted;
    const qint64 kMaxFrames = 5LL * 60LL * qMax(1, sampleRate);
    qint64 length = 0;
    struct Decoded { const LooperLane* lane; QByteArray data; int channels; qint64 frames; qint64 sourceStart; qint64 sourceEnd; };
    std::vector<Decoded> decoded;
    for (const LooperLane& lane : bank.lanes) {
        if (lane.muted || (anySolo && !lane.solo)) continue;
        if (lane.assetPath.trimmed().isEmpty()) continue;
        QFile file(absoluteAsset(projectFolder, lane.assetPath));
        if (!file.open(QIODevice::ReadOnly)) { result.error = QStringLiteral("cannot open lane WAV: %1").arg(lane.name); return result; }
        const QByteArray wav = file.readAll();
        if (wav.size() < 44 || wav.mid(0, 4) != "RIFF" || wav.mid(8, 4) != "WAVE") { result.error = QStringLiteral("invalid WAV: %1").arg(lane.name); return result; }
        qsizetype off = 12, dataOff = -1; quint32 dataSize = 0; int rate = 0, channels = 0, bits = 0;
        while (off + 8 <= wav.size()) { const quint32 n = le32(wav, off + 4); const qsizetype p = off + 8; if (p + n > wav.size()) break; const QByteArray id = wav.mid(off, 4); if (id == "fmt " && n >= 16) { channels = le16(wav, p + 2); rate = int(le32(wav, p + 4)); bits = le16(wav, p + 14); } else if (id == "data") { dataOff = p; dataSize = n; } off = p + n + (n & 1); }
        if (rate != sampleRate || channels < 1 || bits != 16 || dataOff < 0) { result.error = QStringLiteral("WAV must be PCM16 at %1 Hz: %2").arg(sampleRate).arg(lane.name); return result; }
        const qint64 frames = dataSize / (channels * 2);
        if (frames <= 0) continue;
        const qint64 sourceStart = lane.loopStartFrame >= 0 ? qBound<qint64>(0, lane.loopStartFrame, frames - 1) : 0;
        const qint64 sourceEnd = lane.loopEndFrame > sourceStart ? qBound<qint64>(sourceStart + 1, lane.loopEndFrame, frames) : frames;
        const qint64 visibleFrames = qMax<qint64>(0, sourceEnd - sourceStart);
        const qint64 end = lane.stopFrame >= 0 ? lane.stopFrame : lane.startFrame + visibleFrames;
        length = qMax(length, end); decoded.push_back({&lane, wav.mid(dataOff, dataSize), channels, frames, sourceStart, sourceEnd});
    }
    if (length > kMaxFrames) { result.error = QStringLiteral("prepared mix exceeds five-minute limit"); return result; }
    length = qMax<qint64>(1, length);
    std::vector<qint32> mix(static_cast<size_t>(length), 0);
    for (const Decoded& source : decoded) {
        const double gain = std::pow(10.0, source.lane->gainDb / 20.0);
        const qint64 visibleFrames = source.sourceEnd - source.sourceStart;
        if (visibleFrames <= 0) continue;
        const qint64 start = qMax<qint64>(0, source.lane->startFrame);
        const qint64 end = source.lane->stopFrame >= 0 ? qMin<qint64>(length, source.lane->stopFrame) : qMin<qint64>(length, source.lane->startFrame + visibleFrames);
        for (qint64 out = start; out < end; ++out) {
            qint64 in = source.sourceStart + (out - source.lane->startFrame);
            if (source.lane->loopEnabled && in >= source.sourceEnd) {
                in = source.sourceStart + (in - source.sourceStart) % visibleFrames;
            }
            if (in < source.sourceStart || in >= source.sourceEnd || in >= source.frames) continue;
            qint32 sum = 0;
            for (int c = 0; c < source.channels; ++c) sum += qint16(le16(source.data, (in * source.channels + c) * 2));
            mix[size_t(out)] += qint32(std::llround((sum / source.channels) * gain));
        }
    }
    std::vector<float> processed(static_cast<std::size_t>(length), 0.0f);
    for (qint64 i = 0; i < length; ++i) {
        processed[static_cast<std::size_t>(i)] =
            static_cast<float>(qBound<qint32>(-32768, mix[static_cast<std::size_t>(i)], 32767) / 32768.0);
    }
    processed = processTrack(std::move(processed), sampleRate, track, result.error);
    if (!result.error.isEmpty()) return result;
    length = static_cast<qint64>(processed.size());
    QByteArray out; out.reserve(44 + int(length * 2)); out.append("RIFF", 4); put32(out, quint32(36 + length * 2)); out.append("WAVEfmt ", 8); put32(out, 16); put16(out, 1); put16(out, 1); put32(out, quint32(sampleRate)); put32(out, quint32(sampleRate * 2)); put16(out, 2); put16(out, 16); out.append("data", 4); put32(out, quint32(length * 2)); for (float sample : processed) put16(out, quint16(qBound(-32768, static_cast<int>(std::lrint(sample * 32767.0f)), 32767)));
    QFile output(outputPath); if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate) || output.write(out) != out.size()) { result.error = QStringLiteral("cannot write prepared mix"); return result; }
    result.path = outputPath; result.frames = length; result.renderMs = QDateTime::currentMSecsSinceEpoch() - started; return result;
}
