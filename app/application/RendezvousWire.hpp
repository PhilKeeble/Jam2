#pragma once
#include "BinaryWire.hpp"
#include <optional>

namespace jam2::application::rendezvous_wire {
inline constexpr quint8 kVersion = 2;
inline constexpr qsizetype kMaximumBytes = 128;
using Endpoint = binary_wire::Endpoint;
enum class Kind : quint8 {
    Create = 1,
    Join = 2,
    Recover = 3,
    Member = 4,
    Pause = 5,
    Leave = 6,
    Admit = 7,
    Remove = 8,
    Reject = 9,
    Pair = 10,
    Candidate = 11,
    Ready = 12,
    Outcome = 13,
    PairAck = 14,
    Created = 32,
    CreateChallenge = 33,
    Paused = 34,
    Wait = 35,
    JoinRequest = 36,
    Error = 37,
    AdmitAck = 38,
    RemoveAck = 39,
    RejectAck = 40,
    Prepare = 41,
    Registered = 42,
    Offer = 43,
    Go = 44,
    Connected = 45,
    Failed = 46,
};
enum class Error : quint8 { None = 0,
    CredentialsOrCreator = 1,
    Capacity = 2,
    Unavailable = 3,
    Credentials = 4,
    JamFull = 5,
    Declined = 6,
    InvalidSchema = 7,
};
struct Message {
    Kind kind = Kind::Join;
    QString session, key, peer, to, creator, nonce, a, b;
    quint8 rejectedKind = 0;
    QString reference;
    quint64 directoryRevision = 0, memberSequence = 0, introduction = 0;
    quint32 maximumPeers = 0, memberCount = 0;
    quint8 attempt = 0;
    Endpoint endpoint, observed, local;
    bool success = false, lan = false;
    Error error = Error::None;
};
inline QString errorText(Error error) {
    switch (error) {
    case Error::CredentialsOrCreator: return QStringLiteral("session credentials or creator do not match");
    case Error::Capacity: return QStringLiteral("Jam2 Server is at capacity. Try again in a few minutes, or use STUN / Direct mode.");
    case Error::Unavailable: return QStringLiteral("jam unavailable or full");
    case Error::Credentials: return QStringLiteral("session credentials do not match");
    case Error::JamFull: return QStringLiteral("The jam has reached its participant limit.");
    case Error::Declined: return QStringLiteral("creator declined admission or jam is full");
    case Error::InvalidSchema: return QStringLiteral("Jam2 Server could not understand this request. Your version may be incompatible; please update Jam2.");
    default: return QStringLiteral("unknown rendezvous error");
    }
}
inline bool request(Kind kind) { return kind >= Kind::Create && kind <= Kind::PairAck; }
inline QByteArray encode(const Message& m) {
    binary_wire::Writer w; w.bytes.reserve(kMaximumBytes); w.bytes = QByteArrayLiteral("J2RV"); w.number(kVersion); w.number(static_cast<quint8>(m.kind));
    w.hex(m.session, 8);
    if (request(m.kind)) { w.hex(m.peer, 16); w.hex(m.key, 16); }
    else w.hex(m.to, 16);
    switch (m.kind) {
    case Kind::Create: { w.number(m.directoryRevision); if (m.maximumPeers > INT32_MAX) w.okay = false; w.number(m.maximumPeers); if (m.memberCount > INT32_MAX) w.okay = false; w.number(m.memberCount); w.number<quint8>(m.nonce.isEmpty() ? 0 : 1); if (!m.nonce.isEmpty()) w.hex(m.nonce, 16); break; }
    case Kind::Join: {  break; }
    case Kind::Recover: {  break; }
    case Kind::Member: {  break; }
    case Kind::Pause: { w.number(m.directoryRevision); break; }
    case Kind::Leave: { w.number(m.directoryRevision); break; }
    case Kind::Admit: { w.hex(m.to, 16); w.number(m.memberSequence); break; }
    case Kind::Remove: { w.hex(m.to, 16); w.number(m.memberSequence); break; }
    case Kind::Reject: { w.hex(m.to, 16); w.number(m.memberSequence); break; }
    case Kind::Pair: { w.hex(m.nonce, 16); w.hex(m.a, 16); w.hex(m.b, 16); break; }
    case Kind::Candidate: { w.hex(m.nonce, 16); if (m.attempt < 1 || m.attempt > 10) w.okay = false; w.number(m.attempt); w.number(m.introduction); if (!m.local.valid() && !m.local.empty()) w.okay = false; binary_wire::writeEndpoint(w, m.local); break; }
    case Kind::Ready: { w.hex(m.nonce, 16); if (m.attempt < 1 || m.attempt > 10) w.okay = false; w.number(m.attempt); w.number(m.introduction); break; }
    case Kind::Outcome: { w.hex(m.nonce, 16); if (m.attempt < 1 || m.attempt > 10) w.okay = false; w.number(m.attempt); w.number(m.introduction); w.number<quint8>(m.success ? 1 : 0); break; }
    case Kind::PairAck: { w.hex(m.nonce, 16); if (m.attempt < 1 || m.attempt > 10) w.okay = false; w.number(m.attempt); w.number(m.introduction); break; }
    case Kind::Created: { w.number(m.directoryRevision); w.hex(m.creator, 16); break; }
    case Kind::CreateChallenge: { w.number(m.directoryRevision); w.hex(m.nonce, 16); break; }
    case Kind::Paused: { w.number(m.directoryRevision); break; }
    case Kind::Wait: { w.hex(m.creator, 16); break; }
    case Kind::JoinRequest: { w.hex(m.peer, 16); break; }
    case Kind::Error: { if (m.error < Error::CredentialsOrCreator || m.error > Error::InvalidSchema) w.okay = false; w.number<quint8>(static_cast<quint8>(m.error)); if (m.rejectedKind > 14) w.okay = false; w.number(m.rejectedKind); if (m.rejectedKind >= 7) w.hex(m.reference, 16); break; }
    case Kind::AdmitAck: { w.hex(m.peer, 16); w.number(m.memberSequence); break; }
    case Kind::RemoveAck: { w.hex(m.peer, 16); w.number(m.memberSequence); break; }
    case Kind::RejectAck: { w.hex(m.peer, 16); w.number(m.memberSequence); break; }
    case Kind::Prepare: { w.hex(m.nonce, 16); if (m.attempt < 1 || m.attempt > 10) w.okay = false; w.number(m.attempt); w.number(m.introduction); w.hex(m.peer, 16); break; }
    case Kind::Registered: { w.hex(m.nonce, 16); if (m.attempt < 1 || m.attempt > 10) w.okay = false; w.number(m.attempt); w.number(m.introduction); w.hex(m.peer, 16); if (!m.observed.valid()) w.okay = false; binary_wire::writeEndpoint(w, m.observed); break; }
    case Kind::Offer: { w.hex(m.nonce, 16); if (m.attempt < 1 || m.attempt > 10) w.okay = false; w.number(m.attempt); w.number(m.introduction); w.hex(m.peer, 16); if (!m.endpoint.valid()) w.okay = false; binary_wire::writeEndpoint(w, m.endpoint); if (!m.observed.valid()) w.okay = false; binary_wire::writeEndpoint(w, m.observed); w.number<quint8>(m.lan ? 1 : 0); break; }
    case Kind::Go: { w.hex(m.nonce, 16); if (m.attempt < 1 || m.attempt > 10) w.okay = false; w.number(m.attempt); w.number(m.introduction); w.hex(m.peer, 16); break; }
    case Kind::Connected: { w.hex(m.nonce, 16); if (m.attempt < 1 || m.attempt > 10) w.okay = false; w.number(m.attempt); w.number(m.introduction); w.hex(m.peer, 16); w.hex(m.a, 16); w.hex(m.b, 16); break; }
    case Kind::Failed: { w.hex(m.nonce, 16); if (m.attempt < 1 || m.attempt > 10) w.okay = false; w.number(m.attempt); w.number(m.introduction); w.hex(m.peer, 16); w.hex(m.a, 16); w.hex(m.b, 16); break; }
    default: w.okay = false; break;
    }
    return w.okay && w.bytes.size() <= kMaximumBytes ? w.bytes : QByteArray{};
}
inline std::optional<Message> decode(const QByteArray& bytes) {
    if (bytes.size() < 6 || bytes.size() > kMaximumBytes || !bytes.startsWith("J2RV")) return {};
    binary_wire::Reader r{bytes}; r.take(4);
    if (r.number<quint8>() != kVersion) return {};
    Message m; m.kind = static_cast<Kind>(r.number<quint8>()); m.session = r.hex(8);
    if (request(m.kind)) { m.peer = r.hex(16); m.key = r.hex(16); }
    else m.to = r.hex(16);
    switch (m.kind) {
    case Kind::Create: { m.directoryRevision = r.number<quint64>(); m.maximumPeers = r.number<quint32>(); if (m.maximumPeers > INT32_MAX) r.okay = false; m.memberCount = r.number<quint32>(); if (m.memberCount > INT32_MAX) r.okay = false; { const auto present = r.number<quint8>(); if (present > 1) r.okay = false; if (present == 1) m.nonce = r.hex(16); } break; }
    case Kind::Join: {  break; }
    case Kind::Recover: {  break; }
    case Kind::Member: {  break; }
    case Kind::Pause: { m.directoryRevision = r.number<quint64>(); break; }
    case Kind::Leave: { m.directoryRevision = r.number<quint64>(); break; }
    case Kind::Admit: { m.to = r.hex(16); m.memberSequence = r.number<quint64>(); break; }
    case Kind::Remove: { m.to = r.hex(16); m.memberSequence = r.number<quint64>(); break; }
    case Kind::Reject: { m.to = r.hex(16); m.memberSequence = r.number<quint64>(); break; }
    case Kind::Pair: { m.nonce = r.hex(16); m.a = r.hex(16); m.b = r.hex(16); break; }
    case Kind::Candidate: { m.nonce = r.hex(16); m.attempt = r.number<quint8>(); if (m.attempt < 1 || m.attempt > 10) r.okay = false; m.introduction = r.number<quint64>(); m.local = binary_wire::readEndpoint(r); if (!m.local.valid() && !m.local.empty()) r.okay = false; break; }
    case Kind::Ready: { m.nonce = r.hex(16); m.attempt = r.number<quint8>(); if (m.attempt < 1 || m.attempt > 10) r.okay = false; m.introduction = r.number<quint64>(); break; }
    case Kind::Outcome: { m.nonce = r.hex(16); m.attempt = r.number<quint8>(); if (m.attempt < 1 || m.attempt > 10) r.okay = false; m.introduction = r.number<quint64>(); { const auto value = r.number<quint8>(); if (value > 1) r.okay = false; m.success = value == 1; } break; }
    case Kind::PairAck: { m.nonce = r.hex(16); m.attempt = r.number<quint8>(); if (m.attempt < 1 || m.attempt > 10) r.okay = false; m.introduction = r.number<quint64>(); break; }
    case Kind::Created: { m.directoryRevision = r.number<quint64>(); m.creator = r.hex(16); break; }
    case Kind::CreateChallenge: { m.directoryRevision = r.number<quint64>(); m.nonce = r.hex(16); break; }
    case Kind::Paused: { m.directoryRevision = r.number<quint64>(); break; }
    case Kind::Wait: { m.creator = r.hex(16); break; }
    case Kind::JoinRequest: { m.peer = r.hex(16); break; }
    case Kind::Error: { m.error = static_cast<Error>(r.number<quint8>()); if (m.error < Error::CredentialsOrCreator || m.error > Error::InvalidSchema) r.okay = false; m.rejectedKind = r.number<quint8>(); if (m.rejectedKind > 14) r.okay = false; if (m.rejectedKind >= 7) m.reference = r.hex(16); break; }
    case Kind::AdmitAck: { m.peer = r.hex(16); m.memberSequence = r.number<quint64>(); break; }
    case Kind::RemoveAck: { m.peer = r.hex(16); m.memberSequence = r.number<quint64>(); break; }
    case Kind::RejectAck: { m.peer = r.hex(16); m.memberSequence = r.number<quint64>(); break; }
    case Kind::Prepare: { m.nonce = r.hex(16); m.attempt = r.number<quint8>(); if (m.attempt < 1 || m.attempt > 10) r.okay = false; m.introduction = r.number<quint64>(); m.peer = r.hex(16); break; }
    case Kind::Registered: { m.nonce = r.hex(16); m.attempt = r.number<quint8>(); if (m.attempt < 1 || m.attempt > 10) r.okay = false; m.introduction = r.number<quint64>(); m.peer = r.hex(16); m.observed = binary_wire::readEndpoint(r); if (!m.observed.valid()) r.okay = false; break; }
    case Kind::Offer: { m.nonce = r.hex(16); m.attempt = r.number<quint8>(); if (m.attempt < 1 || m.attempt > 10) r.okay = false; m.introduction = r.number<quint64>(); m.peer = r.hex(16); m.endpoint = binary_wire::readEndpoint(r); if (!m.endpoint.valid()) r.okay = false; m.observed = binary_wire::readEndpoint(r); if (!m.observed.valid()) r.okay = false; { const auto value = r.number<quint8>(); if (value > 1) r.okay = false; m.lan = value == 1; } break; }
    case Kind::Go: { m.nonce = r.hex(16); m.attempt = r.number<quint8>(); if (m.attempt < 1 || m.attempt > 10) r.okay = false; m.introduction = r.number<quint64>(); m.peer = r.hex(16); break; }
    case Kind::Connected: { m.nonce = r.hex(16); m.attempt = r.number<quint8>(); if (m.attempt < 1 || m.attempt > 10) r.okay = false; m.introduction = r.number<quint64>(); m.peer = r.hex(16); m.a = r.hex(16); m.b = r.hex(16); break; }
    case Kind::Failed: { m.nonce = r.hex(16); m.attempt = r.number<quint8>(); if (m.attempt < 1 || m.attempt > 10) r.okay = false; m.introduction = r.number<quint64>(); m.peer = r.hex(16); m.a = r.hex(16); m.b = r.hex(16); break; }
    default: return {};
    }
    return r.done() ? std::optional<Message>(std::move(m)) : std::nullopt;
}
} // namespace jam2::application::rendezvous_wire
