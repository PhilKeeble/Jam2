#include "MainWindow.hpp"

#include "SessionController.hpp"

#include "common.hpp"

#include "signalsmith-stretch/signalsmith-stretch.h"

#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QAbstractItemView>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QList>
#include <QMediaDevices>
#include <QMessageBox>
#include <QMouseEvent>
#include <QIODevice>
#include <QPainter>
#include <QPainterPath>
#include <QPolygon>
#include <QProcess>
#include <QProxyStyle>
#include <QRegularExpression>
#include <QScrollBar>
#include <QUrl>
#include <QSlider>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QScrollArea>
#include <QStringList>
#include <QStyleOption>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <functional>
#include <utility>
#include <vector>

quint16 waveformReadLe16(const QByteArray& data, qsizetype offset)
{
    return static_cast<quint16>(static_cast<unsigned char>(data[offset])) |
        static_cast<quint16>(static_cast<unsigned char>(data[offset + 1]) << 8);
}

quint32 waveformReadLe32(const QByteArray& data, qsizetype offset)
{
    return static_cast<quint32>(static_cast<unsigned char>(data[offset])) |
        (static_cast<quint32>(static_cast<unsigned char>(data[offset + 1])) << 8) |
        (static_cast<quint32>(static_cast<unsigned char>(data[offset + 2])) << 16) |
        (static_cast<quint32>(static_cast<unsigned char>(data[offset + 3])) << 24);
}

class LocalMetronomeDevice : public QIODevice {
public:
    LocalMetronomeDevice(int sampleRate, int channels, QObject* parent = nullptr)
        : QIODevice(parent),
          sampleRate_(qMax(1, sampleRate)),
          channels_(qMax(1, channels))
    {
    }

    void configure(jam2::metronome::PatternSnapshot pattern, double level)
    {
        pattern = jam2::metronome::sanitize(pattern);
        bpm_.store(pattern.bpm, std::memory_order_relaxed);
        beatsPerBar_.store(pattern.beats_per_bar, std::memory_order_relaxed);
        division_.store(pattern.division, std::memory_order_relaxed);
        stepCount_.store(pattern.step_count, std::memory_order_relaxed);
        playMaskLow_.store(pattern.play_mask_low, std::memory_order_relaxed);
        playMaskHigh_.store(pattern.play_mask_high, std::memory_order_relaxed);
        accentMaskLow_.store(pattern.accent_mask_low, std::memory_order_relaxed);
        accentMaskHigh_.store(pattern.accent_mask_high, std::memory_order_relaxed);
        levelPpm_.store(static_cast<int>(qBound(0.0, level, 1.0) * 1000000.0), std::memory_order_relaxed);
    }

    void resetGrid()
    {
        sampleCounter_ = 0;
    }

    qint64 bytesAvailable() const override
    {
        return 8192 + QIODevice::bytesAvailable();
    }

    bool isSequential() const override
    {
        return true;
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        if (maxSize <= 0) {
            return 0;
        }
        constexpr int kBytesPerSample = 2;
        const int bytesPerFrame = channels_ * kBytesPerSample;
        const qint64 frames = maxSize / bytesPerFrame;
        if (frames <= 0) {
            return 0;
        }

        const jam2::metronome::PatternSnapshot pattern = jam2::metronome::sanitize({
            bpm_.load(std::memory_order_relaxed),
            beatsPerBar_.load(std::memory_order_relaxed),
            division_.load(std::memory_order_relaxed),
            stepCount_.load(std::memory_order_relaxed),
            playMaskLow_.load(std::memory_order_relaxed),
            playMaskHigh_.load(std::memory_order_relaxed),
            accentMaskLow_.load(std::memory_order_relaxed),
            accentMaskHigh_.load(std::memory_order_relaxed),
        });
        const double level = static_cast<double>(levelPpm_.load(std::memory_order_relaxed)) / 1000000.0;
        const std::uint64_t stepInterval =
            jam2::metronome::step_interval_samples(static_cast<double>(sampleRate_), pattern.bpm, pattern.division);
        auto* output = reinterpret_cast<qint16*>(data);
        for (qint64 frame = 0; frame < frames; ++frame) {
            const double click = jam2::metronome::render_sample(
                pattern,
                sampleCounter_++,
                stepInterval,
                static_cast<double>(sampleRate_),
                level);
            const qint16 pcm = static_cast<qint16>(std::lrint(qBound(-1.0, click, 1.0) * 32767.0));
            for (int channel = 0; channel < channels_; ++channel) {
                *output++ = pcm;
            }
        }
        return frames * bytesPerFrame;
    }

    qint64 writeData(const char*, qint64) override
    {
        return -1;
    }

private:
    int sampleRate_ = 48000;
    int channels_ = 2;
    std::uint64_t sampleCounter_ = 0;
    std::atomic<int> bpm_{120};
    std::atomic<int> beatsPerBar_{4};
    std::atomic<int> division_{1};
    std::atomic<int> stepCount_{4};
    std::atomic<std::uint64_t> playMaskLow_{0x0fULL};
    std::atomic<std::uint64_t> playMaskHigh_{0};
    std::atomic<std::uint64_t> accentMaskLow_{0x01ULL};
    std::atomic<std::uint64_t> accentMaskHigh_{0};
    std::atomic<int> levelPpm_{350000};
};

class WaveformWidget : public QWidget {
public:
    explicit WaveformWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(240);
        setMouseTracking(true);
    }

    std::function<void(qint64)> onSeekMs;

    void clear()
    {
        peaks_.clear();
        label_ = QStringLiteral("No WAV loaded");
        durationMs_ = 0;
        playheadMs_ = 0;
        loopStartMs_ = -1;
        loopEndMs_ = -1;
        update();
    }

    void setDurationMs(qint64 durationMs)
    {
        durationMs_ = qMax<qint64>(0, durationMs);
        playheadMs_ = qBound<qint64>(0, playheadMs_, durationMs_);
        update();
    }

    void setBpm(double bpm)
    {
        bpm_ = qBound(1.0, bpm, 400.0);
        update();
    }

    void setGridVisible(bool visible)
    {
        gridVisible_ = visible;
        update();
    }

    void setPlayheadMs(qint64 positionMs)
    {
        playheadMs_ = qBound<qint64>(0, positionMs, qMax<qint64>(durationMs_, 0));
        update();
    }

    void setLoop(qint64 startMs, qint64 endMs)
    {
        loopStartMs_ = startMs >= 0 ? startMs : -1;
        loopEndMs_ = endMs >= 0 ? endMs : -1;
        update();
    }

    void loadWav(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            clear();
            return;
        }
        const QByteArray data = file.readAll();
        if (data.size() < 44 || data.mid(0, 4) != "RIFF" || data.mid(8, 4) != "WAVE") {
            clear();
            return;
        }

        int channels = 0;
        int bitsPerSample = 0;
        qsizetype dataOffset = -1;
        qsizetype dataBytes = 0;
        qsizetype offset = 12;
        while (offset + 8 <= data.size()) {
            const QByteArray id = data.mid(offset, 4);
            const quint32 size = waveformReadLe32(data, offset + 4);
            const qsizetype payload = offset + 8;
            if (payload + size > data.size()) {
                break;
            }
            if (id == "fmt " && size >= 16) {
                channels = waveformReadLe16(data, payload + 2);
                bitsPerSample = waveformReadLe16(data, payload + 14);
            } else if (id == "data") {
                dataOffset = payload;
                dataBytes = size;
            }
            offset = payload + size + (size % 2);
        }

        if (channels <= 0 || bitsPerSample != 16 || dataOffset < 0 || dataBytes <= 0) {
            peaks_.clear();
            label_ = QStringLiteral("Waveform preview supports PCM16 WAV");
            update();
            return;
        }

        constexpr int kPeakCount = 2048;
        peaks_.assign(kPeakCount, 0.0f);
        const qsizetype frameBytes = channels * 2;
        const qsizetype frames = dataBytes / frameBytes;
        if (frames <= 0) {
            clear();
            return;
        }
        for (int i = 0; i < kPeakCount; ++i) {
            const qsizetype begin = frames * i / kPeakCount;
            const qsizetype end = qMax<qsizetype>(begin + 1, frames * (i + 1) / kPeakCount);
            int peak = 0;
            for (qsizetype frame = begin; frame < end; ++frame) {
                int mixed = 0;
                for (int channel = 0; channel < channels; ++channel) {
                    const qsizetype sampleOffset = dataOffset + frame * frameBytes + channel * 2;
                    const qint16 sample = static_cast<qint16>(waveformReadLe16(data, sampleOffset));
                    mixed += std::abs(static_cast<int>(sample));
                }
                peak = qMax(peak, mixed / channels);
            }
            peaks_[i] = static_cast<float>(peak) / 32768.0f;
        }
        label_ = QFileInfo(path).fileName();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(18, 20, 22));
        painter.setRenderHint(QPainter::Antialiasing, false);

        if (gridVisible_ && durationMs_ > 0 && bpm_ > 0.0) {
            const double beatMs = 60000.0 / bpm_;
            const int beats = static_cast<int>(std::floor(static_cast<double>(durationMs_) / beatMs));
            for (int beat = 0; beat <= beats + 1; ++beat) {
                const qint64 beatPosition = static_cast<qint64>(std::llround(beat * beatMs));
                const int x = xForMs(beatPosition);
                const bool bar = beat % 4 == 0;
                painter.setPen(bar ? QColor(76, 86, 96) : QColor(42, 48, 54));
                painter.drawLine(x, 0, x, height());
                if (bar) {
                    painter.setPen(QColor(150, 158, 166));
                    painter.drawText(x + 4, 18, QString::number(beat + 1));
                }
            }
        }

        painter.setPen(QColor(58, 64, 70));
        painter.drawLine(0, height() / 2, width(), height() / 2);
        painter.setPen(QColor(102, 198, 166));
        if (!peaks_.empty()) {
            for (int x = 0; x < width(); ++x) {
                const int index = qBound(0, x * static_cast<int>(peaks_.size()) / qMax(1, width()), static_cast<int>(peaks_.size()) - 1);
                const int half = qMax(1, static_cast<int>(peaks_[index] * (height() / 2 - 14)));
                painter.drawLine(x, height() / 2 - half, x, height() / 2 + half);
            }
        }
        painter.setPen(QColor(220, 224, 226));
        painter.drawText(rect().adjusted(12, 8, -12, -8), Qt::AlignLeft | Qt::AlignTop, label_);

        drawLoopMarker(painter, loopStartMs_, QColor(82, 170, 255), QStringLiteral("Loop Start"));
        drawLoopMarker(painter, loopEndMs_, QColor(255, 184, 82), QStringLiteral("Loop End"));

        if (loopStartMs_ >= 0 && loopEndMs_ > loopStartMs_) {
            const int startX = xForMs(loopStartMs_);
            const int endX = xForMs(loopEndMs_);
            painter.fillRect(QRect(QPoint(startX, 0), QPoint(endX, height())).normalized(), QColor(86, 132, 210, 28));
        }

        if (durationMs_ > 0) {
            const int playheadX = xForMs(playheadMs_);
            painter.setPen(QPen(QColor(255, 92, 92), 2));
            painter.drawLine(playheadX, 0, playheadX, height());
        }
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton || durationMs_ <= 0) {
            QWidget::mousePressEvent(event);
            return;
        }
        seekToX(event->position().x());
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!(event->buttons() & Qt::LeftButton) || durationMs_ <= 0) {
            QWidget::mouseMoveEvent(event);
            return;
        }
        seekToX(event->position().x());
    }

private:
    int xForMs(qint64 ms) const
    {
        if (durationMs_ <= 0 || width() <= 1) {
            return 0;
        }
        return qBound(0, static_cast<int>((static_cast<double>(ms) / static_cast<double>(durationMs_)) * width()), width() - 1);
    }

    qint64 msForX(double x) const
    {
        if (durationMs_ <= 0 || width() <= 1) {
            return 0;
        }
        const double clampedX = qBound(0.0, x, static_cast<double>(width() - 1));
        return qBound<qint64>(0, static_cast<qint64>(std::llround((clampedX / width()) * durationMs_)), durationMs_);
    }

    void seekToX(double x)
    {
        const qint64 position = msForX(x);
        setPlayheadMs(position);
        if (onSeekMs) {
            onSeekMs(position);
        }
    }

    void drawLoopMarker(QPainter& painter, qint64 positionMs, const QColor& color, const QString& text)
    {
        if (positionMs < 0 || durationMs_ <= 0) {
            return;
        }
        const int x = xForMs(positionMs);
        painter.setPen(QPen(color, 2));
        painter.drawLine(x, 0, x, height());
        painter.setBrush(color);
        QPolygon tag;
        tag << QPoint(x, 0) << QPoint(x + 9, 0) << QPoint(x, 12);
        painter.drawPolygon(tag);
        painter.setPen(color.lighter(135));
        painter.drawText(x + 6, height() - 10, text);
    }

    std::vector<float> peaks_;
    QString label_ = QStringLiteral("No WAV loaded");
    qint64 durationMs_ = 0;
    qint64 playheadMs_ = 0;
    qint64 loopStartMs_ = -1;
    qint64 loopEndMs_ = -1;
    double bpm_ = 120.0;
    bool gridVisible_ = true;
};

struct Pcm16Wav {
    int sampleRate = 0;
    int channels = 0;
    std::vector<std::vector<float>> samples;
};

class TrackPlaybackDevice : public QIODevice {
    struct FocusState {
        double x1 = 0.0;
        double x2 = 0.0;
        double y1 = 0.0;
        double y2 = 0.0;
    };

public:
    explicit TrackPlaybackDevice(QObject* parent = nullptr)
        : QIODevice(parent)
    {
    }

    void setTrack(Pcm16Wav wav)
    {
        wav_ = std::move(wav);
        const int channels = qMax(1, wav_.channels);
        stretch_.presetDefault(channels, static_cast<float>(qMax(1, wav_.sampleRate)), true);
        inputBlock_.assign(channels, std::vector<float>(kMaxInputFrames, 0.0f));
        outputBlock_.assign(channels, std::vector<float>(kMaxOutputFrames, 0.0f));
        focusState_.assign(channels, FocusState{});
        sourceFrame_ = 0.0;
        positionFrame_.store(0, std::memory_order_relaxed);
        pendingSeekFrame_.store(0, std::memory_order_relaxed);
        lastPitchCents_ = std::numeric_limits<int>::min();
        lastFocusEnabled_ = false;
        lastFocusFrequencyHz_ = -1.0;
        lastFocusGainDb_ = 0.0;
        lastFocusQ_ = 0.0;
        stretch_.reset();
    }

    bool hasTrack() const
    {
        return wav_.sampleRate > 0 && wav_.channels > 0 && !wav_.samples.empty() && !wav_.samples.front().empty();
    }

    int sampleRate() const
    {
        return qMax(1, wav_.sampleRate);
    }

    int channels() const
    {
        return qMax(1, wav_.channels);
    }

    qint64 durationMs() const
    {
        if (!hasTrack()) {
            return 0;
        }
        return static_cast<qint64>(std::llround(static_cast<double>(frameCount()) * 1000.0 / sampleRate()));
    }

    qint64 positionMs() const
    {
        return static_cast<qint64>(positionFrame_.load(std::memory_order_relaxed) * 1000 / sampleRate());
    }

    void setPositionMs(qint64 ms)
    {
        const qint64 frame = qBound<qint64>(0, ms * sampleRate() / 1000, frameCount());
        pendingSeekFrame_.store(frame, std::memory_order_relaxed);
    }

    void setSpeed(double speed)
    {
        speedPpm_.store(static_cast<int>(qBound(0.10, speed, 2.0) * 1000000.0), std::memory_order_relaxed);
    }

    void setPitchCents(int cents)
    {
        pitchCents_.store(qBound(-1200, cents, 1200), std::memory_order_relaxed);
    }

    void setGainDb(double gainDb)
    {
        gainPpm_.store(static_cast<int>(std::pow(10.0, qBound(-60.0, gainDb, 12.0) / 20.0) * 1000000.0), std::memory_order_relaxed);
    }

    void setFocus(bool enabled, double frequencyHz, double gainDb, double q)
    {
        focusEnabled_.store(enabled, std::memory_order_relaxed);
        focusFrequencyHz_.store(qBound(20.0, frequencyHz, static_cast<double>(sampleRate()) * 0.45), std::memory_order_relaxed);
        focusGainDb_.store(qBound(-24.0, gainDb, 24.0), std::memory_order_relaxed);
        focusQ_.store(qBound(0.1, q, 20.0), std::memory_order_relaxed);
    }

    void setLoop(bool enabled, qint64 startMs, qint64 endMs)
    {
        loopEnabled_.store(enabled, std::memory_order_relaxed);
        loopStartFrame_.store(qBound<qint64>(0, startMs * sampleRate() / 1000, frameCount()), std::memory_order_relaxed);
        loopEndFrame_.store(qBound<qint64>(0, endMs * sampleRate() / 1000, frameCount()), std::memory_order_relaxed);
    }

    qint64 bytesAvailable() const override
    {
        return kMaxOutputFrames * channels() * 2 + QIODevice::bytesAvailable();
    }

    bool isSequential() const override
    {
        return true;
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        if (!hasTrack() || maxSize <= 0) {
            return 0;
        }
        applyPendingSeek();
        const int channelCount = channels();
        const int requestedFrames = static_cast<int>(maxSize / (channelCount * 2));
        const int outputFrames = qBound(0, requestedFrames, kMaxOutputFrames);
        if (outputFrames <= 0) {
            return 0;
        }

        const double speed = static_cast<double>(speedPpm_.load(std::memory_order_relaxed)) / 1000000.0;
        const int pitchCents = pitchCents_.load(std::memory_order_relaxed);
        const double gain = static_cast<double>(gainPpm_.load(std::memory_order_relaxed)) / 1000000.0;

        if (pitchCents != lastPitchCents_) {
            stretch_.setTransposeSemitones(static_cast<float>(pitchCents) / 100.0f);
            lastPitchCents_ = pitchCents;
        }
        updateFocusCoefficients();

        if (std::abs(speed - 1.0) <= 0.0001 && pitchCents == 0) {
            renderDirect(outputFrames, gain, data);
        } else {
            renderStretched(outputFrames, speed, gain, data);
        }
        positionFrame_.store(qBound<qint64>(0, static_cast<qint64>(std::llround(sourceFrame_)), frameCount()), std::memory_order_relaxed);
        return static_cast<qint64>(outputFrames) * channelCount * 2;
    }

    qint64 writeData(const char*, qint64) override
    {
        return -1;
    }

private:
    static constexpr int kMaxOutputFrames = 4096;
    static constexpr int kMaxInputFrames = kMaxOutputFrames * 2 + 16;

    qint64 frameCount() const
    {
        return hasTrack() ? static_cast<qint64>(wav_.samples.front().size()) : 0;
    }

    void applyPendingSeek()
    {
        const qint64 pending = pendingSeekFrame_.exchange(-1, std::memory_order_relaxed);
        if (pending >= 0) {
            sourceFrame_ = static_cast<double>(qBound<qint64>(0, pending, frameCount()));
            positionFrame_.store(static_cast<qint64>(sourceFrame_), std::memory_order_relaxed);
            stretch_.reset();
            resetFocusState();
        }
    }

    void advanceFrame()
    {
        const bool looping = loopEnabled_.load(std::memory_order_relaxed);
        const qint64 loopStart = loopStartFrame_.load(std::memory_order_relaxed);
        const qint64 loopEnd = loopEndFrame_.load(std::memory_order_relaxed);
        if (looping && loopEnd > loopStart && sourceFrame_ >= static_cast<double>(loopEnd)) {
            sourceFrame_ = static_cast<double>(loopStart);
            stretch_.reset();
            resetFocusState();
        }
    }

    void resetFocusState()
    {
        for (FocusState& state : focusState_) {
            state = {};
        }
    }

    void updateFocusCoefficients()
    {
        const bool enabled = focusEnabled_.load(std::memory_order_relaxed);
        const double frequencyHz = focusFrequencyHz_.load(std::memory_order_relaxed);
        const double gainDb = focusGainDb_.load(std::memory_order_relaxed);
        const double q = focusQ_.load(std::memory_order_relaxed);
        if (enabled == lastFocusEnabled_ &&
            std::abs(frequencyHz - lastFocusFrequencyHz_) < 0.001 &&
            std::abs(gainDb - lastFocusGainDb_) < 0.001 &&
            std::abs(q - lastFocusQ_) < 0.001) {
            return;
        }

        lastFocusEnabled_ = enabled;
        lastFocusFrequencyHz_ = frequencyHz;
        lastFocusGainDb_ = gainDb;
        lastFocusQ_ = q;
        if (!enabled || std::abs(gainDb) < 0.001) {
            focusA0_ = 1.0;
            focusA1_ = 0.0;
            focusA2_ = 0.0;
            focusB1_ = 0.0;
            focusB2_ = 0.0;
            resetFocusState();
            return;
        }

        const double clampedFrequency = qBound(20.0, frequencyHz, static_cast<double>(sampleRate()) * 0.45);
        const double omega = 2.0 * 3.14159265358979323846 * clampedFrequency / static_cast<double>(sampleRate());
        const double sinOmega = std::sin(omega);
        const double cosOmega = std::cos(omega);
        const double alpha = sinOmega / (2.0 * qBound(0.1, q, 20.0));
        const double makeup = std::pow(10.0, gainDb / 20.0);

        const double b0 = alpha;
        const double b1 = 0.0;
        const double b2 = -alpha;
        const double a0 = 1.0 + alpha;
        const double a1 = -2.0 * cosOmega;
        const double a2 = 1.0 - alpha;

        focusA0_ = makeup * b0 / a0;
        focusA1_ = makeup * b1 / a0;
        focusA2_ = makeup * b2 / a0;
        focusB1_ = a1 / a0;
        focusB2_ = a2 / a0;
        resetFocusState();
    }

    double processFocusSample(int channel, double sample)
    {
        if (!lastFocusEnabled_ || channel < 0 || channel >= static_cast<int>(focusState_.size())) {
            return sample;
        }
        FocusState& state = focusState_[channel];
        const double output = focusA0_ * sample + focusA1_ * state.x1 + focusA2_ * state.x2 - focusB1_ * state.y1 - focusB2_ * state.y2;
        state.x2 = state.x1;
        state.x1 = sample;
        state.y2 = state.y1;
        state.y1 = output;
        return output;
    }

    float sampleAt(int channel, qint64 frame) const
    {
        if (frame < 0 || frame >= frameCount() || channel < 0 || channel >= wav_.channels) {
            return 0.0f;
        }
        return wav_.samples[channel][static_cast<std::size_t>(frame)];
    }

    void fillInput(int inputFrames)
    {
        const int channelCount = channels();
        for (int frame = 0; frame < inputFrames; ++frame) {
            advanceFrame();
            const qint64 source = static_cast<qint64>(sourceFrame_);
            for (int channel = 0; channel < channelCount; ++channel) {
                inputBlock_[channel][frame] = sampleAt(channel, source);
            }
            sourceFrame_ += 1.0;
        }
    }

    void renderDirect(int outputFrames, double gain, char* data)
    {
        auto* output = reinterpret_cast<qint16*>(data);
        const int channelCount = channels();
        for (int frame = 0; frame < outputFrames; ++frame) {
            advanceFrame();
            const qint64 source = static_cast<qint64>(sourceFrame_);
            for (int channel = 0; channel < channelCount; ++channel) {
                const double focused = processFocusSample(channel, static_cast<double>(sampleAt(channel, source)));
                const double sample = qBound(-1.0, focused * gain, 1.0);
                *output++ = static_cast<qint16>(std::lrint(sample * 32767.0));
            }
            sourceFrame_ += 1.0;
        }
    }

    void renderStretched(int outputFrames, double speed, double gain, char* data)
    {
        const int channelCount = channels();
        const int inputFrames = qBound(1, static_cast<int>(std::ceil(outputFrames * speed)), kMaxInputFrames);
        fillInput(inputFrames);
        for (int channel = 0; channel < channelCount; ++channel) {
            std::fill(outputBlock_[channel].begin(), outputBlock_[channel].begin() + outputFrames, 0.0f);
        }
        stretch_.process(inputBlock_, inputFrames, outputBlock_, outputFrames);

        auto* output = reinterpret_cast<qint16*>(data);
        for (int frame = 0; frame < outputFrames; ++frame) {
            for (int channel = 0; channel < channelCount; ++channel) {
                const double focused = processFocusSample(channel, static_cast<double>(outputBlock_[channel][frame]));
                const double sample = qBound(-1.0, focused * gain, 1.0);
                *output++ = static_cast<qint16>(std::lrint(sample * 32767.0));
            }
        }
    }

    Pcm16Wav wav_;
    signalsmith::stretch::SignalsmithStretch<float> stretch_;
    std::vector<std::vector<float>> inputBlock_;
    std::vector<std::vector<float>> outputBlock_;
    std::vector<FocusState> focusState_;
    double sourceFrame_ = 0.0;
    int lastPitchCents_ = std::numeric_limits<int>::min();
    std::atomic<int> speedPpm_{1000000};
    std::atomic<int> pitchCents_{0};
    std::atomic<int> gainPpm_{1000000};
    std::atomic<bool> focusEnabled_{false};
    std::atomic<double> focusFrequencyHz_{120.0};
    std::atomic<double> focusGainDb_{12.0};
    std::atomic<double> focusQ_{6.0};
    std::atomic<qint64> pendingSeekFrame_{-1};
    std::atomic<qint64> positionFrame_{0};
    std::atomic<bool> loopEnabled_{false};
    std::atomic<qint64> loopStartFrame_{0};
    std::atomic<qint64> loopEndFrame_{0};
    bool lastFocusEnabled_ = false;
    double lastFocusFrequencyHz_ = -1.0;
    double lastFocusGainDb_ = 0.0;
    double lastFocusQ_ = 0.0;
    double focusA0_ = 1.0;
    double focusA1_ = 0.0;
    double focusA2_ = 0.0;
    double focusB1_ = 0.0;
    double focusB2_ = 0.0;
};

