#include "network_framework_udp.hpp"
#include <Network/Network.h>
#include <dispatch/dispatch.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <time.h>

namespace jam2 {
namespace {
template<class T> struct NetworkOwner {
    T value = nullptr;
    NetworkOwner() = default;
    explicit NetworkOwner(T v) : value(v) {}
    ~NetworkOwner() { if (value) nw_release(value); }
    NetworkOwner(const NetworkOwner&) = delete;
    NetworkOwner& operator=(const NetworkOwner&) = delete;
};
struct DispatchOwner {
    dispatch_queue_t value = dispatch_queue_create("jam2.udp.network-experiment", DISPATCH_QUEUE_SERIAL);
    ~DispatchOwner() { if (value) dispatch_release(value); }
};
NetworkOwner<nw_endpoint_t> make_endpoint(const ResolvedUdpEndpoint& address)
{
    sockaddr_in value{}; value.sin_len = sizeof(value); value.sin_family = AF_INET;
    value.sin_addr.s_addr = address.address; value.sin_port = htons(address.port);
    return NetworkOwner<nw_endpoint_t>(nw_endpoint_create_address(reinterpret_cast<sockaddr*>(&value)));
}
}
struct NetworkFrameworkUdp::State : std::enable_shared_from_this<State> {
    struct Peer {
        NetworkOwner<nw_connection_t> connection;
        ResolvedUdpEndpoint endpoint;
        bool ready = false;
        Peer(nw_connection_t c, ResolvedUdpEndpoint e) : connection(c), endpoint(e) {}
        ~Peer() { if (connection.value) nw_connection_cancel(connection.value); }
    };
    struct Packet {
        UdpSocket::ReceivedDatagram metadata;
        std::array<std::uint8_t, 1500> bytes{};
    };
    DispatchOwner queue;
    NetworkOwner<nw_parameters_t> parameters;
    UdpServiceClass service;
    std::mutex mutex;
    std::condition_variable changed;
    Endpoint local;
    int read_fd = -1, write_fd = -1, error = 0;
    bool ready = false, stopping = false, timestamps = false;
    std::shared_ptr<Peer> peer; // One already-proven remote; dispatch queue owns connection state.
    std::array<Packet,UdpSocket::kFrameworkReceivePackets> packets;
    std::size_t read = 0, write = 0, count = 0;
    std::size_t pending_sends = 0;
    explicit State(UdpServiceClass s) : service(s) {
        if (!queue.value) throw std::runtime_error("create Network UDP queue failed");
        int fds[2];
        if (pipe(fds)) throw std::runtime_error("create Network UDP readiness pipe failed");
        read_fd=fds[0]; write_fd=fds[1];
        for (int fd : fds) if (fcntl(fd,F_SETFL,O_NONBLOCK)<0 || fcntl(fd,F_SETFD,FD_CLOEXEC)<0) {
            close(read_fd); close(write_fd); read_fd=write_fd=-1;
            throw std::runtime_error("configure Network UDP readiness pipe failed");
        }
    }
    ~State() { if (read_fd>=0) close(read_fd); if (write_fd>=0) close(write_fd); }
    void fail(int code) noexcept {
        std::lock_guard lock(mutex); if (!error) error=code ? code : EIO;
        changed.notify_all();
        const std::uint8_t byte=1; (void)::write(write_fd,&byte,1);
    }
    void receive(const std::shared_ptr<Peer>& peer) {
        std::weak_ptr<State> weak=shared_from_this(); std::weak_ptr<Peer> weakPeer=peer;
        nw_connection_receive_message(peer->connection.value, ^(dispatch_data_t data, nw_content_context_t context, bool, nw_error_t failure) {
            const auto self=weak.lock(); const auto flow=weakPeer.lock();
            if (!self || !flow || self->stopping) return;
            if (failure) { self->fail(nw_error_get_error_code(failure)); return; }
            if (data) {
                const auto callback_us=receive_diagnostic_time_us();
                const auto raw_ns=clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
                std::uint64_t arrival=0;
                NetworkOwner<nw_protocol_definition_t> definition(nw_protocol_copy_ip_definition());
                NetworkOwner<nw_protocol_metadata_t> metadata(context ? nw_content_context_copy_protocol_metadata(context,definition.value) : nullptr);
                if (metadata.value) {
                    const auto stamp=nw_ip_metadata_get_receive_time(metadata.value);
                    // Network's CLOCK_MONOTONIC_RAW nanoseconds may have a different
                    // epoch from Mach absolute ticks after sleep. Translate the age
                    // using this callback's adjacent clock pair; never equate epochs.
                    if (stamp && raw_ns>=stamp && (raw_ns-stamp)/1000<=callback_us)
                        arrival=callback_us-(raw_ns-stamp)/1000;
                }
                const auto size=dispatch_data_get_size(data);
                if (size<=1500) {
                    std::lock_guard lock(self->mutex);
                    if (self->count==self->packets.size()) { self->error=ENOBUFS; return; }
                    auto& packet=self->packets[self->write];
                    packet.metadata={flow->endpoint,size};
                    if (self->timestamps) packet.metadata.network_callback_us=callback_us;
                    if (self->timestamps && arrival) {
                        packet.metadata.kernel_arrival_us=arrival;
                        packet.metadata.observed_us=callback_us;
                        // These mark callback entry, not a fabricated recvmsg call.
                        packet.metadata.read_begin_us=callback_us; packet.metadata.read_end_us=callback_us;
                        packet.metadata.socket_residence_us=callback_us-arrival;
                    }
                    auto* destination=packet.bytes.data();
                    dispatch_data_apply(data, ^bool(dispatch_data_t, size_t offset, const void* buffer, size_t length) {
                        std::memcpy(destination+offset,buffer,length); return true;
                    });
                    if (!self->count) { const std::uint8_t byte=1; (void)::write(self->write_fd,&byte,1); }
                    self->write=(self->write+1)%self->packets.size(); ++self->count;
                }
            }
            self->receive(flow);
        });
    }
    void start_peer(const std::shared_ptr<Peer>& peer) {
        std::weak_ptr<State> weak=shared_from_this(); std::weak_ptr<Peer> weakPeer=peer;
        nw_connection_set_queue(peer->connection.value,queue.value);
        nw_connection_set_state_changed_handler(peer->connection.value, ^(nw_connection_state_t status, nw_error_t failure) {
            const auto self=weak.lock(); const auto flow=weakPeer.lock();
            if (!self || !flow || self->stopping) return;
            if (status==nw_connection_state_ready && !flow->ready) {
                flow->ready=true;
                { std::lock_guard lock(self->mutex); self->ready=true; self->changed.notify_all(); }
                self->receive(flow);
            }
            else if (status==nw_connection_state_failed || (status==nw_connection_state_waiting && failure))
                self->fail(failure ? nw_error_get_error_code(failure) : EIO);
        });
        nw_connection_start(peer->connection.value);
    }
};

NetworkFrameworkUdp::NetworkFrameworkUdp(UdpServiceClass service) : state_(std::make_shared<State>(service)) {}
NetworkFrameworkUdp::~NetworkFrameworkUdp() {
    const auto self=state_;
    dispatch_sync(self->queue.value, ^{
        self->stopping=true;
        if (self->peer) nw_connection_cancel(self->peer->connection.value);
        self->peer.reset();
    });
}
int NetworkFrameworkUdp::duplicate_wait_handle() const {
    const int fd=dup(state_->read_fd);
    if (fd<0) throw std::runtime_error("duplicate Network UDP wait handle failed");
    if (fcntl(fd,F_SETFD,FD_CLOEXEC)<0) { close(fd); throw std::runtime_error("configure Network UDP wait handle failed"); }
    return fd;
}
void NetworkFrameworkUdp::bind(const Endpoint& endpoint, const ResolvedUdpEndpoint& remote) {
    const auto self=state_;
    if (self->peer) throw std::runtime_error("Network UDP already bound");
    self->local=endpoint;
    self->parameters.value=nw_parameters_create_secure_udp(NW_PARAMETERS_DISABLE_PROTOCOL,NW_PARAMETERS_DEFAULT_CONFIGURATION);
    if (!self->parameters.value) throw std::runtime_error("create Network UDP parameters failed");
    nw_parameters_set_include_peer_to_peer(self->parameters.value,false);
    nw_parameters_set_service_class(self->parameters.value,self->service==UdpServiceClass::InteractiveVoice
        ? nw_service_class_interactive_voice : nw_service_class_best_effort);
    auto local=make_endpoint(resolve_udp_endpoint(endpoint));
    nw_parameters_set_local_endpoint(self->parameters.value,local.value);
    NetworkOwner<nw_protocol_stack_t> stack(nw_parameters_copy_default_protocol_stack(self->parameters.value));
    NetworkOwner<nw_protocol_options_t> ip(nw_protocol_stack_copy_internet_protocol(stack.value));
    nw_ip_options_set_calculate_receive_time(ip.value,true);
    auto destination=make_endpoint(remote);
    NetworkOwner<nw_connection_t> connection(nw_connection_create(destination.value,self->parameters.value));
    if (!connection.value) throw std::runtime_error("create connected Network UDP flow failed");
    self->peer=std::make_shared<State::Peer>(connection.value,remote);
    connection.value=nullptr; // Peer now owns this retained reference.
    dispatch_sync(self->queue.value, ^{ self->start_peer(self->peer); });
    std::unique_lock lock(self->mutex);
    if (!self->changed.wait_for(lock,std::chrono::seconds(5),[&] { return self->ready || self->error; }) || self->error)
        throw std::system_error(self->error ? self->error : ETIMEDOUT,std::generic_category(),"Network UDP connection failed");
}
Endpoint NetworkFrameworkUdp::local_endpoint() const { std::lock_guard lock(state_->mutex); return state_->local; }
UdpServiceClass NetworkFrameworkUdp::service_class() const { return state_->service; }
void NetworkFrameworkUdp::set_receive_timestamps(bool enabled) { std::lock_guard lock(state_->mutex); state_->timestamps=enabled; }
UdpSendResult NetworkFrameworkUdp::send_to(const ResolvedUdpEndpoint& endpoint,std::span<const std::uint8_t> bytes) noexcept {
    try {
        const auto self=state_;
        __block UdpSendResult result;
        dispatch_sync(self->queue.value, ^{
            try {
                if (self->stopping || self->pending_sends>=UdpSocket::kFrameworkPendingSends) { result={UdpSendOutcome::WouldBlock,EAGAIN}; return; }
                if (!self->peer || self->peer->endpoint!=endpoint) { result={UdpSendOutcome::Fatal,EINVAL}; return; }
                const auto data=dispatch_data_create(bytes.data(),bytes.size(),self->queue.value,DISPATCH_DATA_DESTRUCTOR_DEFAULT);
                if (!data) { result={UdpSendOutcome::Fatal,ENOMEM}; return; }
                ++self->pending_sends; std::weak_ptr<State> weak=self;
                nw_connection_send(self->peer->connection.value,data,NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT,true, ^(nw_error_t error) {
                    if (const auto owner=weak.lock()) { --owner->pending_sends; if (error && !owner->stopping) owner->fail(nw_error_get_error_code(error)); }
                });
                dispatch_release(data);
            } catch (...) { result={UdpSendOutcome::Fatal,ENOMEM}; }
        });
        return result;
    } catch (...) { return {UdpSendOutcome::Fatal,EIO}; }
}
std::optional<UdpSocket::ReceivedDatagram> NetworkFrameworkUdp::take(std::span<std::uint8_t> buffer) {
    const auto begin=receive_diagnostic_time_us();
    std::lock_guard lock(state_->mutex);
    if (state_->error) throw std::system_error(state_->error,std::generic_category(),"Network UDP asynchronous failure");
    if (!state_->count) return {};
    const auto& packet=state_->packets[state_->read]; auto result=packet.metadata;
    const bool fits=result.size<=buffer.size(); if (fits) std::copy_n(packet.bytes.data(),result.size,buffer.data());
    state_->read=(state_->read+1)%state_->packets.size(); --state_->count;
    if (!state_->count) { std::array<std::uint8_t,64> drain; while (::read(state_->read_fd,drain.data(),drain.size())>0) {} }
    if (state_->timestamps) {
        const auto now=receive_diagnostic_time_us();
        result.read_begin_us=begin; result.read_end_us=now; result.observed_us=now;
        if (result.kernel_arrival_us && now>=result.kernel_arrival_us) result.socket_residence_us=now-result.kernel_arrival_us;
    }
    return fits ? std::optional(result) : std::nullopt;
}
}
