#include "TrackLaneRemoval.hpp"
#include "LooperProject.hpp"
#include "ProjectPersistenceCoordinator.hpp"
#include "GuiControlContract.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>

namespace jam2::gui {
namespace {
QString assetPath(const LooperLane& lane, const ProjectPersistenceCoordinator& persistence)
{
    if (lane.assetPath.trimmed().isEmpty()) return {};
    return QDir::cleanPath(QFileInfo(lane.assetPath).isAbsolute() || persistence.projectFolder().isEmpty()
        ? lane.assetPath : QDir(persistence.projectFolder()).absoluteFilePath(lane.assetPath));
}
}
std::optional<TrackLaneRemovalRequest> describeTrackLaneRemoval(
    const LooperProject& project, const ProjectPersistenceCoordinator& persistence, int bank, int lane)
{
    if (bank < 0 || bank >= project.banks().size() || lane < 0 || lane >= project.banks().at(bank).lanes.size()) return {};
    const auto& value = project.banks().at(bank).lanes.at(lane);
    TrackLaneRemovalRequest result{project.banks().at(bank).id, value.id, value.name,
        assetPath(value, persistence), value.assetHash};
    const QFileInfo wav(result.wavPath);
    result.wavExists = !result.wavPath.isEmpty() && wav.isFile();
    if (!result.wavPath.isEmpty())
        for (const auto& section : project.banks())
            for (const auto& other : section.lanes)
                if (assetPath(other, persistence) == result.wavPath) ++result.fileReferences;
    return result;
}
bool confirmTrackLaneRemoval(const TrackLaneRemovalRequest& request, QWidget* parent)
{
    QMessageBox dialog(QMessageBox::Warning, QStringLiteral("Remove lane"),
        QStringLiteral("Remove '%1'?").arg(request.laneName), QMessageBox::Cancel, parent);
    dialog.setInformativeText(request.wavExists
        ? QStringLiteral("This will remove the lane and permanently delete its WAV file from disk:\n%1").arg(QDir::toNativeSeparators(request.wavPath))
        : QStringLiteral("This will remove the lane. No existing WAV file will be deleted."));
    if (request.wavExists && request.fileReferences > 1)
        dialog.setInformativeText(dialog.informativeText() + QStringLiteral(
            "\nThis file is used by %1 lanes. Other lanes will remain, but lose access to this WAV.").arg(request.fileReferences));
    auto* accept = dialog.addButton(request.wavExists ? QStringLiteral("Remove lane and delete WAV") : QStringLiteral("Remove lane"), QMessageBox::DestructiveRole);
    registerGuiControl(*accept, QStringLiteral("looper.lane-remove-dialog.accept"), QStringLiteral("looper.lane-remove"), GuiControlAvailability::Modal, QStringLiteral("looper.lane-remove-dialog"));
    registerGuiControl(*dialog.button(QMessageBox::Cancel), QStringLiteral("looper.lane-remove-dialog.cancel"), QStringLiteral("looper.lane-remove"), GuiControlAvailability::Modal, QStringLiteral("looper.lane-remove-dialog"));
    dialog.setDefaultButton(QMessageBox::Cancel);
    dialog.exec();
    return dialog.clickedButton() == accept;
}
TrackLaneRemovalResult removeTrackLaneAndWav(
    LooperProject& project, ProjectPersistenceCoordinator& persistence, const TrackLaneRemovalRequest& request)
{
    TrackLaneRemovalResult result;
    for (int bank = 0; bank < project.banks().size(); ++bank) {
        if (project.banks().at(bank).id != request.bankId) continue;
        for (int row = 0; row < project.banks().at(bank).lanes.size(); ++row) {
            const auto& lane = project.banks().at(bank).lanes.at(row);
            if (lane.id != request.laneId) continue;
            if (lane.assetHash != request.assetHash || assetPath(lane, persistence) != request.wavPath) {
                result.error = QStringLiteral("The lane's WAV changed while confirmation was open. Please try again.");
                return result;
            }
            const QFileInfo wav(request.wavPath);
            if (!request.wavPath.isEmpty() && wav.exists()) {
                if (!wav.isFile() || wav.suffix().compare(QStringLiteral("wav"), Qt::CaseInsensitive) != 0) {
                    result.error = QStringLiteral("The lane does not reference a regular WAV file. The lane was not removed.");
                    return result;
                }
                const bool deleted = persistence.ownsTransientWav(request.wavPath)
                    ? persistence.discardTransientWav(request.wavPath) : QFile::remove(request.wavPath);
                if (!deleted) {
                    result.error = QStringLiteral("Could not delete the WAV file. The lane was not removed:\n%1").arg(request.wavPath);
                    return result;
                }
            }
            result.removed = project.removeLane(bank, row);
            result.bankIndex = bank;
            result.laneIndex = row;
            if (!result.removed) result.error = QStringLiteral("Could not remove the lane.");
            return result;
        }
    }
    result.error = QStringLiteral("The lane is no longer present.");
    return result;
}
} // namespace jam2::gui
