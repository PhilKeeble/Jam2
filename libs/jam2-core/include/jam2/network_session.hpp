#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "common.hpp"
#include "peer_mixer.hpp"
#include "peer_stream.hpp"
#include "protocol.hpp"
#include "udp_socket.hpp"

namespace jam2 {

struct NetworkSessionContract {
    std::uint8_t protocol_version = protocol::kProtocolVersion;
    NetworkAudioFormat audio_format = NetworkAudioFormat::Pcm24Mono;
    int sample_rate = 48000;
    int frames_per_packet = 128;
};

struct PeerId {
    std::uint64_t value = 0;

    friend constexpr bool operator==(PeerId, PeerId) = default;
};

enum class SessionBootstrapRole : std::uint8_t {
    Creator,
    Joiner,
};

enum class SessionBootstrapState : std::uint8_t {
    Configured,
    Established,
    Closed,
};

enum class PeerEndpointState : std::uint8_t {
    Candidate,
    Probing,
    Active,
    Failed,
};

struct NetworkPeerDescriptor {
    PeerId peer_id;
    ResolvedUdpEndpoint endpoint;
    PeerEndpointState endpoint_state = PeerEndpointState::Active;
};

struct NetworkPeerSendStats {
    std::uint64_t attempts = 0;
    std::uint64_t sent_packets = 0;
    std::uint64_t sent_bytes = 0;
    std::uint64_t would_block_drops = 0;
    std::uint64_t no_buffer_drops = 0;
    std::uint64_t unreachable_errors = 0;
    std::uint64_t refused_errors = 0;
    std::uint64_t fatal_errors = 0;
    std::uint64_t path_reprobe_transitions = 0;
    std::uint32_t consecutive_path_errors = 0;
    int last_error_code = 0;
    UdpSendOutcome last_outcome = UdpSendOutcome::Sent;
};

struct NetworkPeerSnapshot {
    NetworkPeerDescriptor descriptor;
    PeerStreamStats stream;
    PeerMixerPeerStats mix;
    NetworkPeerSendStats send;
    bool has_mix_stats = false;
};

struct NetworkSessionSnapshot {
    NetworkSessionContract contract;
    SessionBootstrapRole bootstrap_role = SessionBootstrapRole::Joiner;
    SessionBootstrapState bootstrap_state = SessionBootstrapState::Configured;
    PeerId local_peer_id;
    PeerMixerStats mix;
    std::vector<NetworkPeerSnapshot> peers;
};

class NetworkPacketSchedule {
public:
    NetworkPacketSchedule(
        int sample_rate,
        int frames_per_packet,
        std::uint64_t start_time_us,
        int clock_drift_ppm = 0);

    std::uint64_t startTimeUs() const noexcept;
    std::uint64_t nextAudioSendUs() const noexcept;
    std::uint64_t nextPingUs() const noexcept;
    std::uint64_t nextGridStateUs() const noexcept;
    std::uint64_t sampleTime() const noexcept;
    std::uint32_t audioSequence() const noexcept;
    std::uint32_t takeControlSequence() noexcept;

    void commitAudioPacket() noexcept;
    void commitPing(std::uint64_t interval_us = 100000ULL) noexcept;
    void scheduleNextGridState(std::uint64_t now_us, std::uint64_t interval_us) noexcept;

private:
    std::uint32_t audio_sequence_ = 1;
    std::uint32_t control_sequence_ = 1;
    std::uint64_t sample_time_ = 0;
    std::uint64_t start_time_us_ = 0;
    std::uint64_t next_audio_send_us_ = 0;
    std::uint64_t next_ping_us_ = 0;
    std::uint64_t next_grid_state_us_ = 0;
    std::uint64_t interval_us_ = 0;
    std::uint64_t interval_remainder_us_ = 0;
    std::uint64_t interval_denominator_ = 1;
    std::uint64_t interval_remainder_accumulator_ = 0;
    std::uint64_t frames_per_packet_ = 0;
};

struct NetworkDatagram {
    ResolvedUdpEndpoint endpoint;
    std::span<const std::uint8_t> bytes;
};

struct NetworkSendResult {
    std::size_t packet_size = 0;
    std::size_t attempted_peer_count = 0;
    std::size_t peer_count = 0;
    std::size_t total_bytes = 0;
    std::size_t transient_drops = 0;
    std::size_t path_errors = 0;
};

// Owns one UDP session, one packet schedule, and one mature PeerStream per
// remote peer. Creator/joiner remains bootstrap metadata only; steady-state
// audio fan-out, timing, resampling, and mixing are role-free.
class NetworkSession {
public:
    NetworkSession(
        UdpSocket&& socket,
        const SessionInfo& session,
        const NetworkSessionContract& contract,
        SessionBootstrapRole bootstrap_role,
        PeerId local_peer_id,
        const NetworkPeerDescriptor& remote_peer,
        const PeerStreamConfig& peer_config,
        PeerStreamPlayback* playback,
        int packet_clock_drift_ppm = 0);
    NetworkSession(
        UdpSocket&& socket,
        const SessionInfo& session,
        const NetworkSessionContract& contract,
        SessionBootstrapRole bootstrap_role,
        PeerId local_peer_id,
        std::span<const NetworkPeerDescriptor> remote_peers,
        const PeerStreamConfig& peer_config,
        PeerStreamPlayback* playback,
        int packet_clock_drift_ppm = 0);
    ~NetworkSession();

