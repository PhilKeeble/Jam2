#include "MainWindow.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>

#include <memory>
#include <algorithm>
#include <utility>

namespace {
struct ListeningRenderJob {
    PreparedMixResult result;
    ~ListeningRenderJob() { if (!result.path.isEmpty()) QFile::remove(result.path); }
};
}

void MainWindow::initializeListeningTools()
{
    using namespace jam2::gui;
    ListeningToolsController::Callbacks callbacks;
    callbacks.context = [this] {
        ListeningToolsController::Context state;
        state.sync = jam2JamSyncAllows(jamSyncPolicy_, JamSyncRoute::ListeningTools);
        state.network = jam2_.isNetworkRunning();
        state.creator = !state.network || sessionController_.isServer();
        state.engineRunning = jam2_.isRunning();
        state.protectedRecording = sharedRecordingProtected();
        state.transportChanging = pendingBankIndex_ >= 0 || sharedBankLaunch_.active();
        state.playing = trackRecordingWorkflow_.globalTransportPlaying();
        state.bank = looperProject_.activeBankIndex();
        state.hasSources = PreparedMixRenderer::hasRenderableSources(looperProject_, state.bank);
        state.contentReady = pendingSongSet_.isEmpty() && pendingLooperAssetHashes_.isEmpty() &&
            !preparedMixLifecycle_.workerRunning();
        state.localToken = meshPeerToken_;
        for (auto it = meshPeerEndpoints_.cbegin(); it != meshPeerEndpoints_.cend(); ++it)
            if (it.key() != state.localToken) state.peers.insert(it.key());
        state.engine = jam2_.engineSnapshot();
        PlaybackGrid grid;
        const auto& pattern = state.engine.metronome_pattern;
        grid.setPattern(pattern.bpm, pattern.beats_per_bar, pattern.division, pattern.tempo_pulse_units);
        const auto offset = state.engine.metronome_render_offset_frames;
        const int sampleRate = static_cast<int>(state.engine.sample_rate);
        grid.updateEngine(state.engine.engine_frame,
            jam2::transport_musical_frame_from_raw(state.engine.engine_frame, offset),
            state.engine.metronome_epoch_frame, offset, sampleRate,
            state.engine.metronome_epoch_valid && sampleRate > 0, state.engine.playback_tempo);
        state.position = grid.position();
        // Play/Stop does not invalidate listening preparation. Only a section
        // transition needs to settle before choosing the content to render.
        const auto& track = trackController_.model();
        const double renderedSpeed = preparedMixLifecycle_.active().path.isEmpty()
            ? trackWorkspace_.listeningTools.appliedSettings().speedPpm / 1000000.0
            : preparedMixLifecycle_.active().listeningSpeed;
        state.sourceSeconds = state.engine.prepared_source_playing
            ? static_cast<double>(state.engine.prepared_source_frame) * renderedSpeed / std::max(1, sampleRate)
            : trackRecordingWorkflow_.currentTransportPositionMs(state.position, track.durationMs) / 1000.0;
        state.loopEnabled = track.loopEnabled;
        state.loopStartSourceSeconds = track.loopStartSeconds >= 0.0 ? track.loopStartSeconds * renderedSpeed : -1.0;
        state.loopEndSourceSeconds = track.loopEndSeconds >= 0.0 ? track.loopEndSeconds * renderedSpeed : -1.0;
        const QByteArray lanes = QJsonDocument(looperProject_.toJson()).toJson(QJsonDocument::Compact);
        state.sourceSignature = QStringLiteral("%1:%2:%3:%4:%5:%6")
            .arg(chordModel_.revision()).arg(QString::fromLatin1(QCryptographicHash::hash(lanes, QCryptographicHash::Sha256).toHex()))
            .arg(activeTrackSampleRate()).arg(track.loopEnabled)
            .arg(track.loopStartSeconds, 0, 'g', 16).arg(track.loopEndSeconds, 0, 'g', 16);
        return state;
    };
    callbacks.send = [this](const QJsonObject& message, const QString& peer) {
        if (!jamSyncAllowsControlMessage(message)) return false;
        return peer.isEmpty() ? sessionController_.send(message) : sendControlTo(peer, message);
    };
    callbacks.prepare = [this](const ListeningToolsSettings& settings, int generation, int bank) {
        return prepareListeningMix(settings, generation, bank);
    };
    callbacks.submit = [this](const jam2::EngineCommand& command) { return jam2_.submit(command); };
    callbacks.applied = [this](const ListeningToolsSettings& settings, const std::optional<PreparedMixResult>& result) {
        adoptListeningMix(settings, result);
        if (trackWorkspace_.playAfterListening && trackWorkspace_.listeningTools.playbackReady()) {
            QTimer::singleShot(0, this, [this] {
                if (trackWorkspace_.playAfterListening && trackWorkspace_.listeningTools.playbackReady()) playTrack();
            });
        }
    };
    callbacks.selected = [this](const ListeningToolsSettings& settings) {
        settings.applyTo(trackController_.model());
        updateTrackControls();
    };
    callbacks.rejected = [this](const ListeningToolsSettings& settings, const QString& error) {
        trackWorkspace_.playAfterListening = false;
        settings.applyTo(trackController_.model());
        updateTrackControls();
        appendLog(error);
    };
    callbacks.log = [this](const QString& message) { appendLog(message); };
    trackWorkspace_.listeningTools.setCallbacks(std::move(callbacks));
    trackWorkspace_.listeningTools.initialize(ListeningToolsSettings::fromModel(trackController_.model()));
}

