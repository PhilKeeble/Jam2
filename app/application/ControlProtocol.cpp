#include "ControlProtocol.hpp"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>

#include <algorithm>

namespace jam2::control_protocol {
namespace {

QByteArray hmacSha256(const QByteArray& key, const QByteArray& data);

void appendU32(QByteArray& out, quint32 value)
{
    out.append(static_cast<char>((value >> 24) & 0xffU));
    out.append(static_cast<char>((value >> 16) & 0xffU));
    out.append(static_cast<char>((value >> 8) & 0xffU));
    out.append(static_cast<char>(value & 0xffU));
}

void appendU64(QByteArray& out, quint64 value)
{
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.append(static_cast<char>((value >> shift) & 0xffU));
    }
}

quint32 readU32(const QByteArray& data, qsizetype offset)
{
    return (static_cast<quint32>(static_cast<unsigned char>(data[offset])) << 24) |
        (static_cast<quint32>(static_cast<unsigned char>(data[offset + 1])) << 16) |
        (static_cast<quint32>(static_cast<unsigned char>(data[offset + 2])) << 8) |
        static_cast<quint32>(static_cast<unsigned char>(data[offset + 3]));
}

void appendField(QByteArray& out, const QByteArray& value)
{
    appendU32(out, static_cast<quint32>(value.size()));
    out.append(value);
}

QByteArray frameBody(const QByteArray& body)
{
    QByteArray frame;
    frame.reserve(4 + body.size());
    appendU32(frame, static_cast<quint32>(body.size()));
    frame.append(body);
    return frame;
}

QByteArray hmacSha256(const QByteArray& key, const QByteArray& data)
{
    return QMessageAuthenticationCode::hash(data, key, QCryptographicHash::Sha256);
}

} // namespace

QByteArray randomNonce()
{
    QByteArray nonce;
    nonce.reserve(16);
    appendU64(nonce, QRandomGenerator::system()->generate64());
    appendU64(nonce, QRandomGenerator::system()->generate64());
    return nonce;
}

QString randomPeerToken()
{
    for (;;) {
        const QString token = encodeHex(randomNonce());
        if (peerIdFromToken(token).has_value()) {
            return token;
        }
    }
}

QByteArray decodeHex(const QString& value, qsizetype expectedBytes)
{
    const QByteArray encoded = value.toLatin1();
    if (encoded.size() != expectedBytes * 2) {
        return {};
    }
    for (const char valueChar : encoded) {
        const bool hex = (valueChar >= '0' && valueChar <= '9') ||
            (valueChar >= 'a' && valueChar <= 'f') ||
            (valueChar >= 'A' && valueChar <= 'F');
        if (!hex) {
            return {};
        }
    }
    const QByteArray decoded = QByteArray::fromHex(encoded);
    return decoded.size() == expectedBytes ? decoded : QByteArray{};
}

QString encodeHex(const QByteArray& value)
{
    return QString::fromLatin1(value.toHex());
}

std::optional<quint64> peerIdFromToken(const QString& token)
{
    const QByteArray decoded = decodeHex(token, 16);
    if (decoded.size() != 16) {
        return std::nullopt;
    }

    quint64 peerId = 0;
    for (qsizetype index = 0; index < 8; ++index) {
        peerId = (peerId << 8U) |
            static_cast<quint64>(static_cast<unsigned char>(decoded.at(index)));
    }
    return peerId == 0 ? std::nullopt : std::optional<quint64>(peerId);
}

QByteArray makeTranscript(
    const QString& sessionHex,
    const QByteArray& serverNonce,
    const QByteArray& clientNonce,
    const QString& peerToken,
    const QString& udpEndpoint,
    const QString& channel,
    const QString& peerName)
{
    QByteArray transcript=QByteArrayLiteral("jam2-control-wire-v")+QByteArray::number(kControlProtocolVersion);
    appendField(transcript, sessionHex.toLower().toLatin1());
    appendField(transcript, serverNonce);
    appendField(transcript, clientNonce);
    appendField(transcript, peerToken.toUtf8());
    appendField(transcript, udpEndpoint.toUtf8());
    appendField(transcript, channel.toUtf8());
    appendField(transcript, peerName.toUtf8());
    return transcript;
}

QByteArray keyedValue(const QByteArray& masterKey, const QByteArray& domain, const QByteArray& transcript)
{
    QByteArray input;
    input.reserve(domain.size() + transcript.size() + 4);
    appendField(input, domain);
    input.append(transcript);
    return hmacSha256(masterKey, input);
}

bool constantTimeEqual(const QByteArray& left, const QByteArray& right)
{
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (qsizetype i = 0; i < left.size(); ++i) {
        difference |= static_cast<unsigned char>(left[i]) ^ static_cast<unsigned char>(right[i]);
    }
    return difference == 0;
}

