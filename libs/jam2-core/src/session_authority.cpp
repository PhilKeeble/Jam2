#include "session_authority.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace jam2 {

SessionAuthority::SessionAuthority(
    std::uint64_t local_peer_id,
    std::uint64_t bootstrap_coordinator_peer_id,
    std::uint64_t arrangement_authority_peer_id)
    : local_peer_id_(local_peer_id),
      bootstrap_coordinator_peer_id_(bootstrap_coordinator_peer_id),
      arrangement_authority_peer_id_(arrangement_authority_peer_id)
{
    if (local_peer_id_ == 0 || bootstrap_coordinator_peer_id_ == 0 ||
        arrangement_authority_peer_id_ == 0) {
        throw std::runtime_error("SessionAuthority requires nonzero peer identities");
    }
}

std::uint64_t SessionAuthority::localPeerId() const noexcept { return local_peer_id_; }
std::uint64_t SessionAuthority::bootstrapCoordinatorPeerId() const noexcept
{
    return bootstrap_coordinator_peer_id_;
}
std::uint64_t SessionAuthority::arrangementAuthorityPeerId() const noexcept
{
    return arrangement_authority_peer_id_;
}
bool SessionAuthority::localIsBootstrapCoordinator() const noexcept
{
    return local_peer_id_ == bootstrap_coordinator_peer_id_;
}
bool SessionAuthority::localIsGridAuthority() const noexcept
{
    return grid_.revision != 0 && grid_.authority_peer_id == local_peer_id_ &&
        grid_.run_state != GridRunState::AuthorityMissing;
}
bool SessionAuthority::localIsArrangementAuthority() const noexcept
{
    return local_peer_id_ == arrangement_authority_peer_id_;
}

const GridAuthorityState& SessionAuthority::grid() const noexcept { return grid_; }
const SessionAuthorityStats& SessionAuthority::stats() const noexcept { return stats_; }

std::optional<GridAuthorityState> SessionAuthority::orderGridProposal(
    const GridProposal& proposal) noexcept
{
    if (!localIsBootstrapCoordinator() || proposal.requester_peer_id == 0 ||
        proposal.request_id == 0 || proposal.mode > 2 ||
        (proposal.claim_leader_audio_source && proposal.mode != 1) ||
        proposal.run_state != GridRunState::Running ||
        grid_.revision == (std::numeric_limits<std::uint64_t>::max)()) {
        ++stats_.grid_proposals_rejected;
        return std::nullopt;
    }
    auto found = std::find_if(
        request_trackers_.begin(),
        request_trackers_.end(),
        [&](const RequestTracker& tracker) {
            return tracker.peer_id == proposal.requester_peer_id;
        });
    if (found == request_trackers_.end()) {
        request_trackers_.push_back({proposal.requester_peer_id, 0});
        found = std::prev(request_trackers_.end());
    }
    if (proposal.request_id <= found->last_request_id) {
        ++stats_.grid_proposals_rejected;
        return std::nullopt;
    }
    found->last_request_id = proposal.request_id;
    const bool first_grid = grid_.revision == 0;
    const bool reset_epoch = proposal.reset_epoch;
    const bool claim_leader_audio = proposal.claim_leader_audio_source;
    const std::uint64_t previous_authority = grid_.authority_peer_id;
    const std::uint64_t previous_epoch = grid_.authority_epoch_frame;
    const std::uint64_t previous_packet = grid_.authority_packet_frame;
    grid_.revision += 1;
    if (first_grid) {
        grid_.authority_peer_id = bootstrap_coordinator_peer_id_;
    } else if (claim_leader_audio) {
        grid_.authority_peer_id = proposal.requester_peer_id;
    } else if (grid_.run_state != GridRunState::AuthorityMissing) {
        grid_.authority_peer_id = previous_authority != 0
            ? previous_authority
            : bootstrap_coordinator_peer_id_;
    } else {
        grid_.authority_peer_id = bootstrap_coordinator_peer_id_;
    }
    grid_.run_state = GridRunState::Running;
    grid_.mode = proposal.mode;
    grid_.authority_epoch_frame = reset_epoch ? 0 : previous_epoch;
    grid_.authority_packet_frame = reset_epoch ? 0 : previous_packet;
    grid_.epoch_reset = reset_epoch;
    ++stats_.grid_proposals_accepted;
    ++stats_.grid_assignments_accepted;
    return grid_;
}

