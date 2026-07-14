#include "network_session.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace jam2 {

NetworkPacketSchedule::NetworkPacketSchedule(
    int sample_rate,
    int frames_per_packet,
    std::uint64_t start_time_us)
    : start_time_us_(start_time_us),
      next_audio_send_us_(start_time_us),
      next_ping_us_(start_time_us),
      next_grid_state_us_(start_time_us),
      interval_denominator_(static_cast<std::uint64_t>(sample_rate)),
      frames_per_packet_(static_cast<std::uint64_t>(frames_per_packet))
{
    if (sample_rate <= 0 || frames_per_packet <= 0) {
        throw std::runtime_error("invalid NetworkSession packet schedule");
    }
    const std::uint64_t numerator = frames_per_packet_ * 1000000ULL;
    interval_us_ = numerator / interval_denominator_;
    interval_remainder_us_ = numerator % interval_denominator_;
}

std::uint64_t NetworkPacketSchedule::startTimeUs() const noexcept { return start_time_us_; }
std::uint64_t NetworkPacketSchedule::nextAudioSendUs() const noexcept { return next_audio_send_us_; }
std::uint64_t NetworkPacketSchedule::nextPingUs() const noexcept { return next_ping_us_; }
std::uint64_t NetworkPacketSchedule::nextGridStateUs() const noexcept { return next_grid_state_us_; }
std::uint64_t NetworkPacketSchedule::sampleTime() const noexcept { return sample_time_; }
std::uint32_t NetworkPacketSchedule::audioSequence() const noexcept { return audio_sequence_; }

std::uint32_t NetworkPacketSchedule::takeControlSequence() noexcept
{
    return control_sequence_++;
}

void NetworkPacketSchedule::commitAudioPacket() noexcept
{
    ++audio_sequence_;
    sample_time_ += frames_per_packet_;
    std::uint64_t step = interval_us_ == 0 ? 1 : interval_us_;
    interval_remainder_accumulator_ += interval_remainder_us_;
    if (interval_remainder_accumulator_ >= interval_denominator_) {
        ++step;
        interval_remainder_accumulator_ -= interval_denominator_;
    }
    next_audio_send_us_ += step;
}

void NetworkPacketSchedule::commitPing(std::uint64_t interval_us) noexcept
{
    next_ping_us_ += interval_us;
}

void NetworkPacketSchedule::scheduleNextGridState(
    std::uint64_t now_us,
    std::uint64_t interval_us) noexcept
{
    next_grid_state_us_ = now_us + interval_us;
}

struct NetworkSession::Impl {
    struct PeerEntry {
        NetworkPeerDescriptor descriptor;
        PeerStreamConfig config;
        std::unique_ptr<PeerStream> stream;
    };

    UdpSocket socket;
    SessionInfo session;
    const NetworkSessionContract contract;
    const SessionBootstrapRole bootstrap_role;
    SessionBootstrapState bootstrap_state = SessionBootstrapState::Established;
    const PeerId local_peer_id;
    NetworkPacketSchedule schedule;
    PeerMixer mixer;
    PeerStreamPlayback* playback = nullptr;
    std::vector<std::unique_ptr<PeerEntry>> peers;
    std::array<std::uint8_t, protocol::kMaxDatagramSize> transmit_packet{};
    std::array<std::uint8_t, protocol::kMaxDatagramSize> receive_packet{};

