#include "ArtifactReader.hpp"
#include "AudioBaselineAnalysis.hpp"
#include "LoopbackPortReservations.hpp"
#include "ProxyPollWait.hpp"
#include "SelectiveDeliveryProxy.hpp"
#include "UdpCandidateNetwork.hpp"
#include "UdpImpairmentProxy.hpp"
#include "audio_device.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {
using namespace jam2::test;
void require(bool okay, const QString& reason) { if (!okay) throw std::runtime_error(reason.toStdString()); }
QJsonObject readJson(const QString& path) {
    QJsonObject result; QString error; require(readJsonObject(path,result,error),error); return result;
}
void writeJson(const QString& path, const QJsonObject& value) {
    QFile f(path); require(f.open(QIODevice::WriteOnly),f.errorString());
    const auto bytes=QJsonDocument(value).toJson(QJsonDocument::Indented);
    require(f.write(bytes)==bytes.size(),QStringLiteral("write artifact ")+path);
    require(f.flush(),QStringLiteral("flush artifact ")+path+": "+f.errorString());
}
struct Config {
    int rate=44100, callback=32, packet=128, cleanMs=40000, impairedMs=230000;
    double baseMs=7.5, forwardJitterMs=3.6, reverseJitterMs=.5, recoveredLimitMs=40;
    double batchMs=0;
    int releasePpm=-1;
    DeliveryDelayEnvelope envelope;
    DeliveryDelayScope scope=DeliveryDelayScope::All;
    enum class GridMode { SharedGrid, LeaderAudio };
    GridMode gridMode=GridMode::SharedGrid;
    jam2::NetworkAudioFormat format=jam2::NetworkAudioFormat::Pcm24Mono;
    QStringList devices;
    // Hardware identities are explicit: ASIO names or stable CoreAudio UIDs.
    enum class Backend { Asio, CoreAudio };
    Backend backend=Backend::Asio;
    int jitterFrames=-1, adaptiveTarget=-1, adaptiveMin=-1, adaptiveMax=-1;
    std::optional<bool> driftCorrection;
    int statsIntervalMs=250;
    bool recordAudio=false;
    QString testInput="tone-440";
    double mappingErrorLimitMs=0;
    double underrunRateLimitMs=-1, trimRateLimitMs=-1;
    int quietAfterMs=0;
    int bpm=120;
    enum class ConnectionMode { ProxiedDirect, Direct, Server };
    ConnectionMode connectionMode=ConnectionMode::ProxiedDirect;
    QString signaller;
    QString tuningProfile="fast";
};
Config configuration(const QJsonObject& p) {
    require(p.value("schema")=="jam2-asio-delivery-profile-v1" ||
        p.value("schema")=="jam2-coreaudio-delivery-profile-v1","unexpected hardware profile schema");
    Config c;
    if (p.contains("drift_correction")) {
        require(p.value("drift_correction").isBool(),"drift_correction must be a boolean");
        c.driftCorrection=p.value("drift_correction").toBool();
    }
    c.statsIntervalMs=p.value("stats_interval_ms").toInt(250);
    require(c.statsIntervalMs>=100 && c.statsIntervalMs<=2000,"invalid stats interval");
    c.backend=p.value("schema")=="jam2-coreaudio-delivery-profile-v1" ? Config::Backend::CoreAudio : Config::Backend::Asio;
    c.tuningProfile=p.value("tuning_profile").toString("fast");
    require(c.tuningProfile=="fast" || c.tuningProfile=="moderate","invalid tuning profile");
    c.jitterFrames=p.value("jitter_buffer_frames").toInt(-1);
    c.adaptiveTarget=p.value("adaptive_playback_target_frames").toInt(-1);
    c.adaptiveMin=p.value("adaptive_playback_min_frames").toInt(-1);
    c.adaptiveMax=p.value("adaptive_playback_max_frames").toInt(-1);
    for (const auto frames:{c.jitterFrames,c.adaptiveTarget,c.adaptiveMin,c.adaptiveMax})
        require(frames>=-1 && frames<=4096,"invalid explicit buffer setting");
    const auto connection=p.value("connection_mode").toString("proxied-direct");
    require(connection=="proxied-direct" || connection=="direct" || connection=="server","invalid connection_mode");
    c.connectionMode=connection=="server" ? Config::ConnectionMode::Server :
        connection=="direct" ? Config::ConnectionMode::Direct : Config::ConnectionMode::ProxiedDirect;
    c.signaller=p.value("signaller_executable").toString(qEnvironmentVariable("JAM2_RENDEZVOUS_TEST_SERVER"));
    c.recordAudio=p.value("record_audio").toBool(false);
    c.testInput=p.value("test_input").toString("tone-440");
    c.mappingErrorLimitMs=p.value("maximum_mapping_error_ms").toDouble(0);
    c.underrunRateLimitMs=p.value("maximum_underrun_ms_per_minute").toDouble(-1);
    c.trimRateLimitMs=p.value("maximum_trim_ms_per_minute").toDouble(-1);
    c.quietAfterMs=p.value("assert_quiet_after_ms").toInt(0);
    c.bpm=p.value("bpm").toInt(120);require(c.bpm>=1 && c.bpm<=400,"invalid test BPM");
    require(c.testInput=="tone-440" || c.testInput=="pulse-1s","invalid recorded test signal");
    c.rate=p.value("sample_rate").toInt(); c.callback=p.value("callback_frames").toInt();
    c.packet=p.value("packet_frames").toInt(); c.cleanMs=p.value("clean_duration_ms").toInt();
    c.impairedMs=p.value("impaired_duration_ms").toInt(); c.baseMs=p.value("base_one_way_delay_ms").toDouble(-1);
    c.forwardJitterMs=p.value("forward_jitter_ms").toDouble(-1);
    c.reverseJitterMs=p.value("reverse_jitter_ms").toDouble(-1);
    c.recoveredLimitMs=p.value("recovered_buffer_limit_ms").toDouble(-1);
    c.envelope.repeatMs=p.value("repeat_ms").toDouble(-1);
    c.batchMs=p.value("batch_interval_ms").toDouble(0);
    c.releasePpm=p.value("adaptive_playback_release_ppm").toInt(-1);
    const auto scope=deliveryDelayScope(p.value("delay_scope").toString("all").toStdString());
    require(bool(scope),"invalid delay_scope");c.scope=*scope;
    const auto mode=p.value("metronome_mode").toString("shared-grid");
    require(mode=="shared-grid" || mode=="leader-audio","invalid diagnostic metronome mode");
    c.gridMode=mode=="shared-grid" ? Config::GridMode::SharedGrid : Config::GridMode::LeaderAudio;
    const auto format=jam2::protocol::parse_audio_format(p.value("network_audio_format").toString("pcm24").toStdString());
    require(bool(format),"invalid diagnostic PCM format");c.format=*format;
    for (const auto& value : p.value("delay_points").toArray()) {
        const auto a=value.toArray(); require(a.size()==2 && a[0].isDouble() && a[1].isDouble(),"delay point requires [time_ms, additional_delay_ms]");
        c.envelope.points.push_back({a[0].toDouble(),a[1].toDouble()});
    }
    for (const auto& value : p.value(c.backend==Config::Backend::CoreAudio ? "device_uids" : "device_names").toArray()) c.devices<<value.toString();
    require(c.rate>=8000 && c.rate<=192000 && c.callback>=16 && c.callback<=1024 &&
        (c.packet==64 || c.packet==128) && (c.cleanMs==0 || c.cleanMs>=15000) && c.cleanMs<=60000 &&
        c.impairedMs>=15000 && c.impairedMs<=600000 && c.baseMs>=0 && c.baseMs<=100 &&
        c.forwardJitterMs>=0 && c.forwardJitterMs<=100 && c.reverseJitterMs>=0 && c.reverseJitterMs<=100 &&
        c.recoveredLimitMs>0 && std::isfinite(c.underrunRateLimitMs) && c.underrunRateLimitMs>=-1 &&
        std::isfinite(c.trimRateLimitMs) && c.trimRateLimitMs>=-1 &&
        c.releasePpm>=-1 && c.releasePpm<=1000000 && c.batchMs>=0 && c.batchMs<=1000 && c.envelope.valid() && !c.envelope.points.empty() &&
        c.devices.size()==2 && !c.devices[0].isEmpty() && !c.devices[1].isEmpty() &&
        (c.backend==Config::Backend::CoreAudio || c.devices[0]!=c.devices[1]),"invalid bounded delivery/device profile");
    if (c.connectionMode!=Config::ConnectionMode::ProxiedDirect)
        require(c.scope==DeliveryDelayScope::None && c.baseMs==0 && c.forwardJitterMs==0 && c.reverseJitterMs==0 &&
            c.batchMs==0 && std::all_of(c.envelope.points.begin(),c.envelope.points.end(),[](const auto& p){return p.delayMs==0;}),
            "real mode comparisons require a clean unproxied path");
    if (c.connectionMode==Config::ConnectionMode::Server)
        require(QFileInfo(c.signaller).isFile(),"server comparison needs an explicit local signaller_executable");
    return c;
}
struct Child {
    QProcess process;
    QString root;
    ~Child() {
        if (process.state()!=QProcess::NotRunning) {
            process.terminate(); if (!process.waitForFinished(2000)) { process.kill(); process.waitForFinished(2000); }
        }
    }
};
QJsonObject directionStats(const DirectionProxyStats& s, const DirectionProxyStats& b={}) {
    return {{"received",qint64(s.packets+b.packets)},{"forwarded",qint64(s.forwarded+b.forwarded)},
        {"requested_delay_max_us",qint64(std::max(s.requestedDelayMaxUs,b.requestedDelayMaxUs))},
        {"release_lateness_max_us",qint64(std::max(s.releaseLatenessMaxUs,b.releaseLatenessMaxUs))},
        {"capacity_dropped",qint64(s.capacityDropped+b.capacityDropped)},{"send_errors",qint64(s.sendErrors+b.sendErrors)},
        {"receive_errors",qint64(s.receiveErrors+b.receiveErrors)},{"injected_loss",qint64(s.dropped+b.dropped)}};
}
QJsonObject trafficCounts(const DeliveryTrafficCounts& counts) {
    QJsonObject selected,baseline;
    const char* names[]={"audio","beacons","ping-pong","control"};
    for (std::size_t i=0;i<std::size_t(DeliveryTraffic::Count);++i) {
        selected[names[i]]=qint64(counts.selected[i]);baseline[names[i]]=qint64(counts.baseline[i]);
    }
    return {{"selected",selected},{"baseline",baseline}};
}
QJsonObject edgeEvidence(int a,int b,const SelectiveDeliveryProxy& proxy) {
    const auto s=proxy.selectedStats(),base=proxy.baselineStats();
    return {{"source",a},{"destination",b},
        {"forward",directionStats(s.serverToClient,base.serverToClient)},
        {"reverse",directionStats(s.clientToServer,base.clientToServer)},
        {"selected_forward",directionStats(s.serverToClient)},{"baseline_forward",directionStats(base.serverToClient)},
        {"forward_types",trafficCounts(proxy.forwardCounts())},{"reverse_types",trafficCounts(proxy.reverseCounts())},
        {"pending",qint64(s.pendingPackets+base.pendingPackets)},
        {"pending_high_water",qint64(s.pendingHighWater+base.pendingHighWater)}};
}
QJsonObject csvEvidence(const Child& child, const Config& c, int count, bool hardware, const QString& expectedDevice) {
    QDirIterator files(child.root,{"jam2_stats*.csv"},QDir::Files,QDirIterator::Subdirectories);
    require(files.hasNext(),"missing peer stats CSV: "+child.root);
    const auto path=files.next(); CsvTable csv; QString error; require(csv.read(path,error),error);
    const auto final=csv.rowsOfType("final"); require(final.size()==1,"peer must close normally with final stats");
    const auto f=final.front();
    require(csv.integer(f,"active_sample_rate")==c.rate && csv.integer(f,"active_audio_buffer_frames")==c.callback &&
        csv.integer(f,"session_frames_per_packet")==c.packet && csv.yes(f,"capture_clock_packet_pacing_active"),
        "test must exercise requested sample rate, small callbacks and capture-clock pacing");
    if (c.driftCorrection) require(csv.yes(f,"drift_correction")==*c.driftCorrection,
        "test did not apply its explicit drift-correction setting");
    if (c.releasePpm>=0) require(csv.integer(f,"adaptive_playback_release_ppm")==c.releasePpm,
        "test did not apply its explicit playback release setting");
    if (hardware) {
        const bool core=c.backend==Config::Backend::CoreAudio;
        require(csv.value(f,"device_backend")== (core ? "CoreAudio" : "ASIO"),"hardware backend differs from profile");
        require(core ? csv.value(f,"device_driver_id")==expectedDevice : csv.value(f,"device_name")==expectedDevice,
            "hardware test did not use its explicitly profiled device");
        if (core) require(csv.integer(f,"actual_audio_callback_frames_min")==c.callback &&
            csv.integer(f,"actual_audio_callback_frames_max")==c.callback,"actual hardware callback sizes differ from profile");
    }
    QVector<qsizetype> rows;
    for (auto r : csv.rowsOfType("periodic")) if (csv.integer(r,"network_active_peer_count")==count-1) rows.push_back(r);
    require(rows.size()>=10,"insufficient fully connected observations");
    // A peer can retain the connected count briefly after its partner closes.
    // Exclude the final two seconds rather than count shutdown as starvation.
    const double stopAt=csv.number(rows.back(),"elapsed_ms")-2000;
    while (!rows.isEmpty() && csv.number(rows.back(),"elapsed_ms")>stopAt) rows.removeLast();
    require(rows.size()>=10,"insufficient observations before shutdown exclusion");
    QJsonArray timeline;double mappingErrorPeak=0;
    const auto first=rows.front(), last=rows.back();
    const double seconds=(csv.number(last,"elapsed_ms")-csv.number(first,"elapsed_ms"))/1000.;
    require(seconds>5,"insufficient observed audio duration");
    for (auto r : rows) {
        QJsonObject item;
        for (const char* k : {"elapsed_ms","recv_packets","sent_packets","playback_ring_underruns",
                "mix_live_tail_trimmed_frames","mix_receive_recovery_events","total_current_buffered_ms",
                "rtt_avg_ms","rtt_max_ms","grid_mapping_error_frames","audio_packet_gap_max_ms",
                "audio_callbacks","audio_callback_interval_max_ms","receive_loop_gap_max_ms",
                "mix_missing_peer_frames","mix_late_after_release_frames","mix_receive_recovery_debt_frames",
                "raw_drift_ppm","drift_ppm","sequence_lost","mix_receive_recovery_completions",
                "audio_packet_gap_over_2x_count","audio_packet_gap_over_4x_count","jitter_avg_ms",
                "mix_worker_service_gap_us","mix_playback_stopping_frames","mix_adaptive_release_alignment_margin_frames",
                "audio_control_playback_ratio","audio_applied_playback_ratio","resampler_ratio",
                "jitter_buffer_depth_frames","playback_ring_readable_frames","mix_worker_runway_frames",
                "adaptive_playback_target_frames","adaptive_playback_padding_frames",
                "adaptive_delivery_gap_hold_remaining_ms","mix_partial_release_events","mix_partial_release_frames",
                "mix_recovery_safe_depth_frames","mix_recovery_low_water_frames",
                "mix_recovery_release_events","mix_recovery_retired_frames",
                "coreaudio_processor_overloads","coreaudio_timestamp_invalid_callbacks",
                "audio_callback_work_max_us","actual_audio_callback_frames_min","actual_audio_callback_frames_max"}) {
            require(!csv.value(r,k).isEmpty(),QString("missing measurement column: ")+k);
            item[k]=csv.number(r,k);
        }
        for (const char* k : {"drift_arrival_step_threshold_us", "drift_arrival_offset_adjustments", "drift_arrival_offset_us"})
            if (!csv.value(r,k).isEmpty()) item[k]=csv.number(r,k);
        item["mix_release_worker_guard_active"]=csv.yes(r,"mix_release_worker_guard_active");
        timeline.push_back(item);
        mappingErrorPeak=std::max(mappingErrorPeak,std::abs(csv.number(r,"grid_mapping_error_frames"))*1000./c.rate);
    }
    const double tailStart=csv.number(last,"elapsed_ms")-5000.;
    qsizetype tail=last; double tailBufferMax=0;
    for (auto r : rows) if (csv.number(r,"elapsed_ms")>=tailStart) {
        tail=std::min(tail,r); tailBufferMax=std::max(tailBufferMax,csv.number(r,"total_current_buffered_ms"));
    }
    const double tailSeconds=(csv.number(last,"elapsed_ms")-csv.number(tail,"elapsed_ms"))/1000.;
    require(tailSeconds>0,"missing final packet-flow window");
    const double tailPps=(csv.number(last,"recv_packets")-csv.number(tail,"recv_packets"))/tailSeconds;
    // The fake backend is sleep-driven; use its observed clock rather than
    // claiming that Windows sustained the nominal hardware rate.
    const double expectedPps=hardware ? double(c.rate)/c.packet*(count-1) :
        (csv.number(last,"audio_callbacks")-csv.number(tail,"audio_callbacks"))*c.callback/c.packet/tailSeconds*(count-1);
    QJsonArray failures;
    if (c.quietAfterMs>0) {
        qsizetype quietFirst=last;
        for (auto r:rows) if (csv.number(r,"elapsed_ms")>=c.quietAfterMs) quietFirst=std::min(quietFirst,r);
        if (csv.number(last,"elapsed_ms")-csv.number(quietFirst,"elapsed_ms")<5000)
            failures.append("insufficient post-stress quiet observations");
        for (const auto* field:{"playback_ring_underruns","mix_live_tail_trimmed_frames","mix_receive_recovery_events"})
            if (csv.number(last,field)!=csv.number(quietFirst,field)) failures.append(QString(field)+" continued after stress stopped");
    }
    if (c.mappingErrorLimitMs>0 && mappingErrorPeak>c.mappingErrorLimitMs)
        failures.append("clock-mapping correction exceeds the explicit profile limit");
    if (!(tailPps>expectedPps*.85 && tailPps<expectedPps*1.15)) failures.append("audio packet flow did not recover in the final five seconds");
    if (tailBufferMax>c.recoveredLimitMs) failures.append("final five-second buffering exceeds explicit profile limit");
    if (csv.integer(f,"capture_ring_overruns")!=0 || csv.integer(f,"udp_authentication_failed")!=0 ||
        csv.integer(f,"udp_send_fatal_errors")!=0) failures.append("capture overflow, authentication rejection or fatal send during test");
    QJsonObject result{{"csv",path},{"device",csv.value(f,"device_name")},{"connected_seconds",seconds},
        {"callback_work_max_us",csv.number(last,"audio_callback_work_max_us")},
        {"driver_output_latency_frames",csv.number(last,"driver_output_latency_frames")},
        {"callback_interval_max_ms",csv.number(last,"audio_callback_interval_max_ms")},
        {"grid_mapping_target_error_peak_ms",mappingErrorPeak},
        {"rtt_max_ms",csv.number(last,"rtt_max_ms")},{"arrival_gap_max_ms",csv.number(last,"audio_packet_gap_max_ms")},
        {"tail_received_packets_per_second",tailPps},{"tail_expected_packets_per_second",expectedPps},
        {"tail_buffer_max_ms",tailBufferMax},{"failures",failures},{"observations",timeline}};
    QVector<double> steadyBuffers;qsizetype steadyFirst=last;
    for (auto r:rows) if (csv.number(r,"elapsed_ms")>=10000)
        { steadyBuffers.push_back(csv.number(r,"total_current_buffered_ms"));steadyFirst=std::min(steadyFirst,r); }
    std::sort(steadyBuffers.begin(),steadyBuffers.end());
    if (!steadyBuffers.isEmpty()) {
        result["steady_buffer_median_ms"]=steadyBuffers[steadyBuffers.size()/2];
        result["steady_buffer_p99_ms"]=steadyBuffers[qsizetype((steadyBuffers.size()-1)*.99)];
    }
    for (const auto* field:{"playback_ring_underruns","mix_live_tail_trimmed_frames"})
        result[QString("steady_")+field+"_delta"]=csv.number(last,field)-csv.number(steadyFirst,field);
    for (const char* k : {"capture_ring_overruns","udp_authentication_failed","udp_send_fatal_errors"}) {
        require(!csv.value(f,k).isEmpty(),QString("missing final measurement: ")+k);
        result[k]=csv.number(f,k);
    }
    for (const char* k : {"playback_ring_underruns","mix_live_tail_trimmed_frames","mix_receive_recovery_events","sequence_lost"}) {
        const double delta=csv.number(last,k)-csv.number(first,k); result[QString(k)+"_delta"]=delta;
        result[QString("tail_")+k+"_delta"]=csv.number(last,k)-csv.number(tail,k);
        if (QString(k).endsWith("frames") || QString(k)=="playback_ring_underruns")
            result[QString(k)+"_ms_per_minute"]=delta*1000./c.rate*60./seconds;
    }
    if (c.underrunRateLimitMs>=0 && result.value("playback_ring_underruns_ms_per_minute").toDouble()>c.underrunRateLimitMs)
        failures.append("underrun time exceeds the explicit per-minute profile limit");
    if (c.trimRateLimitMs>=0 && result.value("mix_live_tail_trimmed_frames_ms_per_minute").toDouble()>c.trimRateLimitMs)
        failures.append("trimmed audio exceeds the explicit per-minute profile limit");
    result["failures"]=failures;
    return result;
}
QJsonObject recordedAudioEvidence(const Child& child,const Config& c) {
    const auto root=QDir(child.root).filePath("recording");
    const auto sidecar=readJson(QDir(root).filePath("recording.json"));
    require(sidecar.value("dropped_frames").toInteger()==0 && sidecar.value("writer_errors").toInteger()==0,
        "recording dropped audio or encountered writer errors");
    QJsonObject result{{"root",root},{"signal",c.testInput},{"sidecar",sidecar}};
    for (const auto* stem:{"my-input","their-input"}) {
        Pcm16MonoWav wav;QString error;
        require(readPcm16MonoWav(QDir(root).filePath(QString(stem)+".wav"),wav,error),error);
        require(wav.sampleRate==c.rate && wav.samples.size()>std::size_t(c.rate*10),"insufficient real callback audio recording");
        const auto begin=std::int64_t(c.rate*3),end=std::int64_t(wav.samples.size())-c.rate;
        int peak=0,maxStep=0;std::int64_t zeroRun=0,maxZeroRun=0;
        for (auto n=begin;n<end;++n) {
            const int sample=wav.samples[std::size_t(n)];peak=std::max(peak,std::abs(sample));
            maxStep=std::max(maxStep,std::abs(sample-int(wav.samples[std::size_t(n-1)])));
            zeroRun=std::abs(sample)<=1 ? zeroRun+1 : 0;maxZeroRun=std::max(maxZeroRun,zeroRun);
        }
        require(peak>20,"recorded signal missing");
        QJsonObject evidence{{"frames",qint64(wav.samples.size())},{"peak",peak},{"rms",rootMeanSquare(wav.samples,begin,end)},
            {"maximum_step",maxStep},{"longest_near_zero_ms",double(maxZeroRun)*1000/c.rate}};
        if (c.testInput=="tone-440") {
            evidence["tone_hz"]=crossingToneHz(wav.samples,c.rate,begin,end);
            evidence["second_difference_rms_pcm16"]=secondDifferenceRms(wav.samples,begin,end);
            qint64 jumps=0;
            for (auto n=begin;n<end;++n) jumps+=std::abs(int(wav.samples[std::size_t(n)])-int(wav.samples[std::size_t(n-1)]))>peak*.25;
            evidence["large_step_count"]=jumps;
        } else {
            const auto detected=detectEvents(wav.samples,std::max(4,peak/8),c.rate/2);
            QJsonArray events;for (auto frame:detected) if (frame>=begin && frame<end) events.append(qint64(frame));
            evidence["pulse_frames"]=events;
        }
        result[stem]=evidence;
    }
    return result;
}
QJsonObject audioTimingEvidence(const QJsonArray& evidence,const Config& c) {
    if (!c.recordAudio || c.testInput!="pulse-1s" || evidence.size()!=2) return {};
    std::array<double,2> phases{};double outputFrames=0;
    for (int i=0;i<2;++i) {
        const auto recording=evidence[i].toObject().value("recorded_audio").toObject();
        const auto events=[&](const char* stem) {std::vector<std::int64_t> result;
            for (const auto& frame:recording.value(stem).toObject().value("pulse_frames").toArray()) result.push_back(frame.toInteger());return result;};
        phases[i]=pulsePhaseFrames(events("my-input"),events("their-input"),c.rate);
        outputFrames+=evidence[i].toObject().value("driver_output_latency_frames").toDouble();
    }
    return {{"rendered_round_trip_ms",pulseRoundTripMs(phases[0],phases[1],c.rate)},
        {"directional_phase_frames",QJsonArray{phases[0],phases[1]}},
        {"reported_output_paths_ms",outputFrames*1000/c.rate},
        {"measurement","recorded real-callback audio round trip; excludes analog converters; opposite clock origins cancel"}};
}
void compareCleanBaseline(const QString& root,const QString& reference,const Config& c,bool equivalent=false) {
    const auto oldConfig=configuration(readJson(QDir(reference).filePath("configuration.json")));
    require(oldConfig.rate==c.rate && oldConfig.callback==c.callback && oldConfig.packet==c.packet &&
        oldConfig.format==c.format && oldConfig.testInput==c.testInput,"clean baseline contract differs");
    const auto reviewed=[&](const QString& directory) {
        const auto review=QDir(directory).filePath("audio-review.json");
        return readJson(QFileInfo::exists(review) ? review : QDir(directory).filePath("summary.json"));
    };
    const auto current=reviewed(root),baseline=reviewed(reference);
    const auto peers=current.value("peers").toArray(),oldPeers=baseline.value("peers").toArray();
    require(peers.size()==oldPeers.size(),"clean baseline peer count differs");
    QJsonArray differences,failures;
    // Device-clock phase and callback scheduling vary between runs. The
    // deterministic test separately requires no clean latency increase at all.
    const double callbackMs=c.callback*1000./c.rate;
    for (qsizetype i=0;i<peers.size();++i) {
        const auto p=peers[i].toObject(),old=oldPeers[i].toObject();
        require(p.value("device")==old.value("device"),"clean baseline device roles differ");
        QJsonObject delta{{"device",p.value("device")}};
        for (const auto* field:{"steady_buffer_median_ms","steady_buffer_p99_ms"}) {
            require(p.contains(field) && old.contains(field),"baseline latency observations missing");
            const auto difference=p.value(field).toDouble()-old.value(field).toDouble();delta[field]=difference;
            if ((equivalent ? std::abs(difference) : difference)>callbackMs) failures.append(QString(field)+" differs by more than one callback");
        }
        for (const auto* field:{"steady_playback_ring_underruns_delta","steady_mix_live_tail_trimmed_frames_delta"}) {
            require(p.contains(field) && old.contains(field),"baseline steady counters missing; review retained recordings first");
            const auto difference=p.value(field).toDouble()-old.value(field).toDouble();delta[field]=difference;
            if (difference>0) failures.append(QString(field)+" regressed from clean baseline");
        }
        if (c.testInput=="tone-440") {
            const auto a=p.value("recorded_audio").toObject().value("their-input").toObject();
            const auto b=old.value("recorded_audio").toObject().value("their-input").toObject();
            require(a.contains("second_difference_rms_pcm16") && b.contains("second_difference_rms_pcm16"),
                "baseline waveform curvature missing; review retained recordings first");
            for (const auto* field:{"large_step_count","longest_near_zero_ms","second_difference_rms_pcm16"}) {
                const auto difference=a.value(field).toDouble()-b.value(field).toDouble();delta[field]=difference;
                const double tolerance=QString(field)=="longest_near_zero_ms" ? 1000./c.rate :
                    (QString(field)=="second_difference_rms_pcm16" ? 1 : 0);
                if (difference>tolerance+1e-8) failures.append(QString(field)+" regressed in recorded clean audio");
            }
        }
        differences.append(delta);
    }
    double roundTripDelta=0;
    if (c.testInput=="pulse-1s") {
        auto old=baseline;
        const auto reviewPath=QDir(reference).filePath("audio-review.json");
        if (QFileInfo::exists(reviewPath)) old=readJson(reviewPath);
        require(old.value("audio_timing").toObject().contains("rendered_round_trip_ms"),"baseline pulse measurement missing");
        roundTripDelta=current.value("audio_timing").toObject().value("rendered_round_trip_ms").toDouble()-
            old.value("audio_timing").toObject().value("rendered_round_trip_ms").toDouble();
        if ((equivalent ? std::abs(roundTripDelta) : roundTripDelta)>2*callbackMs) failures.append("recorded round trip differs by more than one callback per direction");
    }
    writeJson(QDir(root).filePath("baseline-comparison.json"),{{"reference",reference},{"callback_tolerance_ms",callbackMs},
        {"round_trip_delta_ms",roundTripDelta},{"peers",differences},{"failures",failures}});
    require(failures.isEmpty(),"clean latency or recorded audio regressed; inspect baseline-comparison.json");
}
void runCase(const QString& executable, const QString& root, const Config& c,
    const std::vector<int>& devices, bool impaired, bool hardware) {
    require(QDir().mkpath(root),"create case artifacts");
    const int count=int(devices.size()), duration=impaired ? c.impairedMs : c.cleanMs;
    const bool realMode=c.connectionMode!=Config::ConnectionMode::ProxiedDirect;
    const bool serverMode=c.connectionMode==Config::ConnectionMode::Server;
    LoopbackPortReservations ports; QString error; require(ports.reserve(count,error),error);
    std::vector<quint16> localPorts;
    for (int i=0;i<count;++i) localPorts.push_back(ports.port(i));
    const QString sid="0123456789abcdef", key="00112233445566778899aabbccddeeff";
    CandidateNetwork network(localPorts[0],CandidateNetwork::IsolatedHosts{sid.toULongLong(nullptr,16),QByteArray::fromHex(key.toLatin1())});
    network.blockFirst=false; network.impairMultiplex=false;
    struct Edge { int a,b; std::unique_ptr<SelectiveDeliveryProxy> proxy; };
    std::vector<Edge> edges;
    QStringList identities;
    for (int i=0;i<count;++i) identities<<QString(32,QChar('1'+i));
    for (int a=0;!realMode && a<count;++a) for (int b=a+1;b<count;++b) {
        DirectionImpairment forward,reverse;
        if (impaired) {
            forward.fixedDelayMs=reverse.fixedDelayMs=c.baseMs;
            forward.jitterMs=c.forwardJitterMs; reverse.jitterMs=c.reverseJitterMs;
            // The observed bad direction was creator -> Windows joiner. Only
            // edge 0->1 grows a backlog; the other four-peer paths stay healthy.
            if (a==0 && b==1) {forward.deliveryDelay=c.envelope;forward.deliveryBatchMs=c.batchMs;}
        }
        auto proxy=std::make_unique<SelectiveDeliveryProxy>(localPorts[a],reverse,forward,c.scope,0x30abcdef+a*4+b,2048);
        require(proxy->start(error),error); edges.push_back({a,b,std::move(proxy)});
    }
    network.datagramForwarder=[&](const QString& from,const QString& to,const QByteArray& bytes,CandidateNetwork::Delivery deliver) {
        const int a=int(identities.indexOf(from)),b=int(identities.indexOf(to));
        require(a>=0 && b>=0 && a!=b,"impairment route lacks peer identity");
        for (auto& edge:edges) if ((a==edge.a && b==edge.b)||(a==edge.b && b==edge.a)) {
            edge.proxy->forward(a==edge.a ? UdpProxyDirection::ServerToClient : UdpProxyDirection::ClientToServer,bytes,std::move(deliver));return;
        }
        throw std::runtime_error("missing mesh impairment edge");
    };
    std::vector<std::unique_ptr<Child>> peers;
    LoopbackPortReservations signalPorts;
    QString signalEndpoint;
    std::unique_ptr<Child> signaller;
    if (serverMode) {
        require(signalPorts.reserve(1,error),error);
        signalEndpoint=QString("127.0.0.1:%1").arg(signalPorts.port(0));
        signalPorts.release();
        signaller=std::make_unique<Child>();
        signaller->process.setStandardOutputFile(QDir(root).filePath("signaller-stdout.log"));
        signaller->process.setStandardErrorFile(QDir(root).filePath("signaller-stderr.log"));
        signaller->process.start(c.signaller,{"--jam2-server","--listen",signalEndpoint,"--allow-local",
            "--log-dir",QDir(root).filePath("signaller-logs"),"--debug"});
        require(signaller->process.waitForStarted(5000),"start local real signaller: "+signaller->process.errorString());
    }
    const QString invite=QString("jam2://v1?endpoint=%1&session=%2&key=%3&strategy=%4")
        .arg(serverMode ? signalEndpoint : QString("127.0.0.1:%1").arg(realMode ? localPorts[0] : network.port()),
            sid,key,serverMode ? "server" : "direct");
    ports.release();
    for (int i=0;i<count;++i) {
        auto peer=std::make_unique<Child>(); peer->root=QDir(root).filePath(QString("peer-%1").arg(i));
        require(QDir().mkpath(peer->root),"create peer artifacts");
        QJsonObject runtime{{"headless_audio",!hardware},{"sample_rate",c.rate},{"audio_buffer_size",c.callback},
            {"frame_size",c.packet},{"network_audio_format",c.format==jam2::NetworkAudioFormat::Pcm24Mono ? "pcm24" : "pcm16"},{"stats",true},{"stats_interval_ms",c.statsIntervalMs},
            {"stats_warmup_ms",0},{"stream_ms",0},{"test_input",c.testInput},{"os_priority","high"},
            {"metronome",true},{"metronome_mode",c.gridMode==Config::GridMode::SharedGrid ? "shared-grid" : "leader-audio"},{"metronome_level",0.05},
            {"bpm",c.bpm},
            {"remote_level",0.1},{"input_channels","1,2"},{"output_channels","1,2"}};
        if (hardware) runtime["audio_device"]=devices[i];
        if (c.driftCorrection) runtime["drift_correction"]=*c.driftCorrection;
        if (c.backend==Config::Backend::CoreAudio) runtime["input_channels"]="1";
        for (const auto& [name,frames]:{std::pair{"jitter_buffer_frames",c.jitterFrames},
                {"adaptive_playback_target_frames",c.adaptiveTarget},{"adaptive_playback_min_frames",c.adaptiveMin},
                {"adaptive_playback_max_frames",c.adaptiveMax}})
            if (frames>=0) runtime[name]=frames;
        if (c.releasePpm>=0) runtime["adaptive_playback_release_ppm"]=c.releasePpm;
        QJsonObject net{{"name",i==0 ? "Creator" : QString("Peer%1").arg(QChar('A'+i))},
            {"bind",QString("127.0.0.1:%1").arg(localPorts[i])},{"no_stun",true},
            {"wait_ms",30000},{"peer_token",identities[i]}};
        if (i==0) { net["session_id"]=sid;net["session_key"]=key;net["max_peers"]=count; }
        else net["join_url"]=invite;
        const auto action=[&](const char* type,int milliseconds) {
            return QJsonObject{{"id",QString(type)},{"type",QString(type)},{"after_event","network.connected"},
                {"delay_frames",qint64(double(milliseconds)*c.rate/1000.)}};
        };
        auto actions=QJsonArray{action("snapshot",duration-1000),action("shutdown",duration+(i==0 ? 1000 : 0))};
        if (c.recordAudio) {
            auto start=action("recording.start",1000);start["path"]=QDir(peer->root).filePath("recording");
            actions.prepend(start);actions.insert(1,action("recording.stop",duration-1500));
        }
        QJsonObject scenario{{"schema","jam2-debug-scenario"},{"run_id",QString("delivery-peer-%1").arg(i)},
            {"operation",i==0 ? "network.create" : "network.join"},{"profile",c.tuningProfile},
            {"runtime",runtime},{"network",net},{"artifacts",QJsonObject{{"root",peer->root}}},
            {"actions",actions}};
        const auto path=QDir(peer->root).filePath("scenario.json"); writeJson(path,scenario);
        peer->process.setWorkingDirectory(QFileInfo(executable).absolutePath());
        peer->process.setStandardOutputFile(QDir(peer->root).filePath("stdout.log"));
        peer->process.setStandardErrorFile(QDir(peer->root).filePath("stderr.log"));
        QStringList arguments{"debug","run",path};
        if (realMode) {
            // Exercise the normal public CLI and real server discovery. There
            // is no candidate-network/proxy hop on either compared audio path.
            arguments={"network",i==0 ? "create" : "join"};
            if (i!=0) arguments<<invite;
            arguments<<"--name"<<(i==0 ? "Creator" : QString("Peer%1").arg(QChar('A'+i)))
                <<"--profile"<<c.tuningProfile<<"--no-stun"<<"--bind"<<QString("127.0.0.1:%1").arg(localPorts[i])
                <<"--sample-rate"<<QString::number(c.rate)<<"--audio-buffer-size"<<QString::number(c.callback)
                <<"--frame-size"<<QString::number(c.packet)<<"--network-audio-format"<<(c.format==jam2::NetworkAudioFormat::Pcm24Mono ? "pcm24" : "pcm16")
                <<"--test-input"<<c.testInput<<"--os-priority"<<"high"<<"--stats"<<"enabled"
                <<"--stats-interval-ms"<<QString::number(c.statsIntervalMs)<<"--stats-warmup-ms"<<"0"<<"--log-stats"<<QDir(peer->root).filePath("logs")
                <<"--stream-ms"<<QString::number(duration)<<"--metronome"<<"on"<<"--metronome-level"<<"0.05"
                <<"--bpm"<<QString::number(c.bpm)<<"--remote-level"<<"0.1"<<"--input-channels"
                <<(c.backend==Config::Backend::CoreAudio ? "1" : "1,2")<<"--output-channels"<<"1,2";
            for (const auto& [flag,frames]:{std::pair{"--jitter-buffer-frames",c.jitterFrames},
                    {"--adaptive-playback-target-frames",c.adaptiveTarget},{"--adaptive-playback-min-frames",c.adaptiveMin},
                    {"--adaptive-playback-max-frames",c.adaptiveMax}})
                if (frames>=0) arguments<<flag<<QString::number(frames);
            if (hardware) arguments<<"--audio-device"<<QString::number(devices[i]);
            else arguments<<"--headless-audio"<<"on";
            if (c.driftCorrection) arguments<<"--drift-correction"<<(*c.driftCorrection ? "on" : "off");
            if (c.releasePpm>=0) arguments<<"--adaptive-playback-release-ppm"<<QString::number(c.releasePpm);
            if (i==0) {
                arguments<<"--session-id"<<sid<<"--session-key"<<key<<"--max-peers"<<QString::number(count)
                    <<"--connection-strategy"<<(serverMode ? "server" : "direct");
                if (serverMode) arguments<<"--rendezvous"<<signalEndpoint;
            }
            if (c.recordAudio) arguments<<"--record-jam-folder"<<QDir(peer->root).filePath("recording");
            QJsonArray saved;for(const auto& a:arguments) saved.append(a);
            writeJson(QDir(peer->root).filePath("command.json"),{{"executable",executable},{"arguments",saved}});
        }
        peer->process.start(executable,arguments);
        require(peer->process.waitForStarted(5000),peer->process.errorString()); peers.push_back(std::move(peer));
    }
    ProxyPollWait wait; QElapsedTimer elapsed; elapsed.start();
    QFile trace(QDir(root).filePath("proxy.jsonl"));require(trace.open(QIODevice::WriteOnly),trace.errorString());
    qint64 nextTrace=0, nextProgress=0, previousPumpUs=0, pumpGapMaxUs=0;
    while (std::any_of(peers.begin(),peers.end(),[](const auto& p){return p->process.state()!=QProcess::NotRunning;})) {
        require(elapsed.elapsed()<duration+60000,"bounded test timeout; retained peer logs");
        const auto now=elapsed.nsecsElapsed()/1000;
        if (previousPumpUs) pumpGapMaxUs=std::max(pumpGapMaxUs,now-previousPumpUs);
        previousPumpUs=now;
        network.pump(); for (auto& edge:edges) edge.proxy->pump();
        if (signaller) require(signaller->process.state()!=QProcess::NotRunning,"local real signaller exited during mode comparison");
        QCoreApplication::processEvents(QEventLoop::AllEvents,1);
        if (elapsed.elapsed()>=nextTrace) {
            QJsonArray detail;
            for (const auto& edge:edges) detail.append(edgeEvidence(edge.a,edge.b,*edge.proxy));
            const auto line=QJsonDocument(QJsonObject{{"elapsed_ms",elapsed.elapsed()},{"utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                {"pump_gap_max_us",pumpGapMaxUs},{"edges",detail}}).toJson(QJsonDocument::Compact)+'\n';
            require(trace.write(line)==line.size() && trace.flush(),"write proxy trace: "+trace.errorString());nextTrace+=250;
        }
        if (elapsed.elapsed()>=nextProgress) {
            std::cout << (impaired ? "impaired" : "clean") << " elapsed_ms=" << elapsed.elapsed() << " pump_gap_max_us=" << pumpGapMaxUs << std::endl;nextProgress+=10000;
        }
        require(wait.wait(),"high-resolution proxy wait failed");
    }
    QJsonArray evidence;
    for (std::size_t i=0;i<peers.size();++i) {
        const auto& peer=peers[i];
        require(peer->process.exitStatus()==QProcess::NormalExit && peer->process.exitCode()==0,
            "public Jam2 child failed; inspect "+peer->root);
        auto item=csvEvidence(*peer,c,count,hardware,hardware ? c.devices[int(i)] : QString{});
        if (c.recordAudio) item["recorded_audio"]=recordedAudioEvidence(*peer,c);
        evidence.append(item);
    }
    QJsonArray finalEdges;
    for (const auto& edge:edges) {
        const auto s=edge.proxy->selectedStats(),b=edge.proxy->baselineStats();
        require(s.clientToServer.forwarded+b.clientToServer.forwarded>1000 &&
            s.serverToClient.forwarded+b.serverToClient.forwarded>1000 &&
            s.clientToServer.capacityDropped+s.serverToClient.capacityDropped+b.clientToServer.capacityDropped+b.serverToClient.capacityDropped==0 &&
            s.clientToServer.sendErrors+s.serverToClient.sendErrors+b.clientToServer.sendErrors+b.serverToClient.sendErrors==0,
            "proxy failed to carry the intended bounded traffic");
        finalEdges.append(edgeEvidence(edge.a,edge.b,*edge.proxy));
    }
    double envelopePeak=0;for (const auto& point:c.envelope.points) envelopePeak=std::max(envelopePeak,point.delayMs);
    // A one-second beacon cadence samples the envelope more sparsely than audio.
    if (impaired && c.scope!=DeliveryDelayScope::None && envelopePeak>0)
        require(edges.front().proxy->selectedStats().serverToClient.requestedDelayMaxUs>=envelopePeak*500,
            "selected traffic never exercised at least half of the configured peak delay");
    const auto audioTiming=audioTimingEvidence(evidence,c);
    writeJson(QDir(root).filePath("summary.json"),{{"hardware",hardware},{"peer_count",count},{"audio_timing",audioTiming},
        {"connection_mode",serverMode ? "server" : realMode ? "direct" : "proxied-direct"},
        {"delay_scope",deliveryDelayScopeText(c.scope)},{"packet_frames",c.packet},{"batch_interval_ms",c.batchMs},
        {"adaptive_playback_release_ppm_override",c.releasePpm},
        {"metronome_mode",c.gridMode==Config::GridMode::SharedGrid ? "shared-grid" : "leader-audio"},
        {"pump_gap_max_us",pumpGapMaxUs},{"recovered_buffer_limit_ms",c.recoveredLimitMs},{"peers",evidence},{"edges",finalEdges}});
    std::cout << "Case evidence: " << root.toStdString() << std::endl;
    for (const auto& peer:evidence) require(peer.toObject().value("failures").toArray().isEmpty(),
        "observed recovery/capture assertions failed; inspect retained summary.json and CSV");
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    if (argc<3) { std::cerr<<"usage: test public-jam2-executable profile.json | --fake-four\n";return 2; }
    const bool fake=QString::fromLocal8Bit(argv[2])=="--fake-four";
    if (!fake && QString::fromLocal8Bit(argv[2]).isEmpty()) { std::cout<<"Set an explicit ASIO or CoreAudio delivery hardware profile.\n";return 77; }
    QString root;
    try {
        if (QString::fromLocal8Bit(argv[1])=="--review-recorded") {
            require(argc==4,"review requires case root and merged configuration JSON");
            const auto c=configuration(readJson(QString::fromLocal8Bit(argv[3])));
            root=QString::fromLocal8Bit(argv[2]);QJsonArray evidence;
            for (int i=0;i<2;++i) {Child child;child.root=QDir(root).filePath(QString("peer-%1").arg(i));
                auto item=csvEvidence(child,c,2,true,c.devices[i]);item["recorded_audio"]=recordedAudioEvidence(child,c);evidence.append(item);}
            const QJsonObject review{{"peers",evidence},{"audio_timing",audioTimingEvidence(evidence,c)}};
            writeJson(QDir(root).filePath("audio-review.json"),review);
            const auto reference=readJson(QString::fromLocal8Bit(argv[3])).value("reference_baseline_root").toString();
            if (!reference.isEmpty() && c.scope==DeliveryDelayScope::None && c.baseMs==0)
                compareCleanBaseline(root,QDir(reference).filePath(QFileInfo(root).fileName()),c);
            for (const auto& peer:evidence) require(peer.toObject().value("failures").toArray().isEmpty(),
                "recorded observations violate profile limits; inspect audio-review.json");
            std::cout<<QJsonDocument(review.value("audio_timing").toObject()).toJson(QJsonDocument::Compact).constData()<<std::endl;return 0;
        }
        QJsonObject profile;
        if (fake) profile={{"schema","jam2-asio-delivery-profile-v1"},{"device_names",QJsonArray{"fakeA","fakeB"}},
            {"sample_rate",44100},{"callback_frames",128},{"packet_frames",128},{"clean_duration_ms",15000},
            {"impaired_duration_ms",38000},{"base_one_way_delay_ms",7.5},{"forward_jitter_ms",3.6},
            {"reverse_jitter_ms",.5},{"repeat_ms",18000},{"recovered_buffer_limit_ms",40},
            {"delay_points",QJsonArray{QJsonArray{0,0},QJsonArray{6000,0},QJsonArray{7000,200},QJsonArray{7001,325},QJsonArray{8000,400},QJsonArray{10000,650},QJsonArray{12000,0}}},
            {"diagnostic_cases",QJsonArray{
                QJsonObject{{"name","all"},{"delay_scope","all"}},
                QJsonObject{{"name","audio-only"},{"delay_scope","audio"},{"impaired_duration_ms",24000},{"repeat_ms",180000}},
                QJsonObject{{"name","beacons-only"},{"delay_scope","beacons"},{"impaired_duration_ms",24000},{"repeat_ms",180000}}}}};
        else profile=readJson(QString::fromLocal8Bit(argv[2]));
        const auto c=configuration(profile);
        const auto executable=QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
        QFile binary(executable);require(binary.open(QIODevice::ReadOnly),binary.errorString());
        QCryptographicHash hash(QCryptographicHash::Sha256);require(hash.addData(&binary),"hash tested executable");
        root=QDir::current().absoluteFilePath(QString("artifacts/%1/%2-%3")
            .arg(c.backend==Config::Backend::CoreAudio ? "coreaudio-delivery" : "asio-delivery",
                QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz"),fake ? "four-fake" :
                    c.backend==Config::Backend::CoreAudio ? "two-coreaudio" : "two-asio"));
        require(QDir().mkpath(root),"create run artifacts");
        writeJson(QDir(root).filePath("profile.json"),profile);
        writeJson(QDir(root).filePath("provenance.json"),{{"executable",executable},{"sha256",QString::fromLatin1(hash.result().toHex())},
            {"model","aggregate-derived approximation, not a packet-capture replay"}});
        std::vector<int> devices;
        if (fake) devices={-1,-1,-1,-1};
        else {
            const auto available=jam2::audio::list_devices();
            for (const auto& name:c.devices) {
                const bool core=c.backend==Config::Backend::CoreAudio;
                const auto found=std::find_if(available.begin(),available.end(),[&](const auto& d){
                    return d.backend==(core ? "CoreAudio" : "ASIO") &&
                        QString::fromStdString(core ? jam2::audio::stable_device_id(d) : d.name)==name;});
                require(found!=available.end(),"profiled hardware device missing: "+name); devices.push_back(found->id);
            }
        }
        if (!fake && c.cleanMs>0) runCase(executable,QDir(root).filePath("clean"),c,devices,false,true);
        require(!profile.contains("diagnostic_cases") || profile.value("diagnostic_cases").isArray(),"diagnostic_cases must be an array");
        const auto cases=profile.value("diagnostic_cases").toArray();
        require(cases.size()<=12,"too many diagnostic cases");
        if (cases.isEmpty()) runCase(executable,QDir(root).filePath("oct30-envelope"),c,devices,true,!fake);
        else {
            QJsonArray results;QStringList names;bool passed=true;
            for (const auto& value:cases) {
                require(value.isObject(),"diagnostic case must be an object");
                const auto overrides=value.toObject();const auto name=overrides.value("name").toString();
                require(!name.isEmpty() && name.size()<=64 && !names.contains(name) && name!="clean","invalid/duplicate diagnostic case name");
                for (const auto ch:name) require((ch>='a' && ch<='z') || (ch>='0' && ch<='9') || ch=='-',"case name must use lowercase letters, digits or hyphens");
                names<<name;QJsonObject merged=profile;
                for (auto it=overrides.begin();it!=overrides.end();++it) merged[it.key()]=it.value();
                const auto caseConfig=configuration(merged);
                require(caseConfig.devices==c.devices,"diagnostic cases cannot silently change device roles");
                const auto caseRoot=QDir(root).filePath(name);require(QDir().mkpath(caseRoot),"create diagnostic case");
                writeJson(QDir(caseRoot).filePath("configuration.json"),merged);
                std::cout<<"Diagnostic case: "<<name.toStdString()<<" scope="<<deliveryDelayScopeText(caseConfig.scope)<<std::endl;
                QJsonObject result{{"name",name},{"root",caseRoot}};
                try {runCase(executable,caseRoot,caseConfig,devices,true,!fake);
                    const auto reference=profile.value("reference_baseline_root").toString();
                    if (!reference.isEmpty() && caseConfig.scope==DeliveryDelayScope::None && caseConfig.baseMs==0)
                        compareCleanBaseline(caseRoot,QDir(reference).filePath(name),caseConfig);
                    const auto modeReference=overrides.value("mode_reference_case").toString();
                    if (!modeReference.isEmpty()) {
                        require(names.contains(modeReference) && modeReference!=name,"mode_reference_case must name an earlier case");
                        compareCleanBaseline(caseRoot,QDir(root).filePath(modeReference),caseConfig,true);
                    }
                    result["passed"]=true;}
                catch (const std::exception& e) {result["passed"]=false;result["error"]=QString::fromUtf8(e.what());passed=false;std::cerr<<name.toStdString()<<": "<<e.what()<<std::endl;}
                results.append(result);writeJson(QDir(root).filePath("matrix-results.json"),{{"cases",results}});
            }
            require(passed,"one or more diagnostic cases failed; inspect matrix-results.json");
        }
        std::cout<<"Run artifacts retained: "<<root.toStdString()<<std::endl;
    } catch (const std::exception& e) { std::cerr<<e.what()<<"\nArtifacts retained: "<<root.toStdString()<<std::endl;return 1; }
}