namespace {

class Jam2Style final : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter, const QWidget* widget = nullptr) const override
    {
        if (element != PE_IndicatorCheckBox) {
            QProxyStyle::drawPrimitive(element, option, painter, widget);
            return;
        }

        const QRect box = option->rect.adjusted(1, 1, -1, -1);
        const bool enabled = (option->state & State_Enabled) != 0;
        const bool checked = (option->state & State_On) != 0;
        const bool hovered = (option->state & State_MouseOver) != 0;
        const bool focused = (option->state & State_HasFocus) != 0;
        const QColor border = enabled
            ? (hovered || focused ? QColor(QStringLiteral("#66c6a6")) : QColor(QStringLiteral("#8a97a1")))
            : QColor(QStringLiteral("#4d565d"));
        const QColor fill = checked ? QColor(QStringLiteral("#1b3b33")) : QColor(QStringLiteral("#101214"));
        const QColor tick = enabled ? QColor(QStringLiteral("#f2f5f7")) : QColor(QStringLiteral("#7a858c"));

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(border, 1.25));
        painter->setBrush(fill);
        painter->drawRect(box);

        if (checked) {
            painter->setPen(QPen(tick, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            QPainterPath path;
            path.moveTo(box.left() + box.width() * 0.24, box.top() + box.height() * 0.54);
            path.lineTo(box.left() + box.width() * 0.43, box.top() + box.height() * 0.72);
            path.lineTo(box.left() + box.width() * 0.77, box.top() + box.height() * 0.30);
            painter->drawPath(path);
        }
        painter->restore();
    }
};

void installJam2Style()
{
    static bool installed = false;
    if (installed) {
        return;
    }
    QApplication::setStyle(new Jam2Style(QApplication::style()));
    installed = true;
}

QString keyToHex(const std::array<std::uint8_t, 16>& key)
{
    return QString::fromStdString(jam2::hex_encode(key.data(), key.size()));
}

QString sessionToHex(std::uint64_t session)
{
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[bytes.size() - 1 - i] = static_cast<std::uint8_t>((session >> (i * 8)) & 0xffU);
    }
    return QString::fromStdString(jam2::hex_encode(bytes.data(), bytes.size()));
}

QString doubleText(const QJsonObject& object, const QString& key, const QString& suffix, int precision = 1)
{
    return QString::number(object.value(key).toDouble(), 'f', precision) + suffix;
}

bool hasNumber(const QJsonObject& object, const QString& key)
{
    return object.contains(key) && object.value(key).isDouble();
}

QString numberText(const QJsonObject& object, const QString& key, const QString& suffix = QString(), int precision = 1)
{
    if (!hasNumber(object, key)) {
        return QStringLiteral("-");
    }
    return QString::number(object.value(key).toDouble(), 'f', precision) + suffix;
}

QString integerText(const QJsonObject& object, const QString& key, const QString& fallbackKey = QString())
{
    QString actualKey = key;
    if (!hasNumber(object, actualKey) && !fallbackKey.isEmpty()) {
        actualKey = fallbackKey;
    }
    if (!hasNumber(object, actualKey)) {
        return QStringLiteral("-");
    }
    return QString::number(static_cast<qlonglong>(object.value(actualKey).toDouble()));
}

QString durationText(double ms)
{
    if (ms >= 1000.0) {
        return QString::number(ms / 1000.0, 'f', 2) + QStringLiteral(" s");
    }
    return QString::number(ms, 'f', 1) + QStringLiteral(" ms");
}

QString metricText(
    const QJsonObject& object,
    const QString& key,
    const QString& suffix = QString(),
    int precision = 1,
    const QString& fallbackKey = QString())
{
    QString actualKey = key;
    if (!hasNumber(object, actualKey) && !fallbackKey.isEmpty()) {
        actualKey = fallbackKey;
    }
    return numberText(object, actualKey, suffix, precision);
}

double metricValue(const QJsonObject& object, const QString& key, double fallback = 0.0)
{
    return hasNumber(object, key) ? object.value(key).toDouble() : fallback;
}

double percentOf(double part, double whole)
{
    return whole > 0.0 ? part * 100.0 / whole : 0.0;
}

double perMinute(double count, double elapsedMs)
{
    return elapsedMs > 0.0 ? count * 60000.0 / elapsedMs : 0.0;
}

QString percentText(double value, int precision = 2)
{
    return QString::number(value, 'f', precision) + QStringLiteral("%");
}

QString diagnoseStats(const QJsonObject& stats)
{
    if (!hasNumber(stats, QStringLiteral("elapsed_ms"))) {
        return QStringLiteral("Diagnosis -");
    }

    QStringList findings;
    const double elapsedMs = metricValue(stats, QStringLiteral("elapsed_ms"));
    const double recvPackets = metricValue(stats, QStringLiteral("recv_packets"));
    const double frameSize = metricValue(stats, QStringLiteral("frame_size"));
    const double receivedFrames = recvPackets * frameSize;
    const double packetGapSamples = metricValue(stats, QStringLiteral("audio_packet_gap_samples"));
    const double lossPercent = metricValue(stats, QStringLiteral("sequence_loss_percent"));
    const double underrunMs = metricValue(stats, QStringLiteral("playback_ring_underrun_time_ms"));
    const double underrunEvents = metricValue(stats, QStringLiteral("playback_ring_underrun_events"));
    const double underrunBurstMaxMs = metricValue(stats, QStringLiteral("playback_ring_underrun_burst_max_ms"));
    const double gapOver4x = metricValue(stats, QStringLiteral("audio_packet_gap_over_4x_count"));
    const double reorderedLost = metricValue(stats, QStringLiteral("reordered_lost"));
    const double sequenceLate = metricValue(stats, QStringLiteral("sequence_late"));
    const double lateFrames = metricValue(stats, QStringLiteral("late_audio_frames_dropped"));
    const double missingFrames = metricValue(stats, QStringLiteral("missing_audio_frames_inserted"));
    const double driftPpm = std::abs(metricValue(stats, QStringLiteral("drift_ppm")));

    const double underrunPercent = percentOf(underrunMs, elapsedMs);
    const double underrunEventsPerMinute = perMinute(underrunEvents, elapsedMs);
    const double gapOver4xPercent = percentOf(gapOver4x, packetGapSamples);
    const double gapOver4xPerMinute = perMinute(gapOver4x, elapsedMs);
    const double reorderLatePercent = percentOf(reorderedLost + sequenceLate, recvPackets);
    const double lateFramePercent = percentOf(lateFrames, lateFrames + missingFrames + receivedFrames);
    const double missingPressurePercent = percentOf(missingFrames, missingFrames + lateFrames + receivedFrames);

    if (underrunPercent >= 0.10 || underrunEventsPerMinute >= 2.0 || underrunBurstMaxMs >= 10.0) {
        findings << QStringLiteral("Underrun %1: +prefill/+max").arg(percentText(underrunPercent));
    }
    if (gapOver4xPercent >= 0.50 || gapOver4xPerMinute >= 2.0) {
        findings << QStringLiteral("Bursts %1: +prefill/+adaptive max").arg(percentText(gapOver4xPercent));
    }
    if (lossPercent >= 0.50) {
        findings << QStringLiteral("Loss %1: +frame/wired").arg(metricText(stats, QStringLiteral("sequence_loss_percent"), QStringLiteral("%"), 2));
    }
    if (reorderLatePercent >= 0.25 || lateFramePercent >= 0.25) {
        findings << QStringLiteral("Late/reorder %1: +playout").arg(percentText(qMax(reorderLatePercent, lateFramePercent)));
    }
    if (missingPressurePercent >= 0.25 && findings.size() < 2) {
        findings << QStringLiteral("Missing %1: +playout/prefill").arg(percentText(missingPressurePercent));
    }
    if (driftPpm >= 200.0 && findings.size() < 2) {
        findings << QStringLiteral("Drift %1ppm: +correction").arg(metricText(stats, QStringLiteral("drift_ppm"), QString{}, 0));
    }

    if (findings.isEmpty()) {
        return QStringLiteral("Diagnosis OK");
    }
    while (findings.size() > 2) {
        findings.removeLast();
    }
    return QStringLiteral("Diagnosis ") + findings.join(QStringLiteral(" | "));
}

QSlider* makeUnitSlider(double value, QWidget* parent)
{
    auto* slider = new QSlider(Qt::Horizontal, parent);
    slider->setRange(0, 100);
    slider->setValue(qRound(value * 100.0));
    slider->setMinimumWidth(160);
    return slider;
}

struct FocusPreset {
    double frequencyHz = 120.0;
    double gainDb = 12.0;
    double q = 6.0;
};

FocusPreset focusPresetForKey(const QString& key)
{
    if (key == QStringLiteral("bass")) {
        return {95.0, 14.0, 7.0};
    }
    if (key == QStringLiteral("guitar")) {
        return {850.0, 12.0, 5.0};
    }
    if (key == QStringLiteral("vocals")) {
        return {1800.0, 12.0, 4.5};
    }
    if (key == QStringLiteral("drums")) {
        return {3200.0, 10.0, 3.5};
    }
    return {120.0, 12.0, 6.0};
}

bool isCustomFocusPreset(const QString& key)
{
    return key.isEmpty() || key == QStringLiteral("custom");
}

QString trackValueEditorStyle()
{
    return QStringLiteral(
        "QAbstractSpinBox, QComboBox, QLineEdit { border: 1px solid #52616c; background: #101214; color: #f2f5f7; padding: 2px 6px; }"
        "QAbstractSpinBox:focus, QComboBox:focus, QLineEdit:focus { border: 1px solid #66c6a6; }"
        "QAbstractSpinBox:disabled, QComboBox:disabled, QLineEdit:disabled { border: 1px solid #343c42; background: #171a1d; color: #6f7a82; }");
}

void applyMutedEditorStyle(QWidget* widget)
{
    if (widget == nullptr) {
        return;
    }
    widget->setAttribute(Qt::WA_MacShowFocusRect, false);
    widget->setStyleSheet(trackValueEditorStyle());
    if (auto* combo = qobject_cast<QComboBox*>(widget); combo != nullptr && combo->lineEdit() != nullptr) {
        combo->lineEdit()->setAttribute(Qt::WA_MacShowFocusRect, false);
        combo->lineEdit()->setStyleSheet(trackValueEditorStyle());
    }
}

void updateCaptureDurationControl(QCheckBox* manualStopCheck, QSpinBox* durationSpin)
{
    if (manualStopCheck == nullptr || durationSpin == nullptr) {
        return;
    }
    durationSpin->setEnabled(!manualStopCheck->isChecked());
}

void updateCaptureDurationControl(QCheckBox* manualStopCheck, QSpinBox* durationSpin, QLabel* durationLabel)
{
    updateCaptureDurationControl(manualStopCheck, durationSpin);
    if (durationLabel != nullptr && manualStopCheck != nullptr) {
        durationLabel->setEnabled(!manualStopCheck->isChecked());
    }
}

QString deviceId(const QString& text)
{
    const QRegularExpression re(QStringLiteral("^\\s*\\[?(\\d+)\\]?"));
    const QRegularExpressionMatch match = re.match(text);
    return match.hasMatch() ? match.captured(1) : text.trimmed();
}

QString audioDeviceIdText(const QAudioDevice& device)
{
    return QString::fromLatin1(device.id().toHex());
}

struct WavMetadata {
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    qint64 dataBytes = 0;
    int durationMs = 0;
    QString sha256;
};

quint16 readLe16(const QByteArray& data, qsizetype offset)
{
    return static_cast<quint16>(static_cast<unsigned char>(data[offset])) |
        static_cast<quint16>(static_cast<unsigned char>(data[offset + 1]) << 8);
}

quint32 readLe32(const QByteArray& data, qsizetype offset)
{
    return static_cast<quint32>(static_cast<unsigned char>(data[offset])) |
        (static_cast<quint32>(static_cast<unsigned char>(data[offset + 1])) << 8) |
        (static_cast<quint32>(static_cast<unsigned char>(data[offset + 2])) << 16) |
        (static_cast<quint32>(static_cast<unsigned char>(data[offset + 3])) << 24);
}

WavMetadata readWavMetadata(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(QStringLiteral("failed to open WAV: %1").arg(path).toStdString());
    }
    const QByteArray bytes = file.readAll();
    if (bytes.size() < 12 || bytes.mid(0, 4) != "RIFF" || bytes.mid(8, 4) != "WAVE") {
        throw std::runtime_error("not a RIFF/WAVE file");
    }

    WavMetadata meta;
    meta.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    qsizetype offset = 12;
    while (offset + 8 <= bytes.size()) {
        const QByteArray id = bytes.mid(offset, 4);
        const quint32 size = readLe32(bytes, offset + 4);
        const qsizetype payload = offset + 8;
        if (payload + size > bytes.size()) {
            break;
        }
        if (id == "fmt " && size >= 16) {
            meta.channels = readLe16(bytes, payload + 2);
            meta.sampleRate = static_cast<int>(readLe32(bytes, payload + 4));
            meta.bitsPerSample = readLe16(bytes, payload + 14);
        } else if (id == "data") {
            meta.dataBytes = size;
        }
        offset = payload + size + (size % 2);
    }
    if (meta.sampleRate > 0 && meta.channels > 0 && meta.bitsPerSample > 0 && meta.dataBytes > 0) {
        const qint64 frameBytes = static_cast<qint64>(meta.channels) * meta.bitsPerSample / 8;
        if (frameBytes > 0) {
            meta.durationMs = static_cast<int>((meta.dataBytes / frameBytes) * 1000 / meta.sampleRate);
        }
    }
    return meta;
}

QJsonObject readSidecarJson(const QString& wavPath)
{
    QFile file(wavPath + QStringLiteral(".json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject{};
}

Pcm16Wav readPcm16Wav(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(QStringLiteral("failed to open WAV: %1").arg(path).toStdString());
    }
    const QByteArray bytes = file.readAll();
    if (bytes.size() < 12 || bytes.mid(0, 4) != "RIFF" || bytes.mid(8, 4) != "WAVE") {
        throw std::runtime_error("not a RIFF/WAVE file");
    }

    int channels = 0;
    int sampleRate = 0;
    int bitsPerSample = 0;
    qsizetype dataOffset = -1;
    qsizetype dataBytes = 0;
    qsizetype offset = 12;
    while (offset + 8 <= bytes.size()) {
        const QByteArray id = bytes.mid(offset, 4);
        const quint32 size = readLe32(bytes, offset + 4);
        const qsizetype payload = offset + 8;
        if (payload + size > bytes.size()) {
            break;
        }
        if (id == "fmt " && size >= 16) {
            const quint16 format = readLe16(bytes, payload);
            channels = readLe16(bytes, payload + 2);
            sampleRate = static_cast<int>(readLe32(bytes, payload + 4));
            bitsPerSample = readLe16(bytes, payload + 14);
            if (format != 1) {
                throw std::runtime_error("only PCM WAV input is supported for track playback");
            }
        } else if (id == "data") {
            dataOffset = payload;
            dataBytes = size;
        }
        offset = payload + size + (size % 2);
    }

    if (channels <= 0 || sampleRate <= 0 || bitsPerSample != 16 || dataOffset < 0 || dataBytes <= 0) {
        throw std::runtime_error("track playback currently supports PCM16 WAV input");
    }
    const qsizetype frameBytes = channels * 2;
    const qsizetype frames = dataBytes / frameBytes;
    Pcm16Wav wav;
    wav.sampleRate = sampleRate;
    wav.channels = channels;
    wav.samples.assign(channels, std::vector<float>(static_cast<std::size_t>(frames), 0.0f));
    for (qsizetype frame = 0; frame < frames; ++frame) {
        for (int channel = 0; channel < channels; ++channel) {
            const qsizetype sampleOffset = dataOffset + frame * frameBytes + channel * 2;
            const qint16 sample = static_cast<qint16>(readLe16(bytes, sampleOffset));
            wav.samples[channel][static_cast<std::size_t>(frame)] = static_cast<float>(sample) / 32768.0f;
        }
    }
    return wav;
}

QString onOff(bool value)
{
    return value ? QStringLiteral("on") : QStringLiteral("off");
}

QString dbText(double db)
{
    return QStringLiteral("%1%2 dB")
        .arg(db >= 0.0 ? QStringLiteral("+") : QString())
        .arg(db, 0, 'f', 1);
}

bool isWheelValueEditor(QObject* object)
{
    for (QObject* current = object; current != nullptr; current = current->parent()) {
        const QString className = QString::fromLatin1(current->metaObject()->className());
        if (qobject_cast<QAbstractSpinBox*>(current) ||
            qobject_cast<QAbstractSlider*>(current) ||
            qobject_cast<QComboBox*>(current) ||
            className.contains(QStringLiteral("QComboBox"))) {
            return true;
        }
    }
    return false;
}

QScrollArea* parentScrollArea(QObject* object)
{
    auto* widget = qobject_cast<QWidget*>(object);
    while (widget != nullptr) {
        if (auto* scrollArea = qobject_cast<QScrollArea*>(widget)) {
            return scrollArea;
        }
        widget = widget->parentWidget();
    }
    return nullptr;
}

void scrollAreaByWheel(QScrollArea& scrollArea, QWheelEvent& wheel)
{
    QScrollBar* bar = scrollArea.verticalScrollBar();
    if (bar == nullptr) {
        return;
    }
    int delta = wheel.pixelDelta().y();
    if (delta == 0) {
        delta = wheel.angleDelta().y() / 8;
    }
    if (delta == 0) {
        return;
    }
    bar->setValue(bar->value() - delta);
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QWidget(parent)
{
    installJam2Style();
    generateSession();
    buildUi();
    QApplication::instance()->installEventFilter(this);

    jam2_.onOutputLine = [this](const QString& line) { handleOutputLine(line); };
    jam2_.onErrorLine = [this](const QString& line) { appendLog(QStringLiteral("stderr: ") + line); };
    jam2_.onStatus = [this](const QJsonObject& status) { handleStatus(status); };
    jam2_.onFinished = [this](int code) {
        appendLog(QStringLiteral("jam2 exited rc=%1").arg(code));
        connectionLabel_->setText(QStringLiteral("Stopped"));
        localMetronomeRunning_ = false;
        if (trackMetronomeLabel_) {
            trackMetronomeLabel_->setText(QStringLiteral("Local metronome stopped"));
        }
        if (startTrackMetronomeButton_) {
            startTrackMetronomeButton_->setEnabled(true);
        }
        if (stopTrackMetronomeButton_) {
            stopTrackMetronomeButton_->setEnabled(false);
        }
        startButton_->setEnabled(true);
        joinButton_->setEnabled(true);
        stopButton_->setEnabled(false);
        if (refreshControlButton_) {
            refreshControlButton_->setEnabled(false);
        }
        controlReconnectEnabled_ = false;
        controlReconnectTimer_.stop();
        if (runtimeMixBox_) {
            runtimeMixBox_->setVisible(false);
        }
        if (leadSwapButton_) {
            leadSwapButton_->setEnabled(false);
        }
    };
    controlServer_.onState = [this](const QString& state) { handleControlState(state, true); };
    controlServer_.onMessage = [this](const QJsonObject& message) { handleControlMessage(message); };
    controlClient_.onState = [this](const QString& state) { handleControlState(state, false); };
    controlClient_.onMessage = [this](const QJsonObject& message) { handleControlMessage(message); };
    controlReconnectTimer_.setInterval(2000);
    QObject::connect(&controlReconnectTimer_, &QTimer::timeout, this, [this] { refreshControlConnection(); });
    QObject::connect(&captureProcess_, &QProcess::readyReadStandardOutput, this, [this] {
        const QString text = QString::fromUtf8(captureProcess_.readAllStandardOutput());
        for (const QString& line : text.split(QLatin1Char('\n'))) {
            if (!line.trimmed().isEmpty()) {
                handleCaptureOutputLine(line.trimmed());
            }
        }
    });
    QObject::connect(&captureProcess_, &QProcess::readyReadStandardError, this, [this] {
        const QString text = QString::fromUtf8(captureProcess_.readAllStandardError());
        for (const QString& line : text.split(QLatin1Char('\n'))) {
            if (!line.trimmed().isEmpty()) {
                appendLog(QStringLiteral("capture stderr: ") + line.trimmed());
            }
        }
    });
    QObject::connect(&captureProcess_, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus) {
        appendLog(QStringLiteral("capture exited rc=%1").arg(exitCode));
        if (captureButton_) {
            captureButton_->setEnabled(true);
        }
        if (loopbackCaptureButton_) {
            loopbackCaptureButton_->setEnabled(true);
        }
        if (stopCaptureButton_) {
            stopCaptureButton_->setEnabled(false);
        }
        if (importCaptureButton_) {
            importCaptureButton_->setEnabled(exitCode == 0 && QFileInfo::exists(lastCapturePath_));
        }
    });
    trackTimelineTimer_.setInterval(33);
    QObject::connect(&trackTimelineTimer_, &QTimer::timeout, this, [this] { updateTrackTimeline(); });
    trackTimelineTimer_.start();
}

MainWindow::~MainWindow()
{
    QApplication::instance()->removeEventFilter(this);
    stopJam();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Wheel && isWheelValueEditor(watched)) {
        if (auto* scrollArea = parentScrollArea(watched)) {
            scrollAreaByWheel(*scrollArea, *static_cast<QWheelEvent*>(event));
            return true;
        }
        return false;
    }
    return QWidget::eventFilter(watched, event);
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("Jam2"));
    setMinimumSize(1280, 720);

    titleLabel_ = new QLabel(QStringLiteral("Jam2"), this);
    titleLabel_->setObjectName(QStringLiteral("AppTitle"));
    connectionLabel_ = new QLabel(QStringLiteral("Idle"), this);
    connectionLabel_->setObjectName(QStringLiteral("StatusPill"));
    jitterLabel_ = new QLabel(QStringLiteral("Jitter -"), this);
    jitterLabel_->setObjectName(QStringLiteral("StatusPill"));
    lossLabel_ = new QLabel(QStringLiteral("Loss -"), this);
    lossLabel_->setObjectName(QStringLiteral("StatusPill"));

    auto* header = new QHBoxLayout();
    header->addWidget(titleLabel_);
    header->addStretch(1);
    header->addWidget(connectionLabel_);

    songTitleEdit_ = new QLineEdit(chordModel_.title(), this);
    songTitleEdit_->setMinimumWidth(300);
    auto* newSongButton = new QPushButton(QStringLiteral("New Song"), this);
    auto* openSongButton = new QPushButton(QStringLiteral("Open"), this);
    auto* saveSongButton = new QPushButton(QStringLiteral("Save"), this);
    auto* library = new QHBoxLayout();
    library->addWidget(new QLabel(QStringLiteral("Song"), this));
    library->addWidget(songTitleEdit_, 1);
    library->addWidget(newSongButton);
    library->addWidget(openSongButton);
    library->addWidget(saveSongButton);

    QObject::connect(songTitleEdit_, &QLineEdit::editingFinished, this, [this] {
        chordModel_.setTitle(songTitleEdit_->text());
        beatModel_.setTitle(songTitleEdit_->text());
        lyricModel_.setTitle(songTitleEdit_->text());
        sendSongSnapshot();
    });
    QObject::connect(newSongButton, &QPushButton::clicked, this, [this] { newSong(); });
    QObject::connect(openSongButton, &QPushButton::clicked, this, [this] { openSong(); });
    QObject::connect(saveSongButton, &QPushButton::clicked, this, [this] { saveSong(); });

    tabs_ = new QTabWidget(this);
    tabs_->addTab(buildSongPage(), QStringLiteral("Chord View"));
    beatGrid_ = new BeatGridWidget(&beatModel_, QStringLiteral("beat"), this);
    tabs_->addTab(beatGrid_, QStringLiteral("Beat View"));
    lyricGrid_ = new BeatGridWidget(&lyricModel_, QStringLiteral("lyric"), this);
    tabs_->addTab(lyricGrid_, QStringLiteral("Lyrics"));
    tabs_->addTab(buildTrackPage(), QStringLiteral("Track"));
    tabs_->addTab(buildMetronomePage(), QStringLiteral("Metronome"));

    auto sendCellEdit = [this](int section, const QString& lane, int beat, const QString& text, int revision) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("beat.set")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("lane"), lane},
            {QStringLiteral("beat"), beat},
            {QStringLiteral("text"), text},
        });
    };
    beatGrid_->onCellEdited = sendCellEdit;
    lyricGrid_->onCellEdited = sendCellEdit;
    beatGrid_->onBeatHitEdited = [this](int section, int beat, int lane, const QString& text, int revision) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("beat.hit")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("beat"), beat},
            {QStringLiteral("lane"), lane},
            {QStringLiteral("text"), text},
        });
    };
    beatGrid_->onBeatDivisionChanged = [this](int section, int beat, int division, int revision) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("beat.division")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("beat"), beat},
            {QStringLiteral("division"), division},
        });
    };
    lyricGrid_->onLyricsEdited = [this](const QString& text, int revision) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("lyrics.set")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("text"), text},
        });
    };
    beatGrid_->onStructureChanged = [this] {
        sendSongSnapshot();
    };
    lyricGrid_->onStructureChanged = [this] {
        sendSongSnapshot();
    };
    beatGrid_->onGridResized = [this](int section, int beats, int revision) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("grid.resize")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("lane"), QStringLiteral("beat")},
            {QStringLiteral("beats"), beats},
        });
    };
    lyricGrid_->onGridResized = beatGrid_->onGridResized;

    latencyLabel_ = new QLabel(QStringLiteral("Latency -"), this);
    latencyLabel_->setObjectName(QStringLiteral("StatusPill"));
    depthLabel_ = new QLabel(QStringLiteral("Depth -"), this);
    depthLabel_->setObjectName(QStringLiteral("StatusPill"));
    ringDepthLabel_ = new QLabel(QStringLiteral("Ring -"), this);
    ringDepthLabel_->setObjectName(QStringLiteral("StatusPill"));
    underrunLabel_ = new QLabel(QStringLiteral("Underrun -"), this);
    underrunLabel_->setObjectName(QStringLiteral("StatusPill"));
    missingFramesLabel_ = new QLabel(QStringLiteral("Missing -"), this);
    missingFramesLabel_->setObjectName(QStringLiteral("StatusPill"));
    driftLabel_ = new QLabel(QStringLiteral("Drift -"), this);
    driftLabel_->setObjectName(QStringLiteral("StatusPill"));
    diagnosisLabel_ = new QLabel(QStringLiteral("Diagnosis -"), this);
    diagnosisLabel_->setObjectName(QStringLiteral("StatusPill"));
    diagnosisLabel_->setMinimumWidth(260);
    diagnosisLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto* footer = new QHBoxLayout();
    footer->addWidget(latencyLabel_);
    footer->addWidget(jitterLabel_);
    footer->addWidget(lossLabel_);
    footer->addWidget(depthLabel_);
    footer->addWidget(ringDepthLabel_);
    footer->addWidget(underrunLabel_);
    footer->addWidget(missingFramesLabel_);
    footer->addWidget(driftLabel_);
    footer->addStretch(1);
    footer->addWidget(diagnosisLabel_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);
    layout->addLayout(header);
    layout->addLayout(library);
    layout->addWidget(buildSessionPage());
    layout->addWidget(tabs_, 1);

    logEdit_ = new QPlainTextEdit(this);
    logEdit_->setReadOnly(true);
    logEdit_->setMaximumBlockCount(2000);
    logEdit_->setMaximumHeight(150);
    layout->addWidget(logEdit_);
    layout->addLayout(footer);

    QTimer::singleShot(0, this, [this] {
        refreshDevices();
        refreshLocalOutputs();
        refreshLoopbackSources();
    });
}