    Impl(
        UdpSocket&& owned_socket,
        const SessionInfo& session_info,
        const NetworkSessionContract& session_contract,
        SessionBootstrapRole role,
        PeerId local_id,
        std::span<const NetworkPeerDescriptor> initial_peers,
        const PeerStreamConfig& peer_config,
        PeerStreamPlayback* output)
        : socket(std::move(owned_socket)),
          session(session_info),
          contract(session_contract),
          bootstrap_role(role),
          local_peer_id(local_id),
          schedule(contract.sample_rate, contract.frames_per_packet, monotonic_us()),
          mixer({
              contract.sample_rate,
              contract.frames_per_packet,
              std::max<std::size_t>({
                  static_cast<std::size_t>(contract.frames_per_packet) * 2U,
                  peer_config.playout_delay_frames,
                  peer_config.jitter_buffer_frames,
                  peer_config.jitter_buffer_max_frames,
              }),
              peer_config.playback_max_frames,
              64,
              peer_config.adaptive_playback_cushion,
              peer_config.adaptive_playback_target_frames,
              peer_config.adaptive_playback_min_frames,
              peer_config.adaptive_playback_max_frames,
              peer_config.adaptive_playback_release_ppm}, output),
          playback(output)
    {
        validateContract();
        validatePeerConfig(peer_config);
        if (local_peer_id.value == 0) {
            throw std::runtime_error("NetworkSession requires a nonzero local peer ID");
        }
        peers.reserve(initial_peers.size());
        for (const auto& peer : initial_peers) {
            if (!addPeer(peer, peer_config)) {
                throw std::runtime_error("duplicate NetworkSession peer descriptor");
            }
        }
    }

    void validateContract() const
    {
        if (contract.protocol_version != 1 || contract.audio_format != NetworkAudioFormat::Pcm24Mono ||
            contract.sample_rate <= 0 || contract.frames_per_packet <= 0 ||
            contract.frames_per_packet > static_cast<int>(protocol::kMaxAudioFramesPerPacket)) {
            throw std::runtime_error("unsupported immutable NetworkSession contract");
        }
    }

    void validatePeerConfig(const PeerStreamConfig& config) const
    {
        if (config.sample_rate != contract.sample_rate ||
            config.frames_per_packet != contract.frames_per_packet) {
            throw std::runtime_error("PeerStream tuning does not match the NetworkSession contract");
        }
    }

    PeerEntry* find(PeerId peer_id) noexcept
    {
        for (auto& peer : peers) {
            if (peer->descriptor.peer_id == peer_id) {
                return peer.get();
            }
        }
        return nullptr;
    }

    const PeerEntry* find(PeerId peer_id) const noexcept
    {
        for (const auto& peer : peers) {
            if (peer->descriptor.peer_id == peer_id) {
                return peer.get();
            }
        }
        return nullptr;
    }

    PeerEntry* find(const Endpoint& endpoint) noexcept
    {
        for (auto& peer : peers) {
            if (peer->descriptor.endpoint.host == endpoint.host &&
                peer->descriptor.endpoint.port == endpoint.port) {
                return peer.get();
            }
        }
        return nullptr;
    }

    const PeerEntry* find(const Endpoint& endpoint) const noexcept
    {
        for (const auto& peer : peers) {
            if (peer->descriptor.endpoint.host == endpoint.host &&
                peer->descriptor.endpoint.port == endpoint.port) {
                return peer.get();
            }
        }
        return nullptr;
    }

    bool addPeer(const NetworkPeerDescriptor& descriptor, const PeerStreamConfig& config)
    {
        validatePeerConfig(config);
        if (descriptor.peer_id.value == 0 || descriptor.peer_id == local_peer_id ||
            find(descriptor.peer_id) != nullptr || find(descriptor.endpoint) != nullptr) {
            return false;
        }
        PeerStreamPlayback* peer_playback = nullptr;
        if (playback != nullptr) {
            const std::size_t base_capacity = config.playback_queue_capacity_frames > 0
                ? config.playback_queue_capacity_frames
                : std::max<std::size_t>(
                      config.adaptive_playback_max_frames,
                      static_cast<std::size_t>(config.frames_per_packet) * 8U);
            const std::size_t timing_headroom = std::max<std::size_t>(
                config.adaptive_playback_max_frames,
                config.jitter_buffer_max_frames);
            const std::size_t requested_capacity = base_capacity >
                    (std::numeric_limits<std::size_t>::max)() - timing_headroom
                ? base_capacity
                : base_capacity + timing_headroom;
            peer_playback = mixer.addPeer(descriptor.peer_id.value, requested_capacity);
        }
        try {
            auto entry = std::make_unique<PeerEntry>();
            entry->descriptor = descriptor;
            entry->config = config;
            if (playback != nullptr) {
                // The shared final output limit is enforced once by PeerMixer.
                // A per-peer limit that includes the shared output depth would
                // make every stream repeatedly discard its own source frames.
                entry->config.playback_max_frames = 0;
                // The same applies to adaptive cushion padding: it belongs to
                // the one mixed local output, not independently to every
                // source queue. PeerMixer preserves the configured behavior.
                entry->config.adaptive_playback_cushion = false;
            }
            entry->stream = std::make_unique<PeerStream>(
                entry->config,
                schedule.startTimeUs(),
                peer_playback);
            peers.push_back(std::move(entry));
            if (peer_playback != nullptr) {
                mixer.setPeerActive(
                    descriptor.peer_id.value,
                    descriptor.endpoint_state == PeerEndpointState::Active);
            }
        } catch (...) {
            if (peer_playback != nullptr) {
                mixer.removePeer(descriptor.peer_id.value);
            }
            throw;
        }
        return true;
    }

