#include "PeerBootstrap.hpp"
#include "ControlProtocol.hpp"
#include "UdpStreamConnection.hpp"
#include <QHostAddress>
#include <QMessageAuthenticationCode>
#include <QCryptographicHash>
#include <QtEndian>
#include <algorithm>

namespace jam2::application {
namespace {
constexpr qsizetype headerBytes = 32, tagBytes = 16;
bool identifier(const QString& text)
{
    return text.size()==32 && control_protocol::decodeHex(text,16).size()==16 &&
        text!=QString(32,QLatin1Char('0'));
}
struct Writer {
    QByteArray bytes;
    template<class T> void number(T value) {
        const auto at=bytes.size(); bytes.resize(at+sizeof(T));
        qToBigEndian(value,reinterpret_cast<uchar*>(bytes.data()+at));
    }
    void id(const QString& text) { bytes+=QByteArray::fromHex(text.toLatin1()); }
    void endpoint(PeerBootstrapEndpoint value) { number(value.address); number(value.port); }
};
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t at=0;
    bool okay=true;
    template<class T> T number() {
        if (at+sizeof(T)>bytes.size()) { okay=false; return {}; }
        const auto value=qFromBigEndian<T>(bytes.data()+at); at+=sizeof(T); return value;
    }
    QString id() {
        if (at+16>bytes.size()) { okay=false; return {}; }
        const auto value=QByteArray(reinterpret_cast<const char*>(bytes.data()+at),16).toHex(); at+=16;
        return QString::fromLatin1(value);
    }
    PeerBootstrapEndpoint endpoint() { return {number<quint32>(),number<quint16>()}; }
};
bool pairMessage(PeerBootstrapKind kind)
{ return kind>=PeerBootstrapKind::Prepare && kind<=PeerBootstrapKind::CompleteAck; }
bool carriesPartner(PeerBootstrapKind kind)
{ return kind==PeerBootstrapKind::Prepare || kind==PeerBootstrapKind::Offer; }
bool valid(const PeerBootstrapMessage& m)
{
    if (m.kind<PeerBootstrapKind::Join || m.kind>PeerBootstrapKind::Rejected || !m.session ||
        !identifier(m.sender) || !control_protocol::peerIdFromToken(m.sender).value_or(0) || !identifier(m.request)) return false;
    if (m.kind!=PeerBootstrapKind::Join && m.kind!=PeerBootstrapKind::Rejected && !m.introduction) return false;
    if (pairMessage(m.kind) && (m.attempt<1 || m.attempt>10)) return false;
    if (carriesPartner(m.kind) && (!identifier(m.partner) || m.partner==m.sender ||
        !control_protocol::peerIdFromToken(m.partner).value_or(0))) return false;
    if (m.kind==PeerBootstrapKind::Welcome && !m.observed.valid()) return false;
    if (m.kind==PeerBootstrapKind::Offer && (!m.endpoint.valid() || !m.observed.valid())) return false;
    if (m.kind==PeerBootstrapKind::Candidate && ((m.local.address==0)!=(m.local.port==0))) return false;
    return true;
}
}
PeerBootstrapEndpoint PeerBootstrapEndpoint::from(const jam2::Endpoint& value)
{
    bool okay=false; const auto address=QHostAddress(QString::fromStdString(value.host)).toIPv4Address(&okay);
    return okay ? PeerBootstrapEndpoint{address,value.port} : PeerBootstrapEndpoint{};
}
jam2::Endpoint PeerBootstrapEndpoint::endpoint() const
{ return {QHostAddress(address).toString().toStdString(),port}; }
bool isPeerBootstrap(std::span<const std::uint8_t> packet) noexcept
{ return packet.size()>=4 && std::equal(packet.begin(),packet.begin()+4,"J2PB"); }
QByteArray encodePeerBootstrap(const PeerBootstrapMessage& m,const QByteArray& key)
{
    if (key.size()!=16 || !valid(m)) return {};
    Writer w; w.bytes="J2PB"; w.number<quint8>(kUdpMuxVersion); w.number<quint8>(static_cast<quint8>(m.kind));
    w.number<quint16>(0); w.number(m.session); w.id(m.sender); w.id(m.request);
    if (m.kind!=PeerBootstrapKind::Join && m.kind!=PeerBootstrapKind::Rejected) w.number(m.introduction);
    if (pairMessage(m.kind)) w.number(m.attempt);
    if (carriesPartner(m.kind)) w.id(m.partner);
    if (m.kind==PeerBootstrapKind::Welcome) w.endpoint(m.observed);
    if (m.kind==PeerBootstrapKind::Candidate) w.endpoint(m.local);
    if (m.kind==PeerBootstrapKind::Offer) { w.endpoint(m.endpoint); w.endpoint(m.observed); w.number<quint8>(m.lan ? 1 : 0); }
    if (m.kind==PeerBootstrapKind::Outcome) w.number<quint8>(m.success ? 1 : 0);
    qToBigEndian(static_cast<quint16>(w.bytes.size()-headerBytes),reinterpret_cast<uchar*>(w.bytes.data()+6));
    w.bytes+=QMessageAuthenticationCode::hash(w.bytes,key,QCryptographicHash::Sha256).first(tagBytes);
    return w.bytes.size()<=kPeerBootstrapMaximumBytes ? w.bytes : QByteArray{};
}
std::optional<PeerBootstrapMessage> decodePeerBootstrap(std::span<const std::uint8_t> packet,quint64 session,const QByteArray& key)
{
    if (key.size()!=16 || packet.size()<64 || packet.size()>kPeerBootstrapMaximumBytes || !isPeerBootstrap(packet) ||
        packet[4]!=kUdpMuxVersion || qFromBigEndian<quint16>(packet.data()+6)!=packet.size()-headerBytes-tagBytes ||
        qFromBigEndian<quint64>(packet.data()+8)!=session) return {};
    const auto content=QByteArrayView(reinterpret_cast<const char*>(packet.data()),packet.size()-tagBytes);
    const auto tag=QMessageAuthenticationCode::hash(content,key,QCryptographicHash::Sha256).first(tagBytes);
    if (!control_protocol::constantTimeEqual(tag,QByteArray::fromRawData(reinterpret_cast<const char*>(packet.data()+packet.size()-tagBytes),tagBytes))) return {};
    Reader r{packet.first(packet.size()-tagBytes),16};
    PeerBootstrapMessage m; m.kind=static_cast<PeerBootstrapKind>(packet[5]); m.session=session; m.sender=r.id(); m.request=r.id();
    if (m.kind!=PeerBootstrapKind::Join && m.kind!=PeerBootstrapKind::Rejected) m.introduction=r.number<quint64>();
    if (pairMessage(m.kind)) m.attempt=r.number<quint8>();
    if (carriesPartner(m.kind)) m.partner=r.id();
    if (m.kind==PeerBootstrapKind::Welcome) m.observed=r.endpoint();
    if (m.kind==PeerBootstrapKind::Candidate) m.local=r.endpoint();
    if (m.kind==PeerBootstrapKind::Offer) { m.endpoint=r.endpoint(); m.observed=r.endpoint(); const auto lan=r.number<quint8>(); if (lan>1) r.okay=false; m.lan=lan!=0; }
    if (m.kind==PeerBootstrapKind::Outcome) { const auto success=r.number<quint8>(); if (success>1) r.okay=false; m.success=success!=0; }
    return r.okay && r.at==r.bytes.size() && valid(m) ? std::optional{m} : std::nullopt;
}
} // namespace jam2::application
