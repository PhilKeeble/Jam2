#pragma once
#include <QString>
#include <optional>
class QWidget;
class LooperProject;
class ProjectPersistenceCoordinator;
namespace jam2::gui {
struct TrackLaneRemovalRequest {
    QString bankId, laneId, laneName, wavPath, assetHash;
    int fileReferences = 0;
    bool wavExists = false;
};
struct TrackLaneRemovalResult {
    bool removed = false;
    int bankIndex = -1, laneIndex = -1;
    QString error;
};
std::optional<TrackLaneRemovalRequest> describeTrackLaneRemoval(
    const LooperProject&, const ProjectPersistenceCoordinator&, int bank, int lane);
bool confirmTrackLaneRemoval(const TrackLaneRemovalRequest&, QWidget* parent);
TrackLaneRemovalResult removeTrackLaneAndWav(
    LooperProject&, ProjectPersistenceCoordinator&, const TrackLaneRemovalRequest&);
} // namespace jam2::gui