QString rejectionText(HandshakeRejection reason)
{
    switch (reason) {
    case HandshakeRejection::InvalidFields: return QStringLiteral("authentication fields are invalid");
    case HandshakeRejection::InvalidProof: return QStringLiteral("authentication proof is invalid");
    case HandshakeRejection::DuplicatePeer: return QStringLiteral("peer token is already active");
    case HandshakeRejection::ControlRequired: return QStringLiteral("asset stream requires an authenticated control peer");
    case HandshakeRejection::Capacity: return QStringLiteral("authentication capacity reached");
    }
    return QStringLiteral("unknown authentication rejection");
}
QByteArray encodeHandshake(const Handshake& message)
{
    using namespace jam2::application::binary_wire;
    Writer w; w.bytes.reserve(kMaximumHandshakeBodyBytes); w.number<quint8>(kControlProtocolVersion);
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Challenge>) {
            w.number<quint8>(1); w.hex(value.session, 8); w.fixed(value.nonce, 16);
        } else if constexpr (std::is_same_v<T, Proof>) {
            w.number<quint8>(2); w.fixed(value.nonce, 16); w.hex(value.peer, 16); w.fixed(value.proof, 16);
            w.number<quint8>(static_cast<quint8>(value.channel));
            if (!peerIdFromToken(value.peer)) w.okay = false;
            if (value.channel == HandshakeChannel::Control) {
                if (!value.endpoint.valid() || !value.name.valid()) w.okay = false;
                writeEndpoint(w, value.endpoint);
                const auto name = value.name.utf8(); w.number<quint8>(static_cast<quint8>(name.size())); w.bytes += name;
            } else if (value.channel != HandshakeChannel::Asset || !value.endpoint.empty() || value.name.valid()) w.okay = false;
        } else if constexpr (std::is_same_v<T, Accepted>) {
            w.number<quint8>(3); w.fixed(value.proof, 16);
        } else {
            w.number<quint8>(4); w.number<quint8>(static_cast<quint8>(value.reason));
            if (value.reason < HandshakeRejection::InvalidFields || value.reason > HandshakeRejection::Capacity) w.okay = false;
        }
    }, message);
    return w.okay && w.bytes.size() <= kMaximumHandshakeBodyBytes ? frameBody(w.bytes) : QByteArray{};
}

TakeFrameResult takeFrame(QByteArray& buffer, QByteArray& body, QString& error, qsizetype maximumBodyBytes)
{
    if (buffer.size() < 4) {
        return TakeFrameResult::NeedMore;
    }
    const quint32 size = readU32(buffer, 0);
    if (size == 0 || size > static_cast<quint32>(maximumBodyBytes)) {
        error = QStringLiteral("control frame length exceeds the current record bound");
        return TakeFrameResult::Invalid;
    }
    if (buffer.size() < 4 + static_cast<qsizetype>(size)) {
        return TakeFrameResult::NeedMore;
    }
    body = buffer.mid(4, size);
    buffer.remove(0, 4 + size);
    return TakeFrameResult::Ready;
}

bool decodeHandshake(const QByteArray& body, Handshake& message, QString& error)
{
    using namespace jam2::application::binary_wire;
    Reader r{body}; Handshake decoded;
    if (body.size() > kMaximumHandshakeBodyBytes || r.number<quint8>() != kControlProtocolVersion) {
        error = QStringLiteral("unsupported or oversized binary handshake"); return false;
    }
    switch (r.number<quint8>()) {
    case 1: { Challenge m; m.session = r.hex(8); m.nonce = r.take(16); decoded = std::move(m); break; }
    case 2: {
        Proof m; m.nonce = r.take(16); m.peer = r.hex(16); m.proof = r.take(16);
        m.channel = static_cast<HandshakeChannel>(r.number<quint8>());
        if (m.channel == HandshakeChannel::Control) {
            m.endpoint = readEndpoint(r);
            const auto size = r.number<quint8>();
            if (size > jam2::application::PeerName::kMaximumUtf8Bytes) r.okay = false;
            const auto name = jam2::application::PeerName::fromWire(r.take(size));
            if (!m.endpoint.valid() || !name) r.okay = false;
            else m.name = *name;
        } else if (m.channel != HandshakeChannel::Asset) r.okay = false;
        if (!peerIdFromToken(m.peer)) r.okay = false;
        decoded = std::move(m); break;
    }
    case 3: decoded = Accepted{r.take(16)}; break;
    case 4: {
        const auto reason = static_cast<HandshakeRejection>(r.number<quint8>());
        if (reason < HandshakeRejection::InvalidFields || reason > HandshakeRejection::Capacity) r.okay = false;
        decoded = Rejected{reason}; break;
    }
    default: r.okay = false; break;
    }
    if (!r.done()) { error = QStringLiteral("invalid binary handshake fields or length"); return false; }
    message = std::move(decoded); return true;
}

} // namespace jam2::control_protocol
