#include "RecordingCalibrationController.hpp"
#include "RecordingCalibrationDialog.hpp"
#include "LaneRecordingDialog.hpp"
#include "GuiControlContract.hpp"
#include "GuiPresentation.hpp"
#include "recording_calibration.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
QPushButton* button(QWidget& root, const QString& id) {
    for (auto* b : root.findChildren<QPushButton*>()) if (jam2::gui::guiControlId(*b) == id) return b;
    throw std::runtime_error("Missing real button: " + id.toStdString());
}
QComboBox* combo(QWidget& root, const QString& id) {
    for (auto* c : root.findChildren<QComboBox*>()) if (jam2::gui::guiControlId(*c) == id) return c;
    throw std::runtime_error("Missing real combo: " + id.toStdString());
}
struct Fake {
    UserPreferences preferences;
    jam2::audio::StreamInfo stream;
    jam2::EngineSnapshot snapshot;
    std::array<jam2::audio::InputSourceSlotSnapshot,16> sources{};
    std::vector<jam2::EngineCommand> commands;
    bool network = false, reject = false;
    int starts = 0;
    std::size_t measuredChannel = jam2::audio::kNoInputChannel;
    Fake(const char* backend = "CoreAudio") {
        stream.device.backend = backend; stream.device.clsid = "stable-device-A"; stream.device.name = "Test Interface";
        stream.sample_rate = 48000; stream.buffer_size = 64; stream.channels.input = {0,1}; stream.channels.output = {0,1};
        snapshot.frame_clock_active = true; snapshot.engine_frame = 1000;
        snapshot.local_monitor_enabled = true; snapshot.metronome_enabled = true;
        sources[0] = {jam2::audio::InputSourceKind::Audio,0,jam2::audio::kNoInputChannel,1000000,true,true,false};
    }
    RecordingCalibrationController::Backend backend() {
        return {[this]{return snapshot;},[this]{return stream;},[this](std::size_t slot){return sources.at(slot);},
            [this]{return network;},[this](const jam2::EngineCommand& c){
                if (reject) return false;
                commands.push_back(c);
                if (c.type == jam2::EngineCommandType::SetRecordingCalibration)
                    snapshot.device_recording_calibration_frames = c.enabled ? c.signed_value : 0;
                return true;
            },[this](std::size_t input,QString&){
                ++starts; measuredChannel=input; auto c=std::make_shared<jam2::audio::RecordingCalibrationCapture>(48000);
                require(c->start(input),"fake capture starts at the real asynchronous boundary");return c;
            }};
    }
};
jam2::audio::RecordingCalibrationCapture::Result measured() {
    jam2::audio::RecordingCalibrationCapture::Result r;
    r.ok=true;r.measured_frames=180;r.reported_frames=200;r.adjustment_frames=-20;
    r.spread_frames=1;r.detected_clicks=6;r.peak=.2;return r;
}
void testPersistenceAndConfiguration(const QString& file) {
    QString error;require(UserPreferencesStore::setFilePathForTesting(file,error),"isolated settings accepted");
    for (const char* backend : {"CoreAudio","ASIO","WASAPI"}) {
        Fake f(backend);RecordingCalibrationController c(f.preferences,f.backend());
        const auto key=c.key(0);
        require(c.save(key,RecordingCalibrationMethod::Cable,measured(),error),"valid measured profile saves");
        Fake restarted(backend);restarted.preferences=UserPreferencesStore::load();
        RecordingCalibrationController restored(restarted.preferences,restarted.backend());restored.update();
        require(restarted.commands.size()==1 && restarted.commands.back().enabled && restarted.commands.back().signed_value==-20,
            "profile survives restart and is automatically applied to its device and config");
        const auto appliedCommands = restarted.commands.size(); restored.update();
        require(restarted.commands.size()==appliedCommands,"matching profile is not resubmitted after acknowledgement");
        restarted.snapshot.device_recording_calibration_frames=0; ++restarted.snapshot.engine_frame;
        restored.update(); require(restarted.commands.size()==appliedCommands+1 && restarted.commands.back().enabled,
            "engine restart restores calibration even if the new frame counter already passed the old one");
        const auto config= restarted.stream;
        const auto mismatch=[&] {
            restored.update();require(!restarted.commands.back().enabled,"different configuration clears calibration");
            restarted.stream=config;restored.update();require(restarted.commands.back().enabled,"returning to matching configuration restores calibration");
        };
        restarted.stream.device.clsid="different-device";mismatch();
        if (std::string(backend) == "WASAPI") {
            restarted.stream.wasapi.input_endpoint_id="different-input";mismatch();
            restarted.stream.input_period_frames=240;mismatch();
            restarted.stream.input_mix_sample_rate=44100;mismatch();
            restarted.stream.wasapi.capture_target_ms=30;mismatch();
            restarted.stream.low_latency_shared=true;mismatch();
        }
        restarted.stream.device.backend="different-backend";mismatch();
        restarted.stream.sample_rate=44100;mismatch();restarted.stream.buffer_size=128;mismatch();
        restarted.stream.channels.input={1,0};mismatch();restarted.stream.channels.output={2,3};mismatch();
        restarted.sources[1]={jam2::audio::InputSourceKind::Audio,1,jam2::audio::kNoInputChannel,1000000,true,true,false};
        require(restored.prepareForSource(1) && restarted.commands.back().enabled &&
                restored.status(1).contains("Device calibrated"),
            "another physical input inherits the same device calibration");
        restarted.snapshot.device_recording_calibration_frames=0;
        restarted.reject=true;
        require(!restored.prepareForSource(1),"take preparation reports a rejected calibration command");
        restarted.reject=false;
        restored.update();
        restarted.sources[0].renderer_attached=true;
        require(restored.channelForSource(0)==0,"audio plugin retains physical input identity");
        restarted.sources[0].kind=jam2::audio::InputSourceKind::MidiInstrument;
        const auto beforeMidi=restarted.commands.size();restored.update();restored.update();
        require(restarted.commands.back().enabled && !restored.channelForSource(0) && restarted.commands.size()==beforeMidi,
            "MIDI selection retains the device binding while the callback excludes MIDI-only recordings");
        restarted.sources[0].kind=jam2::audio::InputSourceKind::Audio;
        restarted.sources[1]={jam2::audio::InputSourceKind::MidiInstrument,jam2::audio::kNoInputChannel,jam2::audio::kNoInputChannel,1000000,true,true,true};
        require(restored.channelForSource(std::nullopt)==0,"combined audio/MIDI can measure the device through its audio input");
        auto newMeasurement=measured();newMeasurement.measured_frames=210;newMeasurement.adjustment_frames=10;
        require(restored.save(restored.key(1),RecordingCalibrationMethod::Cable,newMeasurement,error) &&
                restarted.preferences.recordingCalibrations.size()==1 &&
                restored.status(0).contains("0.21 ms"),
            "measurement through another input replaces the device profile for all inputs");
        const auto oldCount=restarted.preferences.recordingCalibrations.size();auto bad=measured();bad.ok=false;
        require(!restored.save(restored.key(0),RecordingCalibrationMethod::Microphone,bad,error) && restarted.preferences.recordingCalibrations.size()==oldCount,
            "failed measurement retains the previous calibration");
        require(restored.reset(restored.key(0),error),"profile reset persists");
        require(UserPreferencesStore::load().recordingCalibrations.empty(),"reset survives restart");
    }
    QSettings corrupt(file,QSettings::IniFormat);corrupt.beginWriteArray("recording_calibrations",1);corrupt.setArrayIndex(0);
    corrupt.setValue("backend","ASIO");corrupt.setValue("device_id","bad");corrupt.setValue("adjustment_frames","not-a-number");corrupt.endArray();corrupt.sync();
    require(UserPreferencesStore::load().recordingCalibrations.empty(),"malformed/incomplete persisted calibration is ignored");
}
void testLegacyDeviceProfiles(const QString& file) {
    QString error;require(UserPreferencesStore::setFilePathForTesting(file,error),"migration settings accepted");
    Fake f;RecordingCalibrationController controller(f.preferences,f.backend());
    auto r=measured();
    RecordingCalibrationProfile older{controller.key(0),RecordingCalibrationMethod::Cable,
        r.adjustment_frames,r.measured_frames,r.reported_frames,r.spread_frames,r.detected_clicks,"2026-10-17T12:00:00Z"};
    auto newer=older;newer.key=controller.key(1);newer.calibratedAt="2026-10-18T12:00:00Z";
    newer.adjustmentFrames=10;newer.measuredFrames=210;
    f.preferences.recordingCalibrations={newer,older};
    require(UserPreferencesStore::saveChecked(f.preferences),"legacy per-input profiles persist");
    f.preferences=UserPreferencesStore::load();
    require(f.preferences.recordingCalibrations.size()==1 && f.preferences.recordingCalibrations.front().adjustmentFrames==10,
        "latest legacy per-input measurement becomes the device profile regardless of array order");
    controller.update();require(f.commands.back().enabled && f.commands.back().signed_value==10,
        "legacy input-two measurement applies to the whole device");
    require(controller.reset(controller.key(0),error) && UserPreferencesStore::load().recordingCalibrations.empty(),
        "reset from another input removes the device calibration");
}
class Renderer final : public jam2::audio::InputSourceRenderer {
public:
    bool render_mono(const jam2::audio::InputSourceRenderRequest&,std::span<std::int32_t> output) noexcept override {std::fill(output.begin(),output.end(),0);return true;}
    std::size_t latency_frames(std::size_t) const noexcept override{return 23;}
};
void testSourceCompensation() {
    using namespace jam2::audio;
    InputSourceRouter router(64,3);StreamControl control;control.input_source_router=&router;
    control.input_latency_frames=100;control.output_latency_frames=50;control.recording_latency_adjustment_frames=7;
    set_recording_calibration(control,-20);Renderer renderer;
    std::array<std::int32_t,64> zeros{};std::array<const std::int32_t*,3> inputs{zeros.data(),zeros.data(),zeros.data()};
    const auto process=[&]{require(router.process(inputs,64,0,48000,zeros),"production input router renders source timing");};
    require(router.configure(0,{InputSourceKind::Audio,0,kNoInputChannel,1000000,true,nullptr}),"physical audio config accepted");
    router.set_recording_source(0);process();require(input_take_compensation_frames(control)==137,"physical recording adds calibration and manual offset once");
    require(router.configure(1,{InputSourceKind::Audio,1,kNoInputChannel,1000000,true,nullptr}),"second device input config accepted");
    router.set_recording_source(1);process();require(input_take_compensation_frames(control)==137,
        "switching device input retains the device correction without a controller update");
    router.set_recording_source(kCombinedInputSources);process();require(input_take_compensation_frames(control)==137,
        "two device inputs share one calibration correction in the combined send");
    require(router.configure(2,{InputSourceKind::Audio,2,kNoInputChannel,1000000,true,nullptr}),"third device input config accepted");
    process();require(input_take_compensation_frames(control)==137,"three device inputs apply calibration once rather than per source");
    router.clear(1);router.clear(2);
    require(router.configure(0,{InputSourceKind::Audio,0,1,1000000,true,nullptr}),"stereo device input config accepted");
    router.set_recording_source(0);process();require(input_take_compensation_frames(control)==137,"stereo input uses device calibration");
    require(router.configure(0,{InputSourceKind::Audio,0,kNoInputChannel,1000000,true,&renderer}),"audio plugin config accepted");
    process();require(input_take_compensation_frames(control)==160,"audio plugin preserves hardware calibration and adds plugin latency once");
    require(router.configure(0,{InputSourceKind::MidiInstrument,kNoInputChannel,kNoInputChannel,1000000,true,&renderer}),"MIDI plugin config accepted");
    process();require(input_take_compensation_frames(control)==80 && applied_recording_calibration_frames(control)==0,
        "MIDI output uses output/plugin latency but skips physical input latency and calibration");
    require(router.configure(1,{InputSourceKind::Audio,0,kNoInputChannel,1000000,true,nullptr}),"mixed physical source config accepted");
    router.set_recording_source(kCombinedInputSources);process();require(input_take_compensation_frames(control)==160,
        "mix containing physical audio applies device correction once alongside plugin latency");
    router.set_recording_source(1);process();set_recording_calibration(control,500);
    require(input_take_compensation_frames(control)==657,"positive device calibration is independent of input channel identity");
    set_recording_calibration(control,-1000);require(input_take_compensation_frames(control)==0,"negative calibration cannot underflow recording time");
    router.clear(0);router.clear(1);require(!router.process(inputs,64,0,48000,zeros) &&
        applied_recording_calibration_frames(control)==0,"unavailable input cannot inherit device calibration");
}
void testEngineCommandApplication() {
    using namespace jam2::audio;
    InputSourceRouter router(8192,2);
    require(router.configure(0,{InputSourceKind::Audio,0,kNoInputChannel,1000000,true,nullptr}),"engine physical source config accepted");
    router.set_recording_source(0);
    Renderer renderer;
    jam2::Engine engine; jam2::EngineConfig config;
    config.backend=jam2::EngineAudioBackend::Headless; config.sample_rate=48000;
    config.audio_buffer_frames=64; config.channels.input={0,1}; config.channels.output={0};
    config.test_input=jam2::EngineTestInput::Tone440; config.input_source_router=&router;
    engine.start(config);
    const auto wait=[&](std::int64_t calibration,std::uint64_t compensation) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(std::chrono::steady_clock::now()<deadline) {
            const auto s=engine.snapshot();
            if(s.recording_calibration_adjustment_frames==calibration && s.recording_latency_compensation_frames==compensation) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };
    jam2::EngineCommand command; command.type=jam2::EngineCommandType::SetRecordingCalibration;
    command.enabled=true; command.signed_value=20;
    require(engine.submit(command) && wait(20,20),"production engine command applies calibration to the selected input");
    require(router.configure(1,{InputSourceKind::Audio,1,kNoInputChannel,1000000,true,nullptr}),"engine enables second physical device input");
    router.set_recording_source(kCombinedInputSources);
    require(wait(20,20),"production combined callback applies the device calibration once for multiple physical inputs");
    router.set_recording_source(1);require(wait(20,20),"production callback retains correction on another physical input");
    router.set_recording_source(0);
    require(router.configure(0,{InputSourceKind::MidiInstrument,kNoInputChannel,kNoInputChannel,1000000,true,&renderer}),"engine switches to MIDI plugin");
    require(wait(0,23),"production callback drops physical calibration on MIDI and retains plugin latency");
    require(engine.snapshot().device_recording_calibration_frames==20,"MIDI-only application does not discard the saved device correction");
    router.set_recording_source(kCombinedInputSources);
    require(wait(20,43),"combined audio/MIDI applies device and plugin timing once");
    router.set_recording_source(0);
    require(router.configure(0,{InputSourceKind::Audio,0,kNoInputChannel,1000000,true,&renderer}),"engine switches to audio plugin");
    require(wait(20,43),"production callback restores physical calibration with audio plugin latency once");
    command.enabled=false; require(engine.submit(command) && wait(0,23),"production engine reset clears calibration");
    std::string error;
    require(!engine.beginRecordingCalibration(0,error) && !error.empty(),"headless backend rejects a physical measurement without opening hardware");
    engine.requestStop(); engine.join();
}
void testDialogAndRealControl(const QString& file) {
    QString error;require(UserPreferencesStore::setFilePathForTesting(file,error),"dialog settings accepted");
    Fake f;RecordingCalibrationController controller(f.preferences,f.backend());
    LaneRecordingDialog::Input input;input.laneName="Track 1";input.engineRunning=true;input.preferredMode="input";input.calibration=&controller;
    input.inputSources={{"Combined My Send mix",-1},{"Audio input 1",0},{"Stereo input",1},{"MIDI instrument",2}};
    f.sources[1]={jam2::audio::InputSourceKind::Audio,0,1,1000000,true,true,false};
    f.sources[2]={jam2::audio::InputSourceKind::MidiInstrument,jam2::audio::kNoInputChannel,jam2::audio::kNoInputChannel,1000000,true,true,true};
    LaneRecordingDialog arm(input,{},{});arm.show();button(arm,"looper.recording-dialog.advanced")->click();
    require(button(arm,"looper.recording.calibrate")->isEnabled(),"real arm dialog exposes device calibration for combined send with multiple audio/MIDI sources");
    auto* source=combo(arm,"looper.recording-dialog.input-source");
    source->setCurrentIndex(2);require(button(arm,"looper.recording.calibrate")->isEnabled(),"stereo source permits device calibration");
    source->setCurrentIndex(3);require(!button(arm,"looper.recording.calibrate")->isEnabled(),"MIDI-only source does not offer physical input calibration");
    source->setCurrentIndex(0);
    QTimer::singleShot(0,[&]{
        RecordingCalibrationDialog* popup=nullptr;
        for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=dynamic_cast<RecordingCalibrationDialog*>(widget))popup=candidate;
        require(popup,"real calibration button opens owned modal dialog");
        auto* testInput=combo(*popup,"looper.calibration.input");require(testInput->count()==2,"device calibration offers each available physical test input");
        testInput->setCurrentIndex(1);
        auto* method=combo(*popup,"looper.calibration.method");require(method->currentIndex()==0,"cable calibration is the default");
        method->setCurrentIndex(1);
        bool microphoneInstructions=false;for(auto* label:popup->findChildren<QLabel*>())microphoneInstructions|=label->text().contains("microphone",Qt::CaseInsensitive);
        require(microphoneInstructions,"microphone fallback has setup instructions");
        button(*popup,"looper.calibration.start")->click();require(f.starts==1 && !button(*popup,"looper.calibration.save")->isEnabled(),"start enters asynchronous capture and prevents premature save");
        require(f.measuredChannel==1 && !testInput->isEnabled(),"asynchronous measurement uses chosen cable input and locks routing during capture");
        popup->presentResult(measured());require(button(*popup,"looper.calibration.save")->isEnabled(),"successful result enables saving");
        const auto artifacts=qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT");if(!artifacts.isEmpty())require(popup->grab().save(QDir(artifacts).filePath("calibration-dialog.png")),"dialog visual evidence saves");
        button(*popup,"looper.calibration.save")->click();
    });
    button(arm,"looper.recording.calibrate")->click();
    require(f.preferences.recordingCalibrations.size()==1 && f.preferences.recordingCalibrations.front().method==RecordingCalibrationMethod::Microphone,
        "real popup Save persists chosen calibration method and result");
    require(f.preferences.recordingCalibrations.front().key.physicalInput==1 && controller.status(0).contains("Device calibrated"),
        "measurement on input two calibrates input one and combined send");
    RecordingCalibrationDialog cancelled(controller,0);cancelled.show();button(cancelled,"looper.calibration.start")->click();
    button(cancelled,"looper.calibration.cancel")->click();QApplication::processEvents();
    require(f.preferences.recordingCalibrations.size()==1,"cancelled run preserves previous saved profile");
    require(f.commands.back().type==jam2::EngineCommandType::SetMetronomeEnabled && f.commands.back().enabled,
        "transient monitoring/metronome settings restore after cancellation");
    cancelled.presentResult(measured());
    require(!button(cancelled,"looper.calibration.save")->isEnabled(),"late analysis completion cannot revive a dismissed calibration");
    f.network=true;require(!controller.unavailableReason().isEmpty(),"network session cannot leak test clicks into peers");f.network=false;
    f.snapshot.track_take.armed=true;require(!controller.unavailableReason().isEmpty(),"armed recording prevents calibration takeover");
}
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);installJam2Style();
    try {QTemporaryDir dir;require(dir.isValid(),"temporary directory available");
        testPersistenceAndConfiguration(dir.filePath("profiles.ini"));testLegacyDeviceProfiles(dir.filePath("legacy.ini"));
        testSourceCompensation();testEngineCommandApplication();testDialogAndRealControl(dir.filePath("dialog.ini"));
        std::cout<<"Recording calibration persistence, source policy and dialog tests passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
