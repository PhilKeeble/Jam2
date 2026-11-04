#include "ApplicationRuntime.hpp"
#include "SharedSessionController.hpp"
#include "CliOptions.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QThread>
#include <QUdpSocket>
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const QString& reason) { if (!value) throw std::runtime_error(reason.toStdString()); }
template<class Predicate> void until(Predicate predicate, int timeout=15000) {
    QElapsedTimer timer; timer.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents,1);
        if (predicate()) return;
        QThread::msleep(1);
    } while (timer.elapsed()<timeout);
    throw std::runtime_error("timed out waiting for real CoreAudio lifecycle transition");
}
quint16 port() {
    QUdpSocket socket;
    require(socket.bind(QHostAddress::LocalHost,0),"reserve local UDP port");
    return socket.localPort();
}
struct Signaller {
    QProcess process;
    ~Signaller() { if (process.state()!=QProcess::NotRunning) {process.terminate();if(!process.waitForFinished(2000)){process.kill();process.waitForFinished(2000);}} }
};
Jam2RuntimeOptions options(int device, bool larger) {
    QStringList args{"--profile",larger ? "moderate" : "fast","--audio-device",QString::number(device),
        "--sample-rate","48000","--frame-size",larger ? "128" : "64","--network-audio-format","pcm16",
        "--input-channels","1","--output-channels","1,2","--test-input","tone-440",
        "--stats","enabled","--stats-interval-ms","250","--stats-warmup-ms","0",
        "--metronome","on","--metronome-level","0.05","--remote-level","0.1"};
    std::vector<QByteArray> bytes;for(const auto& a:args) bytes.push_back(a.toLocal8Bit());
    std::vector<char*> argv;for(auto& a:bytes) argv.push_back(a.data());
    return jam2::cli::parse_options(int(argv.size()),argv.data(),0);
}
}

