#include "PeerName.hpp"
#include <iostream>
#include <stdexcept>

using jam2::application::PeerName;
void require(bool okay, const char* why) { if (!okay) throw std::runtime_error(why); }
int main()
{
    try {
        require(PeerName::fromInput("Phil").has_value(), "plain letters rejected");
        require(PeerName::fromInput(QString::fromUtf8("Élodie")).has_value(), "Unicode letters rejected");
        require(PeerName::fromInput(QString::fromUtf8("李明")).has_value(), "CJK letters rejected");
        const auto decomposed=PeerName::fromInput(QStringLiteral("E\u0301lodie"));
        require(decomposed && decomposed->text()==QString::fromUtf8("Élodie"), "NFC normalization");
        const char32_t astral=0x10400;
        const auto letter=QString::fromUcs4(&astral,1);
        const auto maximum=PeerName::fromInput(letter.repeated(8));
        require(maximum && maximum->utf8().size()==32, "eight scalar letters/UTF8 byte bound");
        require(!PeerName::fromInput(letter.repeated(9)), "nine astral letters accepted");
        for (const auto& input : {QString{},QStringLiteral("Alexander"),QStringLiteral("Peer1"),QStringLiteral("A B"),
                QStringLiteral("<script>"),QStringLiteral("A\nB"),QStringLiteral("\u200d"),QStringLiteral("\u3164"),
                QStringLiteral("\u202eABC"),QStringLiteral("\u0301"),QString(100000,QLatin1Char('A'))})
            require(!PeerName::fromInput(input), "invalid name accepted");
        require(!PeerName::fromWire(QByteArray::fromHex("c081")), "overlong UTF8 accepted");
        require(!PeerName::fromWire(QByteArray::fromHex("eda080")), "surrogate UTF8 accepted");
        require(!PeerName::fromWire(QByteArray::fromHex("41ff42")), "malformed UTF8 accepted");
        require(!PeerName::fromWire(QStringLiteral("E\u0301").toUtf8()), "noncanonical wire accepted");
        require(PeerName::fromWire(maximum->utf8())==maximum, "maximum name roundtrip");
        require(PeerName::fromInput("Alex")==PeerName::fromInput("Alex"), "duplicate display names must be allowed");
        std::cout << "peer name Unicode, bounds and invalid input checks passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
