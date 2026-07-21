#pragma once

#include <QString>

struct AudioDevicePreference {
    QString backend;
    QString stableId;
    QString name;
    QString inputChannels = QStringLiteral("1");
    QString outputChannels = QStringLiteral("1,2");
    int sampleRate = 48000;
    int bufferSize = 64;
};

struct LocalTuningPreference {
    QString profile = QStringLiteral("fast");
    int bufferSize = 32;
    int frameSize = 64;
    int prefillFrames = 256;
    int playbackMaxFrames = 1536;
    int captureRingFrames = 4096;
    int playbackRingFrames = 4096;
    bool driftCorrection = true;
    double driftSmoothing = 0.02;
    int driftDeadbandPpm = 25;
    int driftMaxCorrectionPpm = 500;
    bool sampleTimePlayout = true;
    int playoutDelayFrames = 256;
    int jitterBufferFrames = 512;
    int jitterBufferMaxFrames = 1024;
    bool adaptiveCushion = true;
    int adaptiveTargetFrames = 256;
    int adaptiveMinFrames = 256;
    int adaptiveMaxFrames = 1536;
    int adaptiveReleasePpm = 5000;
    int adaptiveRatioRampMs = 250;
};

struct RuntimePreference {
    bool diagnostics = true;
    int diagnosticsWarmupMs = 3000;
    QString logStatsFolder;
    QString osPriority = QStringLiteral("high");
    int waitMs = 0;
    int streamMs = 0;
    int streamLingerMs = 100;
};

struct CreatePreference {
    QString bindHost = QStringLiteral("0.0.0.0");
    int port = 49000;
    QString publicHost;
    QString stunServer = QStringLiteral("stun.l.google.com:19302");
    int stunTimeoutMs = 1000;
    int stunRetries = 3;
    bool noStun = false;
    int maxPeers = 0;
    int sampleRate = 48000;
    QString audioFormat = QStringLiteral("pcm24-mono");
    int socketSendBuffer = 0;
    int socketRecvBuffer = 0;
    LocalTuningPreference tuning;
    RuntimePreference runtime;
};

struct JoinPreference {
    QString bindHost = QStringLiteral("0.0.0.0");
    int port = 49000;
    LocalTuningPreference tuning;
    RuntimePreference runtime;
};

struct LoggingPreference {
    QString folder;
};

struct InputRecordingPreference {
    QString outputFolder;
    bool recordUntilStopped = true;
    int durationBars = 8;
    bool countIn = true;
    int countInBars = 1;
    bool countInMetronome = true;
    bool keepMetronome = false;
    int latencyAdjustmentFrames = 0;
};

struct LoopbackRecordingPreference {
    QString outputFolder;
    QString sourceId = QStringLiteral("default");
    QString sourceName = QStringLiteral("[default] System mix");
    bool recordUntilStopped = true;
    int durationBars = 8;
    bool trigger = false;
    double triggerThresholdDb = -45.0;
    int triggerHoldMs = 50;
    int preRollMs = 250;
    double tailThresholdDb = -50.0;
    int tailSilenceMs = 1000;
    bool trimLeading = true;
    bool trimTrailing = true;
};

struct RecordingPreference {
    QString preferredMode = QStringLiteral("input");
    InputRecordingPreference input;
    LoopbackRecordingPreference loopback;
};

struct UserPreferences {
    static constexpr int kSchemaVersion = 3;
    AudioDevicePreference localAudio;
    AudioDevicePreference networkAudio;
    bool splitNetworkAudioByRole = false;
    AudioDevicePreference createJamAudio;
    AudioDevicePreference joinJamAudio;
    CreatePreference create;
    JoinPreference join;
    LoggingPreference logging;
    RecordingPreference recording;

    const AudioDevicePreference& createAudio() const noexcept
    {
        return splitNetworkAudioByRole ? createJamAudio : networkAudio;
    }

    const AudioDevicePreference& joinAudio() const noexcept
    {
        return splitNetworkAudioByRole ? joinJamAudio : networkAudio;
    }

    AudioDevicePreference& createAudio() noexcept
    {
        return splitNetworkAudioByRole ? createJamAudio : networkAudio;
    }

    AudioDevicePreference& joinAudio() noexcept
    {
        return splitNetworkAudioByRole ? joinJamAudio : networkAudio;
    }
};

class UserPreferencesStore final {
public:
    static UserPreferences load();
    static void save(const UserPreferences& preferences);
    static QString filePath();
};
