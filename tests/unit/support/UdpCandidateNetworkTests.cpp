#include "UdpCandidateNetwork.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <array>
#include <iostream>

namespace {
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
template<class Predicate> void until(Predicate ready) {
    QElapsedTimer elapsed; elapsed.start();
    while (!ready() && elapsed.elapsed() < 3000) {
        QCoreApplication::processEvents(); QThread::msleep(1);
    }
    require(ready(), "candidate fixture timed out");
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        const CandidateNetwork::IsolatedHosts identity{0x0123456789abcdefULL, QByteArray(16, 'k')};
        QUdpSocket coordinator, sender;
        require(coordinator.bind(QHostAddress::LocalHost, 0) && sender.bind(QHostAddress::LocalHost, 0), "bind fixture sockets");
        CandidateNetwork network(coordinator.localPort(), identity); network.blockFirst = false;
        jam2::application::PeerBootstrapMessage candidate;
        candidate.kind = jam2::application::PeerBootstrapKind::Candidate;
        candidate.session = identity.session; candidate.sender = QString(32, '1'); candidate.request = QString(32, '2');
        candidate.introduction = 1; candidate.attempt = 1;
        candidate.local = jam2::application::PeerBootstrapEndpoint::from({"127.0.0.1", sender.localPort()});
        const auto packet = jam2::application::encodePeerBootstrap(candidate, identity.key);
        require(!packet.isEmpty() && sender.writeDatagram(packet, QHostAddress::LocalHost, network.port()) == packet.size(), "send candidate");
        until([&] { network.pump(); return coordinator.hasPendingDatagrams(); });
        QByteArray received(128, '\0'); QHostAddress from; quint16 port = 0;
        const auto size = coordinator.readDatagram(received.data(), received.size(), &from, &port);
        require(size > 0 && from == QHostAddress(QHostAddress::LocalHost), "mapping uses an assigned local address");
        const auto decoded = jam2::application::decodePeerBootstrap(
            std::span(reinterpret_cast<const std::uint8_t*>(received.constData()), size), identity.session, identity.key);
        require(decoded && !decoded->local.valid() && decoded->sender == candidate.sender && network.isolatedCandidates == 1,
            "isolated LAN metadata is removed and authenticated by the real bootstrap codec");
        {
            ThreadedCandidateNetwork threaded(coordinator.localPort(), identity);
            const QByteArray invalid = QByteArray("J2PB") + QByteArray(60, '\0');
            require(sender.writeDatagram(invalid, QHostAddress::LocalHost, threaded.port()) == invalid.size(), "send bad proxy fixture input");
            bool observed = false;
            until([&] {
                try { (void)threaded.port(); }
                catch (const std::runtime_error& error) { observed = QByteArray(error.what()).contains("invalid authenticated bootstrap"); }
                return observed;
            });
            require(observed, "runtime pump failures retain their original reason without completing the startup promise twice");
        }
        std::cout << "portable candidate mapping and threaded failure cleanup passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