    std::span<const std::uint8_t> encode(
        protocol::PacketType type,
        std::uint32_t sequence,
        std::uint64_t sample_time,
        std::uint64_t send_time_us,
        std::span<const std::uint8_t> payload)
    {
        if (bootstrap_state != SessionBootstrapState::Established) {
            throw std::runtime_error("cannot send on a closed NetworkSession");
        }
        const protocol::Header header{
            type,
            0,
            session.session_id,
            sequence,
            sample_time,
            send_time_us,
            0,
            0,
        };
        const std::size_t packet_size = protocol::encode_packet_into(
            header,
            payload,
            session.key,
            transmit_packet);
        if (packet_size == 0) {
            throw std::runtime_error("failed to encode bounded UDP v1 packet");
        }
        return std::span<const std::uint8_t>(transmit_packet.data(), packet_size);
    }
};

NetworkSession::NetworkSession(
    UdpSocket&& socket,
    const SessionInfo& session,
    const NetworkSessionContract& contract,
    SessionBootstrapRole bootstrap_role,
    PeerId local_peer_id,
    const NetworkPeerDescriptor& remote_peer,
    const PeerStreamConfig& peer_config,
    PeerStreamPlayback* playback)
    : NetworkSession(
          std::move(socket),
          session,
          contract,
          bootstrap_role,
          local_peer_id,
          std::span<const NetworkPeerDescriptor>(&remote_peer, 1),
          peer_config,
          playback)
{
}

NetworkSession::NetworkSession(
    UdpSocket&& socket,
    const SessionInfo& session,
    const NetworkSessionContract& contract,
    SessionBootstrapRole bootstrap_role,
    PeerId local_peer_id,
    std::span<const NetworkPeerDescriptor> remote_peers,
    const PeerStreamConfig& peer_config,
    PeerStreamPlayback* playback)
    : impl_(std::make_unique<Impl>(
          std::move(socket),
          session,
          contract,
          bootstrap_role,
          local_peer_id,
          remote_peers,
          peer_config,
          playback))
{
}

NetworkSession::~NetworkSession() = default;
NetworkSession::NetworkSession(NetworkSession&&) noexcept = default;
NetworkSession& NetworkSession::operator=(NetworkSession&&) noexcept = default;

const NetworkSessionContract& NetworkSession::contract() const noexcept { return impl_->contract; }
std::uint64_t NetworkSession::sessionId() const noexcept { return impl_->session.session_id; }
SessionBootstrapRole NetworkSession::bootstrapRole() const noexcept { return impl_->bootstrap_role; }
SessionBootstrapState NetworkSession::bootstrapState() const noexcept { return impl_->bootstrap_state; }
PeerId NetworkSession::localPeerId() const noexcept { return impl_->local_peer_id; }
std::size_t NetworkSession::peerCount() const noexcept { return impl_->peers.size(); }

std::size_t NetworkSession::activePeerCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(
        impl_->peers.begin(),
        impl_->peers.end(),
        [](const auto& peer) {
            return peer->descriptor.endpoint_state == PeerEndpointState::Active;
        }));
}

const NetworkPeerDescriptor& NetworkSession::remotePeer() const
{
    if (impl_->peers.empty()) {
        throw std::runtime_error("NetworkSession has no remote peers");
    }
    return impl_->peers.front()->descriptor;
}

