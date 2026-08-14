#pragma once

#include "AutomationProcess.hpp"

#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <array>
#include <cstddef>
#include <memory>

class FourPeerCoordinator final {
public:
    static constexpr std::size_t kPeerCount = 4;

    ~FourPeerCoordinator();

    bool launch(
        const QString& executable,
        const std::array<QStringList, kPeerCount>& arguments,
        QString& error);

    AutomationProcess& peer(std::size_t index);
    const AutomationProcess& peer(std::size_t index) const;
    QString storageRoot(std::size_t index) const;
    void markSuccessful() noexcept;

private:
    bool successful_ = false;
    std::array<std::unique_ptr<QTemporaryDir>, kPeerCount> storageRoots_;
    // Declared after storage roots so child processes are destroyed first on
    // a failed test path and cannot access a directory being removed.
    std::array<std::unique_ptr<AutomationProcess>, kPeerCount> peers_;
};
