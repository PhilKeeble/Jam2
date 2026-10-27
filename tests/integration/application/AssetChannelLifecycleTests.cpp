#include "ControlClient.hpp"
#include "ControlByteLink.hpp"
#include "AssetChunkProtocol.hpp"
#include "ControlProtocol.hpp"
#include "ControlServer.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QThread>

#include <functional>
#include <iostream>

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

bool pumpUntil(const std::function<bool()>& predicate, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (predicate()) return true;
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    using jam2::control_protocol::TransportEvent;
    using jam2::control_protocol::TransportEventType;

    constexpr auto session = "0102030405060708";
    constexpr auto key = "000102030405060708090a0b0c0d0e0f";
    constexpr auto token = "00000000000000020000000000000002";

    ControlServer server;
    expect(server.listenViaTransport(QString::fromLatin1(session),QString::fromLatin1(key)),"configure typed lifecycle listener");
    bool controlAuthenticated = false;
    ControlClient control;
    control.setPeerName(*jam2::application::PeerName::fromInput("Alex"));
    control.onEvent = [&](const TransportEvent& event) {
        controlAuthenticated = controlAuthenticated ||
            event.type == TransportEventType::Authenticated;
    };
    auto [controlServer,controlBytes]=jam2::test::endpoints();
    server.acceptTransport(controlServer);
    control.connectViaTransport(
        controlBytes,
        QString::fromLatin1(session), QString::fromLatin1(key),
        QString::fromLatin1(token), QStringLiteral("127.0.0.1:42002"));
    expect(pumpUntil([&] { return controlAuthenticated; }),
        "control channel authenticates before an asset channel");

    bool firstAssetAuthenticated = false;
    bool firstAssetDisconnected = false;
    ControlClient firstAsset;
    firstAsset.setPeerName(*jam2::application::PeerName::fromInput("Alex"));
    firstAsset.onEvent = [&](const TransportEvent& event) {
        firstAssetAuthenticated = firstAssetAuthenticated ||
            event.type == TransportEventType::Authenticated;
        firstAssetDisconnected = firstAssetDisconnected ||
            (event.type == TransportEventType::Disconnected && event.authenticated);
    };
    auto [firstAssetServer,firstAssetBytes]=jam2::test::endpoints();
    server.acceptTransport(firstAssetServer);
    firstAsset.connectViaTransport(
        firstAssetBytes,
        QString::fromLatin1(session), QString::fromLatin1(key),
        QString::fromLatin1(token), QString{}, ControlClient::Channel::Asset);
    expect(pumpUntil([&] {
        return firstAssetAuthenticated &&
            server.stats().assetActiveConnections == 1;
    }), "first on-demand asset channel authenticates");

    const ControlServer::Stats beforeReplacement = server.stats();
    bool replacementAuthenticated = false;
    bool replacementRejected = false;
    ControlClient replacement;
    replacement.setPeerName(*jam2::application::PeerName::fromInput("Alex"));
    replacement.onEvent = [&](const TransportEvent& event) {
        replacementAuthenticated = replacementAuthenticated ||
            event.type == TransportEventType::Authenticated;
        replacementRejected = replacementRejected ||
            event.failure == jam2::control_protocol::TransportFailure::AuthenticationRejected;
    };
    auto [replacementServer,replacementBytes]=jam2::test::endpoints();
    server.acceptTransport(replacementServer);
    replacement.connectViaTransport(
        replacementBytes,
        QString::fromLatin1(session), QString::fromLatin1(key),
        QString::fromLatin1(token), QString{}, ControlClient::Channel::Asset);

    expect(pumpUntil([&] {
        return replacementAuthenticated && firstAssetDisconnected &&
            server.stats().assetActiveConnections == 1;
    }), "authenticated same-token asset reconnect replaces a stale channel");

    QByteArray received;
    QString receivedFrom;
    server.onAssetBinaryMessage = [&](const QString& source, const QByteArray& payload) {
        receivedFrom = source;
        received = payload;
    };
    const auto data=QByteArray(256,'w');
    const QString hash(64,'a');
    expect(replacement.send({{"type","looper.asset.start"},{"sha256",hash},{"file_bytes",data.size()},{"chunk_size",24576}}),"bind replacement transfer");
    const QByteArray probe=jam2::application::asset_chunk::encode({hash,0,0,data});
    const bool sent = !probe.isEmpty() && replacement.sendBinary(probe);
    expect(sent && pumpUntil([&] {
        return received == probe && receivedFrom == QString::fromLatin1(token);
    }), "replacement asset channel carries authenticated binary data");

    const ControlServer::Stats afterReplacement = server.stats();
    expect(!replacementRejected &&
            afterReplacement.authenticationRejects == beforeReplacement.authenticationRejects &&
            afterReplacement.assetAcceptedConnections ==
                beforeReplacement.assetAcceptedConnections + 1 &&
            afterReplacement.assetDisconnectedConnections ==
                beforeReplacement.assetDisconnectedConnections + 1 &&
            afterReplacement.assetActiveConnections == 1,
        "replacement is accepted without a duplicate-token rejection or leaked socket");

    replacement.close();
    firstAsset.close();
    control.close();
    server.close();

    if (failures != 0) {
        std::cerr << failures << " asset channel lifecycle checks failed\n";
        return 1;
    }
    std::cout << "asset channel lifecycle checks passed\n";
    return 0;
}
