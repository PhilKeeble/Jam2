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
    int prefillFrames = 64;
    int playbackMaxFrames = 1024;
    int captureRingFrames = 4096;
    int playbackRingFrames = 4096;
    bool driftCorrection = true;
    double driftSmoothing = 0.02;
    int driftDeadbandPpm = 25;
    int driftMaxCorrectionPpm = 500;
    bool sampleTimePlayout = true;
    int playoutDelayFrames = 64;
    int jitterBufferFrames = 64;
    int jitterBufferMaxFrames = 512;
    bool adaptiveCushion = true;
    int adaptiveTargetFrames = 64;
    int adaptiveMinFrames = 64;
    int adaptiveMaxFrames = 512;
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
    double silenceThresholdDb = -50.0;
    int tailSilenceMs = 1000;
    bool trimLeading = true;
    bool trimTrailing = true;
};

struct JamRecordingPreference {
    bool promptForName = true;
    QString completionAction = QStringLiteral("ask");
    bool importMix = true;
    bool importMyInput = false;
    bool importTheirInput = false;
    bool importInputsMix = false;
    bool importMetronome = false;
};

struct JamMixTrackRecordingPreference {
    bool includeBackingTrack = false;
    bool includeMetronome = false;
};

struct RecordingPreference {
    QString preferredMode = QStringLiteral("input");
    InputRecordingPreference input;
    LoopbackRecordingPreference loopback;
    JamRecordingPreference jam;
    JamMixTrackRecordingPreference jamMixTrack;
};

struct GeneralPreference {
    QString startupView = QStringLiteral("performance");
    int bpm = 80;
    int meterNumerator = 4;
    int meterDenominator = 4;
    int tempoPulseUnits = 1;
    int clickDivision = 1;
    bool generateIdeaOnStartup = false;
};

struct IdeaPreference {
    int parts = 0;
    int key = -1;
    QString styleId;
    QString profileId;
    QString meterId;
    int bars = 0;
    bool exactBpm = false;
    int bpm = 80;
    int complexity = 2;
    bool renderWavsOnStartup = false;
    bool renderChords = true;
    bool renderDrums = true;
    bool renderMelody = false;
    bool renderBass = true;
    bool renderSupport = true;
    int chordVoicing = 0;
    int drumKit = 0;
    bool grooveUseIdeaTiming = true;
    int grooveBars = 0;
};

struct LevelPreference {
    int sendDb = 0;
    bool monitorInput = false;
    int monitorDb = 0;
    int metronomeDb = -10;
    int masterDb = 0;
    int remotePeerDb = 0;
    int backingTrackDb = -3;
};

struct MetronomePreference {
    int sound = 0;
    QString mode = QStringLiteral("shared-grid");
    double compensationMaxMs = 250.0;
    double compensationSmoothingMs = 750.0;
    double compensationDeadbandMs = 1.0;
    double compensationSlewMsPerSecond = 40.0;
};

struct ViewPreference {
    bool performanceChordPreview = true;
    bool performanceBeatPreview = true;
    bool chordFocusCurrentBar = false;
    bool drumFocusCurrentBar = false;
    int guitarStrings = 6;
    bool guitarDropTuning = false;
    bool trackGridLock = true;
    bool trackLoop = true;
    double trackSpeed = 1.0;
    int trackPitch = 0;
    bool focusFrequencyEnabled = false;
    QString focusPreset = QStringLiteral("custom");
    int focusFrequencyHz = 120;
};

struct SyncPreference {
    bool trackLanes = true;
    bool autoShareWavs = true;
    bool globalPlayback = true;
    int generatedIdeas = 1;
    bool metronomeState = false;
    bool recordings = true;
};

struct UserPreferences {
    static constexpr int kSchemaVersion = 8;
    AudioDevicePreference localAudio;
    AudioDevicePreference networkAudio;
    bool splitNetworkAudioByRole = false;
    AudioDevicePreference createJamAudio;
    AudioDevicePreference joinJamAudio;
    CreatePreference create;
    JoinPreference join;
    LoggingPreference logging;
    RecordingPreference recording;
    GeneralPreference general;
    IdeaPreference ideas;
    LevelPreference levels;
    MetronomePreference metronome;
    ViewPreference views;
    SyncPreference sync;

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
    static bool setFilePathForTesting(const QString& path, QString& error);
};
