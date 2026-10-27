#include "FourPeerCoordinator.hpp"
#include "LoopbackPortReservations.hpp"
#include "UdpCandidateNetwork.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QProcess>
#include <QThread>
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;
void require(bool value,const QString& reason) { if (!value) throw std::runtime_error(reason.toStdString()); }
QJsonObject command(AutomationProcess& peer,const QJsonObject& request,const QString& expected="command_applied")
{
    QString error; QJsonObject result;
    require(peer.send(request,error) && peer.readEvent(result,30s,error),error);
    require(result.value("event")==expected,QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
    return result;
}
using States=std::array<QJsonObject,4>;
struct Server {
    QProcess process;
    std::unique_ptr<ThreadedCandidateNetwork> network;
    QString endpoint;
    explicit Server(const QString& executable) {
        QUdpSocket reserve; require(reserve.bind(QHostAddress::LocalHost,0),"reserve server port"); const auto port=reserve.localPort(); reserve.close();
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(executable,{"--jam2-server","--listen",QStringLiteral("127.0.0.1:%1").arg(port)});
        require(process.waitForStarted(5000),"start signaller");
        network=std::make_unique<ThreadedCandidateNetwork>(port); network->setMultiplexImpairment(false);
        network->setAuthorityBeaconsEnabled(false);
        endpoint=QStringLiteral("127.0.0.1:%1").arg(network->port());
    }
    ~Server() { if (process.state()!=QProcess::NotRunning) { process.kill(); process.waitForFinished(5000); } }
};
QJsonObject jam(const QJsonObject& state) { return state.value("jam").toObject(); }
QJsonObject transport(const QJsonObject& state) { return jam(state).value("udp_multiplex").toObject(); }
States observe(FourPeerCoordinator& peers,QFile& raw,const QString& phase)
{
    States states;
    for (int i=0;i<4;++i) {
        states[i]=command(peers.peer(i),{{"type","snapshot"},{"id","grid-beacon"},{"control","looper.listening-tools"}},"snapshot");
        const auto bytes=QJsonDocument(QJsonObject{{"phase",phase},{"peer",i},{"state",states[i]}}).toJson(QJsonDocument::Compact)+'\n';
        require(raw.write(bytes)==bytes.size(),"write beacon snapshot evidence");
    }
    require(raw.flush(),"flush beacon snapshots"); return states;
}
template<class Predicate> States until(FourPeerCoordinator& peers,QFile& raw,const QString& phase,Predicate predicate,int timeoutMs=20000)
{
    QElapsedTimer timer; timer.start(); States states;
    do { states=observe(peers,raw,phase); if (predicate(states)) return states; QThread::msleep(timeoutMs>20000 ? 500 : 50); } while (timer.elapsed()<timeoutMs);
    for (const auto& state : states) std::cerr << QJsonDocument(state).toJson(QJsonDocument::Compact).constData() << '\n';
    throw std::runtime_error(("timeout: "+phase).toStdString());
}
bool allReady(const States& states)
{ return std::all_of(states.begin(),states.end(),[](const auto& state) { return jam(state).value("transport_grid_ready").toBool(); }); }
void activeAudio(const States& states)
{
    for (const auto& state : states) require(jam(state).value("grid_authority_beacon_interval_us").toInteger()==1000000,
        "Server runtime did not select the one-second authority beacon");
    for (const auto& state : states) require(jam(state).value("active_remote_peer_count").toInt()==3 &&
        transport(state).value("enabled").toBool() && transport(state).value("runtime_native_delivered").toInteger()>0,
        "exact-four-peer Server audio topology lost");
    for (const auto& state : states) require(jam(state).value("incoming_audio_breakdown").toString().count("<b>Peer ")==3 &&
        state.value("performance").toObject().value("headless_audio").toBool(),"all twelve directed fake-audio paths must remain measurable");
}
void report(const States& states,const QString& phase,CandidateNetwork::AuthorityBeaconStats beacons)
{
    for (int i=0;i<4;++i) std::cout << "server_grid_beacon phase=" << phase.toStdString() << " peer=" << i
        << " ready=" << jam(states[i]).value("transport_grid_ready").toBool()
        << " mapping_error_frames=" << jam(states[i]).value("grid_mapping_error_frames").toInteger()
        << " epoch_valid=" << states[i].value("performance").toObject().value("metronome_epoch_valid").toBool()
        << " native_delivered=" << transport(states[i]).value("runtime_native_delivered").toInteger()
        << " beacons_forwarded=" << beacons.forwarded << " beacons_dropped=" << beacons.dropped << '\n';
}
States sample(FourPeerCoordinator& peers,QFile& raw,const QString& phase,int milliseconds,int sampleMs=100)
{
    QElapsedTimer timer; timer.start(); States result;
    do { result=observe(peers,raw,phase); activeAudio(result); QThread::msleep(sampleMs); } while (timer.elapsed()<milliseconds);
    return result;
}
qint64 epoch(const QJsonObject& state) { return state.value("performance").toObject().value("metronome_epoch_frame").toInteger(); }
bool epochConverged(const States& states)
{
    const auto reference=epoch(states[0]);
    return reference>0 && std::all_of(states.begin(),states.end(),[&](const auto& state) {
        return epoch(state)==reference && state.value("performance").toObject().value("metronome_epoch_valid").toBool();
    });
}
int assignmentTrial(const QString& executable,const QString& signaller)
{
    Server server(signaller); server.network->setAuthorityBeaconsEnabled(true); server.network->setGridAssignmentsEnabled(false);
    QFile raw(QDir(qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT",QDir::tempPath())).filePath("server-grid-assignment-states.jsonl"));
    require(raw.open(QIODevice::WriteOnly),raw.errorString());
    LoopbackPortReservations reservations; QString error; require(reservations.reserve(4,error),error);
    std::array<quint16,4> ports; std::array<QStringList,4> arguments;
    for (int i=0;i<4;++i) { ports[i]=reservations.port(i); arguments[i]={"debug","gui-agent","--instance-id",QStringLiteral("assignment-peer-%1").arg(i)}; }
    FourPeerCoordinator peers; require(peers.launch(executable,arguments,error),error);
    for (int i=0;i<4;++i) { QJsonObject hello; require(peers.peer(i).readEvent(hello,30s,error) && hello.value("event")=="hello",error); }
    reservations.release();
    const auto created=command(peers.peer(0),{{"type","jam.create"},{"id","create"},{"port",ports[0]},
        {"strategy","server"},{"rendezvous",server.endpoint},{"test_input","tone-440"},{"diagnostics",true}});
    for (int i=1;i<4;++i) command(peers.peer(i),{{"type","jam.join"},{"id","join"},{"port",ports[i]},
        {"invite_url",created.value("invite_url")},{"test_input","tone-440"},{"diagnostics",true}});
    until(peers,raw,"assignments-off-start",[](const auto& states) {
        return std::all_of(states.begin(),states.end(),[](const auto& state) {
            return jam(state).value("active_remote_peer_count").toInt()==3 &&
                jam(state).value("incoming_audio_breakdown").toString().count("<b>Peer ")==3;
        });
    });
    const auto off=sample(peers,raw,"assignments-off-start",1500);
    for (int i=1;i<4;++i) require(!off[i].value("performance").toObject().value("metronome_epoch_valid").toBool(),"beacons initialized a follower without its assignment");
    require(server.network->authorityBeaconStats().forwarded>0 && server.network->gridAssignmentStats().forwarded==0 &&
        server.network->gridAssignmentStats().dropped>0,"assignment-only startup filter not exercised");
    report(off,"assignments-off-start",server.network->authorityBeaconStats());
    server.network->setGridAssignmentsEnabled(true);
    auto allApplied=[](const States& states) {
        return allReady(states) && jam(states[0]).value("grid_assignment_pending_peers").toInteger()==0 &&
            jam(states[0]).value("grid_assignment_applied_peers").toInteger()==3;
    };
    auto current=until(peers,raw,"assignment-bootstrap-applied",allApplied);
    activeAudio(current);
    require(jam(current[0]).value("grid_assignment_reliable").toBool() &&
        jam(current[0]).value("grid_assignment_retry_initial_us").toInteger()==100000 &&
        jam(current[0]).value("grid_assignment_retry_max_us").toInteger()==1000000,"reliable delivery stats/configuration missing");
    auto quiet=[&](const QString& phase) {
        const auto initial=observe(peers,raw,phase+"-start");
        const auto before=server.network->gridAssignmentStats().forwarded;
        const auto ackBefore=server.network->gridAssignmentAckStats().forwarded;
        const auto state=sample(peers,raw,phase,3000);
        require(allApplied(state) && server.network->gridAssignmentStats().forwarded==before &&
            server.network->gridAssignmentAckStats().forwarded==ackBefore &&
            jam(state[0]).value("grid_assignment_packets")==jam(initial[0]).value("grid_assignment_packets"),
            "settled assignment/ACK traffic did not stop at the production sender");
        std::cout << "assignment_socket policy=event-driven phase=" << phase.toStdString() << " steady_assignment_packets=0 steady_ack_packets=0 window_ms=3000\n";
        return state;
    };
    current=quiet("settled-startup");
    const auto reconnectRevision=jam(current[0]).value("grid_assignment_revision");
    const auto reconnectToken=jam(current[3]).value("local_token");
    const auto reconnectCount=jam(current[3]).value("control_completed_connections").toInteger();
    const auto reconnectPackets=jam(current[0]).value("grid_assignment_packets").toInteger();
    const auto physicalLinks=[](const QJsonObject& state) {
        QJsonObject result;
        for (const auto& entry : transport(state).value("links").toArray()) {
            const auto link=entry.toObject(); result.insert(link.value("peer").toString(),link.value("local"));
        }
        return result;
    };
    std::array<QJsonObject,4> originalLinks;
    for (int i=0;i<4;++i) originalLinks[i]=physicalLinks(current[i]);
    server.network->dropGridRecoveryRequests(1);
    command(peers.peer(3),{{"type","jam.control.reconnect"},{"id","grid-same-membership-reconnect"}});
    current=until(peers,raw,"control-reconnect-current-assignment",[&](const States& states) {
        return allApplied(states) && jam(states[3]).value("control_completed_connections").toInteger()>reconnectCount &&
            jam(states[0]).value("grid_assignment_packets").toInteger()>reconnectPackets &&
            jam(states[0]).value("grid_assignment_revision")==reconnectRevision &&
            jam(states[3]).value("local_token")==reconnectToken &&
            jam(states[3]).value("grid_assignment_recovery_requests").toInteger()>=2 &&
            !jam(states[3]).value("grid_assignment_recovery_pending").toBool();
    });
    for (int i=0;i<4;++i) require(physicalLinks(current[i])==originalLinks[i],"grid recovery replaced an audio socket");
    std::cout << "assignment_socket logical_reconnect=same_revision same_membership=retained sockets=retained first_request_lost=recovered\n";
    current=quiet("settled-after-control-reconnect");
    auto change=[&](const QString& id) {
        const int bpm=current[0].value("performance").toObject().value("metronome_bpm").toInt()==150 ? 120 : 150;
        command(peers.peer(0),{{"type","performance.metronome"},{"id",id},{"enabled",true},{"bpm",bpm}});
    };
    const auto oldRevision=jam(current[0]).value("grid_assignment_revision").toInteger();
    server.network->setGridAssignmentAcksEnabled(false);
    change("lost-applied-receipts");
    current=until(peers,raw,"applied-with-acks-lost",[&](const States& states) {
        if (!allReady(states) || jam(states[0]).value("grid_assignment_revision").toInteger()==oldRevision ||
            jam(states[0]).value("grid_assignment_pending_peers").toInteger()!=3) return false;
        const auto revision=jam(states[0]).value("grid_assignment_revision");
        for (int i=1;i<4;++i) if (jam(states[i]).value("grid_assignment_revision")!=revision) return false;
        return true;
    });
    const auto appliedEpoch=epoch(current[3]);
    const auto lostAcks=sample(peers,raw,"lost-acks-retry-without-reapplication",1600);
    require(jam(lostAcks[0]).value("grid_assignment_pending_peers").toInteger()==3 &&
        epoch(lostAcks[3])==appliedEpoch && server.network->gridAssignmentAckStats().dropped>0,
        "lost ACK stopped retrying, falsely completed delivery or reset the epoch again");
    const auto supersededRevision=jam(lostAcks[0]).value("grid_assignment_revision").toInteger();
    current=lostAcks; change("supersede-unacknowledged-assignment");
    current=until(peers,raw,"latest-snapshot-supersedes-unacknowledged",[&](const States& states) {
        const auto revision=jam(states[0]).value("grid_assignment_revision").toInteger();
        if (!allReady(states) || revision<=supersededRevision ||
            jam(states[0]).value("grid_assignment_pending_peers").toInteger()!=3) return false;
        for (int i=1;i<4;++i) if (jam(states[i]).value("grid_assignment_revision").toInteger()!=revision) return false;
        return true;
    });
    server.network->setGridAssignmentAcksEnabled(true);
    current=until(peers,raw,"lost-acks-recovered",allApplied);
    current=quiet("settled-after-ack-loss");

    const auto beforeLoss=server.network->gridAssignmentStats();
    const auto retries=jam(current[0]).value("grid_assignment_retries").toInteger();
    server.network->dropGridAssignments(3);
    const auto revisionBeforeLoss=jam(current[0]).value("grid_assignment_revision").toInteger();
    QElapsedTimer recovery; recovery.start(); change("lost-first-assignment");
    current=until(peers,raw,"lost-first-assignment-recovered",[&](const States& states) {
        return jam(states[0]).value("grid_assignment_revision").toInteger()>revisionBeforeLoss && allApplied(states);
    });
    require(server.network->gridAssignmentStats().dropped-beforeLoss.dropped>=3 &&
        jam(current[0]).value("grid_assignment_retries").toInteger()-retries>=3,"first-assignment loss did not exercise retries");
    std::cout << "assignment_socket loss_recovery_ms=" << recovery.elapsed() << " dropped_assignments=3\n";

    const auto blockedToken=jam(current[3]).value("local_token").toString();
    require(!blockedToken.isEmpty(),"missing targeted peer token");
    const auto blockedRevision=jam(current[3]).value("grid_assignment_revision").toInteger();
    server.network->blockGridAssignmentsTo(blockedToken); change("one-peer-missing-assignment");
    current=until(peers,raw,"healthy-peers-not-blocked",[&](const States& states) {
        const auto revision=jam(states[0]).value("grid_assignment_revision").toInteger();
        return revision>blockedRevision && jam(states[0]).value("grid_assignment_applied_peers").toInteger()==2 &&
            jam(states[0]).value("grid_assignment_pending_peers").toInteger()==1 &&
            jam(states[1]).value("grid_assignment_revision").toInteger()==revision &&
            jam(states[2]).value("grid_assignment_revision").toInteger()==revision &&
            jam(states[3]).value("grid_assignment_revision").toInteger()==blockedRevision;
    });
    activeAudio(current);
    server.network->blockGridAssignmentsTo({}); current=until(peers,raw,"one-peer-recovered",allApplied);

    // Exercise one local recording peer. Synced recording intentionally protects
    // the whole group and rejects the creator's edit before it reaches runtime.
    command(peers.peer(0),{{"type","jam.policy"},{"id","local-recording-policy"},
        {"track_lanes",true},{"auto_share_wavs",false},{"global_playback",true},
        {"generated_ideas","full"},{"metronome_state",false},{"recordings",false}});
    until(peers,raw,"local-recording-policy",[](const States& states) {
        return std::all_of(states.begin(),states.end(),[](const QJsonObject& state) {
            return !jam(state).value("policy").toObject().value("recordings").toBool(true);
        });
    });

    const auto invoke=[&](const QString& control,const QString& operation,QJsonValue value={}) {
        command(peers.peer(3),{{"type","invoke"},{"id",control},{"control",control},{"operation",operation},{"value",value}});
    };
    const int lane=current[3].value("content").toObject().value("active_bank_lane_ids").toArray().size();
    invoke("looper.lane.add-empty","click");
    until(peers,raw,"recording-lane-created",[&](const States& states) {
        return states[3].value("content").toObject().value("active_bank_lane_ids").toArray().size()==lane+1;
    });
    invoke(QStringLiteral("looper.lane.%1.arm").arg(lane),"click-async");
    invoke("looper.recording-dialog.mode","set-index",0);
    invoke("looper.recording-dialog.arm","click");
    invoke("looper.recording.manual-stop","set-checked",true);
    invoke("looper.recording.count-in","set-checked",false);
    invoke("looper.recording.start-stop","click");
    current=until(peers,raw,"recording-isolation-active",[](const States& states) {
        return states[3].value("content").toObject().value("lane_recording_isolation_active").toBool() &&
            states[3].value("content").toObject().value("lane_recording_local_phase")=="recording";
    });
    const auto recordingRevision=jam(current[3]).value("grid_assignment_revision").toInteger();
    change("change-during-remote-recording");
    current=until(peers,raw,"recording-defers-application-ack",[&](const States& states) {
        return jam(states[0]).value("grid_assignment_revision").toInteger()>recordingRevision &&
            jam(states[0]).value("grid_assignment_applied_peers").toInteger()==2 &&
            jam(states[0]).value("grid_assignment_pending_peers").toInteger()==1 &&
            jam(states[3]).value("grid_assignment_revision").toInteger()==recordingRevision &&
            jam(states[3]).value("grid_assignment_deferred").toInteger()>0;
    });
    activeAudio(current);
    invoke("looper.recording.start-stop","click");
    current=until(peers,raw,"recording-finished-application-ack",allApplied);
    std::cout << "assignment_socket recording_isolation=deferred_ack recovery=applied\n";

    command(peers.peer(3),{{"type","jam.leave"},{"id","assignment-leave"}});
    current=until(peers,raw,"assignment-peer-left",[](const States& states) {
        return jam(states[3]).value("role")=="inactive" && !jam(states[3]).value("network_running").toBool() &&
            jam(states[0]).value("remote_peer_count").toInt()==2 &&
            jam(states[0]).value("grid_assignment_applied_peers").toInteger()==2 &&
            jam(states[0]).value("grid_assignment_pending_peers").toInteger()==0;
    });
    const auto absentRevision=jam(current[0]).value("grid_assignment_revision").toInteger();
    change("change-while-peer-absent");
    current=until(peers,raw,"absent-peer-not-blocking",[&](const States& states) {
        return jam(states[0]).value("grid_assignment_revision").toInteger()>absentRevision &&
            jam(states[0]).value("grid_assignment_applied_peers").toInteger()==2 &&
            jam(states[0]).value("grid_assignment_pending_peers").toInteger()==0;
    });
    command(peers.peer(3),{{"type","jam.join"},{"id","assignment-rejoin"},{"port",ports[3]},
        {"invite_url",created.value("invite_url")},{"test_input","tone-440"},{"diagnostics",true}});
    current=until(peers,raw,"rejoin-current-assignment",[&](const States& states) {
        return allApplied(states) && jam(states[3]).value("grid_assignment_revision")==jam(states[0]).value("grid_assignment_revision") &&
            std::all_of(states.begin(),states.end(),[](const QJsonObject& state) {
                return jam(state).value("active_remote_peer_count").toInt()==3 &&
                    jam(state).value("incoming_audio_breakdown").toString().count("<b>Peer ")==3;
            });
    });
    activeAudio(current); quiet("settled-after-rejoin");
    command(peers.peer(3),{{"type","invoke"},{"id","assignment-leader-mode"},{"control","metronome.mode"},
        {"operation","set-index"},{"value",1}});
    current=until(peers,raw,"leader-mode-applied",[&](const States& states) {
        return allApplied(states) && std::all_of(states.begin(),states.end(),[](const QJsonObject& state) {
            return state.value("performance").toObject().value("metronome_mode")=="leader-audio";
        });
    });
    const auto leaderRevision=jam(current[0]).value("grid_assignment_revision").toInteger();
    // The existing GUI allows only the creator to emit leader audio. Exercise
    // that supported role while retaining native state-machine handoff coverage.
    command(peers.peer(0),{{"type","performance.metronome"},{"id","assignment-leader-disable"},{"enabled",false},
        {"bpm",current[0].value("performance").toObject().value("metronome_bpm")}});
    command(peers.peer(0),{{"type","performance.metronome"},{"id","assignment-leader-enable"},{"enabled",true},
        {"bpm",current[0].value("performance").toObject().value("metronome_bpm")}});
    current=until(peers,raw,"leader-role-applied",[&](const States& states) {
        return allApplied(states) && jam(states[0]).value("grid_assignment_revision").toInteger()>leaderRevision &&
            jam(states[0]).value("grid_authority_beacon_packets").toInteger()>0;
    });
    activeAudio(current);
    std::cout << "assignment_socket leader_mode_and_role=applied_and_acknowledged\n";
    std::cout << "assignment_socket application_ack=verified lost_ack=retry-safe targeted_peer=independent rejoin=current_snapshot\n";
    for (int i=0;i<4;++i) {
        command(peers.peer(i),{{"type","shutdown"},{"id","shutdown"}}); QJsonObject stopped; int code=-1;
        require(peers.peer(i).readEvent(stopped,30s,error) && peers.peer(i).waitForExit(30s,code,error) && code==0,error);
    }
    peers.markSuccessful(); std::cout << "four-peer Server acknowledged grid assignment checks passed\n"; return 0;
}
}
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);
    try {
        if (argc==4 && QString::fromLocal8Bit(argv[3])=="assignments") return assignmentTrial(QString::fromLocal8Bit(argv[1]),QString::fromLocal8Bit(argv[2]));
        require(argc==3 || argc==4,"expected public jam2, Go signaller and optional interval");
        const int intervalMs=argc==4 ? QString::fromLocal8Bit(argv[3]).toInt() : 0;
        require(argc==3 || intervalMs==1000 || intervalMs==60000,"expected 1000 or 60000-ms experimental interval");
        Server server(QString::fromLocal8Bit(argv[2])); server.network->setAuthorityBeaconInterval(intervalMs);
        QFile raw(QDir(qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT",QDir::tempPath())).filePath("server-grid-beacon-states.jsonl"));
        require(raw.open(QIODevice::WriteOnly),raw.errorString());
        LoopbackPortReservations reservations; QString error; require(reservations.reserve(4,error),error);
        std::array<quint16,4> ports; std::array<QStringList,4> arguments;
        for (int i=0;i<4;++i) { ports[i]=reservations.port(i); arguments[i]={"debug","gui-agent","--instance-id",QStringLiteral("beacon-peer-%1").arg(i)}; }
        FourPeerCoordinator peers; require(peers.launch(QString::fromLocal8Bit(argv[1]),arguments,error),error);
        for (int i=0;i<4;++i) { QJsonObject hello; require(peers.peer(i).readEvent(hello,30s,error) && hello.value("event")=="hello",error); }
        reservations.release();
        const auto created=command(peers.peer(0),{{"type","jam.create"},{"id","create"},{"port",ports[0]},
            {"strategy","server"},{"rendezvous",server.endpoint},{"test_input","tone-440"},{"diagnostics",true}});
        require(created.value("invite_url").toString().contains("strategy=server"),"Server mode not selected");
        for (int i=1;i<4;++i) command(peers.peer(i),{{"type","jam.join"},{"id","join"},{"port",ports[i]},
            {"invite_url",created.value("invite_url")},{"test_input","tone-440"},{"diagnostics",true}});
        until(peers,raw,"off-start-audio",[](const auto& states) {
            return std::all_of(states.begin(),states.end(),[](const auto& state) { return jam(state).value("active_remote_peer_count").toInt()==3 &&
                transport(state).value("runtime_native_delivered").toInteger()>0 &&
                jam(state).value("incoming_audio_breakdown").toString().count("<b>Peer ")==3; });
        });
        const auto disabled=sample(peers,raw,"off-from-start",1500);
        require(jam(disabled[0]).value("transport_grid_ready").toBool(),"local authority failed to initialize its own grid");
        for (int i=1;i<4;++i) require(!jam(disabled[i]).value("transport_grid_ready").toBool() &&
            !disabled[i].value("performance").toObject().value("metronome_epoch_valid").toBool(),"listener initialized without authority timing");
        require(server.network->authorityBeaconStats().forwarded==0 && server.network->authorityBeaconStats().dropped>0,"off filter not exercised");
        report(disabled,"off-from-start",server.network->authorityBeaconStats());

        server.network->setAuthorityBeaconsEnabled(true);
        const auto acquired=until(peers,raw,"enabled-acquire",allReady);
        const auto beforePeriodic=server.network->authorityBeaconStats();
        const auto periodic=sample(peers,raw,"enabled",intervalMs ? intervalMs+2000 : 6000,intervalMs>1000 ? 1000 : 100);
        require(allReady(periodic),"periodic beacons lost grid readiness");
        if (intervalMs) {
            const auto forwarded=server.network->authorityBeaconStats().forwarded-beforePeriodic.forwarded;
            require(forwarded>=3 && forwarded<=3*static_cast<quint64>((intervalMs+3000)/intervalMs+1),"slow beacon delivery interval/count mismatch");
            std::cout << "server_grid_beacon interval_ms=" << intervalMs << " steady_new_datagrams=" << forwarded << '\n';
        }
        report(periodic,"enabled",server.network->authorityBeaconStats());
        if (!intervalMs) {
            const auto batches=jam(periodic[0]).value("grid_authority_beacon_batches").toInteger()-
                jam(acquired[0]).value("grid_authority_beacon_batches").toInteger();
            const auto forwarded=server.network->authorityBeaconStats().forwarded-beforePeriodic.forwarded;
            require(batches>=4 && batches<=8 && forwarded>=12 && forwarded<=24,"production Server beacon was not sent at one-second steady cadence");
            std::cout << "production_beacon interval_us=1000000 steady_batches=" << batches << " datagrams=" << forwarded << '\n';
        }

        if (!intervalMs) {
        server.network->setAuthorityBeaconsEnabled(false); QThread::msleep(250);
        const auto before=server.network->authorityBeaconStats();
        const auto free=sample(peers,raw,"off-after-alignment",6000);
        require(allReady(free),"already mapped fake clocks lost grid readiness during six seconds of silence");
        const auto after=server.network->authorityBeaconStats();
        require(after.forwarded==before.forwarded && after.dropped>before.dropped,"ongoing authority beacons escaped off filter");
        for (int i=0;i<4;++i) require(transport(free[i]).value("runtime_native_delivered").toInteger()>
            transport(periodic[i]).value("runtime_native_delivered").toInteger(),"beacon filtering stopped native audio delivery");
        report(free,"off-after-alignment",after);
        }

        const int newBpm=periodic[0].value("performance").toObject().value("metronome_bpm").toInt()==150 ? 120 : 150;
        QElapsedTimer recovery; recovery.start();
        command(peers.peer(0),{{"type","performance.metronome"},{"id","new-epoch"},{"enabled",true},{"bpm",newBpm}});
        const auto reset=until(peers,raw,intervalMs ? "tempo-reset" : "off-new-epoch",[newBpm,intervalMs](const auto& states) {
            for (int i=1;i<4;++i) if (states[i].value("performance").toObject().value("metronome_bpm").toInt()!=newBpm ||
                (!intervalMs && jam(states[i]).value("transport_grid_ready").toBool())) return false;
            return true;
        });
        activeAudio(reset); report(reset,intervalMs ? "tempo-reset" : "off-new-epoch",server.network->authorityBeaconStats());
        if (!intervalMs) server.network->setAuthorityBeaconsEnabled(true);
        const auto recovered=until(peers,raw,"restored",allReady,intervalMs+20000); activeAudio(recovered);
        std::cout << "server_grid_beacon interval_ms=" << intervalMs << " epoch_recovery_ms=" << recovery.elapsed() << '\n';
        report(recovered,"restored",server.network->authorityBeaconStats());
        if (!intervalMs) {
            command(peers.peer(0),{{"type","invoke"},{"id","listener-mode"},{"control","metronome.mode"},
                {"operation","set-index"},{"value",2}});
            until(peers,raw,"listener-mode",[](const auto& states) {
                return std::all_of(states.begin(),states.end(),[](const auto& state) {
                    return state.value("performance").toObject().value("metronome_mode")=="listener-compensated" &&
                        jam(state).value("grid_authority_phase_fresh").toBool();
                });
            });
            QElapsedTimer phaseTime; phaseTime.start(); qint64 maxAuthorityAge=0;
            do {
                const auto states=observe(peers,raw,"listener-freshness"); activeAudio(states);
                for (int i=1;i<4;++i) {
                    require(jam(states[i]).value("grid_authority_phase_freshness_us").toInteger()==2500000 &&
                        jam(states[i]).value("grid_authority_phase_fresh").toBool(),"one-second authority observations became periodically stale in listener mode");
                    maxAuthorityAge=(std::max)(maxAuthorityAge,jam(states[i]).value("grid_authority_phase_age_us").toInteger());
                }
                QThread::msleep(100);
            } while (phaseTime.elapsed()<6000);
            require(maxAuthorityAge>500000,"listener test did not cross the old phase expiration");
            std::cout << "listener_authority freshness_us=2500000 max_observed_age_us=" << maxAuthorityAge << '\n';
        }
        for (int i=0;i<4;++i) {
            command(peers.peer(i),{{"type","shutdown"},{"id","shutdown"}}); QJsonObject stopped;
            require(peers.peer(i).readEvent(stopped,30s,error),error); int code=-1;
            require(peers.peer(i).waitForExit(30s,code,error) && code==0,error);
        }
        peers.markSuccessful(); std::cout << "four-peer Server authority beacon on/off integration passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