QWidget* MainWindow::buildSessionPage()
{
    auto* page = new QWidget(this);
    modeBox_ = new QComboBox(page);
    modeBox_->addItems({QStringLiteral("Listen"), QStringLiteral("Connect")});

    jam2PathEdit_ = new QLineEdit(SessionController::defaultJam2Path(), page);
    bindHostEdit_ = new QLineEdit(SessionController::defaultBindHost(), page);
    portSpin_ = new QSpinBox(page);
    portSpin_->setRange(1, 65535);
    portSpin_->setValue(49000);
    publicHostEdit_ = new QLineEdit(SessionController::defaultPublicHost(), page);
    connectUrlEdit_ = new QLineEdit(page);
    generatedUrlEdit_ = new QLineEdit(page);
    generatedUrlEdit_->setReadOnly(true);
    stunServerEdit_ = new QLineEdit(QStringLiteral("stun.l.google.com:19302"), page);
    stunTimeoutSpin_ = new QSpinBox(page);
    stunTimeoutSpin_->setRange(1, 60000);
    stunTimeoutSpin_->setValue(1000);
    stunRetriesSpin_ = new QSpinBox(page);
    stunRetriesSpin_->setRange(1, 100);
    stunRetriesSpin_->setValue(3);
    waitMsSpin_ = new QSpinBox(page);
    waitMsSpin_->setRange(0, 24 * 60 * 60 * 1000);
    waitMsSpin_->setValue(0);
    streamMsSpin_ = new QSpinBox(page);
    streamMsSpin_->setRange(0, 24 * 60 * 60 * 1000);
    streamMsSpin_->setValue(0);
    streamLingerMsSpin_ = new QSpinBox(page);
    streamLingerMsSpin_->setRange(0, 60000);
    streamLingerMsSpin_->setValue(100);
    statsCheck_ = new QCheckBox(QStringLiteral("Periodic stats"), page);
    statsCheck_->setChecked(true);
    statsWarmupMsSpin_ = new QSpinBox(page);
    statsWarmupMsSpin_->setRange(0, 600000);
    statsWarmupMsSpin_->setValue(3000);
    logStatsEdit_ = new QLineEdit(page);
    socketSendBufferSpin_ = new QSpinBox(page);
    socketSendBufferSpin_->setRange(0, std::numeric_limits<int>::max());
    socketSendBufferSpin_->setValue(0);
    socketRecvBufferSpin_ = new QSpinBox(page);
    socketRecvBufferSpin_->setRange(0, std::numeric_limits<int>::max());
    socketRecvBufferSpin_->setValue(0);

    deviceBox_ = new QComboBox(page);
    deviceBox_->setEditable(true);
    localOutputBox_ = new QComboBox(page);
    inputChannelsEdit_ = new QLineEdit(QStringLiteral("1"), page);
    outputChannelsEdit_ = new QLineEdit(QStringLiteral("1,2"), page);
    sampleRateSpin_ = new QSpinBox(page);
    sampleRateSpin_->setRange(8000, 384000);
    sampleRateSpin_->setValue(44100);
    bufferSizeSpin_ = new QSpinBox(page);
    bufferSizeSpin_->setRange(16, 4096);
    bufferSizeSpin_->setValue(128);
    frameSizeSpin_ = new QSpinBox(page);
    frameSizeSpin_->setRange(32, 256);
    frameSizeSpin_->setValue(128);
    prefillSpin_ = new QSpinBox(page);
    prefillSpin_->setRange(0, 65536);
    prefillSpin_->setValue(1536);
    playbackMaxSpin_ = new QSpinBox(page);
    playbackMaxSpin_->setRange(0, 65536);
    playbackMaxSpin_->setValue(0);
    captureRingSpin_ = new QSpinBox(page);
    captureRingSpin_->setRange(1, 1048576);
    captureRingSpin_->setValue(4096);
    playbackRingSpin_ = new QSpinBox(page);
    playbackRingSpin_->setRange(1, 1048576);
    playbackRingSpin_->setValue(4096);
    driftCorrectionCheck_ = new QCheckBox(QStringLiteral("Drift correction"), page);
    driftCorrectionCheck_->setChecked(true);
    driftSmoothingSpin_ = new QDoubleSpinBox(page);
    driftSmoothingSpin_->setRange(0.0, 1.0);
    driftSmoothingSpin_->setDecimals(3);
    driftSmoothingSpin_->setSingleStep(0.005);
    driftSmoothingSpin_->setValue(0.02);
    driftDeadbandSpin_ = new QSpinBox(page);
    driftDeadbandSpin_->setRange(0, 50000);
    driftDeadbandSpin_->setValue(25);
    driftMaxCorrectionSpin_ = new QSpinBox(page);
    driftMaxCorrectionSpin_->setRange(0, 50000);
    driftMaxCorrectionSpin_->setValue(500);
    noStunCheck_ = new QCheckBox(QStringLiteral("No STUN"), page);
    bpmSpin_ = new QSpinBox(page);
    bpmSpin_->setRange(1, 400);
    bpmSpin_->setValue(120);
    bpmSpin_->hide();
    remoteLevelSlider_ = makeUnitSlider(1.0, page);
    sampleTimePlayoutCheck_ = new QCheckBox(QStringLiteral("Sample-time playout"), page);
    sampleTimePlayoutCheck_->setChecked(true);
    playoutDelaySpin_ = new QSpinBox(page);
    playoutDelaySpin_->setRange(0, 1048576);
    playoutDelaySpin_->setValue(0);
    adaptiveCushionCheck_ = new QCheckBox(QStringLiteral("Adaptive cushion"), page);
    adaptiveTargetSpin_ = new QSpinBox(page);
    adaptiveTargetSpin_->setRange(0, 1048576);
    adaptiveMinSpin_ = new QSpinBox(page);
    adaptiveMinSpin_->setRange(0, 1048576);
    adaptiveMaxSpin_ = new QSpinBox(page);
    adaptiveMaxSpin_->setRange(0, 1048576);
    adaptiveReleaseSpin_ = new QSpinBox(page);
    adaptiveReleaseSpin_->setRange(0, 1000000);
    adaptiveReleaseSpin_->setValue(1000);
    extraArgsEdit_ = new QLineEdit(page);
    startButton_ = new QPushButton(QStringLiteral("Start Jam"), page);
    joinButton_ = new QPushButton(QStringLiteral("Join Jam"), page);
    stopButton_ = new QPushButton(QStringLiteral("End Jam"), page);
    refreshControlButton_ = new QPushButton(QStringLiteral("Refresh Control"), page);
    stopButton_->setEnabled(false);
    refreshControlButton_->setEnabled(false);

    jam2PathEdit_->setMinimumWidth(360);
    connectUrlEdit_->setMinimumWidth(420);
    generatedUrlEdit_->setMinimumWidth(420);
    deviceBox_->setEditable(false);
    deviceBox_->setMinimumWidth(280);
    localOutputBox_->setMinimumWidth(280);
    const QList<QWidget*> sessionEditors{
        modeBox_, jam2PathEdit_, bindHostEdit_, portSpin_, publicHostEdit_, connectUrlEdit_,
        generatedUrlEdit_, stunServerEdit_, stunTimeoutSpin_, stunRetriesSpin_, waitMsSpin_,
        streamMsSpin_, streamLingerMsSpin_, statsWarmupMsSpin_, logStatsEdit_,
        socketSendBufferSpin_, socketRecvBufferSpin_, deviceBox_, localOutputBox_, inputChannelsEdit_,
        outputChannelsEdit_, sampleRateSpin_, bufferSizeSpin_, frameSizeSpin_, prefillSpin_,
        playbackMaxSpin_, captureRingSpin_, playbackRingSpin_, driftSmoothingSpin_,
        driftDeadbandSpin_, driftMaxCorrectionSpin_, playoutDelaySpin_, adaptiveTargetSpin_,
        adaptiveMinSpin_, adaptiveMaxSpin_, adaptiveReleaseSpin_, extraArgsEdit_,
    };
    for (QWidget* widget : sessionEditors) {
        applyMutedEditorStyle(widget);
    }
    const QList<QWidget*> sessionDialogWidgets{
        modeBox_, jam2PathEdit_, bindHostEdit_, portSpin_, publicHostEdit_, connectUrlEdit_,
        generatedUrlEdit_, stunServerEdit_, stunTimeoutSpin_, stunRetriesSpin_, waitMsSpin_,
        streamMsSpin_, streamLingerMsSpin_, statsCheck_, statsWarmupMsSpin_, logStatsEdit_,
        socketSendBufferSpin_, socketRecvBufferSpin_, deviceBox_, localOutputBox_, inputChannelsEdit_,
        outputChannelsEdit_, sampleRateSpin_, bufferSizeSpin_, frameSizeSpin_, prefillSpin_,
        playbackMaxSpin_, captureRingSpin_, playbackRingSpin_, driftCorrectionCheck_,
        driftSmoothingSpin_, driftDeadbandSpin_, driftMaxCorrectionSpin_, noStunCheck_,
        sampleTimePlayoutCheck_, playoutDelaySpin_, adaptiveCushionCheck_, adaptiveTargetSpin_,
        adaptiveMinSpin_, adaptiveMaxSpin_, adaptiveReleaseSpin_, extraArgsEdit_,
    };
    for (QWidget* widget : sessionDialogWidgets) {
        widget->hide();
    }

    auto* runtimeLayout = new QGridLayout();
    runtimeLayout->addWidget(new QLabel(QStringLiteral("Remote"), page), 0, 0);
    runtimeLayout->addWidget(remoteLevelSlider_, 0, 1);

    runtimeMixBox_ = new QGroupBox(QStringLiteral("Runtime Mix"), page);
    runtimeMixBox_->setLayout(runtimeLayout);
    runtimeMixBox_->setVisible(jam2_.isRunning());

    auto* buttons = new QHBoxLayout();
    buttons->addWidget(startButton_);
    buttons->addWidget(joinButton_);
    buttons->addWidget(stopButton_);
    buttons->addWidget(refreshControlButton_);
    buttons->addStretch(1);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(runtimeMixBox_);
    layout->addLayout(buttons);

    QObject::connect(startButton_, &QPushButton::clicked, this, [this] { showStartJamDialog(); });
    QObject::connect(joinButton_, &QPushButton::clicked, this, [this] { showJoinJamDialog(); });
    QObject::connect(stopButton_, &QPushButton::clicked, this, [this] { stopJam(); });
    QObject::connect(refreshControlButton_, &QPushButton::clicked, this, [this] { refreshControlConnection(); });
    QObject::connect(remoteLevelSlider_, &QSlider::valueChanged, this, [this] { updateRuntimeControls(); });

    return page;
}

QWidget* MainWindow::buildSongPage()
{
    auto* page = new QWidget(this);
    chordGrid_ = new BeatGridWidget(&chordModel_, QStringLiteral("chord"), page);
    leadLabel_ = new QLabel(page);
    leadPendingLabel_ = new QLabel(page);
    updateLeadLabels();

    leadSwapButton_ = new QPushButton(QStringLiteral("Request Lead Swap"), page);
    leadSwapButton_->setEnabled(jam2_.isRunning());
    QObject::connect(leadSwapButton_, &QPushButton::clicked, this, [this] { requestLeadSwap(); });

    auto* top = new QHBoxLayout();
    top->addWidget(leadLabel_);
    top->addWidget(leadPendingLabel_);
    top->addStretch(1);
    top->addWidget(leadSwapButton_);

    auto* layout = new QVBoxLayout(page);
    layout->addLayout(top);
    layout->addWidget(chordGrid_, 1);

    chordGrid_->onCellEdited = [this](int section, const QString& lane, int beat, const QString& text, int revision) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("beat.set")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("lane"), lane},
            {QStringLiteral("beat"), beat},
            {QStringLiteral("text"), text},
        });
    };
    chordGrid_->onGridResized = [this](int section, int beats, int revision) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("grid.resize")},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("section"), section},
            {QStringLiteral("lane"), QStringLiteral("chord")},
            {QStringLiteral("beats"), beats},
        });
    };
    chordGrid_->onStructureChanged = [this] {
        sendSongSnapshot();
    };

    return page;
}

QWidget* MainWindow::buildTrackPage()
{
    auto* page = new QWidget(this);
    trackNameLabel_ = new QLabel(QStringLiteral("Track: No track loaded"), page);

    auto* loadButton = new QPushButton(QStringLiteral("Load WAV"), page);
    shareTrackFileButton_ = new QPushButton(QStringLiteral("Share WAV"), page);

    trackWaveform_ = new WaveformWidget(page);
    trackSpeedSlider_ = new QSlider(Qt::Horizontal, page);
    trackSpeedSlider_->setRange(10, 200);
    trackSpeedSlider_->setValue(100);
    trackSpeedSlider_->setMinimumWidth(220);
    trackSpeedSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    trackSpeedSpin_ = new QDoubleSpinBox(page);
    trackSpeedSpin_->setRange(0.10, 2.00);
    trackSpeedSpin_->setSingleStep(0.01);
    trackSpeedSpin_->setDecimals(2);
    trackSpeedSpin_->setValue(1.0);
    trackSpeedSpin_->setFixedWidth(92);
    applyMutedEditorStyle(trackSpeedSpin_);
    trackPitchSlider_ = new QSlider(Qt::Horizontal, page);
    trackPitchSlider_->setRange(-12, 12);
    trackPitchSlider_->setValue(0);
    trackPitchSlider_->setMinimumWidth(220);
    trackPitchSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    trackPitchSpin_ = new QSpinBox(page);
    trackPitchSpin_->setRange(-12, 12);
    trackPitchSpin_->setSingleStep(1);
    trackPitchSpin_->setSuffix(QStringLiteral(" semitones"));
    trackPitchSpin_->setFixedWidth(128);
    applyMutedEditorStyle(trackPitchSpin_);
    focusFrequencySlider_ = new QSlider(Qt::Horizontal, page);
    focusFrequencySlider_->setRange(40, 8000);
    focusFrequencySlider_->setValue(120);
    focusFrequencySlider_->setMinimumWidth(220);
    focusFrequencySlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    focusFrequencySpin_ = new QSpinBox(page);
    focusFrequencySpin_->setRange(40, 8000);
    focusFrequencySpin_->setValue(120);
    focusFrequencySpin_->setSuffix(QStringLiteral(" Hz"));
    focusFrequencySpin_->setFixedWidth(108);
    applyMutedEditorStyle(focusFrequencySpin_);
    auto* syncBox = new QCheckBox(QStringLiteral("Sync track controls"), page);
    syncBox->setChecked(true);
    capturePathEdit_ = new QLineEdit(SessionController::defaultCapturePath(), page);
    capturePathEdit_->setMinimumWidth(420);
    applyMutedEditorStyle(capturePathEdit_);
    captureOutputEdit_ = new QLineEdit(page);
    captureOutputEdit_->setMinimumWidth(420);
    applyMutedEditorStyle(captureOutputEdit_);
    loopbackSourceBox_ = new QComboBox(page);
    loopbackSourceBox_->setEditable(true);
    loopbackSourceBox_->addItem(QStringLiteral("[default] System mix"), QStringLiteral("default"));
    loopbackSourceBox_->setMinimumWidth(360);
    applyMutedEditorStyle(loopbackSourceBox_);
    captureManualStopCheck_ = new QCheckBox(QStringLiteral("Record until stopped"), page);
    captureManualStopCheck_->setChecked(true);
    captureDurationSpin_ = new QSpinBox(page);
    captureDurationSpin_->setRange(1, 600);
    captureDurationSpin_->setValue(30);
    captureDurationSpin_->setSuffix(QStringLiteral(" s"));
    captureDurationSpin_->setEnabled(false);
    captureDurationSpin_->setMinimumWidth(120);
    applyMutedEditorStyle(captureDurationSpin_);
    captureTriggerCheck_ = new QCheckBox(QStringLiteral("Trigger on signal"), page);
    trimLeadingCheck_ = new QCheckBox(QStringLiteral("Trim leading silence"), page);
    trimTrailingCheck_ = new QCheckBox(QStringLiteral("Trim trailing silence"), page);
    trimLeadingCheck_->setChecked(true);
    trimTrailingCheck_->setChecked(true);
    triggerThresholdSpin_ = new QDoubleSpinBox(page);
    triggerThresholdSpin_->setRange(-120.0, 0.0);
    triggerThresholdSpin_->setDecimals(1);
    triggerThresholdSpin_->setValue(-45.0);
    triggerThresholdSpin_->setSuffix(QStringLiteral(" dB"));
    triggerThresholdSpin_->setMinimumWidth(120);
    applyMutedEditorStyle(triggerThresholdSpin_);
    tailThresholdSpin_ = new QDoubleSpinBox(page);
    tailThresholdSpin_->setRange(-120.0, 0.0);
    tailThresholdSpin_->setDecimals(1);
    tailThresholdSpin_->setValue(-50.0);
    tailThresholdSpin_->setSuffix(QStringLiteral(" dB"));
    tailThresholdSpin_->setMinimumWidth(120);
    applyMutedEditorStyle(tailThresholdSpin_);
    preRollSpin_ = new QSpinBox(page);
    preRollSpin_->setRange(0, 10000);
    preRollSpin_->setValue(250);
    preRollSpin_->setSuffix(QStringLiteral(" ms"));
    preRollSpin_->setMinimumWidth(120);
    applyMutedEditorStyle(preRollSpin_);
    triggerHoldSpin_ = new QSpinBox(page);
    triggerHoldSpin_->setRange(1, 5000);
    triggerHoldSpin_->setValue(50);
    triggerHoldSpin_->setSuffix(QStringLiteral(" ms"));
    triggerHoldSpin_->setMinimumWidth(120);
    applyMutedEditorStyle(triggerHoldSpin_);
    tailSilenceSpin_ = new QSpinBox(page);
    tailSilenceSpin_->setRange(0, 30000);
    tailSilenceSpin_->setValue(1000);
    tailSilenceSpin_->setSuffix(QStringLiteral(" ms"));
    tailSilenceSpin_->setMinimumWidth(120);
    applyMutedEditorStyle(tailSilenceSpin_);
    playTrackButton_ = new QPushButton(QStringLiteral("Play Track"), page);
    stopTrackButton_ = new QPushButton(QStringLiteral("Stop Track"), page);
    loopStartButton_ = new QPushButton(QStringLiteral("Loop Start"), page);
    loopEndButton_ = new QPushButton(QStringLiteral("Loop End"), page);
    clearLoopButton_ = new QPushButton(QStringLiteral("Clear Loop"), page);
    loopEnabledCheck_ = new QCheckBox(QStringLiteral("Loop whole track"), page);
    loopEnabledCheck_->setChecked(false);
    waveformGridCheck_ = new QCheckBox(QStringLiteral("Show waveform grid"), page);
    waveformGridCheck_->setChecked(trackController_.model().waveformGridVisible);
    trackLevelSlider_ = new QSlider(Qt::Horizontal, page);
    trackLevelSlider_->setRange(-60, 12);
    trackLevelSlider_->setValue(0);
    trackLevelSlider_->setMinimumWidth(160);
    trackLevelSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    trackController_.model().trackGainDb = 0.0;
    trackLevelDbLabel_ = new QLabel(dbText(trackController_.model().trackGainDb), page);
    trackLevelDbLabel_->setFrameShape(QFrame::StyledPanel);
    trackLevelDbLabel_->setAlignment(Qt::AlignCenter);
    trackLevelDbLabel_->setMinimumWidth(82);
    captureButton_ = new QPushButton(QStringLiteral("Record Input"), page);
    loopbackCaptureButton_ = new QPushButton(QStringLiteral("Record Loopback"), page);
    stopCaptureButton_ = new QPushButton(QStringLiteral("Stop Capture"), page);
    stopCaptureButton_->setEnabled(false);
    importCaptureButton_ = new QPushButton(QStringLiteral("Import Capture"), page);
    importCaptureButton_->setEnabled(false);

    QDir captureDir(QDir::current());
    captureDir.mkpath(QStringLiteral("captures"));
    captureOutputEdit_->setText(captureDir.absoluteFilePath(QStringLiteral("captures/capture.wav")));
    const QList<QWidget*> captureDialogWidgets{
        capturePathEdit_, captureOutputEdit_, loopbackSourceBox_, captureDurationSpin_,
        captureManualStopCheck_,
        captureTriggerCheck_, trimLeadingCheck_, trimTrailingCheck_, triggerThresholdSpin_,
        tailThresholdSpin_, preRollSpin_, triggerHoldSpin_, tailSilenceSpin_,
    };
    for (QWidget* widget : captureDialogWidgets) {
        widget->hide();
    }
    auto* speedControl = new QWidget(page);
    speedControl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* speedLayout = new QHBoxLayout(speedControl);
    speedLayout->setContentsMargins(0, 0, 0, 0);
    speedLayout->addWidget(trackSpeedSlider_, 1);
    speedLayout->addWidget(trackSpeedSpin_);
    auto* pitchControl = new QWidget(page);
    pitchControl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* pitchLayout = new QHBoxLayout(pitchControl);
    pitchLayout->setContentsMargins(0, 0, 0, 0);
    pitchLayout->addWidget(trackPitchSlider_, 1);
    pitchLayout->addWidget(trackPitchSpin_);
    auto* trackLevelControl = new QWidget(page);
    trackLevelControl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* trackLevelLayout = new QHBoxLayout(trackLevelControl);
    trackLevelLayout->setContentsMargins(0, 0, 0, 0);
    trackLevelLayout->addWidget(trackLevelSlider_, 1);
    trackLevelLayout->addWidget(trackLevelDbLabel_);
    auto* focusControl = new QWidget(page);
    focusControl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* focusLayout = new QHBoxLayout(focusControl);
    focusLayout->setContentsMargins(0, 0, 0, 0);
    focusFrequencyCheck_ = new QCheckBox(page);
    focusPresetBox_ = new QComboBox(page);
    focusPresetBox_->addItem(QStringLiteral("Custom"), QStringLiteral("custom"));
    focusPresetBox_->addItem(QStringLiteral("Bass"), QStringLiteral("bass"));
    focusPresetBox_->addItem(QStringLiteral("Guitar"), QStringLiteral("guitar"));
    focusPresetBox_->addItem(QStringLiteral("Vocals"), QStringLiteral("vocals"));
    focusPresetBox_->addItem(QStringLiteral("Drums"), QStringLiteral("drums"));
    focusPresetBox_->setFixedWidth(108);
    applyMutedEditorStyle(focusPresetBox_);
    focusLayout->addWidget(focusFrequencyCheck_);
    focusLayout->addWidget(focusPresetBox_);
    focusLayout->addWidget(focusFrequencySlider_, 1);
    focusLayout->addWidget(focusFrequencySpin_);
    auto* loopOptionsControl = new QWidget(page);
    auto* loopOptionsLayout = new QHBoxLayout(loopOptionsControl);
    loopOptionsLayout->setContentsMargins(0, 0, 0, 0);
    loopOptionsLayout->addWidget(loopEnabledCheck_);
    loopOptionsLayout->addWidget(waveformGridCheck_);
    loopOptionsLayout->addStretch(1);

    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(trackNameLabel_);
    form->addRow(QStringLiteral("Speed"), speedControl);
    form->addRow(QStringLiteral("Pitch"), pitchControl);
    form->addRow(QStringLiteral("Track level"), trackLevelControl);
    form->addRow(QStringLiteral("Focus frequency"), focusControl);
    form->addRow(loopOptionsControl);
    form->addRow(syncBox);

    auto* buttons = new QHBoxLayout();
    buttons->addWidget(loadButton);
    buttons->addWidget(shareTrackFileButton_);
    buttons->addWidget(playTrackButton_);
    buttons->addWidget(stopTrackButton_);
    buttons->addWidget(loopStartButton_);
    buttons->addWidget(loopEndButton_);
    buttons->addWidget(clearLoopButton_);
    buttons->addWidget(captureButton_);
    buttons->addWidget(loopbackCaptureButton_);
    buttons->addWidget(stopCaptureButton_);
    buttons->addWidget(importCaptureButton_);
    buttons->addStretch(1);

    auto* layout = new QVBoxLayout(page);
    layout->addLayout(buttons);
    layout->addWidget(trackWaveform_, 1);
    layout->addLayout(form);

    QObject::connect(loadButton, &QPushButton::clicked, this, [this] { loadTrackMetadata(); });
    QObject::connect(shareTrackFileButton_, &QPushButton::clicked, this, [this] { sendTrackFile(); });
    QObject::connect(trackSpeedSlider_, &QSlider::valueChanged, this, [this](int value) {
        const double speed = static_cast<double>(value) / 100.0;
        trackController_.model().speed = speed;
        if (trackSpeedSpin_) {
            const QSignalBlocker blocker(trackSpeedSpin_);
            trackSpeedSpin_->setValue(speed);
        }
        applyTrackPlaybackSettings();
    });
    QObject::connect(trackSpeedSpin_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        trackController_.model().speed = value;
        if (trackSpeedSlider_) {
            const QSignalBlocker blocker(trackSpeedSlider_);
            trackSpeedSlider_->setValue(qBound(10, qRound(value * 100.0), 200));
        }
        applyTrackPlaybackSettings();
    });
    QObject::connect(trackPitchSlider_, &QSlider::valueChanged, this, [this](int value) {
        trackController_.model().pitchCents = value * 100;
        if (trackPitchSpin_) {
            const QSignalBlocker blocker(trackPitchSpin_);
            trackPitchSpin_->setValue(value);
        }
        applyTrackPlaybackSettings();
    });
    QObject::connect(trackPitchSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        trackController_.model().pitchCents = value * 100;
        if (trackPitchSlider_) {
            const QSignalBlocker blocker(trackPitchSlider_);
            trackPitchSlider_->setValue(value);
        }
        applyTrackPlaybackSettings();
    });
    trackWaveform_->onSeekMs = [this](qint64 positionMs) {
        if (trackDevice_) {
            trackDevice_->setPositionMs(positionMs);
        }
        updateTrackTimeline();
    };
    QObject::connect(playTrackButton_, &QPushButton::clicked, this, [this] { playTrack(); });
    QObject::connect(stopTrackButton_, &QPushButton::clicked, this, [this] { stopTrack(); });
    QObject::connect(loopStartButton_, &QPushButton::clicked, this, [this] { setLoopStartAtCurrentPosition(); });
    QObject::connect(loopEndButton_, &QPushButton::clicked, this, [this] { setLoopEndAtCurrentPosition(); });
    QObject::connect(clearLoopButton_, &QPushButton::clicked, this, [this] { clearTrackLoop(); });
    QObject::connect(loopEnabledCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        trackController_.model().loopEnabled = checked;
        applyTrackPlaybackSettings();
        updateTrackControls();
        if (trackController_.model().syncControls) {
            sendControl(trackMetadataMessage(QStringLiteral("track.offer")));
        }
    });
    QObject::connect(waveformGridCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        trackController_.model().waveformGridVisible = checked;
        if (trackWaveform_) {
            trackWaveform_->setGridVisible(checked);
        }
    });
    QObject::connect(trackLevelSlider_, &QSlider::valueChanged, this, [this](int value) {
        trackController_.model().trackGainDb = static_cast<double>(value);
        if (trackLevelDbLabel_) {
            trackLevelDbLabel_->setText(dbText(trackController_.model().trackGainDb));
        }
        applyTrackPlaybackSettings();
    });
    QObject::connect(focusFrequencyCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        trackController_.model().focusEnabled = checked;
        applyTrackPlaybackSettings();
        if (trackController_.model().syncControls) {
            sendControl(trackMetadataMessage(QStringLiteral("track.offer")));
        }
    });
    QObject::connect(focusPresetBox_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        QString key = focusPresetBox_->currentData().toString();
        if (key.isEmpty()) {
            key = QStringLiteral("custom");
        }
        auto& model = trackController_.model();
        model.focusPreset = key;
        if (!isCustomFocusPreset(key)) {
            const FocusPreset preset = focusPresetForKey(key);
            model.focusEnabled = true;
            model.focusFrequencyHz = preset.frequencyHz;
            model.focusGainDb = preset.gainDb;
            model.focusQ = preset.q;
        }
        updateTrackControls();
        applyTrackPlaybackSettings();
        if (model.syncControls) {
            sendControl(trackMetadataMessage(QStringLiteral("track.offer")));
        }
    });
    QObject::connect(focusFrequencySlider_, &QSlider::valueChanged, this, [this](int value) {
        auto& model = trackController_.model();
        model.focusPreset = QStringLiteral("custom");
        model.focusFrequencyHz = value;
        if (focusPresetBox_) {
            const QSignalBlocker blocker(focusPresetBox_);
            focusPresetBox_->setCurrentIndex(qMax(0, focusPresetBox_->findData(QStringLiteral("custom"))));
        }
        if (focusFrequencySlider_) {
            focusFrequencySlider_->setEnabled(true);
        }
        if (focusFrequencySpin_) {
            focusFrequencySpin_->setEnabled(true);
        }
        if (focusFrequencySpin_) {
            const QSignalBlocker blocker(focusFrequencySpin_);
            focusFrequencySpin_->setValue(value);
        }
        applyTrackPlaybackSettings();
    });
    QObject::connect(focusFrequencySpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        auto& model = trackController_.model();
        model.focusPreset = QStringLiteral("custom");
        model.focusFrequencyHz = value;
        if (focusPresetBox_) {
            const QSignalBlocker blocker(focusPresetBox_);
            focusPresetBox_->setCurrentIndex(qMax(0, focusPresetBox_->findData(QStringLiteral("custom"))));
        }
        if (focusFrequencySlider_) {
            focusFrequencySlider_->setEnabled(true);
        }
        if (focusFrequencySpin_) {
            focusFrequencySpin_->setEnabled(true);
        }
        if (focusFrequencySlider_) {
            const QSignalBlocker blocker(focusFrequencySlider_);
            focusFrequencySlider_->setValue(value);
        }
        applyTrackPlaybackSettings();
    });
    QObject::connect(syncBox, &QCheckBox::toggled, this, [this](bool checked) {
        trackController_.model().syncControls = checked;
    });
    QObject::connect(captureManualStopCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        (void)checked;
        updateCaptureDurationControl(captureManualStopCheck_, captureDurationSpin_);
    });
    QObject::connect(captureButton_, &QPushButton::clicked, this, [this] { showInputCaptureDialog(); });
    QObject::connect(loopbackCaptureButton_, &QPushButton::clicked, this, [this] { showLoopbackCaptureDialog(); });
    QObject::connect(stopCaptureButton_, &QPushButton::clicked, this, [this] { stopInputCapture(); });
    QObject::connect(importCaptureButton_, &QPushButton::clicked, this, [this] { importLastCapture(); });

    return page;
}

