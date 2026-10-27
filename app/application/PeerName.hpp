#pragma once

#include <QByteArray>
#include <QChar>
#include <QList>
#include <QString>
#include <optional>
#include <utility>

namespace jam2::application {

// A session display name, never an authentication identity or a packet address.
// NFC Unicode letters, at most eight scalar values and 32 UTF-8 bytes.
class PeerName final {
public:
    PeerName() = default;
    static constexpr qsizetype kMaximumLetters = 8, kMaximumUtf8Bytes = 32;

    static std::optional<PeerName> fromInput(const QString& input)
    {
        // Bound normalization work too, including decomposed input from a GUI.
        if (input.isEmpty() || input.size() > 32) return {};
        const auto text = input.normalized(QString::NormalizationForm_C);
        const auto letters = text.toUcs4();
        if (letters.isEmpty() || letters.size() > kMaximumLetters) return {};
        for (const auto letter : letters) {
            if (!QChar::isLetter(letter) || letter == 0x115f || letter == 0x1160 ||
                letter == 0x3164 || letter == 0xffa0) return {}; // invisible letter fillers
        }
        if (text.toUtf8().size() > kMaximumUtf8Bytes) return {};
        return PeerName(text);
    }
    static std::optional<PeerName> fromWire(const QByteArray& utf8)
    {
        if (utf8.isEmpty() || utf8.size() > kMaximumUtf8Bytes) return {};
        const auto decoded = QString::fromUtf8(utf8);
        if (decoded.toUtf8() != utf8) return {}; // reject malformed/non-shortest UTF-8
        auto name = fromInput(decoded);
        if (!name || name->text_ != decoded) return {}; // wire is canonical NFC
        return name;
    }
    const QString& text() const noexcept { return text_; }
    bool valid() const noexcept { return !text_.isEmpty(); }
    QByteArray utf8() const { return text_.toUtf8(); }
    friend bool operator==(const PeerName&, const PeerName&) = default;
private:
    explicit PeerName(QString text) : text_(std::move(text)) {}
    QString text_;
};

inline QString peerNameRequirement()
{
    return QStringLiteral("Enter a name of 1 to 8 letters. Numbers, spaces and symbols are not allowed.");
}
} // namespace jam2::application