AuthorityUpdateResult SessionAuthority::acceptGridAssignment(
    std::uint64_t source_peer_id,
    const GridAuthorityState& assignment) noexcept
{
    if (source_peer_id != bootstrap_coordinator_peer_id_) {
        ++stats_.grid_assignments_rejected;
        return AuthorityUpdateResult::UnauthorizedSource;
    }
    if (assignment.revision == 0 || assignment.authority_peer_id == 0 ||
        assignment.mode > 2 || assignment.run_state != GridRunState::Running) {
        ++stats_.grid_assignments_rejected;
        return AuthorityUpdateResult::Invalid;
    }
    if (assignment.revision < grid_.revision) {
        ++stats_.grid_assignments_rejected;
        return AuthorityUpdateResult::StaleRevision;
    }
    if (assignment.revision == grid_.revision) {
        const bool same = assignment.authority_peer_id == grid_.authority_peer_id &&
            assignment.run_state == grid_.run_state && assignment.mode == grid_.mode;
        if (same) {
            ++stats_.grid_assignments_duplicate;
            return AuthorityUpdateResult::Duplicate;
        }
        ++stats_.grid_assignments_rejected;
        return AuthorityUpdateResult::StaleRevision;
    }
    grid_ = assignment;
    ++stats_.grid_assignments_accepted;
    return AuthorityUpdateResult::Accepted;
}

AuthorityUpdateResult SessionAuthority::acceptGridAuthorityState(
    std::uint64_t source_peer_id,
    const GridAuthorityState& state) noexcept
{
    if (grid_.revision == 0 || state.revision != grid_.revision) {
        ++stats_.grid_authority_states_rejected;
        return state.revision < grid_.revision
            ? AuthorityUpdateResult::StaleRevision
            : AuthorityUpdateResult::Invalid;
    }
    if (source_peer_id != grid_.authority_peer_id ||
        state.authority_peer_id != grid_.authority_peer_id) {
        ++stats_.grid_authority_states_rejected;
        return AuthorityUpdateResult::UnauthorizedSource;
    }
    if (state.mode > 2 || state.run_state != GridRunState::Running) {
        ++stats_.grid_authority_states_rejected;
        return AuthorityUpdateResult::Invalid;
    }
    grid_.run_state = state.run_state;
    grid_.mode = state.mode;
    grid_.authority_epoch_frame = state.authority_epoch_frame;
    grid_.authority_packet_frame = state.authority_packet_frame;
    ++stats_.grid_authority_states_accepted;
    return AuthorityUpdateResult::Accepted;
}

bool SessionAuthority::activateLocalGrid(
    std::uint64_t epoch_frame,
    std::uint64_t packet_frame) noexcept
{
    if (!localIsGridAuthority()) {
        return false;
    }
    grid_.authority_epoch_frame = epoch_frame;
    grid_.authority_packet_frame = packet_frame;
    return true;
}

bool SessionAuthority::markPeerInactive(std::uint64_t peer_id) noexcept
{
    if (grid_.revision == 0 || peer_id == 0 || peer_id != grid_.authority_peer_id ||
        grid_.run_state == GridRunState::AuthorityMissing) {
        return false;
    }
    grid_.run_state = GridRunState::AuthorityMissing;
    ++stats_.grid_authority_missing_events;
    return true;
}

bool SessionAuthority::acceptTransportEvent(
    std::uint64_t source_peer_id,
    std::uint64_t event_counter,
    std::uint64_t grid_revision,
    bool requires_arrangement_authority) noexcept
{
    const bool grid_matches = grid_revision == grid_.revision ||
        (grid_revision == 0 && grid_.revision == 0);
    if (source_peer_id == 0 || event_counter == 0 || !grid_matches ||
        (requires_arrangement_authority && source_peer_id != arrangement_authority_peer_id_)) {
        ++stats_.transport_events_rejected;
        return false;
    }

    const auto tracker = std::find_if(
        transport_trackers_.begin(),
        transport_trackers_.end(),
        [source_peer_id](const RequestTracker& value) { return value.peer_id == source_peer_id; });
    if (tracker != transport_trackers_.end()) {
        if (event_counter <= tracker->last_request_id) {
            ++stats_.transport_events_rejected;
            return false;
        }
        tracker->last_request_id = event_counter;
    } else {
        transport_trackers_.push_back({source_peer_id, event_counter});
    }
    ++stats_.transport_events_accepted;
    return true;
}

} // namespace jam2