QWidget* MainWindow::buildMetronomePage()
{
    auto* page = new QWidget(this);

    metronomeBpmSpin_ = new QSpinBox(page);
    metronomeBpmSpin_->setRange(1, 400);
    metronomeBpmSpin_->setValue(qBound(1, static_cast<int>(std::lround(trackController_.model().acceptedBpm)), 400));
    metronomeBpmSpin_->setSuffix(QStringLiteral(" BPM"));
    applyMutedEditorStyle(metronomeBpmSpin_);

    metronomeBeatsSpin_ = new QSpinBox(page);
    metronomeBeatsSpin_->setRange(1, 16);
    metronomeBeatsSpin_->setValue(4);
    applyMutedEditorStyle(metronomeBeatsSpin_);

    metronomeDivisionBox_ = new QComboBox(page);
    metronomeDivisionBox_->addItem(QStringLiteral("Quarter"), 1);
    metronomeDivisionBox_->addItem(QStringLiteral("Eighth"), 2);
    metronomeDivisionBox_->addItem(QStringLiteral("Triplet"), 3);
    metronomeDivisionBox_->addItem(QStringLiteral("16th"), 4);
    metronomeDivisionBox_->addItem(QStringLiteral("6th"), 6);
    metronomeDivisionBox_->addItem(QStringLiteral("32nd"), 8);
    metronomeDivisionBox_->setCurrentIndex(metronomeDivisionBox_->findData(1));
    applyMutedEditorStyle(metronomeDivisionBox_);

    metronomeModeBox_ = new QComboBox(page);
    metronomeModeBox_->addItems({
        QStringLiteral("shared-grid"),
        QStringLiteral("leader-audio"),
        QStringLiteral("symmetric-delay"),
        QStringLiteral("listener-compensated"),
    });
    applyMutedEditorStyle(metronomeModeBox_);

    metronomeLevelSlider_ = makeUnitSlider(0.2, page);
    localMetronomeLevelSlider_ = makeUnitSlider(0.35, page);
    trackMetronomeLabel_ = new QLabel(QStringLiteral("Local metronome stopped"), page);
    startTrackMetronomeButton_ = new QPushButton(QStringLiteral("Start"), page);
    stopTrackMetronomeButton_ = new QPushButton(QStringLiteral("Stop"), page);
    stopTrackMetronomeButton_->setEnabled(false);

    metronomePatternTable_ = new QTableWidget(page);
    metronomePatternTable_->setRowCount(2);
    metronomePatternTable_->verticalHeader()->setVisible(true);
    metronomePatternTable_->setVerticalHeaderLabels(QStringList{QStringLiteral("Play"), QStringLiteral("Accent")});
    metronomePatternTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    metronomePatternTable_->setSelectionMode(QAbstractItemView::NoSelection);
    metronomePatternTable_->setMinimumHeight(110);
    metronomePatternTable_->setStyleSheet(QStringLiteral(
        "QTableWidget::item { padding: 3px 6px; }"
        "QCheckBox::indicator { width: 15px; height: 15px; border: 1px solid #52616c; background: #1f2428; }"
        "QCheckBox::indicator:checked { border: 1px solid #66c6a6; background: #66c6a6; }"));

    auto* controls = new QGridLayout();
    controls->addWidget(new QLabel(QStringLiteral("BPM"), page), 0, 0);
    controls->addWidget(metronomeBpmSpin_, 0, 1);
    controls->addWidget(new QLabel(QStringLiteral("Beats"), page), 0, 2);
    controls->addWidget(metronomeBeatsSpin_, 0, 3);
    controls->addWidget(new QLabel(QStringLiteral("Division"), page), 0, 4);
    controls->addWidget(metronomeDivisionBox_, 0, 5);
    controls->addWidget(new QLabel(QStringLiteral("Mode"), page), 0, 6);
    controls->addWidget(metronomeModeBox_, 0, 7);
    controls->addWidget(new QLabel(QStringLiteral("Jam level"), page), 1, 0);
    controls->addWidget(metronomeLevelSlider_, 1, 1, 1, 3);
    controls->addWidget(new QLabel(QStringLiteral("Standalone level"), page), 1, 4);
    controls->addWidget(localMetronomeLevelSlider_, 1, 5, 1, 3);

    auto* buttons = new QHBoxLayout();
    buttons->addWidget(startTrackMetronomeButton_);
    buttons->addWidget(stopTrackMetronomeButton_);
    buttons->addWidget(trackMetronomeLabel_);
    buttons->addStretch(1);

    auto* layout = new QVBoxLayout(page);
    layout->addLayout(controls);
    layout->addLayout(buttons);
    layout->addWidget(metronomePatternTable_, 1);

    QObject::connect(startTrackMetronomeButton_, &QPushButton::clicked, this, [this] { startTrackMetronome(); });
    QObject::connect(stopTrackMetronomeButton_, &QPushButton::clicked, this, [this] { stopTrackMetronome(); });
    QObject::connect(metronomeBpmSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
        updateTrackMetronomeInterval();
    });
    QObject::connect(metronomeModeBox_, &QComboBox::currentTextChanged, this, [this] {
        updateRuntimeControls();
        sendMetronomeSettingsToPeer();
    });
    QObject::connect(metronomeLevelSlider_, &QSlider::valueChanged, this, [this] {
        updateRuntimeControls();
    });
    QObject::connect(localMetronomeLevelSlider_, &QSlider::valueChanged, this, [this] {
        applyMetronomePatternToLocalDevice();
    });
    QObject::connect(metronomeBeatsSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
        rebuildMetronomePattern();
        updateTrackMetronomeInterval();
    });
    QObject::connect(metronomeDivisionBox_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        rebuildMetronomePattern();
        updateTrackMetronomeInterval();
    });

    rebuildMetronomePattern();
    return page;
}

void MainWindow::startJam()
{
    if (jam2_.isRunning()) {
        return;
    }
    QStringList args;
    const bool listenMode = modeBox_->currentText() == QStringLiteral("Listen");
    pendingJoinLaunch_ = false;
    pendingJoinBaseArgs_.clear();
    try {
        if (listenMode) {
            jam2::SessionInfo info;
            info.endpoint = {publicHostEdit_->text().toStdString(), static_cast<std::uint16_t>(portSpin_->value())};
            info.session_id = sessionId_;
            info.key = sessionKey_;
            generatedUrlEdit_->setText(QString::fromStdString(jam2::make_jam_url(info)));
            args << QStringLiteral("listen")
                 << QStringLiteral("--bind") << QStringLiteral("%1:%2").arg(bindHostEdit_->text()).arg(portSpin_->value())
                 << QStringLiteral("--public-endpoint") << QStringLiteral("%1:%2").arg(publicHostEdit_->text()).arg(portSpin_->value())
                 << QStringLiteral("--session-id") << sessionHex()
                 << QStringLiteral("--session-key") << keyHex();
            if (!noStunCheck_->isChecked()) {
                args << QStringLiteral("--stun") << stunServerEdit_->text()
                     << QStringLiteral("--stun-timeout-ms") << QString::number(stunTimeoutSpin_->value())
                     << QStringLiteral("--stun-retries") << QString::number(stunRetriesSpin_->value());
            }
            if (!controlServer_.listen(static_cast<quint16>(portSpin_->value()), sessionHex(), keyHex())) {
                appendLog(QStringLiteral("control server failed: ") + controlServer_.errorString());
            }
            controlServerMode_ = true;
            controlHost_ = bindHostEdit_->text();
            controlPort_ = static_cast<quint16>(portSpin_->value());
            controlSessionHex_ = sessionHex();
            controlKeyHex_ = keyHex();
        } else {
            const std::string url = connectUrlEdit_->text().toStdString();
            const jam2::SessionInfo info = jam2::parse_jam_url(url);
            sessionId_ = info.session_id;
            sessionKey_ = info.key;
            args << QStringLiteral("connect") << connectUrlEdit_->text();
            pendingJoinBaseArgs_ = args;
            pendingJoinLaunch_ = true;
            controlServerMode_ = false;
            controlHost_ = QString::fromStdString(info.endpoint.host);
            controlPort_ = info.endpoint.port;
            controlSessionHex_ = sessionHex();
            controlKeyHex_ = keyHex();
            controlClient_.connectToHost(QString::fromStdString(info.endpoint.host), info.endpoint.port, sessionHex(), keyHex());
        }
    } catch (const std::exception& error) {
        QMessageBox::warning(this, QStringLiteral("Jam2"), QString::fromUtf8(error.what()));
        return;
    }

    controlReconnectEnabled_ = true;
    controlReconnectAttempts_ = 0;
    if (!listenMode) {
        appendLog(QStringLiteral("waiting for leader settings before launching Jam2"));
        startButton_->setEnabled(false);
        joinButton_->setEnabled(false);
        stopButton_->setEnabled(true);
        if (refreshControlButton_) {
            refreshControlButton_->setEnabled(true);
        }
        return;
    }
    args << commonJamArgs();
    launchJamProcess(args);
}

void MainWindow::launchJamProcess(const QStringList& args)
{
    if (localMetronomeSink_) {
        localMetronomeSink_->stop();
        localMetronomeSink_.reset();
    }
    if (localMetronomeDevice_) {
        localMetronomeDevice_->close();
        localMetronomeDevice_.reset();
    }
    localMetronomeRunning_ = false;
    if (trackMetronomeLabel_) {
        trackMetronomeLabel_->setText(QStringLiteral("Jam metronome stopped"));
    }
    if (startTrackMetronomeButton_) {
        startTrackMetronomeButton_->setEnabled(true);
    }
    if (stopTrackMetronomeButton_) {
        stopTrackMetronomeButton_->setEnabled(false);
    }
    jam2_.start(jam2PathEdit_->text(), args);
    appendLog(QStringLiteral("starting: %1 %2").arg(jam2PathEdit_->text(), args.join(QLatin1Char(' '))));
    startButton_->setEnabled(false);
    joinButton_->setEnabled(false);
    stopButton_->setEnabled(true);
    if (refreshControlButton_) {
        refreshControlButton_->setEnabled(true);
    }
    if (runtimeMixBox_) {
        runtimeMixBox_->setVisible(true);
    }
    if (leadSwapButton_) {
        leadSwapButton_->setEnabled(true);
    }
    connectionLabel_->setText(QStringLiteral("Starting"));
    QTimer::singleShot(250, this, [this] {
        updateRuntimeControls();
    });
    QTimer::singleShot(500, this, [this] {
        sendMetronomeSettingsToPeer();
    });
}