const NetworkPeerDescriptor* NetworkSession::peer(PeerId peer_id) const noexcept
{
    const auto* entry = impl_->find(peer_id);
    return entry != nullptr ? &entry->descriptor : nullptr;
}

PeerId NetworkSession::peerIdForEndpoint(const Endpoint& endpoint) const noexcept
{
    const auto* entry = impl_->find(endpoint);
    return entry != nullptr ? entry->descriptor.peer_id : PeerId{};
}

bool NetworkSession::recognizesEndpoint(const Endpoint& endpoint) const noexcept
{
    return impl_->bootstrap_state == SessionBootstrapState::Established &&
        impl_->find(endpoint) != nullptr;
}

bool NetworkSession::acceptsEndpoint(const Endpoint& endpoint) const noexcept
{
    const auto* entry = impl_->find(endpoint);
    return impl_->bootstrap_state == SessionBootstrapState::Established &&
        entry != nullptr &&
        entry->descriptor.endpoint_state == PeerEndpointState::Active;
}

bool NetworkSession::addPeer(
    const NetworkPeerDescriptor& peer,
    const PeerStreamConfig& config)
{
    return impl_->addPeer(peer, config);
}

bool NetworkSession::removePeer(PeerId peer_id) noexcept
{
    const auto found = std::find_if(
        impl_->peers.begin(),
        impl_->peers.end(),
        [peer_id](const auto& peer) { return peer->descriptor.peer_id == peer_id; });
    if (found == impl_->peers.end()) {
        return false;
    }
    impl_->mixer.removePeer(peer_id.value);
    impl_->peers.erase(found);
    return true;
}

bool NetworkSession::updatePeerEndpoint(
    PeerId peer_id,
    const Endpoint& endpoint,
    PeerEndpointState state)
{
    auto* entry = impl_->find(peer_id);
    if (entry == nullptr) {
        return false;
    }
    const auto* endpoint_owner = impl_->find(endpoint);
    if (endpoint_owner != nullptr && endpoint_owner->descriptor.peer_id != peer_id) {
        return false;
    }
    const PeerStreamConfig config = entry->config;
    impl_->mixer.removePeer(peer_id.value);
    PeerStreamPlayback* playback = nullptr;
    if (impl_->playback != nullptr) {
        const std::size_t capacity = config.playback_queue_capacity_frames > 0
            ? config.playback_queue_capacity_frames
            : std::max<std::size_t>(
                  config.adaptive_playback_max_frames,
                  static_cast<std::size_t>(config.frames_per_packet) * 8U);
        const std::size_t timing_headroom = std::max<std::size_t>(
            config.adaptive_playback_max_frames,
            config.jitter_buffer_max_frames);
        const std::size_t bounded_capacity = capacity >
                (std::numeric_limits<std::size_t>::max)() - timing_headroom
            ? capacity
            : capacity + timing_headroom;
        playback = impl_->mixer.addPeer(peer_id.value, bounded_capacity);
    }
    entry->descriptor.endpoint = endpoint;
    entry->descriptor.endpoint_state = state;
    entry->stream = std::make_unique<PeerStream>(config, monotonic_us(), playback);
    if (playback != nullptr) {
        impl_->mixer.setPeerActive(peer_id.value, state == PeerEndpointState::Active);
    }
    return true;
}

bool NetworkSession::setPeerEndpointState(PeerId peer_id, PeerEndpointState state) noexcept
{
    auto* entry = impl_->find(peer_id);
    if (entry == nullptr) {
        return false;
    }
    entry->descriptor.endpoint_state = state;
    if (impl_->playback != nullptr) {
        impl_->mixer.setPeerActive(peer_id.value, state == PeerEndpointState::Active);
    }
    return true;
}

bool NetworkSession::setPeerGain(PeerId peer_id, int gain_ppm) noexcept
{
    return impl_->mixer.setPeerGain(peer_id.value, gain_ppm);
}

bool NetworkSession::setPeerMuted(PeerId peer_id, bool muted) noexcept
{
    return impl_->mixer.setPeerMuted(peer_id.value, muted);
}

