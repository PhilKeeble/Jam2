#include "SharedSessionController.hpp"
#include "RuntimeContracts.hpp"
#include "ApplicationRuntime.hpp"
#include "engine.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <array>
#include <iostream>
#include <set>

namespace {
using Controller = SharedSessionController;
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
template<class Predicate> void until(Predicate predicate, const char* reason, int timeout=10000)
{
    QElapsedTimer timer; timer.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents,5);
        if (predicate()) return;
        QThread::msleep(1);
    } while (timer.elapsed()<timeout);
    throw std::runtime_error(reason);
}
QString token(int i) { return QString(32,QChar('1'+i)); }
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc,argv);
    try {
        Jam2RuntimeOptions options;
        options.coreaudio.setup = jam2::audio::AudioDeviceSetup::SeparateInputOutput;
        options.coreaudio.output_device_uid = "speakers";
        options.coreaudio.input_device_uid = "microphone";
        const auto config = jam2_make_engine_config(options, true);
        require(config.coreaudio == options.coreaudio, "runtime must forward typed CoreAudio device setup to the engine");
        require(!jam2_engine_restart_required(config, config), "unchanged CoreAudio configuration must reuse the engine");
        auto changed = config; changed.coreaudio.input_device_uid = "another-microphone";
        require(jam2_engine_restart_required(config, changed), "changing a CoreAudio input must restart the engine");
        changed = config; changed.coreaudio.setup = jam2::audio::AudioDeviceSetup::SingleDevice;
        require(jam2_engine_restart_required(config, changed), "changing device setup must restart the engine");
        changed = config; changed.coreaudio.output_device_uid = "another-speaker";
        require(jam2_engine_restart_required(config, changed), "changing a CoreAudio output must restart the engine");
        ApplicationRuntime runtime;
        int audioErrors = 0;
        runtime.onError = [&](const QString&) { ++audioErrors; };
        auto unavailable = options;
        unavailable.audio_device_id = 0;
        unavailable.coreaudio.output_device_uid = "jam2-intentionally-unavailable-output";
        require(!runtime.startLocal(unavailable) && audioErrors == 1 && runtime.engineStarts() == 0,
            "missing CoreAudio devices must report a recoverable startup error before opening an engine");
        std::array<Controller,4> peers;
        Controller::CreatorConfig creator;
        creator.sessionHex="0123456789abcdef"; creator.keyHex="0123456789abcdef0123456789abcdef";
        creator.localToken=token(0); creator.localEndpoint="127.0.0.1:1";
        creator.bindHost="127.0.0.1"; creator.strategy=jam2::ConnectionStrategy::Direct;
        creator.contract={2,"pcm24-mono","fast",48000,64};
        creator.heartbeatIntervalMs=100; creator.heartbeatMissLimit=20; creator.sessionPeerLimit=3;
        require(!peers[0].startCreator(creator),"creator accepted a missing name");
        creator.name="Alice";
        require(peers[0].startCreator(creator),"creator start failed");
        until([&] { return peers[0].multiplexSnapshot().registered; },"creator UDP endpoint not registered");
        const auto port=peers[0].multiplexSnapshot().bootstrapPublic.port;
        std::array<Controller::JoinerConfig,4> joins;
        for (int i=1;i<4;++i) {
            auto& join=joins[i]; join.host="127.0.0.1"; join.port=port;
            join.sessionHex=creator.sessionHex; join.keyHex=creator.keyHex;
            join.localToken=token(i); join.localEndpoint="127.0.0.1:1";
            join.bindHost="127.0.0.1"; join.strategy=jam2::ConnectionStrategy::Direct;
            join.name=i==2 ? QStringLiteral("\u00c9lodie") : QStringLiteral("Alex");
            join.reconnectIntervalMs=20; join.heartbeatIntervalMs=100; join.heartbeatMissLimit=20;
            require(peers[i].startJoiner(join),"joiner start failed");
            until([&] { return peers[i].snapshot().lifecycle==Controller::Lifecycle::Active; },"joiner did not receive typed membership");
        }
        until([&] {
            for (auto& peer : peers) {
                const auto snapshot=peer.snapshot();
                if (snapshot.totalPeerCount!=4 || !snapshot.contractReady || !snapshot.membershipReady) return false;
                if (peer.multiplexSnapshot().links.size()!=3) return false;
                for (const auto& link : peer.multiplexSnapshot().links)
                    if (link.phase!=jam2::application::UdpMultiplexTransport::LinkPhase::Active) return false;
            }
            return true;
        },"four-peer common controller mesh did not converge");
        for (auto& peer : peers) {
            std::set<quint64> ids; int duplicates=0;
            for (const auto& member : peer.snapshot().peers) {
                ids.insert(member.peerId); duplicates+=member.name.text()=="Alex";
                require(peer.peerName(member.peerId)==member.name.text(),"display lookup lost name binding");
            }
            require(ids.size()==4 && duplicates==2,"duplicate names changed stable IDs");
            require(!peer.multiplexSnapshot().usesRendezvous,"Direct controller used a server");
        }
        until([&] { return peers[1].snapshot().heartbeatsReceived>0 && peers[0].snapshot().heartbeatAcksReceived>0; },"typed heartbeat did not acknowledge");
        std::array<bool,4> bankSwitchDelivered{};
        for (int i=1;i<4;++i) peers[i].onMessage = [&,i](const QString&, const QJsonObject& message) {
            if (message.value("type") == "bank.switch") {
                require(message.value("bank").toInt() == 1 && message.value("arrangement_revision").toInteger() > 0 &&
                    message.value("authority_token").toString() == token(0),
                    "bank switch lost its coordinator arrangement authority");
                bankSwitchDelivered[i] = true;
            }
        };
        require(peers[0].send({{"type","bank.switch"},{"bank",1},
            {"switch_id","12345678-1234-1234-1234-123456789abc"},{"target_abs_beat","8"}}),
            "creator bank switch failed to queue");
        until([&] { return bankSwitchDelivered[1] && bankSwitchDelivered[2] && bankSwitchDelivered[3]; },
            "authoritative bank switch failed to reach all three joiners");
        const auto revision=peers[1].snapshot().membershipRevision;
        require(peers[1].reconnectControlStream(),"control refresh not accepted");
        until([&] { return peers[1].snapshot().lifecycle==Controller::Lifecycle::Active && peers[1].snapshot().membershipRevision>revision; },"logical stream did not reauthenticate and recover snapshot");
        peers[1].setAssetChannelRequired(true);
        until([&] { return peers[1].assetChannelConnected(); },"asset stream did not authenticate existing name binding");
        peers[1].setAssetChannelRequired(false);
        peers[3].close();
        until([&] { return peers[0].snapshot().totalPeerCount==3 && peers[1].snapshot().totalPeerCount==3; },"peer departure did not reconcile membership");
        for (int i=0;i<3;++i) require(peers[i].peerName(token(3))=="Alex",
            "peer departure lost the name needed by delayed completion/disconnect messages");
        require(peers[3].startJoiner(joins[3]),"same-ID rejoin failed");
        until([&] { return peers[3].snapshot().totalPeerCount==4 && peers[3].snapshot().lifecycle==Controller::Lifecycle::Active; },"rejoin did not get current snapshot");
        require(peers[0].endSession("lifecycle test complete"),"creator session end was not queued");
        until([&] {
            for (int i=1;i<4;++i) if (peers[i].snapshot().lifecycle!=Controller::Lifecycle::Inactive) return false;
            return true;
        },"typed end did not reach every joiner");
        for (auto& peer : peers) peer.close();
        require(peers[1].startJoiner(joins[1]),"pending join start failed");
        require(peers[1].peerName(token(3)).isEmpty(),"new session retained a previous session name binding");
        peers[1].close();
        require(!peers[1].multiplexSnapshot().running && peers[1].snapshot().role==Controller::Role::Inactive,"cancel retained transport ownership");
        std::cout << "four-peer multiplex controller: names, typed membership/heartbeat, refresh, asset binding, leave/rejoin, end and cancellation passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