    NetworkSession(const NetworkSession&) = delete;
    NetworkSession& operator=(const NetworkSession&) = delete;
    NetworkSession(NetworkSession&&) noexcept;
    NetworkSession& operator=(NetworkSession&&) noexcept;

    const NetworkSessionContract& contract() const noexcept;
    std::uint64_t sessionId() const noexcept;
    SessionBootstrapRole bootstrapRole() const noexcept;
    SessionBootstrapState bootstrapState() const noexcept;
    PeerId localPeerId() const noexcept;
    std::size_t peerCount() const noexcept;
    std::size_t activePeerCount() const noexcept;
    // Non-real-time diagnostic view. Storage grows only when the caller asks
    // for a snapshot; each peer's stream and mixer queues remain bounded.
    NetworkSessionSnapshot snapshot() const;
    const NetworkPeerDescriptor& remotePeer() const;
    const NetworkPeerDescriptor* peer(PeerId peer_id) const noexcept;
    PeerId peerIdForEndpoint(const ResolvedUdpEndpoint& endpoint) const noexcept;
    bool recognizesEndpoint(const ResolvedUdpEndpoint& endpoint) const noexcept;
    bool acceptsEndpoint(const ResolvedUdpEndpoint& endpoint) const noexcept;

    bool addPeer(const NetworkPeerDescriptor& peer, const PeerStreamConfig& config);
    bool removePeer(PeerId peer_id) noexcept;
    bool updatePeerEndpoint(
        PeerId peer_id,
        const ResolvedUdpEndpoint& endpoint,
        PeerEndpointState state = PeerEndpointState::Candidate);
    bool setPeerEndpointState(PeerId peer_id, PeerEndpointState state) noexcept;
    bool setPeerGain(PeerId peer_id, int gain_ppm) noexcept;
    bool setPeerMuted(PeerId peer_id, bool muted) noexcept;

    NetworkPacketSchedule& schedule() noexcept;
    const NetworkPacketSchedule& schedule() const noexcept;
    PeerStream& peerStream() noexcept;
    const PeerStream& peerStream() const noexcept;
    PeerStream& peerStream(PeerId peer_id);
    const PeerStream& peerStream(PeerId peer_id) const;
    const PeerMixerStats& mixStats() const noexcept;
    const PeerMixerPeerStats* peerMixStats(PeerId peer_id) const noexcept;
    const NetworkPeerSendStats* peerSendStats(PeerId peer_id) const noexcept;

    void advance(std::uint64_t now_us) noexcept;
    void finish(std::uint64_t now_us) noexcept;

    std::size_t send(
        protocol::PacketType type,
        std::uint32_t sequence,
        std::uint64_t timing_value,
        std::span<const std::uint8_t> payload);
    NetworkSendResult sendToActive(
        protocol::PacketType type,
        std::uint32_t sequence,
        std::uint64_t timing_value,
        std::span<const std::uint8_t> payload);
    std::size_t sendToPeer(
        PeerId peer_id,
        protocol::PacketType type,
        std::uint32_t sequence,
        std::uint64_t timing_value,
        std::span<const std::uint8_t> payload,
        bool allow_inactive = false);
    std::optional<NetworkDatagram> receiveFor(std::uint64_t timeout_us);
    protocol::ParseResult parse(std::span<const std::uint8_t> packet) const noexcept;
    void close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jam2