NetworkPacketSchedule& NetworkSession::schedule() noexcept { return impl_->schedule; }
const NetworkPacketSchedule& NetworkSession::schedule() const noexcept { return impl_->schedule; }
PeerStream& NetworkSession::peerStream() noexcept { return *impl_->peers.front()->stream; }
const PeerStream& NetworkSession::peerStream() const noexcept { return *impl_->peers.front()->stream; }

PeerStream& NetworkSession::peerStream(PeerId peer_id)
{
    auto* entry = impl_->find(peer_id);
    if (entry == nullptr) {
        throw std::runtime_error("unknown NetworkSession peer ID");
    }
    return *entry->stream;
}

const PeerStream& NetworkSession::peerStream(PeerId peer_id) const
{
    const auto* entry = impl_->find(peer_id);
    if (entry == nullptr) {
        throw std::runtime_error("unknown NetworkSession peer ID");
    }
    return *entry->stream;
}

const PeerMixerStats& NetworkSession::mixStats() const noexcept { return impl_->mixer.stats(); }

const PeerMixerPeerStats* NetworkSession::peerMixStats(PeerId peer_id) const noexcept
{
    return impl_->mixer.peerStats(peer_id.value);
}

void NetworkSession::advance(std::uint64_t now_us) noexcept
{
    for (auto& peer : impl_->peers) {
        peer->stream->advance(now_us);
    }
    impl_->mixer.advance(now_us);
}

void NetworkSession::finish(std::uint64_t now_us) noexcept
{
    for (auto& peer : impl_->peers) {
        peer->stream->finish(now_us);
    }
    impl_->mixer.finish(now_us);
}

NetworkSendResult NetworkSession::sendToActive(
    protocol::PacketType type,
    std::uint32_t sequence,
    std::uint64_t sample_time,
    std::uint64_t send_time_us,
    std::span<const std::uint8_t> payload)
{
    const auto packet = impl_->encode(type, sequence, sample_time, send_time_us, payload);
    NetworkSendResult result;
    result.packet_size = packet.size();
    for (const auto& peer : impl_->peers) {
        if (peer->descriptor.endpoint_state != PeerEndpointState::Active) {
            continue;
        }
        impl_->socket.send_to(peer->descriptor.endpoint, packet);
        ++result.peer_count;
        result.total_bytes += packet.size();
    }
    return result;
}

std::size_t NetworkSession::send(
    protocol::PacketType type,
    std::uint32_t sequence,
    std::uint64_t sample_time,
    std::uint64_t send_time_us,
    std::span<const std::uint8_t> payload)
{
    return sendToActive(type, sequence, sample_time, send_time_us, payload).packet_size;
}

std::size_t NetworkSession::sendToPeer(
    PeerId peer_id,
    protocol::PacketType type,
    std::uint32_t sequence,
    std::uint64_t sample_time,
    std::uint64_t send_time_us,
    std::span<const std::uint8_t> payload,
    bool allow_inactive)
{
    const auto* peer = impl_->find(peer_id);
    if (peer == nullptr ||
        (!allow_inactive && peer->descriptor.endpoint_state != PeerEndpointState::Active)) {
        return 0;
    }
    const auto packet = impl_->encode(type, sequence, sample_time, send_time_us, payload);
    impl_->socket.send_to(peer->descriptor.endpoint, packet);
    return packet.size();
}

std::optional<NetworkDatagram> NetworkSession::receiveFor(std::uint64_t timeout_us)
{
    const auto received = impl_->socket.recv_from_for(impl_->receive_packet, timeout_us);
    if (!received) {
        return std::nullopt;
    }
    return NetworkDatagram{
        received->endpoint,
        std::span<const std::uint8_t>(impl_->receive_packet.data(), received->size),
    };
}

protocol::ParseResult NetworkSession::parse(std::span<const std::uint8_t> packet) const noexcept
{
    return protocol::parse_packet(packet, impl_->session.key, impl_->session.session_id);
}

void NetworkSession::close() noexcept
{
    impl_->bootstrap_state = SessionBootstrapState::Closed;
    for (const auto& peer : impl_->peers) {
        impl_->mixer.setPeerActive(peer->descriptor.peer_id.value, false);
    }
}

} // namespace jam2