void MainWindow::showStartJamDialog()
{
    if (jam2_.isRunning()) {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Start Jam"));
    dialog.resize(760, 620);
    auto updateGeneratedUrl = [this] {
        jam2::SessionInfo info;
        info.endpoint = {publicHostEdit_->text().toStdString(), static_cast<std::uint16_t>(portSpin_->value())};
        info.session_id = sessionId_;
        info.key = sessionKey_;
        generatedUrlEdit_->setText(QString::fromStdString(jam2::make_jam_url(info)));
    };
    try {
        updateGeneratedUrl();
    } catch (const std::exception&) {
        generatedUrlEdit_->clear();
    }

    auto* content = new QWidget(&dialog);
    auto* layout = new QVBoxLayout(content);
    const QList<QWidget*> visibleWidgets{
        jam2PathEdit_, bindHostEdit_, portSpin_, publicHostEdit_, generatedUrlEdit_,
        stunServerEdit_, stunTimeoutSpin_, stunRetriesSpin_, noStunCheck_, deviceBox_,
        localOutputBox_, inputChannelsEdit_, outputChannelsEdit_, sampleRateSpin_, bufferSizeSpin_, frameSizeSpin_,
        prefillSpin_, playbackMaxSpin_, captureRingSpin_, playbackRingSpin_, waitMsSpin_,
        streamMsSpin_, streamLingerMsSpin_, statsCheck_, statsWarmupMsSpin_, logStatsEdit_, socketSendBufferSpin_,
        socketRecvBufferSpin_, driftCorrectionCheck_, driftSmoothingSpin_, driftDeadbandSpin_,
        driftMaxCorrectionSpin_, sampleTimePlayoutCheck_, playoutDelaySpin_, adaptiveCushionCheck_,
        adaptiveTargetSpin_, adaptiveMinSpin_, adaptiveMaxSpin_, adaptiveReleaseSpin_, extraArgsEdit_,
    };
    for (QWidget* widget : visibleWidgets) {
        widget->show();
    }

    auto* sessionForm = new QFormLayout();
    sessionForm->addRow(QStringLiteral("jam2 binary"), jam2PathEdit_);
    sessionForm->addRow(QStringLiteral("Bind"), bindHostEdit_);
    sessionForm->addRow(QStringLiteral("Port"), portSpin_);
    sessionForm->addRow(QStringLiteral("Public endpoint host"), publicHostEdit_);
    sessionForm->addRow(QStringLiteral("Generated URL"), generatedUrlEdit_);
    sessionForm->addRow(QStringLiteral("STUN server"), stunServerEdit_);
    sessionForm->addRow(QStringLiteral("STUN timeout ms"), stunTimeoutSpin_);
    sessionForm->addRow(QStringLiteral("STUN retries"), stunRetriesSpin_);
    sessionForm->addRow(QString(), noStunCheck_);
    auto* sessionBox = new QGroupBox(QStringLiteral("Connection"), content);
    sessionBox->setLayout(sessionForm);
    layout->addWidget(sessionBox);

    auto* audioForm = new QFormLayout();
    audioForm->addRow(QStringLiteral("Audio device"), deviceBox_);
    audioForm->addRow(QStringLiteral("Local track/metronome output"), localOutputBox_);
    audioForm->addRow(QStringLiteral("Input channels"), inputChannelsEdit_);
    audioForm->addRow(QStringLiteral("Output channels"), outputChannelsEdit_);
    audioForm->addRow(QStringLiteral("Sample rate"), sampleRateSpin_);
    audioForm->addRow(QStringLiteral("Audio buffer size"), bufferSizeSpin_);
    audioForm->addRow(QStringLiteral("Frame size"), frameSizeSpin_);
    audioForm->addRow(QStringLiteral("Playback prefill frames"), prefillSpin_);
    audioForm->addRow(QStringLiteral("Playback max frames"), playbackMaxSpin_);
    audioForm->addRow(QStringLiteral("Capture ring frames"), captureRingSpin_);
    audioForm->addRow(QStringLiteral("Playback ring frames"), playbackRingSpin_);
    auto* audioBox = new QGroupBox(QStringLiteral("Local Audio"), content);
    audioBox->setLayout(audioForm);
    layout->addWidget(audioBox);

    auto* advancedForm = new QFormLayout();
    advancedForm->addRow(QStringLiteral("Wait ms"), waitMsSpin_);
    advancedForm->addRow(QStringLiteral("Stream ms"), streamMsSpin_);
    advancedForm->addRow(QStringLiteral("Stream linger ms"), streamLingerMsSpin_);
    advancedForm->addRow(QString(), statsCheck_);
    advancedForm->addRow(QStringLiteral("Stats warmup ms"), statsWarmupMsSpin_);
    advancedForm->addRow(QStringLiteral("Log stats folder"), logStatsEdit_);
    advancedForm->addRow(QStringLiteral("Socket send buffer"), socketSendBufferSpin_);
    advancedForm->addRow(QStringLiteral("Socket recv buffer"), socketRecvBufferSpin_);
    advancedForm->addRow(QString(), driftCorrectionCheck_);
    advancedForm->addRow(QStringLiteral("Drift smoothing"), driftSmoothingSpin_);
    advancedForm->addRow(QStringLiteral("Drift deadband ppm"), driftDeadbandSpin_);
    advancedForm->addRow(QStringLiteral("Drift max correction ppm"), driftMaxCorrectionSpin_);
    advancedForm->addRow(QString(), sampleTimePlayoutCheck_);
    advancedForm->addRow(QStringLiteral("Playout delay frames"), playoutDelaySpin_);
    advancedForm->addRow(QString(), adaptiveCushionCheck_);
    advancedForm->addRow(QStringLiteral("Adaptive target frames"), adaptiveTargetSpin_);
    advancedForm->addRow(QStringLiteral("Adaptive min frames"), adaptiveMinSpin_);
    advancedForm->addRow(QStringLiteral("Adaptive max frames"), adaptiveMaxSpin_);
    advancedForm->addRow(QStringLiteral("Adaptive release ppm"), adaptiveReleaseSpin_);
    advancedForm->addRow(QStringLiteral("Extra jam2 args"), extraArgsEdit_);
    auto* advancedBox = new QGroupBox(QStringLiteral("Engine Options"), content);
    advancedBox->setLayout(advancedForm);
    layout->addWidget(advancedBox);

    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* start = buttons->addButton(QStringLiteral("Start"), QDialogButtonBox::AcceptRole);
    auto* refresh = buttons->addButton(QStringLiteral("Refresh Devices"), QDialogButtonBox::ActionRole);
    auto* regen = buttons->addButton(QStringLiteral("New Session"), QDialogButtonBox::ActionRole);
    auto* copy = buttons->addButton(QStringLiteral("Copy URL"), QDialogButtonBox::ActionRole);
    QObject::connect(refresh, &QPushButton::clicked, this, [this] {
        refreshDevices();
        refreshLocalOutputs();
    });
    QObject::connect(regen, &QPushButton::clicked, this, [this] {
        generateSession();
        jam2::SessionInfo info;
        info.endpoint = {publicHostEdit_->text().toStdString(), static_cast<std::uint16_t>(portSpin_->value())};
        info.session_id = sessionId_;
        info.key = sessionKey_;
        generatedUrlEdit_->setText(QString::fromStdString(jam2::make_jam_url(info)));
    });
    QObject::connect(copy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(generatedUrlEdit_->text());
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(publicHostEdit_, &QLineEdit::textChanged, &dialog, updateGeneratedUrl);
    QObject::connect(portSpin_, &QSpinBox::valueChanged, &dialog, updateGeneratedUrl);

    auto* outer = new QVBoxLayout(&dialog);
    outer->addWidget(scroll, 1);
    outer->addWidget(buttons);
    start->setDefault(true);

    const int result = dialog.exec();
    const QList<QWidget*> startWidgets{
        jam2PathEdit_, bindHostEdit_, portSpin_, publicHostEdit_, generatedUrlEdit_,
        stunServerEdit_, stunTimeoutSpin_, stunRetriesSpin_, noStunCheck_, deviceBox_,
        localOutputBox_, inputChannelsEdit_, outputChannelsEdit_, sampleRateSpin_, bufferSizeSpin_, frameSizeSpin_,
        prefillSpin_, playbackMaxSpin_, captureRingSpin_, playbackRingSpin_, waitMsSpin_,
        streamMsSpin_, streamLingerMsSpin_, statsCheck_, statsWarmupMsSpin_, logStatsEdit_, socketSendBufferSpin_,
        socketRecvBufferSpin_, driftCorrectionCheck_, driftSmoothingSpin_, driftDeadbandSpin_,
        driftMaxCorrectionSpin_, sampleTimePlayoutCheck_, playoutDelaySpin_, adaptiveCushionCheck_,
        adaptiveTargetSpin_, adaptiveMinSpin_, adaptiveMaxSpin_, adaptiveReleaseSpin_, extraArgsEdit_,
    };
    for (QWidget* widget : startWidgets) {
        widget->setParent(this);
        widget->hide();
    }
    if (result == QDialog::Accepted) {
        modeBox_->setCurrentText(QStringLiteral("Listen"));
        connectionLabel_->setText(QStringLiteral("Starting listener"));
        startJam();
    }
}

void MainWindow::showJoinJamDialog()
{
    if (jam2_.isRunning()) {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Join Jam"));
    dialog.resize(680, 520);

    auto* content = new QWidget(&dialog);
    auto* layout = new QVBoxLayout(content);
    const QList<QWidget*> visibleWidgets{
        connectUrlEdit_, jam2PathEdit_, deviceBox_, localOutputBox_, inputChannelsEdit_, outputChannelsEdit_,
        statsCheck_, statsWarmupMsSpin_, logStatsEdit_,
    };
    for (QWidget* widget : visibleWidgets) {
        widget->show();
    }

    auto* sessionForm = new QFormLayout();
    sessionForm->addRow(QStringLiteral("jam2 URL"), connectUrlEdit_);
    sessionForm->addRow(QStringLiteral("jam2 binary"), jam2PathEdit_);
    auto* sessionBox = new QGroupBox(QStringLiteral("Connection"), content);
    sessionBox->setLayout(sessionForm);
    layout->addWidget(sessionBox);

    auto* audioForm = new QFormLayout();
    audioForm->addRow(QStringLiteral("Audio device"), deviceBox_);
    audioForm->addRow(QStringLiteral("Local track/metronome output"), localOutputBox_);
    audioForm->addRow(QStringLiteral("Input channels"), inputChannelsEdit_);
    audioForm->addRow(QStringLiteral("Output channels"), outputChannelsEdit_);
    auto* audioBox = new QGroupBox(QStringLiteral("Local Audio"), content);
    audioBox->setLayout(audioForm);
    layout->addWidget(audioBox);

    auto* statsForm = new QFormLayout();
    statsForm->addRow(QString(), statsCheck_);
    statsForm->addRow(QStringLiteral("Stats warmup ms"), statsWarmupMsSpin_);
    statsForm->addRow(QStringLiteral("Log stats folder"), logStatsEdit_);
    auto* statsBox = new QGroupBox(QStringLiteral("Local Stats"), content);
    statsBox->setLayout(statsForm);
    layout->addWidget(statsBox);

    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* join = buttons->addButton(QStringLiteral("Join"), QDialogButtonBox::AcceptRole);
    auto* refresh = buttons->addButton(QStringLiteral("Refresh Devices"), QDialogButtonBox::ActionRole);
    QObject::connect(refresh, &QPushButton::clicked, this, [this] {
        refreshDevices();
        refreshLocalOutputs();
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto* outer = new QVBoxLayout(&dialog);
    outer->addWidget(scroll, 1);
    outer->addWidget(buttons);
    join->setDefault(true);

    const int result = dialog.exec();
    const QList<QWidget*> joinWidgets{
        connectUrlEdit_, jam2PathEdit_, deviceBox_, localOutputBox_, inputChannelsEdit_, outputChannelsEdit_,
        statsCheck_, statsWarmupMsSpin_, logStatsEdit_,
    };
    for (QWidget* widget : joinWidgets) {
        widget->setParent(this);
        widget->hide();
    }
    if (result == QDialog::Accepted) {
        modeBox_->setCurrentText(QStringLiteral("Connect"));
        connectionLabel_->setText(QStringLiteral("Joining"));
        startJam();
    }
}

void MainWindow::stopJam()
{
    controlReconnectEnabled_ = false;
    controlReconnectTimer_.stop();
    controlServer_.close();
    controlClient_.close();
    pendingJoinLaunch_ = false;
    pendingJoinBaseArgs_.clear();
    jam2_.stop();
    localMetronomeRunning_ = false;
    if (trackMetronomeLabel_) {
        trackMetronomeLabel_->setText(QStringLiteral("Local metronome stopped"));
    }
    if (startTrackMetronomeButton_) {
        startTrackMetronomeButton_->setEnabled(true);
    }
    if (stopTrackMetronomeButton_) {
        stopTrackMetronomeButton_->setEnabled(false);
    }
    startButton_->setEnabled(true);
    joinButton_->setEnabled(true);
    stopButton_->setEnabled(false);
    if (refreshControlButton_) {
        refreshControlButton_->setEnabled(false);
    }
    if (runtimeMixBox_) {
        runtimeMixBox_->setVisible(false);
    }
    if (leadSwapButton_) {
        leadSwapButton_->setEnabled(false);
    }
}

void MainWindow::refreshDevices()
{
    QProcess process;
    const QFileInfo binary(jam2PathEdit_->text());
    if (binary.exists()) {
        process.setWorkingDirectory(binary.absolutePath());
    }
    process.start(jam2PathEdit_->text(), QStringList{QStringLiteral("list-devices")});
    if (!process.waitForStarted(3000)) {
        appendLog(QStringLiteral("device refresh failed to start: %1").arg(process.errorString()));
        return;
    }
    if (!process.waitForFinished(5000)) {
        appendLog(QStringLiteral("device refresh timed out"));
        process.kill();
        return;
    }
    deviceBox_->clear();
    const QString output = QString::fromUtf8(process.readAllStandardOutput());
    const QString error = QString::fromUtf8(process.readAllStandardError()).trimmed();
    for (const QString& line : output.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QLatin1Char('['))) {
            deviceBox_->addItem(trimmed, deviceId(trimmed));
        }
    }
    if (deviceBox_->count() == 0) {
        appendLog(error.isEmpty() ? QStringLiteral("no devices returned by jam2 list-devices") : error);
    } else {
        appendLog(QStringLiteral("loaded %1 audio devices").arg(deviceBox_->count()));
    }
}

void MainWindow::refreshLocalOutputs()
{
    if (!localOutputBox_) {
        return;
    }
    const QString previous = localOutputBox_->currentData().toString();
    localOutputBox_->clear();
    const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();
    for (const QAudioDevice& device : outputs) {
        localOutputBox_->addItem(device.description(), audioDeviceIdText(device));
    }
    if (localOutputBox_->count() == 0) {
        localOutputBox_->addItem(QStringLiteral("Default audio output"), QString{});
    }
    const int restore = localOutputBox_->findData(previous);
    if (restore >= 0) {
        localOutputBox_->setCurrentIndex(restore);
    }
}

void MainWindow::appendLog(const QString& line)
{
    if (logEdit_) {
        logEdit_->appendPlainText(line);
    }
}

void MainWindow::handleOutputLine(const QString& line)
{
    appendLog(line);
    handleStatsLine(line);
}

void MainWindow::handleStatus(const QJsonObject& status)
{
    if (status.value(QStringLiteral("event")).toString() != QStringLiteral("status")) {
        return;
    }
    updateStatsDisplay(status);
}

void MainWindow::handleStatsLine(const QString& line)
{
    if (!line.startsWith(QStringLiteral("stats "))) {
        return;
    }
    QJsonObject stats;
    stats.insert(QStringLiteral("event"), QStringLiteral("stats"));
    const QRegularExpression fieldRe(QStringLiteral("(\\S+)=([^\\s]+)"));
    auto it = fieldRe.globalMatch(line.mid(6));
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString key = match.captured(1);
        const QString value = match.captured(2);
        bool ok = false;
        const double number = value.toDouble(&ok);
        if (ok) {
            stats.insert(key, number);
        } else {
            stats.insert(key, value);
        }
    }
    if (stats.contains(QStringLiteral("sent")) && !stats.contains(QStringLiteral("sent_packets"))) {
        stats.insert(QStringLiteral("sent_packets"), stats.value(QStringLiteral("sent")).toDouble());
    }
    if (stats.contains(QStringLiteral("recv")) && !stats.contains(QStringLiteral("recv_packets"))) {
        stats.insert(QStringLiteral("recv_packets"), stats.value(QStringLiteral("recv")).toDouble());
    }
    updateStatsDisplay(stats);
}

void MainWindow::updateStatsDisplay(const QJsonObject& stats)
{
    QString latency = metricText(stats, QStringLiteral("estimated_one_way_ms"), QStringLiteral(" ms"));
    if (latency == QStringLiteral("-")) {
        latency = metricText(stats, QStringLiteral("rtt_avg_ms"), QStringLiteral(" ms"));
    }
    latencyLabel_->setText(QStringLiteral("Latency ") + latency);
    const QString jitterAvg = metricText(stats, QStringLiteral("jitter_avg_ms"), QStringLiteral(" ms"));
    const QString jitterMax = metricText(stats, QStringLiteral("jitter_max_ms"), QStringLiteral(" ms"));
    const QString packetGapMax = metricText(stats, QStringLiteral("audio_packet_gap_max_ms"), QStringLiteral(" ms"));
    jitterLabel_->setText(QStringLiteral("Jitter %1/%2 gap %3").arg(jitterAvg, jitterMax, packetGapMax));
    const QString lostPackets = integerText(stats, QStringLiteral("sequence_lost"));
    const QString lossEvents = integerText(stats, QStringLiteral("sequence_loss_events"));
    const QString lossMaxGap = integerText(stats, QStringLiteral("sequence_loss_max_gap"));
    lossLabel_->setText(QStringLiteral("Loss %1 lost %2 ev %3 max %4p")
        .arg(metricText(stats, QStringLiteral("sequence_loss_percent"), QStringLiteral("%"), 2))
        .arg(lostPackets)
        .arg(lossEvents)
        .arg(lossMaxGap));
    depthLabel_->setText(QStringLiteral("Depth ") + metricText(
        stats,
        QStringLiteral("playback_depth_avg_ms"),
        QStringLiteral(" ms"),
        1,
        QStringLiteral("playback_depth_ms")));
    QString outOfOrder = integerText(stats, QStringLiteral("sequence_out_of_order"));
    if (outOfOrder == QStringLiteral("-")) {
        outOfOrder = integerText(stats, QStringLiteral("out_of_order"));
    }
    QString latePackets = integerText(stats, QStringLiteral("sequence_late"));
    if (latePackets == QStringLiteral("-")) {
        latePackets = integerText(stats, QStringLiteral("late"));
    }
    ringDepthLabel_->setText(QStringLiteral("Reorder oo %1 rec %2 lost %3 late %4 max %5p")
        .arg(outOfOrder)
        .arg(integerText(stats, QStringLiteral("reordered_recovered")))
        .arg(integerText(stats, QStringLiteral("reordered_lost")))
        .arg(latePackets)
        .arg(integerText(stats, QStringLiteral("reordered_max_distance_packets"))));
    QString underrunText = QStringLiteral("-");
    if (hasNumber(stats, QStringLiteral("playback_ring_underrun_time_ms"))) {
        const double underrunMs = stats.value(QStringLiteral("playback_ring_underrun_time_ms")).toDouble();
        underrunText = durationText(underrunMs);
        if (hasNumber(stats, QStringLiteral("playback_ring_underrun_events"))) {
            underrunText += QStringLiteral(" ev %1").arg(integerText(stats, QStringLiteral("playback_ring_underrun_events")));
        }
        if (hasNumber(stats, QStringLiteral("playback_ring_underrun_event_max_ms"))) {
            underrunText += QStringLiteral(" max %1").arg(metricText(
                stats,
                QStringLiteral("playback_ring_underrun_event_max_ms"),
                QStringLiteral(" ms")));
        }
        if (hasNumber(stats, QStringLiteral("playback_ring_underrun_burst_max_ms"))) {
            underrunText += QStringLiteral(" burst %1").arg(metricText(
                stats,
                QStringLiteral("playback_ring_underrun_burst_max_ms"),
                QStringLiteral(" ms")));
        }
    } else if (hasNumber(stats, QStringLiteral("playback_ring_underruns"))) {
        underrunText = integerText(stats, QStringLiteral("playback_ring_underruns")) + QStringLiteral(" fr");
    }
    underrunLabel_->setText(QStringLiteral("Underrun ") + underrunText);
    missingFramesLabel_->setText(QStringLiteral("Stall loop %1 burst %2p gap>2x %3 missing %4 latefr %5")
        .arg(metricText(stats, QStringLiteral("receive_loop_gap_max_ms"), QStringLiteral(" ms")))
        .arg(integerText(stats, QStringLiteral("receive_burst_packets_max")))
        .arg(integerText(stats, QStringLiteral("audio_packet_gap_over_2x_count")))
        .arg(integerText(stats, QStringLiteral("missing_audio_frames_inserted")))
        .arg(integerText(stats, QStringLiteral("late_audio_frames_dropped"))));
    driftLabel_->setText(QStringLiteral("Drift ") + metricText(stats, QStringLiteral("drift_ppm"), QStringLiteral(" ppm")));
    diagnosisLabel_->setText(diagnoseStats(stats));
}

void MainWindow::handleControlMessage(const QJsonObject& message)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    const bool trackMessage =
        type == QStringLiteral("track.offer") ||
        type == QStringLiteral("track.processing") ||
        type == QStringLiteral("track.play") ||
        type == QStringLiteral("track.stop") ||
        type == QStringLiteral("track.file.start") ||
        type == QStringLiteral("track.file.chunk") ||
        type == QStringLiteral("track.file.done");
    if (trackMessage && !trackController_.model().syncControls) {
        appendLog(QStringLiteral("ignored remote track sync while local sync is disabled"));
        return;
    }
    if (type == QStringLiteral("session.settings")) {
        applyLeaderSettings(message.value(QStringLiteral("settings")).toObject());
        launchPendingJoin();
    } else if (type == QStringLiteral("session.error")) {
        const QString text = message.value(QStringLiteral("message")).toString(QStringLiteral("Session error"));
        appendLog(QStringLiteral("peer session error: ") + text);
        QMessageBox::warning(this, QStringLiteral("Jam2"), text);
    } else if (type == QStringLiteral("metronome.settings")) {
        applyRemoteMetronomeSettings(message);
    } else if (type == QStringLiteral("beat.set")) {
        const QString lane = message.value(QStringLiteral("lane")).toString();
        BeatGridModel* model = &beatModel_;
        if (lane == QStringLiteral("chord")) {
            model = &chordModel_;
        } else if (lane == QStringLiteral("lyric")) {
            model = &lyricModel_;
        }
        model->setCell(
            message.value(QStringLiteral("section")).toInt(),
            lane,
            message.value(QStringLiteral("beat")).toInt(),
            message.value(QStringLiteral("text")).toString());
        refreshSongView(lane);
    } else if (type == QStringLiteral("grid.resize")) {
        const QString lane = message.value(QStringLiteral("lane")).toString(QStringLiteral("beat"));
        BeatGridModel* model = lane == QStringLiteral("chord") ? &chordModel_ : &beatModel_;
        model->resizeSection(
            message.value(QStringLiteral("section")).toInt(),
            message.value(QStringLiteral("beats")).toInt(8));
        refreshSongView(lane);
    } else if (type == QStringLiteral("beat.hit")) {
        beatModel_.setBeatHit(
            message.value(QStringLiteral("section")).toInt(),
            message.value(QStringLiteral("beat")).toInt(),
            message.value(QStringLiteral("lane")).toInt(),
            message.value(QStringLiteral("text")).toString());
        refreshSongView(QStringLiteral("beat"));
    } else if (type == QStringLiteral("beat.division")) {
        beatModel_.setBeatDivision(
            message.value(QStringLiteral("section")).toInt(),
            message.value(QStringLiteral("beat")).toInt(),
            message.value(QStringLiteral("division")).toInt(4));
        refreshSongView(QStringLiteral("beat"));
    } else if (type == QStringLiteral("lyrics.set")) {
        lyricModel_.setLyricsText(message.value(QStringLiteral("text")).toString());
        refreshSongView(QStringLiteral("lyric"));
    } else if (type == QStringLiteral("song.set")) {
        if (loadSongJson(message.value(QStringLiteral("song")).toObject())) {
            refreshSongViews();
        }
    } else if (type == QStringLiteral("lead.change_pending")) {
        leadCue_.pendingLead = message.value(QStringLiteral("target")).toString();
        leadCue_.pendingBars = message.value(QStringLiteral("bars")).toInt(4);
        updateLeadLabels();
    } else if (type == QStringLiteral("track.offer")) {
        trackController_.model().fileName = message.value(QStringLiteral("name")).toString();
        const QString offeredPath = message.value(QStringLiteral("path")).toString(trackController_.model().filePath);
        const bool offeredPathExists = QFileInfo::exists(offeredPath);
        if (offeredPathExists) {
            trackController_.model().filePath = offeredPath;
        }
        trackController_.model().fileBytes = static_cast<qint64>(message.value(QStringLiteral("file_bytes")).toDouble(trackController_.model().fileBytes));
        trackController_.model().sampleRate = message.value(QStringLiteral("sample_rate")).toInt(trackController_.model().sampleRate);
        trackController_.model().durationMs = message.value(QStringLiteral("duration_ms")).toInt(trackController_.model().durationMs);
        trackController_.model().sha256 = message.value(QStringLiteral("sha256")).toString(trackController_.model().sha256);
        trackController_.model().acceptedBpm = message.value(QStringLiteral("accepted_bpm")).toDouble(120.0);
        trackController_.model().key = message.value(QStringLiteral("key")).toString(QStringLiteral("Unknown"));
        trackController_.model().trackGainDb = message.value(QStringLiteral("track_gain_db")).toDouble(trackController_.model().trackGainDb);
        trackController_.model().loopEnabled = message.value(QStringLiteral("loop_enabled")).toBool(trackController_.model().loopEnabled);
        trackController_.model().loopStartSeconds = message.value(QStringLiteral("loop_start_seconds")).toDouble(trackController_.model().loopStartSeconds);
        trackController_.model().loopEndSeconds = message.value(QStringLiteral("loop_end_seconds")).toDouble(trackController_.model().loopEndSeconds);
        trackController_.model().focusEnabled = message.value(QStringLiteral("focus_enabled")).toBool(trackController_.model().focusEnabled);
        trackController_.model().focusPreset = message.value(QStringLiteral("focus_preset")).toString(trackController_.model().focusPreset);
        trackController_.model().focusFrequencyHz = message.value(QStringLiteral("focus_frequency_hz")).toDouble(trackController_.model().focusFrequencyHz);
        trackController_.model().focusGainDb = message.value(QStringLiteral("focus_gain_db")).toDouble(trackController_.model().focusGainDb);
        trackController_.model().focusQ = message.value(QStringLiteral("focus_q")).toDouble(trackController_.model().focusQ);
        updateTrackControls();
        if (offeredPathExists) {
            loadTrackIntoPlayer();
        }
    } else if (type == QStringLiteral("track.processing")) {
        appendLog(QStringLiteral("ignored remote track processing controls"));
    } else if (type == QStringLiteral("track.play")) {
        loadTrackIntoPlayer();
        if (trackDevice_) {
            trackDevice_->setPositionMs(message.value(QStringLiteral("position_ms")).toInteger(0));
        }
        const bool syncControls = trackController_.model().syncControls;
        trackController_.model().syncControls = false;
        playTrack();
        trackController_.model().syncControls = syncControls;
    } else if (type == QStringLiteral("track.stop")) {
        const bool syncControls = trackController_.model().syncControls;
        trackController_.model().syncControls = false;
        stopTrack();
        trackController_.model().syncControls = syncControls;
    } else if (type == QStringLiteral("track.file.start")) {
        receiveTrackFileStart(message);
    } else if (type == QStringLiteral("track.file.chunk")) {
        receiveTrackFileChunk(message);
    } else if (type == QStringLiteral("track.file.done")) {
        receiveTrackFileDone(message);
    }
}

void MainWindow::handleControlState(const QString& state, bool serverSide)
{
    connectionLabel_->setText(state);
    appendLog(QStringLiteral("control: ") + state);
    if (serverSide && state == QStringLiteral("TCP peer authenticated")) {
        controlReconnectAttempts_ = 0;
        controlReconnectTimer_.stop();
        sendLeaderSettings();
        sendMetronomeSettingsToPeer();
    } else if (!serverSide && state == QStringLiteral("TCP control authenticated")) {
        controlReconnectAttempts_ = 0;
        controlReconnectTimer_.stop();
    } else if ((jam2_.isRunning() || pendingJoinLaunch_) && controlReconnectEnabled_ &&
        (state == QStringLiteral("TCP control disconnected") || state == QStringLiteral("TCP peer disconnected"))) {
        scheduleControlReconnect();
    }
}

void MainWindow::refreshControlConnection()
{
    if (!controlReconnectEnabled_ || controlHost_.isEmpty() || controlPort_ == 0) {
        return;
    }
    if (controlServerMode_ && controlServer_.hasPeer()) {
        sendLeaderSettings();
        appendLog(QStringLiteral("TCP control already connected; resent leader settings"));
        return;
    }
    if (!controlServerMode_ && controlClient_.isConnected()) {
        appendLog(QStringLiteral("TCP control already connected"));
        return;
    }
    ++controlReconnectAttempts_;
    appendLog(QStringLiteral("refreshing TCP control attempt %1").arg(controlReconnectAttempts_));
    if (controlServerMode_) {
        if (!controlServer_.listen(controlPort_, controlSessionHex_, controlKeyHex_)) {
            appendLog(QStringLiteral("control server refresh failed: ") + controlServer_.errorString());
        }
    } else {
        controlClient_.connectToHost(controlHost_, controlPort_, controlSessionHex_, controlKeyHex_);
    }
    if (controlReconnectAttempts_ >= 15) {
        controlReconnectTimer_.stop();
        appendLog(QStringLiteral("TCP control auto reconnect paused; use Refresh Control to retry"));
    }
}

void MainWindow::scheduleControlReconnect()
{
    if (!controlReconnectEnabled_ || controlReconnectTimer_.isActive()) {
        return;
    }
    controlReconnectAttempts_ = 0;
    controlReconnectTimer_.start();
}

void MainWindow::sendLeaderSettings()
{
    if (!controlServer_.hasPeer()) {
        return;
    }
    controlServer_.send(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("session.settings")},
        {QStringLiteral("settings"), leaderSettingsMessage()},
    });
}

QJsonObject MainWindow::leaderSettingsMessage() const
{
    return QJsonObject{
        {QStringLiteral("sample_rate"), sampleRateSpin_->value()},
        {QStringLiteral("audio_buffer_size"), bufferSizeSpin_->value()},
        {QStringLiteral("frame_size"), frameSizeSpin_->value()},
        {QStringLiteral("playback_prefill_frames"), prefillSpin_->value()},
        {QStringLiteral("playback_max_frames"), playbackMaxSpin_->value()},
        {QStringLiteral("capture_ring_frames"), captureRingSpin_->value()},
        {QStringLiteral("playback_ring_frames"), playbackRingSpin_->value()},
        {QStringLiteral("stats_warmup_ms"), statsWarmupMsSpin_->value()},
        {QStringLiteral("socket_send_buffer"), socketSendBufferSpin_->value()},
        {QStringLiteral("socket_recv_buffer"), socketRecvBufferSpin_->value()},
        {QStringLiteral("drift_correction"), driftCorrectionCheck_->isChecked()},
        {QStringLiteral("drift_smoothing"), driftSmoothingSpin_->value()},
        {QStringLiteral("drift_deadband_ppm"), driftDeadbandSpin_->value()},
        {QStringLiteral("drift_max_correction_ppm"), driftMaxCorrectionSpin_->value()},
        {QStringLiteral("bpm"), metronomeBpmSpin_ ? metronomeBpmSpin_->value() : bpmSpin_->value()},
        {QStringLiteral("remote_level"), static_cast<double>(remoteLevelSlider_->value()) / 100.0},
        {QStringLiteral("metronome_mode"), metronomeModeBox_->currentText()},
        {QStringLiteral("sample_time_playout"), sampleTimePlayoutCheck_->isChecked()},
        {QStringLiteral("playout_delay_frames"), playoutDelaySpin_->value()},
        {QStringLiteral("adaptive_playback_cushion"), adaptiveCushionCheck_->isChecked()},
        {QStringLiteral("adaptive_target_frames"), adaptiveTargetSpin_->value()},
        {QStringLiteral("adaptive_min_frames"), adaptiveMinSpin_->value()},
        {QStringLiteral("adaptive_max_frames"), adaptiveMaxSpin_->value()},
        {QStringLiteral("adaptive_release_ppm"), adaptiveReleaseSpin_->value()},
    };
}

