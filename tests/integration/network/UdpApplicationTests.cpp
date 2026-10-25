#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>
#include <QUdpSocket>
#include <array>
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
namespace {
void require(bool condition, const char* detail) { if (!condition) throw std::runtime_error(detail); }
struct Child {
    QProcess process;
    QByteArray output;
    explicit Child(const QString& executable, const QStringList& arguments) {
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(executable, arguments);
        require(process.waitForStarted(5000), "start native child process");
    }
    void read() { output += process.readAll(); }
    ~Child() {
        if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(5000); }
        read();
    }
};
template<class Predicate> void until(Predicate predicate, int timeout, const char* detail) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5); QThread::msleep(1);
    }
    require(predicate(), detail);
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    std::array<std::unique_ptr<Child>, 4> peers;
    auto save = [&] {
        const auto root = qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT", QDir::tempPath());
        QDir().mkpath(root);
        for (int index = 0; index < 4; ++index) if (peers[index]) {
            peers[index]->read();
            QFile file(QDir(root).filePath(QStringLiteral("udp-app-%1.log").arg(index)));
            if (file.open(QIODevice::WriteOnly)) file.write(peers[index]->output);
        }
    };
    try {
        require(argc == 3 || argc == 4, "requires release/jam2 and real Go rendezvous server");
        const bool local = argc == 4;
        require(!local || QString::fromLocal8Bit(argv[3]) == "local", "unknown native application test mode");
        QUdpSocket reserve; require(reserve.bind(QHostAddress::LocalHost, 0), "reserve signaler port");
        const auto port = reserve.localPort(); reserve.close();
        QStringList serverArguments{"--jam2-server", "--listen", QStringLiteral("127.0.0.1:%1").arg(port)};
        if (local) serverArguments << "--allow-local";
        Child signaler(QString::fromLocal8Bit(argv[2]), serverArguments);
        const auto executable = QString::fromLocal8Bit(argv[1]);
        const auto artifact = qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT", QDir::tempPath());
        auto statsArguments = [&](int i) { return QStringList{"--stats", "enabled", "--stats-interval-ms", "250",
            "--stats-warmup-ms", "0", "--log-stats", QDir(artifact).filePath(QString::number(i))}; };
        const QStringList audio{"--headless-audio", "on", "--test-input", "tone-440", "--stream-ms", "10000", "--wait-ms", "15000"};
        QStringList creator{"network", "create", "--connection-strategy", "server",
            "--rendezvous", QStringLiteral("127.0.0.1:%1").arg(port), "--max-peers", "3"};
        creator += audio; creator += statsArguments(0);
        peers[0] = std::make_unique<Child>(executable, creator);
        QString url;
        until([&] {
            peers[0]->read();
            const auto match = QRegularExpression(QStringLiteral("jam2://[^\\s]+")).match(QString::fromUtf8(peers[0]->output));
            if (match.hasMatch()) url = match.captured();
            return !url.isEmpty() || peers[0]->process.state() == QProcess::NotRunning;
        }, 5000, "creator did not generate server-mode invitation");
        require(url.contains("strategy=server"), "creator invitation omitted explicit server strategy");
        for (int index = 1; index < 4; ++index) {
            QStringList arguments{"network", "join", url}; arguments += audio; arguments += statsArguments(index);
            peers[index] = std::make_unique<Child>(executable, arguments);
            // Stagger application joins while the existing synthetic audio runs.
            QElapsedTimer stagger; stagger.start();
            until([&] { for (auto& child : peers) if (child) child->read(); return stagger.elapsed() >= 150; },
                1000, "staggered application join");
        }
        until([&] {
            bool finished = true;
            for (auto& child : peers) {
                child->read(); finished = finished && child->process.state() == QProcess::NotRunning;
            }
            signaler.read(); return finished;
        }, 25000, "four native applications did not finish short stream");
        save();
        for (int index = 0; index < 4; ++index) {
            const auto& child = peers[index];
            require(child->process.exitCode() == 0 && child->process.exitStatus() == QProcess::NormalExit,
                "native application returned failure");
            const auto output = QString::fromUtf8(child->output);
            if (local) require(output.count(QStringLiteral("route=lan")) == 3,
                "application did not establish all three remote links via LAN multiplex");
            bool fullMeshSample = false;
            const QDir folder(QDir(artifact).filePath(QString::number(index)));
            for (const auto& name : folder.entryList({"*.csv"}, QDir::Files)) {
                QFile file(folder.filePath(name)); require(file.open(QIODevice::ReadOnly), "open native CSV");
                const auto lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
                if (lines.isEmpty()) continue;
                const auto header = lines.front().trimmed().split(QLatin1Char(','));
                const int count = header.indexOf("network_peer_count"), active = header.indexOf("network_active_peer_count");
                const int received = header.indexOf("recv_packets");
                require(count >= 0 && active >= 0 && received >= 0, "native CSV mesh measurements missing");
                for (const auto& line : lines) {
                    const auto fields = line.trimmed().split(QLatin1Char(','));
                    if (fields.size() > (std::max)({count, active, received}) &&
                        fields[count].toInt() == 3 && fields[active].toInt() == 3 &&
                        fields[received].toULongLong() > 100) fullMeshSample = true;
                }
            }
            require(fullMeshSample, "no measured three-remote-peer audio sample while all four applications were running");
            require(output.contains("endpoint_proof_state=active"), "native endpoint proof did not become active");
            const auto peak = QRegularExpression(QStringLiteral("mesh_stats [^\\r\\n]*remote_peak=([0-9.eE+-]+)")).match(output);
            require(peak.hasMatch() && peak.captured(1).toDouble() > 0.01, "actual decoded/mixed remote PCM was silent");
        }
        std::cout << "Four public Jam2 applications streamed real synthetic PCM over the UDP multiplex strategy\n";
        return 0;
    } catch (const std::exception& error) {
        save();
        for (int i = 0; i < 4; ++i) if (peers[i])
            std::cerr << "peer " << i << ": " << peers[i]->output.right(2000).constData() << '\n';
        std::cerr << error.what() << '\n'; return 1;
    }
}
