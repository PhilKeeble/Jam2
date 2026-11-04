#pragma once
#include "udp_socket.hpp"

#if defined(__APPLE__)
namespace jam2 {
// Opt-in diagnostic backend. Callbacks run on an owned serial dispatch queue,
// never on the audio thread. BSD remains the product default.
class NetworkFrameworkUdp {
public:
    explicit NetworkFrameworkUdp(UdpServiceClass service);
    ~NetworkFrameworkUdp();
    NetworkFrameworkUdp(const NetworkFrameworkUdp&) = delete;
    NetworkFrameworkUdp& operator=(const NetworkFrameworkUdp&) = delete;
    void bind(const Endpoint& endpoint, const ResolvedUdpEndpoint& remote);
    Endpoint local_endpoint() const;
    int duplicate_wait_handle() const;
    UdpServiceClass service_class() const;
    void set_receive_timestamps(bool enabled);
    UdpSendResult send_to(const ResolvedUdpEndpoint&, std::span<const std::uint8_t>) noexcept;
    std::optional<UdpSocket::ReceivedDatagram> take(std::span<std::uint8_t>);
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
#endif
