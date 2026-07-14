#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace jam2 {

enum class GridRunState : std::uint8_t {
    Stopped = 0,
    Running = 1,
    AuthorityMissing = 2,
};

struct GridAuthorityState {
    std::uint64_t revision = 0;
    std::uint64_t authority_peer_id = 0;
    GridRunState run_state = GridRunState::Stopped;
    std::uint8_t mode = 0;
    std::uint64_t authority_epoch_frame = 0;
    std::uint64_t authority_packet_frame = 0;
};

struct GridProposal {
    std::uint64_t requester_peer_id = 0;
    std::uint64_t request_id = 0;
    GridRunState run_state = GridRunState::Stopped;
    std::uint8_t mode = 0;
    std::uint64_t proposed_epoch_frame = 0;
};

enum class AuthorityUpdateResult : std::uint8_t {
    Accepted,
    Duplicate,
    UnauthorizedSource,
    StaleRevision,
    Invalid,
};

struct SessionAuthorityStats {
    std::uint64_t grid_proposals_accepted = 0;
    std::uint64_t grid_proposals_rejected = 0;
    std::uint64_t grid_assignments_accepted = 0;
    std::uint64_t grid_assignments_duplicate = 0;
    std::uint64_t grid_assignments_rejected = 0;
    std::uint64_t grid_authority_states_accepted = 0;
    std::uint64_t grid_authority_states_rejected = 0;
    std::uint64_t grid_authority_missing_events = 0;
    std::uint64_t transport_events_accepted = 0;
    std::uint64_t transport_events_rejected = 0;
};

// Network/control-thread state machine. The bootstrap coordinator orders grid
// proposals, while only the assigned authority may publish timing for the
// accepted revision. Arrangement authority remains a separate identity.
class SessionAuthority {
public:
    SessionAuthority(
        std::uint64_t local_peer_id,
        std::uint64_t bootstrap_coordinator_peer_id,
        std::uint64_t arrangement_authority_peer_id);

    std::uint64_t localPeerId() const noexcept;
    std::uint64_t bootstrapCoordinatorPeerId() const noexcept;
    std::uint64_t arrangementAuthorityPeerId() const noexcept;
    bool localIsBootstrapCoordinator() const noexcept;
    bool localIsGridAuthority() const noexcept;
    bool localIsArrangementAuthority() const noexcept;

    const GridAuthorityState& grid() const noexcept;
    const SessionAuthorityStats& stats() const noexcept;

    std::optional<GridAuthorityState> orderGridProposal(const GridProposal& proposal) noexcept;
    AuthorityUpdateResult acceptGridAssignment(
        std::uint64_t source_peer_id,
        const GridAuthorityState& assignment) noexcept;
    AuthorityUpdateResult acceptGridAuthorityState(
        std::uint64_t source_peer_id,
        const GridAuthorityState& state) noexcept;
    bool activateLocalGrid(
        std::uint64_t epoch_frame,
        std::uint64_t packet_frame) noexcept;
    bool markPeerInactive(std::uint64_t peer_id) noexcept;

    bool acceptTransportEvent(
        std::uint64_t source_peer_id,
        std::uint64_t event_counter,
        std::uint64_t grid_revision) noexcept;

private:
    struct RequestTracker {
        std::uint64_t peer_id = 0;
        std::uint64_t last_request_id = 0;
    };

    std::uint64_t local_peer_id_ = 0;
    std::uint64_t bootstrap_coordinator_peer_id_ = 0;
    std::uint64_t arrangement_authority_peer_id_ = 0;
    std::uint64_t last_transport_event_counter_ = 0;
    GridAuthorityState grid_;
    SessionAuthorityStats stats_;
    std::vector<RequestTracker> request_trackers_;
};

} // namespace jam2
