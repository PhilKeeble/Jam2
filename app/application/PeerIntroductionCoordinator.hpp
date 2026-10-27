#pragma once
#include "PeerBootstrap.hpp"
#include <array>
#include <map>
#include <vector>

namespace jam2::application {

// Creator-owned state for B-C introductions. It never relays control or audio.
// All calls run on the multiplex worker; sockets remain owned by that worker.
class PeerIntroductionCoordinator final {
public:
    static constexpr quint64 kRepeatUs=500000, kAttemptTimeoutUs=4000000;
    static constexpr quint8 kMaximumAttempts=10;
    struct Transmission { PeerBootstrapEndpoint to; PeerBootstrapMessage message; };
    struct Failure { QString a,b,request; };
    PeerIntroductionCoordinator(quint64 session,QString creator);
    void admit(const QString& peer,PeerBootstrapEndpoint endpoint);
    void remove(const QString& peer);
    bool introduce(const QString& a,const QString& b,const QString& request,quint64 now);
    void receive(const PeerBootstrapMessage& message,PeerBootstrapEndpoint source,quint64 now);
    std::vector<Transmission> poll(quint64 now);
    std::vector<Failure> takeFailures();
    std::size_t pending() const noexcept { return pairs_.size(); }
private:
    enum class Phase { Candidates, Ready, Outcomes, Complete };
    struct Side {
        QString peer;
        PeerBootstrapEndpoint endpoint,local;
        bool candidate=false,ready=false,success=false,outcome=false,ack=false;
    };
    struct Pair {
        QString request;
        std::array<Side,2> sides;
        Phase phase=Phase::Candidates;
        quint64 introduction=0, deadline=0, nextSend=0;
        quint8 attempt=0;
        bool lan=false;
    };
    void nextAttempt(Pair& pair,quint64 now);
    quint64 session_;
    QString creator_;
    std::map<QString,PeerBootstrapEndpoint> members_;
    std::map<QString,Pair> pairs_;
    std::vector<Failure> failures_;
};
} // namespace jam2::application