void MainWindow::editListeningTools(bool commitNow)
{
    trackWorkspace_.listeningTools.edit(jam2::gui::ListeningToolsSettings::fromModel(trackController_.model()), commitNow);
}

bool MainWindow::prepareListeningMix(const jam2::gui::ListeningToolsSettings& settings, int generation, int bank)
{
    preparedMixLifecycle_.invalidateAll();
    const LooperProject project = looperProject_;
    const QString projectFolder = projectPersistence_.projectFolder();
    SharedTrackModel track = trackController_.model();
    settings.applyTo(track);
    const int sampleRate = activeTrackSampleRate();
    const qint64 exactFrames = bankExactOutputFrames(bank, sampleRate);
    const QFileInfo ordinary(PreparedMixRenderer::outputPath(projectPersistence_.workspaceFolder(), bank, generation, QCoreApplication::applicationPid()));
    const QString path = QDir(ordinary.absolutePath()).absoluteFilePath(QStringLiteral("listening-") + ordinary.fileName());
    auto job = std::make_shared<ListeningRenderJob>();
    return startFileWorkerTask([project, projectFolder, track, sampleRate, exactFrames, bank, path, job] {
        job->result = PreparedMixRenderer::render(project, projectFolder, sampleRate, path, track, bank, exactFrames);
    }, [this, generation, job] {
        trackWorkspace_.listeningTools.prepared(generation, std::exchange(job->result, {}));
    }, [this, generation](const QString& error) {
        PreparedMixResult result;
        result.error = error;
        trackWorkspace_.listeningTools.prepared(generation, std::move(result));
    });
}

void MainWindow::adoptListeningMix(const jam2::gui::ListeningToolsSettings& settings, const std::optional<PreparedMixResult>& result)
{
    auto& track = trackController_.model();
    const double previousSpeed = preparedMixLifecycle_.active().path.isEmpty() ? track.speed : preparedMixLifecycle_.active().listeningSpeed;
    const double speed = settings.speedPpm / 1000000.0;
    if (track.loopStartSeconds >= 0.0) track.loopStartSeconds *= previousSpeed / speed;
    if (track.loopEndSeconds >= 0.0) track.loopEndSeconds *= previousSpeed / speed;
    settings.applyTo(track);
    preparedMixLifecycle_.invalidateAll();
    if (result) {
        const auto cached = preparedMixLifecycle_.cacheResult(*result, looperProject_.banks().size());
        if (cached.status == jam2::gui::PreparedMixCacheStatus::Cached && result->bankIndex == looperProject_.activeBankIndex()) {
            preparedMixLifecycle_.activateCachedBank(result->bankIndex, looperProject_.banks().size(), QFileInfo::exists(result->path));
            registerTransientTrackWav(result->path);
            track.fileName = QStringLiteral("Prepared Section %1").arg(looperProject_.banks().at(result->bankIndex).id);
            track.filePath = result->path;
            track.fileBytes = result->fileBytes;
            track.sampleRate = result->sampleRate;
            track.sampleRateCompatible = true;
            track.durationMs = result->durationMs;
            track.sha256 = result->sha256;
            track.acceptedBpm = bankMetronomePattern(result->bankIndex).bpm;
            loadTrackWaveform();
        }
    }
    updateTrackControls();
    updateTrackTimeline();
    updatePlaybackGrid();
    discardObsoletePreparedMixPaths();
}