int runCoreAudioSessionReuse(const QString& executable) {
    const auto uid=qEnvironmentVariable("JAM2_COREAUDIO_OUTPUT_UID");
    if(uid.isEmpty() || executable.isEmpty()) { std::cout<<"Set explicit CoreAudio output UID and local signaller.\n";return 77; }
    try {
        const auto devices=jam2::audio::list_devices();
        const auto device=std::find_if(devices.begin(),devices.end(),[&](const auto& d){return jam2::audio::stable_device_id(d)==uid.toStdString();});
        require(device!=devices.end() && device->input_channel_count>=1 && device->output_channel_count>=2,"profiled duplex CoreAudio device unavailable");
        const auto root=QDir::current().absoluteFilePath("artifacts/coreaudio-reuse/"+QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz"));
        require(QDir().mkpath(root),"create reuse artifacts");
        const auto endpoint=QString("127.0.0.1:%1").arg(port());
        Signaller signaller;
        signaller.process.setStandardOutputFile(root+"/signaller-stdout.log");
        signaller.process.setStandardErrorFile(root+"/signaller-stderr.log");
        signaller.process.start(executable,{"--jam2-server","--listen",endpoint,"--allow-local","--log-dir",root+"/signaller-logs"});
        require(signaller.process.waitForStarted(5000),"start matching local signaller");
        QJsonArray evidence;
        for(bool larger:{false,true}) {
            auto base=options(device->id,larger);
            base.coreaudio.output_device_uid=uid.toStdString();
            base.coreaudio.input_device_uid=uid.toStdString();
            QString caseRoot;
            QString error;
            // Same owners as the GUI: local Engine persists while controller
            // sessions stop/rejoin and switch discovery strategy.
            std::array<ApplicationRuntime,2> runtimes;
            std::array<SharedSessionController,2> controllers;
            for(int i=0;i<2;++i) {
                runtimes[i].onError=[&](const QString& text){error=text;};
                require(runtimes[i].startLocal(base),"start persistent local CoreAudio engine");
                controllers[i].bindRuntime(runtimes[i],[&,i](const SharedSessionController::Snapshot& s) {
                    auto c=base;
                    c.bind={"127.0.0.1",0};c.no_stun=true;c.stream_ms=0;
                    c.connection_strategy=controllers[i].multiplexSnapshot().usesRendezvous
                        ? jam2::ConnectionStrategy::Jam2Server : jam2::ConnectionStrategy::Direct;
                    c.rendezvous_server=jam2::parse_endpoint(endpoint.toStdString());
                    c.session_id=1;c.session_key=std::array<std::uint8_t,16>{};
                    c.local_peer_id=s.localPeerId;c.bootstrap_coordinator_peer_id=s.coordinatorPeerId;
                    c.bootstrap_role=i==0 ? jam2::SessionBootstrapRole::Creator : jam2::SessionBootstrapRole::Joiner;
                    c.mesh_peers_configured=true;
                    for(const auto& peer:s.peers) if(peer.token!=s.localToken && !peer.endpoint.isEmpty()) {
                        c.mesh_peer_ids.push_back(peer.peerId);c.mesh_peers.push_back(jam2::parse_endpoint(peer.endpoint.toStdString()));
                    }
                    const auto peerRoot=caseRoot+QString("/peer-%1").arg(i);
                    c.log_stats_dir=(peerRoot+"/logs").toStdString();
                    c.record_jam_folder=(peerRoot+"/recording").toStdString();
                    return c;
                });
            }
            for(int repetition=0;repetition<4;++repetition) {
                const bool server=repetition==1 || repetition==2;
                caseRoot=root+QString("/%1-%2-%3").arg(larger ? "larger" : "fast",QString::number(repetition),server ? "server" : "direct");
                require(QDir().mkpath(caseRoot),"create session artifacts");
                SharedSessionController::CreatorConfig create;
                create.bindHost="127.0.0.1";create.port=port();create.name="Alice";
                create.localToken=QString(32,'1');create.localEndpoint=QString("127.0.0.1:%1").arg(create.port);
                create.sessionHex="0000000000000001";create.keyHex=QString(32,'0');
                create.strategy=server ? jam2::ConnectionStrategy::Jam2Server : jam2::ConnectionStrategy::Direct;
                create.rendezvousServer=jam2::parse_endpoint(endpoint.toStdString());
                create.contract={2,"pcm16-mono",larger ? "moderate" : "fast",48000,larger ? 128 : 64};
                require(controllers[0].startCreator(create),"start reused-engine creator");
                until([&]{return server ? controllers[0].multiplexSnapshot().registered : controllers[0].snapshot().networkAttachmentReady;});
                SharedSessionController::JoinerConfig join;
                join.bindHost="127.0.0.1";join.name="Bob";join.localToken=QString(32,'2');join.localEndpoint="127.0.0.1:1";
                join.host="127.0.0.1";join.port=server ? jam2::parse_endpoint(endpoint.toStdString()).port : create.port;
                join.sessionHex=create.sessionHex;join.keyHex=create.keyHex;join.strategy=create.strategy;
                require(controllers[1].startJoiner(join),"start reused-engine joiner");
                until([&]{
                    require(error.isEmpty(),error);
                    for(int i=0;i<2;++i) if(!runtimes[i].isNetworkRunning() || !runtimes[i].engineSnapshot().network_playback_enabled || runtimes[i].engineSnapshot().remote_peak_ppm==0) return false;
                    return true;
                });
                QElapsedTimer hold;hold.start();
                until([&]{require(error.isEmpty(),error);return hold.elapsed()>=30000;},35000);
                for(int i=0;i<2;++i) {
                    const auto before=runtimes[i].engineSnapshot();
                    require(controllers[i].multiplexSnapshot().usesRendezvous==server,"controller discovery strategy differs from case");
                    controllers[i].close();
                    require(!runtimes[i].isNetworkRunning(),"closing session must detach network worker");
                    require(runtimes[i].engineStarts()==1 && runtimes[i].engineRestarts()==0,"stop/rejoin must preserve unchanged CoreAudio Engine");
                    until([&]{return runtimes[i].engineSnapshot().callbacks>before.callbacks+16;});
                    const auto after=runtimes[i].engineSnapshot();
                    require(!after.network_capture_enabled && !after.network_playback_enabled,"closed session must release capture and playback ownership");
                    evidence.append(QJsonObject{{"case",caseRoot},{"peer",i},{"server",server},{"starts",qint64(runtimes[i].engineStarts())},
                        {"restarts",qint64(runtimes[i].engineRestarts())},{"reuses",qint64(runtimes[i].engineReuses())},
                        {"frame_before_stop",qint64(before.engine_frame)},{"frame_after_stop",qint64(after.engine_frame)}});
                }
                QFile file(root+"/lifecycle.json");require(file.open(QIODevice::WriteOnly),file.errorString());
                const auto bytes=QJsonDocument(evidence).toJson();
                require(file.write(bytes)==bytes.size() && file.flush(),"write lifecycle evidence");
                std::cout<<"CoreAudio reuse case: "<<caseRoot.toStdString()<<std::endl;
            }
        }
        return 0;
    } catch(const std::exception& error) {std::cerr<<"CoreAudio reuse failed: "<<error.what()<<'\n';return 1;}
}