void MainWindow::applyLeaderSettings(const QJsonObject& settings)
{
    sampleRateSpin_->setValue(settings.value(QStringLiteral("sample_rate")).toInt(sampleRateSpin_->value()));
    bufferSizeSpin_->setValue(settings.value(QStringLiteral("audio_buffer_size")).toInt(bufferSizeSpin_->value()));
    frameSizeSpin_->setValue(settings.value(QStringLiteral("frame_size")).toInt(frameSizeSpin_->value()));
    prefillSpin_->setValue(settings.value(QStringLiteral("playback_prefill_frames")).toInt(prefillSpin_->value()));
    playbackMaxSpin_->setValue(settings.value(QStringLiteral("playback_max_frames")).toInt(playbackMaxSpin_->value()));
    captureRingSpin_->setValue(settings.value(QStringLiteral("capture_ring_frames")).toInt(captureRingSpin_->value()));
    playbackRingSpin_->setValue(settings.value(QStringLiteral("playback_ring_frames")).toInt(playbackRingSpin_->value()));
    statsWarmupMsSpin_->setValue(settings.value(QStringLiteral("stats_warmup_ms")).toInt(statsWarmupMsSpin_->value()));
    socketSendBufferSpin_->setValue(settings.value(QStringLiteral("socket_send_buffer")).toInt(socketSendBufferSpin_->value()));
    socketRecvBufferSpin_->setValue(settings.value(QStringLiteral("socket_recv_buffer")).toInt(socketRecvBufferSpin_->value()));
    driftCorrectionCheck_->setChecked(settings.value(QStringLiteral("drift_correction")).toBool(driftCorrectionCheck_->isChecked()));
    driftSmoothingSpin_->setValue(settings.value(QStringLiteral("drift_smoothing")).toDouble(driftSmoothingSpin_->value()));
    driftDeadbandSpin_->setValue(settings.value(QStringLiteral("drift_deadband_ppm")).toInt(driftDeadbandSpin_->value()));
    driftMaxCorrectionSpin_->setValue(settings.value(QStringLiteral("drift_max_correction_ppm")).toInt(driftMaxCorrectionSpin_->value()));
    if (metronomeBpmSpin_) {
        metronomeBpmSpin_->setValue(settings.value(QStringLiteral("bpm")).toInt(metronomeBpmSpin_->value()));
    }
    remoteLevelSlider_->setValue(qBound(0, qRound(settings.value(QStringLiteral("remote_level")).toDouble(
        static_cast<double>(remoteLevelSlider_->value()) / 100.0) * 100.0), 100));
    const QString mode = settings.value(QStringLiteral("metronome_mode")).toString(metronomeModeBox_->currentText());
    const int modeIndex = metronomeModeBox_->findText(mode);
    if (modeIndex >= 0) {
        metronomeModeBox_->setCurrentIndex(modeIndex);
    }
    sampleTimePlayoutCheck_->setChecked(settings.value(QStringLiteral("sample_time_playout")).toBool(sampleTimePlayoutCheck_->isChecked()));
    playoutDelaySpin_->setValue(settings.value(QStringLiteral("playout_delay_frames")).toInt(playoutDelaySpin_->value()));
    adaptiveCushionCheck_->setChecked(settings.value(QStringLiteral("adaptive_playback_cushion")).toBool(adaptiveCushionCheck_->isChecked()));
    adaptiveTargetSpin_->setValue(settings.value(QStringLiteral("adaptive_target_frames")).toInt(adaptiveTargetSpin_->value()));
    adaptiveMinSpin_->setValue(settings.value(QStringLiteral("adaptive_min_frames")).toInt(adaptiveMinSpin_->value()));
    adaptiveMaxSpin_->setValue(settings.value(QStringLiteral("adaptive_max_frames")).toInt(adaptiveMaxSpin_->value()));
    adaptiveReleaseSpin_->setValue(settings.value(QStringLiteral("adaptive_release_ppm")).toInt(adaptiveReleaseSpin_->value()));
}

bool MainWindow::selectedDeviceSupportsSampleRate(int sampleRate)
{
    if (selectedDeviceId().isEmpty()) {
        return false;
    }
    QProcess process;
    const QFileInfo binary(jam2PathEdit_->text());
    if (binary.exists()) {
        process.setWorkingDirectory(binary.absolutePath());
    }
    process.start(jam2PathEdit_->text(), QStringList{
        QStringLiteral("test-device"),
        selectedDeviceId(),
        QStringLiteral("--sample-rate"),
        QString::number(sampleRate),
    });
    if (!process.waitForStarted(3000) || !process.waitForFinished(5000)) {
        process.kill();
        return false;
    }
    const QString output = QString::fromUtf8(process.readAllStandardOutput());
    if (!output.contains(QStringLiteral("Requested sample rate %1: supported").arg(sampleRate))) {
        appendLog(QStringLiteral("device sample-rate preflight failed: ") + output.trimmed());
        return false;
    }
    return true;
}

void MainWindow::launchPendingJoin()
{
    if (!pendingJoinLaunch_ || jam2_.isRunning()) {
        return;
    }
    if (!selectedDeviceSupportsSampleRate(sampleRateSpin_->value())) {
        const QString message = QStringLiteral("Selected joiner audio device does not support leader sample rate %1").arg(sampleRateSpin_->value());
        appendLog(message);
        sendControl(QJsonObject{{QStringLiteral("type"), QStringLiteral("session.error")}, {QStringLiteral("message"), message}});
        QMessageBox::warning(this, QStringLiteral("Jam2"), message);
        pendingJoinLaunch_ = false;
        return;
    }
    QStringList args = pendingJoinBaseArgs_;
    args << commonJamArgs(false);
    pendingJoinLaunch_ = false;
    pendingJoinBaseArgs_.clear();
    launchJamProcess(args);
}

void MainWindow::sendControl(const QJsonObject& message)
{
    controlServer_.send(message);
    controlClient_.send(message);
}

void MainWindow::updateRuntimeControls()
{
    if (!jam2_.isRunning()) {
        return;
    }
    jam2_.sendLine(QStringLiteral("metro level %1").arg(static_cast<double>(metronomeLevelSlider_->value()) / 100.0, 0, 'f', 2));
    jam2_.sendLine(QStringLiteral("metro mode %1").arg(metronomeModeBox_->currentText()));
    sendMetronomePatternToJam();
    jam2_.sendLine(QStringLiteral("remote level %1").arg(static_cast<double>(remoteLevelSlider_->value()) / 100.0, 0, 'f', 2));
}

void MainWindow::requestLeadSwap()
{
    const QString target = leadCue_.currentLead == QStringLiteral("Teacher") ? QStringLiteral("Student") : QStringLiteral("Teacher");
    leadCue_.pendingLead = target;
    leadCue_.pendingBars = 4;
    updateLeadLabels();
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("lead.change_pending")},
        {QStringLiteral("target"), target},
        {QStringLiteral("bars"), leadCue_.pendingBars},
    });
    QTimer::singleShot(4000, this, [this, target] {
        leadCue_.currentLead = target;
        leadCue_.pendingLead.clear();
        leadCue_.pendingBars = 0;
        updateLeadLabels();
    });
}

void MainWindow::updateLeadLabels()
{
    if (!leadLabel_) {
        return;
    }
    leadLabel_->setText(QStringLiteral("Lead: %1").arg(leadCue_.currentLead));
    leadPendingLabel_->setText(
        leadCue_.pendingLead.isEmpty()
            ? QStringLiteral("No pending lead change")
            : QStringLiteral("%1 leads in %2 bars").arg(leadCue_.pendingLead).arg(leadCue_.pendingBars));
}

void MainWindow::updateTrackControls()
{
    const auto& model = trackController_.model();
    const QString duration = model.durationMs > 0
        ? QStringLiteral(" | %1.%2 s").arg(model.durationMs / 1000).arg((model.durationMs % 1000) / 100)
        : QString();
    const QString rate = model.sampleRate > 0 ? QStringLiteral(" | %1 Hz").arg(model.sampleRate) : QString();
    const QString bytes = model.fileBytes > 0 ? QStringLiteral(" | %1 bytes").arg(model.fileBytes) : QString();
    trackNameLabel_->setText(QStringLiteral("Track: %1 | BPM %2 | Key %3%4%5%6")
        .arg(model.fileName)
        .arg(model.acceptedBpm, 0, 'f', 1)
        .arg(model.key)
        .arg(duration)
        .arg(rate)
        .arg(bytes));
    if (trackSpeedSpin_) {
        const QSignalBlocker blocker(trackSpeedSpin_);
        trackSpeedSpin_->setValue(model.speed);
    }
    if (trackSpeedSlider_) {
        const QSignalBlocker blocker(trackSpeedSlider_);
        trackSpeedSlider_->setValue(qBound(10, qRound(model.speed * 100.0), 200));
    }
    if (trackPitchSpin_) {
        const QSignalBlocker blocker(trackPitchSpin_);
        trackPitchSpin_->setValue(qBound(-12, model.pitchCents / 100, 12));
    }
    if (trackPitchSlider_) {
        const QSignalBlocker blocker(trackPitchSlider_);
        trackPitchSlider_->setValue(qBound(-12, model.pitchCents / 100, 12));
    }
    if (trackLevelDbLabel_ && trackLevelSlider_) {
        trackLevelDbLabel_->setText(dbText(trackController_.model().trackGainDb));
        const QSignalBlocker blocker(trackLevelSlider_);
        trackLevelSlider_->setValue(qBound(-60, qRound(trackController_.model().trackGainDb), 12));
    }
    if (focusFrequencySlider_) {
        const QSignalBlocker blocker(focusFrequencySlider_);
        focusFrequencySlider_->setValue(qBound(40, qRound(model.focusFrequencyHz), 8000));
    }
    if (focusFrequencySpin_) {
        const QSignalBlocker blocker(focusFrequencySpin_);
        focusFrequencySpin_->setValue(qBound(40, qRound(model.focusFrequencyHz), 8000));
    }
    if (focusFrequencyCheck_) {
        const QSignalBlocker blocker(focusFrequencyCheck_);
        focusFrequencyCheck_->setChecked(model.focusEnabled);
    }
    if (waveformGridCheck_) {
        const QSignalBlocker blocker(waveformGridCheck_);
        waveformGridCheck_->setChecked(model.waveformGridVisible);
    }
    const bool knownFocusPreset = !focusPresetBox_ || focusPresetBox_->findData(model.focusPreset) >= 0;
    const bool customFocus = isCustomFocusPreset(model.focusPreset) || !knownFocusPreset;
    if (focusPresetBox_) {
        const QSignalBlocker blocker(focusPresetBox_);
        const int index = focusPresetBox_->findData(customFocus ? QStringLiteral("custom") : model.focusPreset);
        focusPresetBox_->setCurrentIndex(index >= 0 ? index : 0);
    }
    if (focusFrequencySlider_) {
        focusFrequencySlider_->setEnabled(customFocus);
    }
    if (focusFrequencySpin_) {
        focusFrequencySpin_->setEnabled(customFocus);
    }
    if (trackWaveform_) {
        trackWaveform_->setGridVisible(model.waveformGridVisible);
        trackWaveform_->setBpm(model.acceptedBpm);
        trackWaveform_->setLoop(
            model.loopStartSeconds >= 0.0 ? static_cast<qint64>(std::llround(model.loopStartSeconds * 1000.0)) : -1,
            model.loopEndSeconds >= 0.0 ? static_cast<qint64>(std::llround(model.loopEndSeconds * 1000.0)) : -1);
    }
    if (loopEnabledCheck_) {
        const QSignalBlocker blocker(loopEnabledCheck_);
        loopEnabledCheck_->setChecked(model.loopEnabled);
        if (model.loopStartSeconds >= 0.0 && model.loopEndSeconds > model.loopStartSeconds) {
            loopEnabledCheck_->setText(QStringLiteral("Loop: %1.%2 s to %3.%4 s")
                .arg(static_cast<int>(model.loopStartSeconds))
                .arg(static_cast<int>(std::llround(model.loopStartSeconds * 10.0)) % 10)
                .arg(static_cast<int>(model.loopEndSeconds))
                .arg(static_cast<int>(std::llround(model.loopEndSeconds * 10.0)) % 10));
        } else if (model.loopStartSeconds >= 0.0) {
            loopEnabledCheck_->setText(QStringLiteral("Loop from %1.%2 s")
                .arg(static_cast<int>(model.loopStartSeconds))
                .arg(static_cast<int>(std::llround(model.loopStartSeconds * 10.0)) % 10));
        } else if (model.loopEndSeconds >= 0.0) {
            loopEnabledCheck_->setText(QStringLiteral("Loop to %1.%2 s")
                .arg(static_cast<int>(model.loopEndSeconds))
                .arg(static_cast<int>(std::llround(model.loopEndSeconds * 10.0)) % 10));
        } else {
            loopEnabledCheck_->setText(QStringLiteral("Loop whole track"));
        }
    }
}

void MainWindow::loadTrackMetadata()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Load WAV"), QString(), QStringLiteral("WAV files (*.wav);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QFileInfo info(path);
    try {
        const WavMetadata metadata = readWavMetadata(path);
        const QJsonObject sidecar = readSidecarJson(path);
        trackController_.model().fileName = info.fileName();
        trackController_.model().filePath = info.absoluteFilePath();
        trackController_.model().fileBytes = info.size();
        trackController_.model().sampleRate = sidecar.value(QStringLiteral("sample_rate")).toInt(metadata.sampleRate);
        trackController_.model().durationMs = sidecar.value(QStringLiteral("duration_ms")).toInt(metadata.durationMs);
        trackController_.model().sha256 = metadata.sha256;
        trackController_.model().guessedBpm = 0.0;
        trackController_.model().acceptedBpm = sidecar.value(QStringLiteral("accepted_bpm")).toDouble(120.0);
        trackController_.model().key = QStringLiteral("Unknown");
        trackController_.model().loopEnabled = false;
        trackController_.model().loopStartSeconds = -1.0;
        trackController_.model().loopEndSeconds = -1.0;
    } catch (const std::exception& error) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Track"), QString::fromUtf8(error.what()));
        return;
    }
    updateTrackControls();
    loadTrackIntoPlayer();
}

QStringList MainWindow::captureOptionArgs() const
{
    return QStringList{
        QStringLiteral("--trigger"), onOff(captureTriggerCheck_ && captureTriggerCheck_->isChecked()),
        QStringLiteral("--trigger-threshold-db"), QString::number(triggerThresholdSpin_ ? triggerThresholdSpin_->value() : -45.0, 'f', 1),
        QStringLiteral("--trigger-hold-ms"), QString::number(triggerHoldSpin_ ? triggerHoldSpin_->value() : 50),
        QStringLiteral("--pre-roll-ms"), QString::number(preRollSpin_ ? preRollSpin_->value() : 250),
        QStringLiteral("--tail-silence-db"), QString::number(tailThresholdSpin_ ? tailThresholdSpin_->value() : -50.0, 'f', 1),
        QStringLiteral("--tail-silence-ms"), QString::number(tailSilenceSpin_ ? tailSilenceSpin_->value() : 1000),
        QStringLiteral("--trim-leading-silence"), onOff(trimLeadingCheck_ && trimLeadingCheck_->isChecked()),
        QStringLiteral("--trim-trailing-silence"), onOff(trimTrailingCheck_ && trimTrailingCheck_->isChecked()),
        QStringLiteral("--summary-json"), QStringLiteral("on"),
    };
}

void MainWindow::handleCaptureOutputLine(const QString& line)
{
    appendLog(QStringLiteral("capture: ") + line);
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return;
    }
    const QJsonObject object = doc.object();
    if (object.value(QStringLiteral("event")).toString() != QStringLiteral("capture.summary")) {
        return;
    }
    lastCaptureSummary_ = object;
    const QString output = object.value(QStringLiteral("output")).toString();
    if (!output.isEmpty()) {
        lastCapturePath_ = output;
        captureOutputEdit_->setText(output);
    }
    if (importCaptureButton_) {
        importCaptureButton_->setEnabled(QFileInfo::exists(lastCapturePath_));
    }
}

void MainWindow::chooseCaptureFolder()
{
    const QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Capture Output"),
        captureOutputEdit_->text(),
        QStringLiteral("WAV files (*.wav);;All files (*)"));
    if (!path.isEmpty()) {
        captureOutputEdit_->setText(path);
    }
}

void MainWindow::refreshLoopbackSources()
{
    QProcess process;
    const QFileInfo binary(capturePathEdit_->text());
    if (binary.exists()) {
        process.setWorkingDirectory(binary.absolutePath());
    }
    process.start(capturePathEdit_->text(), QStringList{QStringLiteral("list-loopback-sources")});
    if (!process.waitForStarted(3000)) {
        appendLog(QStringLiteral("loopback source refresh failed to start: %1").arg(process.errorString()));
        return;
    }
    if (!process.waitForFinished(5000)) {
        appendLog(QStringLiteral("loopback source refresh timed out"));
        process.kill();
        return;
    }

    const QString previous = loopbackSourceBox_->currentData().toString().isEmpty()
        ? loopbackSourceBox_->currentText()
        : loopbackSourceBox_->currentData().toString();
    loopbackSourceBox_->clear();
    const QString output = QString::fromUtf8(process.readAllStandardOutput());
    const QRegularExpression re(QStringLiteral("^\\s*\\[([^\\]]+)\\]\\s*(.*)$"));
    for (const QString& line : output.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        const QRegularExpressionMatch match = re.match(trimmed);
        if (match.hasMatch()) {
            loopbackSourceBox_->addItem(trimmed, match.captured(1));
        }
    }
    if (loopbackSourceBox_->count() == 0) {
        loopbackSourceBox_->addItem(QStringLiteral("[default] System mix"), QStringLiteral("default"));
        const QString error = QString::fromUtf8(process.readAllStandardError()).trimmed();
        appendLog(error.isEmpty() ? QStringLiteral("no loopback sources returned") : error);
    } else {
        appendLog(QStringLiteral("loaded %1 loopback sources").arg(loopbackSourceBox_->count()));
    }
    const int restore = loopbackSourceBox_->findData(previous);
    if (restore >= 0) {
        loopbackSourceBox_->setCurrentIndex(restore);
    }
}

