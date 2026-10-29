#include "AutomationProcess.hpp"
#include "RendezvousWire.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QTemporaryDir>
#include <QThread>
#include <QUdpSocket>
#include <atomic>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
namespace rv = jam2::application::rendezvous_wire;
using namespace std::chrono_literals;
void require(bool value, const QString& message) { if (!value) throw std::runtime_error(message.toStdString()); }
struct Rejector {
    std::atomic<bool> stopping{false};
    std::thread worker;
    quint16 port;
    std::atomic<int> received{0}, decoded{0};
    explicit Rejector(rv::Error code) {
        std::promise<quint16> ready; auto result=ready.get_future();
        worker=std::thread([this, code, ready=std::move(ready)]() mutable {
            QUdpSocket socket;
            if (!socket.bind(QHostAddress::LocalHost, 0)) { ready.set_value(0); return; }
            ready.set_value(socket.localPort());
            while (!stopping) {
                if (!socket.waitForReadyRead(50)) continue;
                while (socket.hasPendingDatagrams()) {
                    const auto packet=socket.receiveDatagram(); ++received; const auto request=rv::decode(packet.data());
                    if (!request || !rv::request(request->kind)) continue; ++decoded;
                    rv::Message error; error.kind=rv::Kind::Error; error.session=request->session;
                    error.to=request->peer; error.error=code; error.rejectedKind=static_cast<quint8>(request->kind);
                    const auto bytes=rv::encode(error);
                    for (int repeat=0;repeat<3;++repeat) socket.writeDatagram(bytes,packet.senderAddress(),packet.senderPort());
                }
            }
        });
        port=result.get();
    }
    ~Rejector() { stopping=true; worker.join(); }
};
QJsonObject event(AutomationProcess& peer, const QString& expected) {
    QString error; QJsonObject result;
    require(peer.readEvent(result,15s,error),error);
    require(result.value("event").toString()==expected,QString::fromUtf8(QJsonDocument(result).toJson()));
    return result;
}
int main(int argc,char**argv) {
    QCoreApplication application(argc,argv);
    try {
        require(argc==2,"requires public Release executable");
        for (const auto code : {rv::Error::Capacity, rv::Error::InvalidSchema}) {
            Rejector server(code); require(server.port!=0,"bind diagnostic fixture");
            QTemporaryDir storage; require(storage.isValid(),"isolated GUI storage");
            QString error;
            auto peer=AutomationProcess::launch(QString::fromLocal8Bit(argv[1]),
                {"debug","gui-agent","--instance-id","signaler-errors","--storage-root",storage.path()},error);
            require(static_cast<bool>(peer),error); event(*peer,"hello");
            QUdpSocket reservation; require(reservation.bind(QHostAddress::LocalHost,0),"reserve GUI media port");
            const auto port=reservation.localPort(); reservation.close();
            require(peer->send({{"type","jam.create"},{"id","create"},{"strategy","server"},
                {"rendezvous",QStringLiteral("127.0.0.1:%1").arg(server.port)},
                {"name","Alex"},{"port",port},{"test_input","tone-440"}},error),error);
            event(*peer,"command_applied");
            QElapsedTimer deadline; deadline.start(); QString failure; QJsonObject last;
            while (deadline.elapsed()<10000 && failure.isEmpty()) {
                require(peer->send({{"type","snapshot"},{"id","failure"}},error),error);
                last=event(*peer,"snapshot"); failure=last.value("jam").toObject().value("last_startup_failure").toString();
                QThread::msleep(20);
            }
            require(failure==rv::errorText(code),QStringLiteral("public GUI startup diagnostic missing; packets=%1 decoded=%2 state=%3").arg(server.received.load()).arg(server.decoded.load()).arg(QString::fromUtf8(QJsonDocument(last.value("jam").toObject()).toJson(QJsonDocument::Compact))));
            require(peer->send({{"type","snapshot"},{"id","duplicate"}},error),error);
            require(event(*peer,"snapshot").value("jam").toObject().value("last_startup_failure").toString()==failure,
                "duplicate replies changed failure presentation");
        }
        std::cout<<"public GUI signaling failure presentation passed\n"; return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
