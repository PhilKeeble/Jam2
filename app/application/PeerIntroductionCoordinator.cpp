#include "PeerIntroductionCoordinator.hpp"
#include <algorithm>

namespace jam2::application {
namespace {
bool privateAddress(quint32 address)
{
    return (address>>24)==10 || (address>>24)==127 || (address>>16)==0xc0a8 ||
        (address>>20)==0xac1 || (address>>16)==0xa9fe;
}
}
PeerIntroductionCoordinator::PeerIntroductionCoordinator(quint64 session,QString creator)
    : session_(session),creator_(std::move(creator)) {}
void PeerIntroductionCoordinator::admit(const QString& peer,PeerBootstrapEndpoint endpoint)
{ if (endpoint.valid()) members_[peer]=endpoint; }
void PeerIntroductionCoordinator::remove(const QString& peer)
{
    members_.erase(peer);
    std::erase_if(pairs_,[&](const auto& entry) {
        return entry.second.sides[0].peer==peer || entry.second.sides[1].peer==peer;
    });
}
void PeerIntroductionCoordinator::nextAttempt(Pair& pair,quint64 now)
{
    ++pair.attempt; pair.introduction=jam2::random_u64(); pair.phase=Phase::Candidates;
    pair.deadline=now+kAttemptTimeoutUs; pair.nextSend=0; pair.lan=false;
    for (auto& side : pair.sides) {
        side.endpoint={}; side.local={}; side.candidate=side.ready=side.success=side.outcome=side.ack=false;
    }
}
bool PeerIntroductionCoordinator::introduce(const QString& a,const QString& b,const QString& request,quint64 now)
{
    if (a==b || a==creator_ || b==creator_ || !members_.contains(a) || !members_.contains(b) || pairs_.contains(request)) return false;
    for (const auto& [id,pair] : pairs_) {
        (void)id;
        if ((pair.sides[0].peer==a && pair.sides[1].peer==b) || (pair.sides[0].peer==b && pair.sides[1].peer==a)) return false;
    }
    Pair pair; pair.request=request; pair.sides[0].peer=a; pair.sides[1].peer=b; nextAttempt(pair,now);
    pairs_.emplace(request,std::move(pair)); return true;
}
void PeerIntroductionCoordinator::receive(const PeerBootstrapMessage& message,PeerBootstrapEndpoint source,quint64 now)
{
    const auto found=pairs_.find(message.request);
    if (found==pairs_.end()) return;
    auto& pair=found->second;
    if (pair.introduction!=message.introduction || pair.attempt!=message.attempt) return;
    const auto side=std::find_if(pair.sides.begin(),pair.sides.end(),[&](const Side& value) { return value.peer==message.sender; });
    if (side==pair.sides.end()) return;
    if (message.kind==PeerBootstrapKind::Candidate && pair.phase==Phase::Candidates) {
        if (!side->candidate) { side->endpoint=source; side->local=message.local; side->candidate=true; }
        if (pair.sides[0].candidate && pair.sides[1].candidate) {
            pair.lan=(pair.attempt%2)==1 && pair.sides[0].endpoint.address==pair.sides[1].endpoint.address &&
                privateAddress(pair.sides[0].local.address) && privateAddress(pair.sides[1].local.address) &&
                pair.sides[0].local.valid() && pair.sides[1].local.valid();
            pair.phase=Phase::Ready; pair.deadline=now+kAttemptTimeoutUs; pair.nextSend=0;
        }
        return;
    }
    if (side->endpoint!=source) return;
    if (message.kind==PeerBootstrapKind::Ready && pair.phase==Phase::Ready) {
        side->ready=true;
        if (pair.sides[0].ready && pair.sides[1].ready) {
            pair.phase=Phase::Outcomes; pair.deadline=now+kAttemptTimeoutUs; pair.nextSend=0;
        }
    } else if (message.kind==PeerBootstrapKind::Outcome && pair.phase==Phase::Outcomes) {
        side->outcome=true; side->success=message.success;
        if (!message.success) pair.deadline=now;
        else if (pair.sides[0].outcome && pair.sides[1].outcome) {
            pair.phase=Phase::Complete; pair.deadline=now+8000000; pair.nextSend=0;
        }
    } else if (message.kind==PeerBootstrapKind::CompleteAck && pair.phase==Phase::Complete) side->ack=true;
}
std::vector<PeerIntroductionCoordinator::Transmission> PeerIntroductionCoordinator::poll(quint64 now)
{
    std::vector<Transmission> output;
    for (auto it=pairs_.begin();it!=pairs_.end();) {
        auto& pair=it->second;
        if (pair.phase==Phase::Complete && pair.sides[0].ack && pair.sides[1].ack) { it=pairs_.erase(it); continue; }
        if (now>=pair.deadline) {
            if (pair.attempt>=kMaximumAttempts) {
                failures_.push_back({pair.sides[0].peer,pair.sides[1].peer,pair.request}); it=pairs_.erase(it); continue;
            }
            nextAttempt(pair,now);
        }
        if (now>=pair.nextSend) {
            for (std::size_t index=0;index<2;++index) {
                const auto& side=pair.sides[index]; const auto& other=pair.sides[1-index];
                PeerBootstrapMessage message; message.session=session_; message.sender=creator_;
                message.request=pair.request; message.introduction=pair.introduction; message.attempt=pair.attempt;
                PeerBootstrapEndpoint destination=side.endpoint;
                if (pair.phase==Phase::Candidates) { message.kind=PeerBootstrapKind::Prepare; message.partner=other.peer; destination=members_.at(side.peer); }
                else if (pair.phase==Phase::Ready) {
                    message.kind=PeerBootstrapKind::Offer; message.partner=other.peer;
                    message.endpoint=pair.lan ? other.local : other.endpoint; message.observed=side.endpoint; message.lan=pair.lan;
                } else if (pair.phase==Phase::Outcomes) message.kind=PeerBootstrapKind::Go;
                else { if (side.ack) continue; message.kind=PeerBootstrapKind::Complete; }
                output.push_back({destination,std::move(message)});
            }
            pair.nextSend=now+kRepeatUs;
        }
        ++it;
    }
    return output;
}
std::vector<PeerIntroductionCoordinator::Failure> PeerIntroductionCoordinator::takeFailures()
{ auto result=std::move(failures_); failures_.clear(); return result; }
} // namespace jam2::application
