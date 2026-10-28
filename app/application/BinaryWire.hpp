#pragma once

#include <QByteArray>
#include <QString>
#include <QtEndian>
#include <type_traits>

namespace jam2::application::binary_wire {
// Explicit network byte order; never serialize a native struct's memory layout.
struct Writer {
    QByteArray bytes;
    bool okay = true;
    template<class T> void number(T value) {
        static_assert(std::is_unsigned_v<T>);
        const auto offset = bytes.size(); bytes.resize(offset + sizeof(T));
        qToBigEndian<T>(value, reinterpret_cast<uchar*>(bytes.data() + offset));
    }
    void fixed(const QByteArray& value, qsizetype size) {
        if (value.size() != size) { okay = false; return; }
        bytes += value;
    }
    void hex(const QString& value, qsizetype size) {
        const auto encoded = value.toLatin1();
        if (encoded.size() != 2 * size) { okay = false; return; }
        for (const char c : encoded) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) { okay = false; return; }
        fixed(QByteArray::fromHex(encoded), size);
    }
};
struct Reader {
    const QByteArray& bytes;
    qsizetype offset = 0;
    bool okay = true;
    QByteArray take(qsizetype count) {
        if (count < 0 || count > bytes.size() - offset) { okay = false; return {}; }
        const auto value = bytes.mid(offset, count); offset += count; return value;
    }
    template<class T> T number() {
        static_assert(std::is_unsigned_v<T>);
        if (sizeof(T) > static_cast<std::size_t>(bytes.size() - offset)) { okay = false; return 0; }
        const auto value = qFromBigEndian<T>(reinterpret_cast<const uchar*>(bytes.constData() + offset));
        offset += sizeof(T); return value;
    }
    QString hex(qsizetype size) { return QString::fromLatin1(take(size).toHex()); }
    bool done() const { return okay && offset == bytes.size(); }
};
struct Endpoint {
    quint32 address = 0;
    quint16 port = 0;
    bool valid() const { return address != 0 && port != 0; }
    bool empty() const { return address == 0 && port == 0; }
    static Endpoint from(const QString& host, quint16 port) {
        const auto input = host.toLatin1(); quint32 address = 0; int parts = 0, octet = 0, digits = 0;
        for (qsizetype i = 0; i <= input.size(); ++i) {
            const char c = i == input.size() ? '.' : input[i];
            if (c == '.') {
                if (!digits || ++parts > 4) return {};
                address = (address << 8) | static_cast<quint32>(octet); octet = digits = 0;
            } else {
                if (c < '0' || c > '9' || ++digits > 3) return {};
                octet = octet * 10 + c - '0'; if (octet > 255) return {};
            }
        }
        return parts == 4 ? Endpoint{address, port} : Endpoint{};
    }
    QString host() const {
        return QStringLiteral("%1.%2.%3.%4").arg(address >> 24).arg((address >> 16) & 255).arg((address >> 8) & 255).arg(address & 255);
    }
    QString text() const { return valid() ? host() + QLatin1Char(':') + QString::number(port) : QString{}; }
    static Endpoint fromText(const QString& text) {
        const auto colon = text.lastIndexOf(QLatin1Char(':'));
        bool okay = false; const auto port = text.mid(colon + 1).toUInt(&okay);
        return colon > 0 && okay && port > 0 && port <= 65535 ? from(text.left(colon), static_cast<quint16>(port)) : Endpoint{};
    }
    friend bool operator==(const Endpoint&, const Endpoint&) = default;
};
inline void writeEndpoint(Writer& w, Endpoint endpoint) { w.number(endpoint.address); w.number(endpoint.port); }
inline Endpoint readEndpoint(Reader& r) { return {r.number<quint32>(), r.number<quint16>()}; }
} // namespace jam2::application::binary_wire
