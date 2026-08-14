#include "FourPeerCoordinator.hpp"

#include <iostream>
#include <stdexcept>

FourPeerCoordinator::~FourPeerCoordinator()
{
    if (successful_) return;

    bool wroteHeader = false;
    for (std::size_t index = 0; index < storageRoots_.size(); ++index) {
        if (!storageRoots_[index] || !storageRoots_[index]->isValid()) continue;
        storageRoots_[index]->setAutoRemove(false);
        if (!wroteHeader) {
            std::cerr << "four-peer test failed; retained per-peer artifacts:\n";
            wroteHeader = true;
        }
        std::cerr << "  peer " << (index + 1) << ": "
                  << storageRoots_[index]->path().toStdString() << '\n';
    }
}

bool FourPeerCoordinator::launch(
    const QString& executable,
    const std::array<QStringList, kPeerCount>& arguments,
    QString& error)
{
    for (std::size_t index = 0; index < kPeerCount; ++index) {
        storageRoots_[index] = std::make_unique<QTemporaryDir>();
        if (!storageRoots_[index]->isValid()) {
            error = QStringLiteral("peer %1: could not create isolated storage root")
                .arg(index + 1);
            return false;
        }
        QStringList isolatedArguments = arguments[index];
        isolatedArguments << QStringLiteral("--storage-root")
            << storageRoots_[index]->path();
        peers_[index] = AutomationProcess::launch(executable, isolatedArguments, error);
        if (!peers_[index]) {
            error = QStringLiteral("peer %1: ").arg(index + 1) + error;
            return false;
        }
    }
    return true;
}

AutomationProcess& FourPeerCoordinator::peer(std::size_t index)
{
    if (index >= kPeerCount || !peers_[index]) throw std::out_of_range("Jam2 peer is not running");
    return *peers_[index];
}

const AutomationProcess& FourPeerCoordinator::peer(std::size_t index) const
{
    if (index >= kPeerCount || !peers_[index]) throw std::out_of_range("Jam2 peer is not running");
    return *peers_[index];
}

QString FourPeerCoordinator::storageRoot(std::size_t index) const
{
    if (index >= kPeerCount || !storageRoots_[index]) {
        throw std::out_of_range("Jam2 peer storage root is not available");
    }
    return storageRoots_[index]->path();
}

void FourPeerCoordinator::markSuccessful() noexcept
{
    successful_ = true;
}