void MainWindow::showInputCaptureDialog()
{
    if (captureProcess_.state() != QProcess::NotRunning) {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Record Input"));
    dialog.resize(780, 560);

    auto* content = new QWidget(&dialog);
    content->setMinimumWidth(720);
    auto* form = new QFormLayout(content);
    const QList<QWidget*> visibleWidgets{
        capturePathEdit_, captureOutputEdit_, deviceBox_, inputChannelsEdit_, sampleRateSpin_,
        bufferSizeSpin_, captureManualStopCheck_, captureDurationSpin_, captureTriggerCheck_, triggerThresholdSpin_,
        triggerHoldSpin_, preRollSpin_, tailThresholdSpin_, tailSilenceSpin_, trimLeadingCheck_,
        trimTrailingCheck_,
    };
    for (QWidget* widget : visibleWidgets) {
        widget->show();
    }
    updateCaptureDurationControl(captureManualStopCheck_, captureDurationSpin_);
    deviceBox_->setMinimumWidth(420);
    inputChannelsEdit_->setMinimumWidth(220);
    sampleRateSpin_->setMinimumWidth(120);
    bufferSizeSpin_->setMinimumWidth(120);
    form->addRow(QStringLiteral("jam2-capture"), capturePathEdit_);
    auto* outputLayout = new QHBoxLayout();
    outputLayout->addWidget(captureOutputEdit_, 1);
    auto* browse = new QPushButton(QStringLiteral("Browse"), content);
    outputLayout->addWidget(browse);
    form->addRow(QStringLiteral("Capture output"), outputLayout);
    form->addRow(QStringLiteral("Audio device"), deviceBox_);
    form->addRow(QStringLiteral("Input channels"), inputChannelsEdit_);
    form->addRow(QStringLiteral("Sample rate"), sampleRateSpin_);
    form->addRow(QStringLiteral("Buffer size"), bufferSizeSpin_);
    form->addRow(captureManualStopCheck_);
    auto* durationLabel = new QLabel(QStringLiteral("Duration limit"), content);
    form->addRow(durationLabel, captureDurationSpin_);
    QObject::connect(captureManualStopCheck_, &QCheckBox::toggled, &dialog, [this, durationLabel] {
        updateCaptureDurationControl(captureManualStopCheck_, captureDurationSpin_, durationLabel);
    });
    updateCaptureDurationControl(captureManualStopCheck_, captureDurationSpin_, durationLabel);
    form->addRow(captureTriggerCheck_);
    form->addRow(QStringLiteral("Trigger threshold"), triggerThresholdSpin_);
    form->addRow(QStringLiteral("Trigger hold"), triggerHoldSpin_);
    form->addRow(QStringLiteral("Pre-roll"), preRollSpin_);
    form->addRow(QStringLiteral("Tail threshold"), tailThresholdSpin_);
    form->addRow(QStringLiteral("Tail silence"), tailSilenceSpin_);
    form->addRow(trimLeadingCheck_);
    form->addRow(trimTrailingCheck_);

    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* record = buttons->addButton(QStringLiteral("Record"), QDialogButtonBox::AcceptRole);
    auto* refresh = buttons->addButton(QStringLiteral("Refresh Devices"), QDialogButtonBox::ActionRole);
    QObject::connect(browse, &QPushButton::clicked, this, [this] { chooseCaptureFolder(); });
    QObject::connect(refresh, &QPushButton::clicked, this, [this] { refreshDevices(); });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(scroll, 1);
    layout->addWidget(buttons);
    record->setDefault(true);

    const int result = dialog.exec();
    const QList<QWidget*> widgets{
        capturePathEdit_, captureOutputEdit_, deviceBox_, inputChannelsEdit_, sampleRateSpin_,
        bufferSizeSpin_, captureManualStopCheck_, captureDurationSpin_, captureTriggerCheck_, triggerThresholdSpin_,
        triggerHoldSpin_, preRollSpin_, tailThresholdSpin_, tailSilenceSpin_, trimLeadingCheck_,
        trimTrailingCheck_,
    };
    for (QWidget* widget : widgets) {
        widget->setParent(this);
        widget->hide();
    }
    if (result == QDialog::Accepted) {
        startInputCapture();
    }
}

void MainWindow::startInputCapture()
{
    if (captureProcess_.state() != QProcess::NotRunning) {
        return;
    }
    if (selectedDeviceId().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Capture"), QStringLiteral("Select an audio device first."));
        return;
    }
    QDir captureDir(QDir::current());
    captureDir.mkpath(QStringLiteral("captures"));
    QString output = captureOutputEdit_->text().trimmed();
    if (output.isEmpty() || output.endsWith(QStringLiteral("capture.wav"))) {
        output = captureDir.absoluteFilePath(
            QStringLiteral("captures/capture-%1.wav").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"))));
        captureOutputEdit_->setText(output);
    }
    lastCapturePath_ = output;
    lastCaptureSummary_ = QJsonObject{};
    QStringList args{
        QStringLiteral("record-input"),
        QStringLiteral("--audio-device"), selectedDeviceId(),
        QStringLiteral("--input-channels"), inputChannelsEdit_->text(),
        QStringLiteral("--sample-rate"), QString::number(sampleRateSpin_->value()),
        QStringLiteral("--buffer-size"), QString::number(bufferSizeSpin_->value()),
        QStringLiteral("--output"), output,
    };
    if (!captureManualStopCheck_ || !captureManualStopCheck_->isChecked()) {
        args << QStringLiteral("--duration-ms") << QString::number(captureDurationSpin_->value() * 1000);
    }
    args << captureOptionArgs();
    const QFileInfo binary(capturePathEdit_->text());
    if (binary.exists()) {
        captureProcess_.setWorkingDirectory(binary.absolutePath());
    }
    appendLog(QStringLiteral("starting capture: %1 %2").arg(capturePathEdit_->text(), args.join(QLatin1Char(' '))));
    captureProcess_.start(capturePathEdit_->text(), args);
    if (!captureProcess_.waitForStarted(3000)) {
        appendLog(QStringLiteral("capture failed to start: %1").arg(captureProcess_.errorString()));
        return;
    }
    captureButton_->setEnabled(false);
    loopbackCaptureButton_->setEnabled(false);
    stopCaptureButton_->setEnabled(true);
    importCaptureButton_->setEnabled(false);
}

void MainWindow::showLoopbackCaptureDialog()
{
    if (captureProcess_.state() != QProcess::NotRunning) {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Record Loopback"));
    dialog.resize(780, 560);

    auto* content = new QWidget(&dialog);
    content->setMinimumWidth(720);
    auto* form = new QFormLayout(content);
    const QList<QWidget*> visibleWidgets{
        capturePathEdit_, captureOutputEdit_, loopbackSourceBox_, captureManualStopCheck_, captureDurationSpin_,
        captureTriggerCheck_, triggerThresholdSpin_, triggerHoldSpin_, preRollSpin_,
        tailThresholdSpin_, tailSilenceSpin_, trimLeadingCheck_, trimTrailingCheck_,
    };
    for (QWidget* widget : visibleWidgets) {
        widget->show();
    }
    updateCaptureDurationControl(captureManualStopCheck_, captureDurationSpin_);
    loopbackSourceBox_->setMinimumWidth(420);
    form->addRow(QStringLiteral("jam2-capture"), capturePathEdit_);
    auto* outputLayout = new QHBoxLayout();
    outputLayout->addWidget(captureOutputEdit_, 1);
    auto* browse = new QPushButton(QStringLiteral("Browse"), content);
    outputLayout->addWidget(browse);
    form->addRow(QStringLiteral("Capture output"), outputLayout);
    auto* sourceLayout = new QHBoxLayout();
    sourceLayout->addWidget(loopbackSourceBox_, 1);
    auto* refreshSources = new QPushButton(QStringLiteral("Refresh Sources"), content);
    sourceLayout->addWidget(refreshSources);
    form->addRow(QStringLiteral("Loopback source"), sourceLayout);
    form->addRow(captureManualStopCheck_);
    auto* durationLabel = new QLabel(QStringLiteral("Duration limit"), content);
    form->addRow(durationLabel, captureDurationSpin_);
    QObject::connect(captureManualStopCheck_, &QCheckBox::toggled, &dialog, [this, durationLabel] {
        updateCaptureDurationControl(captureManualStopCheck_, captureDurationSpin_, durationLabel);
    });
    updateCaptureDurationControl(captureManualStopCheck_, captureDurationSpin_, durationLabel);
    form->addRow(captureTriggerCheck_);
    form->addRow(QStringLiteral("Trigger threshold"), triggerThresholdSpin_);
    form->addRow(QStringLiteral("Trigger hold"), triggerHoldSpin_);
    form->addRow(QStringLiteral("Pre-roll"), preRollSpin_);
    form->addRow(QStringLiteral("Tail threshold"), tailThresholdSpin_);
    form->addRow(QStringLiteral("Tail silence"), tailSilenceSpin_);
    form->addRow(trimLeadingCheck_);
    form->addRow(trimTrailingCheck_);

    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* record = buttons->addButton(QStringLiteral("Record"), QDialogButtonBox::AcceptRole);
    QObject::connect(browse, &QPushButton::clicked, this, [this] { chooseCaptureFolder(); });
    QObject::connect(refreshSources, &QPushButton::clicked, this, [this] { refreshLoopbackSources(); });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(scroll, 1);
    layout->addWidget(buttons);
    record->setDefault(true);

    const int result = dialog.exec();
    const QList<QWidget*> widgets{
        capturePathEdit_, captureOutputEdit_, loopbackSourceBox_, captureManualStopCheck_, captureDurationSpin_,
        captureTriggerCheck_, triggerThresholdSpin_, triggerHoldSpin_, preRollSpin_,
        tailThresholdSpin_, tailSilenceSpin_, trimLeadingCheck_, trimTrailingCheck_,
    };
    for (QWidget* widget : widgets) {
        widget->setParent(this);
        widget->hide();
    }
    if (result == QDialog::Accepted) {
        startLoopbackCapture();
    }
}

void MainWindow::startLoopbackCapture()
{
    if (captureProcess_.state() != QProcess::NotRunning) {
        return;
    }
    QDir captureDir(QDir::current());
    captureDir.mkpath(QStringLiteral("captures"));
    QString output = captureOutputEdit_->text().trimmed();
    if (output.isEmpty() || output.endsWith(QStringLiteral("capture.wav"))) {
        output = captureDir.absoluteFilePath(
            QStringLiteral("captures/loopback-%1.wav").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"))));
        captureOutputEdit_->setText(output);
    }
    lastCapturePath_ = output;
    lastCaptureSummary_ = QJsonObject{};
    QString source = loopbackSourceBox_->currentData().toString();
    if (source.isEmpty()) {
        source = loopbackSourceBox_->currentText().trimmed();
    }
    if (source.isEmpty()) {
        source = QStringLiteral("default");
    }
    QStringList args{
        QStringLiteral("record-loopback"),
        QStringLiteral("--source"), source,
        QStringLiteral("--output"), output,
    };
    if (!captureManualStopCheck_ || !captureManualStopCheck_->isChecked()) {
        args << QStringLiteral("--duration-ms") << QString::number(captureDurationSpin_->value() * 1000);
    }
    args << captureOptionArgs();
    const QFileInfo binary(capturePathEdit_->text());
    if (binary.exists()) {
        captureProcess_.setWorkingDirectory(binary.absolutePath());
    }
    appendLog(QStringLiteral("starting loopback capture: %1 %2").arg(capturePathEdit_->text(), args.join(QLatin1Char(' '))));
    captureProcess_.start(capturePathEdit_->text(), args);
    if (!captureProcess_.waitForStarted(3000)) {
        appendLog(QStringLiteral("loopback capture failed to start: %1").arg(captureProcess_.errorString()));
        return;
    }
    captureButton_->setEnabled(false);
    loopbackCaptureButton_->setEnabled(false);
    stopCaptureButton_->setEnabled(true);
    importCaptureButton_->setEnabled(false);
}

void MainWindow::stopInputCapture()
{
    if (captureProcess_.state() == QProcess::NotRunning) {
        return;
    }
    captureProcess_.write("stop\n");
    captureProcess_.closeWriteChannel();
    if (!captureProcess_.waitForFinished(3000)) {
        captureProcess_.terminate();
    }
    if (captureProcess_.state() != QProcess::NotRunning && !captureProcess_.waitForFinished(1500)) {
        captureProcess_.kill();
    }
}

void MainWindow::importLastCapture()
{
    if (!QFileInfo::exists(lastCapturePath_)) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Capture"), QStringLiteral("No captured WAV is available to import."));
        return;
    }
    QFileInfo info(lastCapturePath_);
    try {
        const WavMetadata metadata = readWavMetadata(lastCapturePath_);
        const QJsonObject sidecar = readSidecarJson(lastCapturePath_);
        trackController_.model().fileName = info.fileName();
        trackController_.model().filePath = info.absoluteFilePath();
        trackController_.model().fileBytes = info.size();
        trackController_.model().sampleRate = sidecar.value(QStringLiteral("sample_rate")).toInt(metadata.sampleRate);
        trackController_.model().durationMs = lastCaptureSummary_.value(QStringLiteral("duration_ms")).toInt(
            sidecar.value(QStringLiteral("duration_ms")).toInt(metadata.durationMs));
        trackController_.model().sha256 = metadata.sha256;
        trackController_.model().acceptedBpm = sidecar.value(QStringLiteral("accepted_bpm")).toDouble(120.0);
        trackController_.model().key = QStringLiteral("Unknown");
        trackController_.model().loopEnabled = false;
        trackController_.model().loopStartSeconds = -1.0;
        trackController_.model().loopEndSeconds = -1.0;
    } catch (const std::exception& error) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Capture"), QString::fromUtf8(error.what()));
        return;
    }
    updateTrackControls();
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("track.offer")},
        {QStringLiteral("name"), trackController_.model().fileName},
        {QStringLiteral("path"), trackController_.model().filePath},
        {QStringLiteral("file_bytes"), trackController_.model().fileBytes},
        {QStringLiteral("sample_rate"), trackController_.model().sampleRate},
        {QStringLiteral("duration_ms"), trackController_.model().durationMs},
        {QStringLiteral("sha256"), trackController_.model().sha256},
        {QStringLiteral("accepted_bpm"), trackController_.model().acceptedBpm},
        {QStringLiteral("key"), trackController_.model().key},
        {QStringLiteral("track_gain_db"), trackController_.model().trackGainDb},
        {QStringLiteral("loop_enabled"), trackController_.model().loopEnabled},
        {QStringLiteral("loop_start_seconds"), trackController_.model().loopStartSeconds},
        {QStringLiteral("loop_end_seconds"), trackController_.model().loopEndSeconds},
        {QStringLiteral("focus_enabled"), trackController_.model().focusEnabled},
        {QStringLiteral("focus_preset"), trackController_.model().focusPreset},
        {QStringLiteral("focus_frequency_hz"), trackController_.model().focusFrequencyHz},
        {QStringLiteral("focus_gain_db"), trackController_.model().focusGainDb},
        {QStringLiteral("focus_q"), trackController_.model().focusQ},
    });
    loadTrackIntoPlayer();
}

void MainWindow::loadTrackIntoPlayer()
{
    const QString path = trackController_.model().filePath;
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        trackSink_.reset();
        trackDevice_.reset();
        if (trackWaveform_) {
            trackWaveform_->clear();
        }
        return;
    }
    trackSink_.reset();
    if (trackWaveform_) {
        trackWaveform_->loadWav(path);
        trackWaveform_->setDurationMs(trackController_.model().durationMs);
        trackWaveform_->setBpm(trackController_.model().acceptedBpm);
        trackWaveform_->setGridVisible(trackController_.model().waveformGridVisible);
    }

    try {
        Pcm16Wav wav = readPcm16Wav(path);
        trackDevice_ = std::make_unique<TrackPlaybackDevice>();
        trackDevice_->setTrack(std::move(wav));
        if (!trackDevice_->open(QIODevice::ReadOnly)) {
            trackDevice_.reset();
            appendLog(QStringLiteral("track stream failed to open"));
            return;
        }
        applyTrackPlaybackSettings();
    } catch (const std::exception& error) {
        trackDevice_.reset();
        appendLog(QStringLiteral("track load failed: ") + QString::fromUtf8(error.what()));
    }
}

void MainWindow::applyTrackPlaybackSettings()
{
    if (!trackDevice_) {
        return;
    }
    const auto& model = trackController_.model();
    const qint64 durationMs = trackDevice_->durationMs();
    qint64 loopStartMs = model.loopStartSeconds >= 0.0 ? static_cast<qint64>(std::llround(model.loopStartSeconds * 1000.0)) : 0;
    qint64 loopEndMs = model.loopEndSeconds >= 0.0 ? static_cast<qint64>(std::llround(model.loopEndSeconds * 1000.0)) : durationMs;
    loopStartMs = qBound<qint64>(0, loopStartMs, durationMs);
    loopEndMs = qBound<qint64>(0, loopEndMs, durationMs);
    if (loopEndMs <= loopStartMs) {
        loopStartMs = 0;
        loopEndMs = durationMs;
    }
    trackDevice_->setSpeed(model.speed);
    trackDevice_->setPitchCents(model.pitchCents);
    trackDevice_->setGainDb(model.trackGainDb);
    trackDevice_->setFocus(model.focusEnabled, model.focusFrequencyHz, model.focusGainDb, model.focusQ);
    trackDevice_->setLoop(model.loopEnabled, loopStartMs, loopEndMs);
}

void MainWindow::playTrack()
{
    if (!trackDevice_) {
        loadTrackIntoPlayer();
    }
    if (!trackDevice_ || !trackDevice_->hasTrack()) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Track"), QStringLiteral("No local WAV is loaded."));
        return;
    }
    const auto& model = trackController_.model();
    if (model.loopEnabled) {
        const qint64 durationMs = trackDevice_->durationMs();
        qint64 startMs = model.loopStartSeconds >= 0.0 ? static_cast<qint64>(std::llround(model.loopStartSeconds * 1000.0)) : 0;
        qint64 endMs = model.loopEndSeconds >= 0.0 ? static_cast<qint64>(std::llround(model.loopEndSeconds * 1000.0)) : durationMs;
        startMs = qBound<qint64>(0, startMs, durationMs);
        endMs = qBound<qint64>(0, endMs, durationMs);
        if (endMs <= startMs) {
            startMs = 0;
            endMs = durationMs;
        }
        const qint64 position = trackDevice_->positionMs();
        if (position < startMs || position >= endMs) {
            trackDevice_->setPositionMs(startMs);
        }
    } else if (trackDevice_->positionMs() >= trackDevice_->durationMs()) {
        trackDevice_->setPositionMs(0);
    }

    QAudioDevice outputDevice = selectedLocalOutputDevice();
    if (outputDevice.isNull()) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Track"), QStringLiteral("No default audio output is available."));
        return;
    }
    QAudioFormat format;
    format.setSampleRate(trackDevice_->sampleRate());
    format.setChannelCount(trackDevice_->channels());
    format.setSampleFormat(QAudioFormat::Int16);
    if (!outputDevice.isFormatSupported(format)) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Track"), QStringLiteral("Track sample format is not supported by the default audio output."));
        return;
    }
    trackSink_ = std::make_unique<QAudioSink>(outputDevice, format);
    trackSink_->start(trackDevice_.get());
    if (trackController_.model().syncControls) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("track.play")},
            {QStringLiteral("position_ms"), trackDevice_->positionMs()},
        });
    }
}

void MainWindow::stopTrack()
{
    if (trackSink_) {
        trackSink_->stop();
        trackSink_.reset();
    }
    updateTrackTimeline();
    if (trackController_.model().syncControls) {
        sendControl(QJsonObject{{QStringLiteral("type"), QStringLiteral("track.stop")}});
    }
}

void MainWindow::setLoopStartAtCurrentPosition()
{
    const qint64 duration = trackDevice_ ? trackDevice_->durationMs() : trackController_.model().durationMs;
    const qint64 position = qBound<qint64>(0, trackDevice_ ? trackDevice_->positionMs() : 0, duration);
    auto& model = trackController_.model();
    model.loopStartSeconds = static_cast<double>(position) / 1000.0;
    model.loopEnabled = true;
    applyTrackPlaybackSettings();
    updateTrackControls();
    updateTrackTimeline();
    if (model.syncControls) {
        sendControl(trackMetadataMessage(QStringLiteral("track.offer")));
    }
}

void MainWindow::setLoopEndAtCurrentPosition()
{
    const qint64 duration = trackDevice_ ? trackDevice_->durationMs() : trackController_.model().durationMs;
    const qint64 position = qBound<qint64>(0, trackDevice_ ? trackDevice_->positionMs() : 0, duration);
    auto& model = trackController_.model();
    model.loopEndSeconds = static_cast<double>(position) / 1000.0;
    model.loopEnabled = true;
    applyTrackPlaybackSettings();
    updateTrackControls();
    updateTrackTimeline();
    if (model.syncControls) {
        sendControl(trackMetadataMessage(QStringLiteral("track.offer")));
    }
}

void MainWindow::clearTrackLoop()
{
    auto& model = trackController_.model();
    model.loopEnabled = false;
    model.loopStartSeconds = -1.0;
    model.loopEndSeconds = -1.0;
    applyTrackPlaybackSettings();
    updateTrackControls();
    updateTrackTimeline();
    if (model.syncControls) {
        sendControl(trackMetadataMessage(QStringLiteral("track.offer")));
    }
}

void MainWindow::updateTrackTimeline()
{
    auto& model = trackController_.model();
    if (trackWaveform_) {
        const qint64 duration = trackDevice_ ? trackDevice_->durationMs() : model.durationMs;
        const qint64 position = trackDevice_ ? trackDevice_->positionMs() : 0;
        trackWaveform_->setDurationMs(duration);
        trackWaveform_->setPlayheadMs(position);
        trackWaveform_->setBpm(model.acceptedBpm);
        trackWaveform_->setGridVisible(model.waveformGridVisible);
        trackWaveform_->setLoop(
            model.loopStartSeconds >= 0.0 ? static_cast<qint64>(std::llround(model.loopStartSeconds * 1000.0)) : -1,
            model.loopEndSeconds >= 0.0 ? static_cast<qint64>(std::llround(model.loopEndSeconds * 1000.0)) : -1);
    }
}

void MainWindow::startTrackMetronome()
{
    if (jam2_.isRunning()) {
        if (localMetronomeSink_) {
            localMetronomeSink_->stop();
            localMetronomeSink_.reset();
        }
        if (localMetronomeDevice_) {
            localMetronomeDevice_->close();
            localMetronomeDevice_.reset();
        }
        localMetronomeRunning_ = true;
        updateRuntimeControls();
        jam2_.sendLine(QStringLiteral("metro on"));
        sendMetronomeSettingsToPeer();
        if (trackMetronomeLabel_) {
            trackMetronomeLabel_->setText(QStringLiteral("Jam metronome running"));
        }
        if (startTrackMetronomeButton_) {
            startTrackMetronomeButton_->setEnabled(false);
        }
        if (stopTrackMetronomeButton_) {
            stopTrackMetronomeButton_->setEnabled(true);
        }
        return;
    }

    stopTrackMetronome();

    QAudioDevice outputDevice = selectedLocalOutputDevice();
    if (outputDevice.isNull()) {
        if (trackMetronomeLabel_) {
            trackMetronomeLabel_->setText(QStringLiteral("Local metronome unavailable"));
        }
        return;
    }

    QAudioFormat preferred = outputDevice.preferredFormat();
    QAudioFormat format;
    format.setSampleRate(preferred.sampleRate() > 0 ? preferred.sampleRate() : 48000);
    format.setChannelCount(qMax(1, preferred.channelCount() > 0 ? preferred.channelCount() : 2));
    format.setSampleFormat(QAudioFormat::Int16);
    if (!outputDevice.isFormatSupported(format)) {
        format.setSampleRate(48000);
        format.setChannelCount(2);
    }
    if (!outputDevice.isFormatSupported(format)) {
        if (trackMetronomeLabel_) {
            trackMetronomeLabel_->setText(QStringLiteral("Local metronome format unavailable"));
        }
        return;
    }

    localMetronomeDevice_ = std::make_unique<LocalMetronomeDevice>(format.sampleRate(), format.channelCount());
    localMetronomeDevice_->configure(
        currentMetronomePattern(),
        localMetronomeLevelSlider_ ? static_cast<double>(localMetronomeLevelSlider_->value()) / 100.0 : 0.35);
    localMetronomeDevice_->resetGrid();
    if (!localMetronomeDevice_->open(QIODevice::ReadOnly)) {
        localMetronomeDevice_.reset();
        if (trackMetronomeLabel_) {
            trackMetronomeLabel_->setText(QStringLiteral("Local metronome unavailable"));
        }
        return;
    }

    localMetronomeSink_ = std::make_unique<QAudioSink>(outputDevice, format);
    localMetronomeSink_->start(localMetronomeDevice_.get());
    localMetronomeRunning_ = true;
    if (trackMetronomeLabel_) {
        trackMetronomeLabel_->setText(QStringLiteral("Local metronome running"));
    }
    if (startTrackMetronomeButton_) {
        startTrackMetronomeButton_->setEnabled(false);
    }
    if (stopTrackMetronomeButton_) {
        stopTrackMetronomeButton_->setEnabled(true);
    }
}

void MainWindow::stopTrackMetronome()
{
    const bool jamMetronomeWasRunning = jam2_.isRunning() && localMetronomeRunning_;
    if (localMetronomeSink_) {
        localMetronomeSink_->stop();
        localMetronomeSink_.reset();
    }
    if (localMetronomeDevice_) {
        localMetronomeDevice_->close();
        localMetronomeDevice_.reset();
    }
    localMetronomeRunning_ = false;
    if (jamMetronomeWasRunning) {
        jam2_.sendLine(QStringLiteral("metro off"));
        sendMetronomeSettingsToPeer();
    }
    if (trackMetronomeLabel_) {
        trackMetronomeLabel_->setText(jam2_.isRunning()
            ? QStringLiteral("Jam metronome stopped")
            : QStringLiteral("Local metronome stopped"));
    }
    if (startTrackMetronomeButton_) {
        startTrackMetronomeButton_->setEnabled(true);
    }
    if (stopTrackMetronomeButton_) {
        stopTrackMetronomeButton_->setEnabled(false);
    }
}

void MainWindow::updateTrackMetronomeInterval()
{
    if (bpmSpin_ && metronomeBpmSpin_) {
        bpmSpin_->setValue(metronomeBpmSpin_->value());
    }
    applyMetronomePatternToLocalDevice();
    sendMetronomePatternToJam();
    sendMetronomeSettingsToPeer();
}

void MainWindow::rebuildMetronomePattern()
{
    if (!metronomePatternTable_) {
        return;
    }
    const int beats = metronomeBeatsSpin_ ? metronomeBeatsSpin_->value() : 4;
    const int division = metronomeDivisionBox_ ? metronomeDivisionBox_->currentData().toInt() : 1;
    const int steps = qMax(1, beats * qMax(1, division));
    QVector<bool> previousEnabled = metronomeEnabledSteps_;
    QVector<bool> previous = metronomeAccents_;
    metronomeEnabledSteps_.resize(steps);
    metronomeAccents_.resize(steps);
    for (int i = 0; i < steps; ++i) {
        metronomeEnabledSteps_[i] = i < previousEnabled.size() ? previousEnabled[i] : true;
        metronomeAccents_[i] = i < previous.size() ? previous[i] : (i == 0);
    }

    metronomePatternTable_->clear();
    metronomePatternTable_->setRowCount(2);
    metronomePatternTable_->setColumnCount(steps);
    QStringList headers;
    headers.reserve(steps);
    for (int step = 0; step < steps; ++step) {
        headers << QStringLiteral("%1").arg(step + 1);
        auto* playCell = new QWidget(metronomePatternTable_);
        auto* playLayout = new QHBoxLayout(playCell);
        playLayout->setContentsMargins(0, 0, 0, 0);
        playLayout->setAlignment(Qt::AlignCenter);
        auto* playCheck = new QCheckBox(playCell);
        playCheck->setChecked(metronomeEnabledSteps_[step]);
        QObject::connect(playCheck, &QCheckBox::toggled, this, [this, step](bool checked) {
            if (step >= 0 && step < metronomeEnabledSteps_.size()) {
                metronomeEnabledSteps_[step] = checked;
                applyMetronomePatternToLocalDevice();
                sendMetronomePatternToJam();
                sendMetronomeSettingsToPeer();
            }
        });
        playLayout->addWidget(playCheck);
        metronomePatternTable_->setCellWidget(0, step, playCell);

        auto* accentCell = new QWidget(metronomePatternTable_);
        auto* accentLayout = new QHBoxLayout(accentCell);
        accentLayout->setContentsMargins(0, 0, 0, 0);
        accentLayout->setAlignment(Qt::AlignCenter);
        auto* accentCheck = new QCheckBox(accentCell);
        accentCheck->setChecked(metronomeAccents_[step]);
        QObject::connect(accentCheck, &QCheckBox::toggled, this, [this, step](bool checked) {
            if (step >= 0 && step < metronomeAccents_.size()) {
                metronomeAccents_[step] = checked;
                applyMetronomePatternToLocalDevice();
                sendMetronomePatternToJam();
                sendMetronomeSettingsToPeer();
            }
        });
        accentLayout->addWidget(accentCheck);
        metronomePatternTable_->setCellWidget(1, step, accentCell);
    }
    metronomePatternTable_->setHorizontalHeaderLabels(headers);
    metronomePatternTable_->setVerticalHeaderLabels(QStringList{QStringLiteral("Play"), QStringLiteral("Accent")});
    applyMetronomePatternToLocalDevice();
    sendMetronomePatternToJam();
    sendMetronomeSettingsToPeer();
}

jam2::metronome::PatternSnapshot MainWindow::currentMetronomePattern() const
{
    jam2::metronome::PatternSnapshot pattern;
    pattern.bpm = metronomeBpmSpin_ ? metronomeBpmSpin_->value() : 120;
    pattern.beats_per_bar = metronomeBeatsSpin_ ? metronomeBeatsSpin_->value() : 4;
    pattern.division = metronomeDivisionBox_ ? metronomeDivisionBox_->currentData().toInt() : 1;
    pattern.step_count = jam2::metronome::pattern_step_count(pattern.beats_per_bar, pattern.division);
    pattern.play_mask_low = 0;
    pattern.play_mask_high = 0;
    pattern.accent_mask_low = 0;
    pattern.accent_mask_high = 0;
    for (int step = 0; step < pattern.step_count; ++step) {
        const bool play = step < metronomeEnabledSteps_.size() ? metronomeEnabledSteps_[step] : true;
        const bool accent = step < metronomeAccents_.size() ? metronomeAccents_[step] : (step == 0);
        jam2::metronome::set_mask_enabled(pattern.play_mask_low, pattern.play_mask_high, step, play);
        jam2::metronome::set_mask_enabled(pattern.accent_mask_low, pattern.accent_mask_high, step, accent);
    }
    return jam2::metronome::sanitize(pattern);
}

void MainWindow::applyMetronomePatternToLocalDevice()
{
    if (!localMetronomeDevice_) {
        return;
    }
    localMetronomeDevice_->configure(
        currentMetronomePattern(),
        localMetronomeLevelSlider_ ? static_cast<double>(localMetronomeLevelSlider_->value()) / 100.0 : 0.35);
}

void MainWindow::sendMetronomePatternToJam()
{
    if (!jam2_.isRunning()) {
        return;
    }
    const jam2::metronome::PatternSnapshot pattern = currentMetronomePattern();
    jam2_.sendLine(QStringLiteral("metro pattern %1 %2 %3 0x%4 0x%5 0x%6 0x%7")
        .arg(pattern.bpm)
        .arg(pattern.beats_per_bar)
        .arg(pattern.division)
        .arg(QString::number(static_cast<qulonglong>(pattern.play_mask_low), 16))
        .arg(QString::number(static_cast<qulonglong>(pattern.play_mask_high), 16))
        .arg(QString::number(static_cast<qulonglong>(pattern.accent_mask_low), 16))
        .arg(QString::number(static_cast<qulonglong>(pattern.accent_mask_high), 16)));
}

void MainWindow::sendMetronomeSettingsToPeer()
{
    if (applyingRemoteMetronomeSettings_ || !jam2_.isRunning()) {
        return;
    }
    const jam2::metronome::PatternSnapshot pattern = currentMetronomePattern();
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("metronome.settings")},
        {QStringLiteral("running"), localMetronomeRunning_},
        {QStringLiteral("mode"), metronomeModeBox_ ? metronomeModeBox_->currentText() : QStringLiteral("shared-grid")},
        {QStringLiteral("bpm"), pattern.bpm},
        {QStringLiteral("beats"), pattern.beats_per_bar},
        {QStringLiteral("division"), pattern.division},
        {QStringLiteral("play_mask_low"), QString::number(static_cast<qulonglong>(pattern.play_mask_low), 16)},
        {QStringLiteral("play_mask_high"), QString::number(static_cast<qulonglong>(pattern.play_mask_high), 16)},
        {QStringLiteral("accent_mask_low"), QString::number(static_cast<qulonglong>(pattern.accent_mask_low), 16)},
        {QStringLiteral("accent_mask_high"), QString::number(static_cast<qulonglong>(pattern.accent_mask_high), 16)},
    });
}

void MainWindow::applyRemoteMetronomeSettings(const QJsonObject& message)
{
    auto parseMask = [](const QJsonObject& object, const QString& key) {
        bool ok = false;
        const QString text = object.value(key).toString();
        const qulonglong value = text.toULongLong(&ok, 16);
        return ok ? static_cast<std::uint64_t>(value) : 0ULL;
    };

    const bool running = message.value(QStringLiteral("running")).toBool(localMetronomeRunning_);
    const QString mode = message.value(QStringLiteral("mode")).toString(metronomeModeBox_
        ? metronomeModeBox_->currentText()
        : QStringLiteral("shared-grid"));
    const int bpm = qBound(1, message.value(QStringLiteral("bpm")).toInt(metronomeBpmSpin_ ? metronomeBpmSpin_->value() : 120), 400);
    const int beats = qBound(1, message.value(QStringLiteral("beats")).toInt(metronomeBeatsSpin_ ? metronomeBeatsSpin_->value() : 4), 16);
    const int division = qMax(1, message.value(QStringLiteral("division")).toInt(metronomeDivisionBox_ ? metronomeDivisionBox_->currentData().toInt() : 1));
    const std::uint64_t playLow = parseMask(message, QStringLiteral("play_mask_low"));
    const std::uint64_t playHigh = parseMask(message, QStringLiteral("play_mask_high"));
    const std::uint64_t accentLow = parseMask(message, QStringLiteral("accent_mask_low"));
    const std::uint64_t accentHigh = parseMask(message, QStringLiteral("accent_mask_high"));

    applyingRemoteMetronomeSettings_ = true;
    if (metronomeBpmSpin_) {
        const QSignalBlocker blocker(metronomeBpmSpin_);
        metronomeBpmSpin_->setValue(bpm);
    }
    if (bpmSpin_) {
        const QSignalBlocker blocker(bpmSpin_);
        bpmSpin_->setValue(bpm);
    }
    if (metronomeBeatsSpin_) {
        const QSignalBlocker blocker(metronomeBeatsSpin_);
        metronomeBeatsSpin_->setValue(beats);
    }
    if (metronomeDivisionBox_) {
        const QSignalBlocker blocker(metronomeDivisionBox_);
        const int index = metronomeDivisionBox_->findData(division);
        if (index >= 0) {
            metronomeDivisionBox_->setCurrentIndex(index);
        }
    }
    if (metronomeModeBox_) {
        const QSignalBlocker blocker(metronomeModeBox_);
        const int index = metronomeModeBox_->findText(mode);
        if (index >= 0) {
            metronomeModeBox_->setCurrentIndex(index);
        }
    }

    jam2::metronome::PatternSnapshot pattern;
    pattern.bpm = bpm;
    pattern.beats_per_bar = beats;
    pattern.division = division;
    pattern.step_count = jam2::metronome::pattern_step_count(beats, division);
    pattern.play_mask_low = playLow;
    pattern.play_mask_high = playHigh;
    pattern.accent_mask_low = accentLow;
    pattern.accent_mask_high = accentHigh;
    pattern = jam2::metronome::sanitize(pattern);

    metronomeEnabledSteps_.resize(pattern.step_count);
    metronomeAccents_.resize(pattern.step_count);
    for (int step = 0; step < pattern.step_count; ++step) {
        metronomeEnabledSteps_[step] = jam2::metronome::mask_enabled(pattern.play_mask_low, pattern.play_mask_high, step);
        metronomeAccents_[step] = jam2::metronome::mask_enabled(pattern.accent_mask_low, pattern.accent_mask_high, step);
    }
    rebuildMetronomePattern();
    localMetronomeRunning_ = running;
    applyingRemoteMetronomeSettings_ = false;

    applyMetronomePatternToLocalDevice();
    updateRuntimeControls();
    if (jam2_.isRunning()) {
        jam2_.sendLine(running ? QStringLiteral("metro on") : QStringLiteral("metro off"));
    }
    if (trackMetronomeLabel_) {
        trackMetronomeLabel_->setText(running
            ? QStringLiteral("Jam metronome running")
            : QStringLiteral("Jam metronome stopped"));
    }
    if (startTrackMetronomeButton_) {
        startTrackMetronomeButton_->setEnabled(!running);
    }
    if (stopTrackMetronomeButton_) {
        stopTrackMetronomeButton_->setEnabled(running);
    }
}

QJsonObject MainWindow::trackMetadataMessage(const QString& type) const
{
    return QJsonObject{
        {QStringLiteral("type"), type},
        {QStringLiteral("name"), trackController_.model().fileName},
        {QStringLiteral("path"), trackController_.model().filePath},
        {QStringLiteral("file_bytes"), trackController_.model().fileBytes},
        {QStringLiteral("sample_rate"), trackController_.model().sampleRate},
        {QStringLiteral("duration_ms"), trackController_.model().durationMs},
        {QStringLiteral("sha256"), trackController_.model().sha256},
        {QStringLiteral("accepted_bpm"), trackController_.model().acceptedBpm},
        {QStringLiteral("key"), trackController_.model().key},
        {QStringLiteral("track_gain_db"), trackController_.model().trackGainDb},
        {QStringLiteral("loop_enabled"), trackController_.model().loopEnabled},
        {QStringLiteral("loop_start_seconds"), trackController_.model().loopStartSeconds},
        {QStringLiteral("loop_end_seconds"), trackController_.model().loopEndSeconds},
        {QStringLiteral("focus_enabled"), trackController_.model().focusEnabled},
        {QStringLiteral("focus_preset"), trackController_.model().focusPreset},
        {QStringLiteral("focus_frequency_hz"), trackController_.model().focusFrequencyHz},
        {QStringLiteral("focus_gain_db"), trackController_.model().focusGainDb},
        {QStringLiteral("focus_q"), trackController_.model().focusQ},
    };
}

void MainWindow::sendTrackFile()
{
    const QString path = trackController_.model().filePath;
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Track"), QStringLiteral("No local WAV is available to share."));
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Jam2 Track"), QStringLiteral("Could not open track WAV for sharing."));
        return;
    }
    const QByteArray bytes = file.readAll();
    const QString sha = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    constexpr int chunkSize = 48 * 1024;
    sendControl(trackMetadataMessage(QStringLiteral("track.offer")));
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("track.file.start")},
        {QStringLiteral("name"), QFileInfo(path).fileName()},
        {QStringLiteral("file_bytes"), bytes.size()},
        {QStringLiteral("sha256"), sha},
        {QStringLiteral("chunk_size"), chunkSize},
    });
    for (int offset = 0, index = 0; offset < bytes.size(); offset += chunkSize, ++index) {
        sendControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("track.file.chunk")},
            {QStringLiteral("index"), index},
            {QStringLiteral("data"), QString::fromLatin1(bytes.mid(offset, chunkSize).toBase64())},
        });
    }
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("track.file.done")},
        {QStringLiteral("chunks"), (bytes.size() + chunkSize - 1) / chunkSize},
        {QStringLiteral("sha256"), sha},
    });
    appendLog(QStringLiteral("shared track WAV: %1 bytes").arg(bytes.size()));
}

void MainWindow::receiveTrackFileStart(const QJsonObject& message)
{
    incomingTrackBytes_.clear();
    incomingTrackName_ = QFileInfo(message.value(QStringLiteral("name")).toString(QStringLiteral("track.wav"))).fileName();
    incomingTrackSha256_ = message.value(QStringLiteral("sha256")).toString();
    incomingTrackBytesExpected_ = static_cast<qint64>(message.value(QStringLiteral("file_bytes")).toDouble());
    incomingTrackNextChunk_ = 0;
    incomingTrackBytes_.reserve(static_cast<int>(qMin<qint64>(incomingTrackBytesExpected_, std::numeric_limits<int>::max())));
    appendLog(QStringLiteral("receiving track WAV: %1 bytes").arg(incomingTrackBytesExpected_));
}

void MainWindow::receiveTrackFileChunk(const QJsonObject& message)
{
    const int index = message.value(QStringLiteral("index")).toInt(-1);
    if (index != incomingTrackNextChunk_) {
        appendLog(QStringLiteral("track WAV chunk out of order: got %1 expected %2").arg(index).arg(incomingTrackNextChunk_));
        incomingTrackBytes_.clear();
        incomingTrackNextChunk_ = 0;
        return;
    }
    incomingTrackBytes_.append(QByteArray::fromBase64(message.value(QStringLiteral("data")).toString().toLatin1()));
    ++incomingTrackNextChunk_;
}

void MainWindow::receiveTrackFileDone(const QJsonObject& message)
{
    const QString expectedSha = message.value(QStringLiteral("sha256")).toString(incomingTrackSha256_);
    const QString actualSha = QString::fromLatin1(QCryptographicHash::hash(incomingTrackBytes_, QCryptographicHash::Sha256).toHex());
    if (incomingTrackBytesExpected_ != incomingTrackBytes_.size() || (!expectedSha.isEmpty() && expectedSha != actualSha)) {
        appendLog(QStringLiteral("received track WAV failed verification"));
        incomingTrackBytes_.clear();
        return;
    }
    QDir dir(QDir::current());
    dir.mkpath(QStringLiteral("received_tracks"));
    const QString output = dir.absoluteFilePath(QStringLiteral("received_tracks/%1").arg(incomingTrackName_.isEmpty() ? QStringLiteral("track.wav") : incomingTrackName_));
    QFile file(output);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        appendLog(QStringLiteral("failed to write received track WAV: ") + output);
        return;
    }
    file.write(incomingTrackBytes_);
    file.close();

    try {
        const WavMetadata metadata = readWavMetadata(output);
        QFileInfo info(output);
        trackController_.model().fileName = info.fileName();
        trackController_.model().filePath = info.absoluteFilePath();
        trackController_.model().fileBytes = info.size();
        trackController_.model().sampleRate = metadata.sampleRate;
        trackController_.model().durationMs = metadata.durationMs;
        trackController_.model().sha256 = metadata.sha256;
        updateTrackControls();
        loadTrackIntoPlayer();
        appendLog(QStringLiteral("received track WAV: ") + output);
    } catch (const std::exception& error) {
        appendLog(QStringLiteral("received track WAV metadata failed: ") + QString::fromUtf8(error.what()));
    }
    incomingTrackBytes_.clear();
}

QStringList MainWindow::commonJamArgs(bool includeExtraArgs) const
{
    QStringList args;
    args << QStringLiteral("--audio-device") << selectedDeviceId()
         << QStringLiteral("--sample-rate") << QString::number(sampleRateSpin_->value())
         << QStringLiteral("--audio-buffer-size") << QString::number(bufferSizeSpin_->value())
         << QStringLiteral("--frame-size") << QString::number(frameSizeSpin_->value())
         << QStringLiteral("--playback-prefill-frames") << QString::number(prefillSpin_->value())
         << QStringLiteral("--capture-ring-frames") << QString::number(captureRingSpin_->value())
         << QStringLiteral("--playback-ring-frames") << QString::number(playbackRingSpin_->value())
         << QStringLiteral("--input-channels") << inputChannelsEdit_->text()
         << QStringLiteral("--output-channels") << outputChannelsEdit_->text()
         << QStringLiteral("--stats") << (statsCheck_->isChecked() ? QStringLiteral("enabled") : QStringLiteral("disabled"))
         << QStringLiteral("--machine-readable-startup") << QStringLiteral("on")
         << QStringLiteral("--status-format") << QStringLiteral("jsonl")
         << QStringLiteral("--metronome") << QStringLiteral("off")
         << QStringLiteral("--bpm") << QString::number(metronomeBpmSpin_ ? metronomeBpmSpin_->value() : bpmSpin_->value())
         << QStringLiteral("--metronome-level") << QString::number(static_cast<double>(metronomeLevelSlider_->value()) / 100.0, 'f', 2)
         << QStringLiteral("--remote-level") << QString::number(static_cast<double>(remoteLevelSlider_->value()) / 100.0, 'f', 2)
         << QStringLiteral("--metronome-mode") << metronomeModeBox_->currentText()
         << QStringLiteral("--sample-time-playout") << onOff(sampleTimePlayoutCheck_->isChecked())
         << QStringLiteral("--adaptive-playback-cushion") << onOff(adaptiveCushionCheck_->isChecked())
         << QStringLiteral("--adaptive-playback-release-ppm") << QString::number(adaptiveReleaseSpin_->value())
         << QStringLiteral("--drift-correction") << onOff(driftCorrectionCheck_->isChecked())
         << QStringLiteral("--drift-smoothing") << QString::number(driftSmoothingSpin_->value(), 'f', 3)
         << QStringLiteral("--drift-deadband-ppm") << QString::number(driftDeadbandSpin_->value())
         << QStringLiteral("--drift-max-correction-ppm") << QString::number(driftMaxCorrectionSpin_->value())
         << QStringLiteral("--stream-linger-ms") << QString::number(streamLingerMsSpin_->value());
    if (statsCheck_->isChecked()) {
        args << QStringLiteral("--stats-interval-ms") << QStringLiteral("1000")
             << QStringLiteral("--stats-warmup-ms") << QString::number(statsWarmupMsSpin_->value());
    }
    if (playbackMaxSpin_->value() > 0) {
        args << QStringLiteral("--playback-max-frames") << QString::number(playbackMaxSpin_->value());
    }
    if (waitMsSpin_->value() > 0) {
        args << QStringLiteral("--wait-ms") << QString::number(waitMsSpin_->value());
    }
    if (streamMsSpin_->value() > 0) {
        args << QStringLiteral("--stream-ms") << QString::number(streamMsSpin_->value());
    }
    if (statsCheck_->isChecked() && !logStatsEdit_->text().trimmed().isEmpty()) {
        args << QStringLiteral("--log-stats") << logStatsEdit_->text().trimmed();
    }
    if (socketSendBufferSpin_->value() > 0) {
        args << QStringLiteral("--socket-send-buffer") << QString::number(socketSendBufferSpin_->value());
    }
    if (socketRecvBufferSpin_->value() > 0) {
        args << QStringLiteral("--socket-recv-buffer") << QString::number(socketRecvBufferSpin_->value());
    }
    if (playoutDelaySpin_->value() > 0) {
        args << QStringLiteral("--playout-delay-frames") << QString::number(playoutDelaySpin_->value());
    }
    if (adaptiveTargetSpin_->value() > 0) {
        args << QStringLiteral("--adaptive-playback-target-frames") << QString::number(adaptiveTargetSpin_->value());
    }
    if (adaptiveMinSpin_->value() > 0) {
        args << QStringLiteral("--adaptive-playback-min-frames") << QString::number(adaptiveMinSpin_->value());
    }
    if (adaptiveMaxSpin_->value() > 0) {
        args << QStringLiteral("--adaptive-playback-max-frames") << QString::number(adaptiveMaxSpin_->value());
    }
    if (noStunCheck_->isChecked()) {
        args << QStringLiteral("--no-stun");
    }
    if (includeExtraArgs && !extraArgsEdit_->text().trimmed().isEmpty()) {
        args << QProcess::splitCommand(extraArgsEdit_->text().trimmed());
    }
    return args;
}

QString MainWindow::selectedDeviceId() const
{
    const QString data = deviceBox_->currentData().toString();
    return data.isEmpty() ? deviceId(deviceBox_->currentText()) : data;
}

QAudioDevice MainWindow::selectedLocalOutputDevice() const
{
    const QString selected = localOutputBox_ ? localOutputBox_->currentData().toString() : QString{};
    if (!selected.isEmpty()) {
        const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();
        for (const QAudioDevice& device : outputs) {
            if (audioDeviceIdText(device) == selected) {
                return device;
            }
        }
    }
    return QMediaDevices::defaultAudioOutput();
}

QJsonObject MainWindow::trackToJson() const
{
    const auto& model = trackController_.model();
    return QJsonObject{
        {QStringLiteral("file_name"), model.fileName},
        {QStringLiteral("file_path"), model.filePath},
        {QStringLiteral("file_bytes"), model.fileBytes},
        {QStringLiteral("sample_rate"), model.sampleRate},
        {QStringLiteral("duration_ms"), model.durationMs},
        {QStringLiteral("sha256"), model.sha256},
        {QStringLiteral("guessed_bpm"), model.guessedBpm},
        {QStringLiteral("accepted_bpm"), model.acceptedBpm},
        {QStringLiteral("key"), model.key},
        {QStringLiteral("speed"), model.speed},
        {QStringLiteral("pitch_cents"), model.pitchCents},
        {QStringLiteral("track_gain_db"), model.trackGainDb},
        {QStringLiteral("loop_enabled"), model.loopEnabled},
        {QStringLiteral("loop_start_seconds"), model.loopStartSeconds},
        {QStringLiteral("loop_end_seconds"), model.loopEndSeconds},
        {QStringLiteral("waveform_grid_visible"), model.waveformGridVisible},
        {QStringLiteral("sync_controls"), model.syncControls},
        {QStringLiteral("focus_enabled"), model.focusEnabled},
        {QStringLiteral("focus_preset"), model.focusPreset},
        {QStringLiteral("focus_frequency_hz"), model.focusFrequencyHz},
        {QStringLiteral("focus_gain_db"), model.focusGainDb},
        {QStringLiteral("focus_q"), model.focusQ},
        {QStringLiteral("highpass_hz"), model.highpassHz},
        {QStringLiteral("lowpass_hz"), model.lowpassHz},
    };
}

void MainWindow::loadTrackJson(const QJsonObject& object)
{
    if (object.isEmpty()) {
        return;
    }
    auto& model = trackController_.model();
    model.fileName = object.value(QStringLiteral("file_name")).toString(model.fileName);
    model.filePath = object.value(QStringLiteral("file_path")).toString(model.filePath);
    model.fileBytes = static_cast<qint64>(object.value(QStringLiteral("file_bytes")).toDouble(model.fileBytes));
    model.sampleRate = object.value(QStringLiteral("sample_rate")).toInt(model.sampleRate);
    model.durationMs = object.value(QStringLiteral("duration_ms")).toInt(model.durationMs);
    model.sha256 = object.value(QStringLiteral("sha256")).toString(model.sha256);
    model.guessedBpm = object.value(QStringLiteral("guessed_bpm")).toDouble(model.guessedBpm);
    model.acceptedBpm = object.value(QStringLiteral("accepted_bpm")).toDouble(model.acceptedBpm);
    model.key = object.value(QStringLiteral("key")).toString(model.key);
    model.speed = object.value(QStringLiteral("speed")).toDouble(model.speed);
    model.pitchCents = object.value(QStringLiteral("pitch_cents")).toInt(model.pitchCents);
    model.trackGainDb = object.value(QStringLiteral("track_gain_db")).toDouble(model.trackGainDb);
    model.loopEnabled = object.value(QStringLiteral("loop_enabled")).toBool(model.loopEnabled);
    model.loopStartSeconds = object.value(QStringLiteral("loop_start_seconds")).toDouble(model.loopStartSeconds);
    model.loopEndSeconds = object.value(QStringLiteral("loop_end_seconds")).toDouble(model.loopEndSeconds);
    model.waveformGridVisible = object.value(QStringLiteral("waveform_grid_visible")).toBool(model.waveformGridVisible);
    model.syncControls = object.value(QStringLiteral("sync_controls")).toBool(model.syncControls);
    model.focusEnabled = object.value(QStringLiteral("focus_enabled")).toBool(model.focusEnabled);
    model.focusPreset = object.value(QStringLiteral("focus_preset")).toString(model.focusPreset);
    model.focusFrequencyHz = object.value(QStringLiteral("focus_frequency_hz")).toDouble(model.focusFrequencyHz);
    model.focusGainDb = object.value(QStringLiteral("focus_gain_db")).toDouble(model.focusGainDb);
    model.focusQ = object.value(QStringLiteral("focus_q")).toDouble(model.focusQ);
    model.highpassHz = object.value(QStringLiteral("highpass_hz")).toDouble(model.highpassHz);
    model.lowpassHz = object.value(QStringLiteral("lowpass_hz")).toDouble(model.lowpassHz);
    updateTrackControls();
    loadTrackIntoPlayer();
}

QJsonObject MainWindow::songToJson() const
{
    QJsonObject root = chordModel_.toJson();
    root.insert(QStringLiteral("independent_views"), true);
    root.insert(QStringLiteral("beat_view"), beatModel_.toJson());
    root.insert(QStringLiteral("lyric_view"), lyricModel_.toJson());
    return root;
}

bool MainWindow::loadSongJson(const QJsonObject& object)
{
    BeatGridModel loadedChord;
    BeatGridModel loadedBeat;
    BeatGridModel loadedLyric;
    if (!loadedChord.loadJson(object)) {
        return false;
    }
    const QJsonObject beatObject = object.value(QStringLiteral("beat_view")).toObject();
    if (!beatObject.isEmpty()) {
        if (!loadedBeat.loadJson(beatObject)) {
            return false;
        }
    } else if (!loadedBeat.loadJson(object)) {
        return false;
    }
    const QJsonObject lyricObject = object.value(QStringLiteral("lyric_view")).toObject();
    if (!lyricObject.isEmpty()) {
        if (!loadedLyric.loadJson(lyricObject)) {
            return false;
        }
    } else if (!loadedLyric.loadJson(object)) {
        return false;
    }

    const QString title = loadedChord.title();
    loadedBeat.setTitle(title);
    loadedLyric.setTitle(title);
    chordModel_ = loadedChord;
    beatModel_ = loadedBeat;
    lyricModel_ = loadedLyric;
    return true;
}

void MainWindow::newSong()
{
    chordModel_.reset();
    beatModel_.reset();
    lyricModel_.reset();
    if (trackSink_) {
        trackSink_->stop();
        trackSink_.reset();
    }
    stopTrackMetronome();
    trackDevice_.reset();
    trackController_ = SharedTrackController{};
    lastCaptureSummary_ = QJsonObject{};
    lastCapturePath_.clear();
    if (trackWaveform_) {
        trackWaveform_->clear();
    }
    updateTrackControls();
    refreshSongViews();
}

void MainWindow::openSong()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Open Jam2 Song"),
        QString(),
        QStringLiteral("Jam2 song (*.jam2song *.json);;JSON files (*.json);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Jam2"), QStringLiteral("Could not open song file."));
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    const QJsonObject root = document.object();
    if (!document.isObject() || !loadSongJson(root)) {
        QMessageBox::warning(this, QStringLiteral("Jam2"), QStringLiteral("Invalid Jam2 song file."));
        return;
    }
    loadTrackJson(root.value(QStringLiteral("track")).toObject());
    refreshSongViews();
}

void MainWindow::saveSong()
{
    chordModel_.setTitle(songTitleEdit_->text());
    beatModel_.setTitle(songTitleEdit_->text());
    lyricModel_.setTitle(songTitleEdit_->text());
    const QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save Jam2 Song"),
        chordModel_.title() + QStringLiteral(".jam2song"),
        QStringLiteral("Jam2 song (*.jam2song);;JSON files (*.json);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, QStringLiteral("Jam2"), QStringLiteral("Could not save song file."));
        return;
    }
    QJsonObject root = songToJson();
    root.insert(QStringLiteral("track"), trackToJson());
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void MainWindow::sendSongSnapshot()
{
    sendControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("song.set")},
        {QStringLiteral("revision"), chordModel_.revision() + beatModel_.revision() + lyricModel_.revision()},
        {QStringLiteral("song"), songToJson()},
    });
}

void MainWindow::refreshSongViews()
{
    if (songTitleEdit_) {
        songTitleEdit_->setText(chordModel_.title());
    }
    if (chordGrid_) {
        chordGrid_->refresh();
    }
    if (beatGrid_) {
        beatGrid_->refresh();
    }
    if (lyricGrid_) {
        lyricGrid_->refresh();
    }
}

void MainWindow::refreshSongView(const QString& lane)
{
    if (lane == QStringLiteral("chord")) {
        if (chordGrid_) {
            chordGrid_->refresh();
        }
    } else if (lane == QStringLiteral("lyric")) {
        if (lyricGrid_) {
            lyricGrid_->refresh();
        }
    } else {
        if (beatGrid_) {
            beatGrid_->refresh();
        }
    }
}

QString MainWindow::sessionHex() const
{
    return sessionToHex(sessionId_);
}

QString MainWindow::keyHex() const
{
    return keyToHex(sessionKey_);
}

void MainWindow::generateSession()
{
    sessionId_ = jam2::random_u64();
    sessionKey_ = jam2::random_key();
}
